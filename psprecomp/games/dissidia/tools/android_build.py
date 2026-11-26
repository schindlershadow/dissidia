"""Android build for psp_build.py (--android): the player's machine compiles the generated game with Google's NDK compiler against the
release's prebuilt Android core and libraries, then packs and signs an APK in pure Python. Python 3 standard library only.

Release layout (<release>/android, built by release/build_android_sdk.py):
  versions.json   pinned NDK / API / package         android.json  what the prebuilt parts were built with (checked)
  sdk/            the Android runtime core + link project (CMakeLists.txt, lib/libpsprecomp_core.a, include/, src/main.cpp)
  deps/           deps.cmake + deps/{include,lib}: SDL2 (shared), FreeType, FFmpeg (ATRAC3/3+ and H.264 decoders)
  apk/            skeleton.apk (manifest + classes.dex), apk_pack.py, debug_key.json
  libc++_shared.so
  ndk/            downloaded on first use (about 0.7 GB download, 2 GB on disk): Google's Android NDK, Apache-2.0 + component licenses
"""
import concurrent.futures
import hashlib
import json
import os
import re
import shutil
import subprocess
import stat
import sys
import urllib.request
import zipfile


def is_device():
    """True when this runs ON an Android phone (the setup app's bundled Python): there the game is compiled by the bundled zig, see device_build()."""
    return os.path.exists("/system/bin/app_process") or bool(os.environ.get("PSPRECOMP_ANDROID_DEVICE"))


def host_key():
    return "windows" if os.name == "nt" else "linux"


SYSROOT_REL = "toolchains/llvm/prebuilt/linux-x86_64/sysroot"   # the NDK's target files (headers, libc++, stub libraries) are the same for every host


def load_versions(adir):
    return json.load(open(os.path.join(adir, "versions.json")))


def ndk_ready(adir):
    v = load_versions(adir)
    marker = os.path.join(adir, "ndk", ".psprecomp-ndk")
    return os.path.isfile(marker) and open(marker).read().strip() == v["ndk"]["version"]


def fetch_ndk(adir, say, tag, die):
    """Download (once) and unpack the part of the NDK that is needed: the clang toolchain with its sysroot and the CMake build files."""
    v = load_versions(adir)
    ent = v["ndk"]["files"][host_key()]
    dest = os.path.join(adir, "ndk")
    if ndk_ready(adir):
        return dest
    os.makedirs(os.path.join(adir, ".download"), exist_ok=True)
    zp = os.path.join(adir, ".download", ent["url"].rsplit("/", 1)[1])
    if not (os.path.isfile(zp) and os.path.getsize(zp) == ent["size"]):
        say("Downloading Google's Android NDK %s (%d MB, one time) from %s" % (v["ndk"]["name"], ent["size"] // 1000000, ent["url"]))
        req = urllib.request.Request(ent["url"], headers={"User-Agent": "psprecomp-android/1"})
        part = zp + ".part"
        done = 0
        last = -1
        with urllib.request.urlopen(req, timeout=60) as r, open(part, "wb") as f:
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
                done += len(chunk)
                pct = 100 * done // ent["size"]
                if pct != last:
                    last = pct
                    tag("PROGRESS", str(min(pct, 99)))
        os.replace(part, zp)
    h = hashlib.sha1()
    with open(zp, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    if h.hexdigest() != ent["sha1"]:
        os.remove(zp)
        die("the downloaded NDK does not match its published checksum (deleted; try again)")
    say("Unpacking the NDK")
    shutil.rmtree(dest, ignore_errors=True)
    prebuilt = "toolchains/llvm/prebuilt/" + ent["prebuilt"] + "/"
    keep = (prebuilt, "build/", "meta/")
    with zipfile.ZipFile(zp) as z:
        names = [i for i in z.infolist() if i.filename.split("/", 1)[-1].startswith(keep) or
                 i.filename.split("/", 1)[-1] == "source.properties"]
        for k, info in enumerate(names):
            rel = info.filename.split("/", 1)[1]
            if not rel or info.is_dir():
                continue
            out = os.path.join(dest, rel)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            mode = info.external_attr >> 16
            if stat.S_ISLNK(mode) and os.name != "nt":
                target = z.read(info).decode()
                if os.path.lexists(out):
                    os.remove(out)
                os.symlink(target, out)
                continue
            with z.open(info) as src, open(out, "wb") as dst:
                shutil.copyfileobj(src, dst)
            if mode & 0o111 and os.name != "nt":
                os.chmod(out, 0o755)
            if k % 500 == 0:
                tag("PROGRESS", str(99))
    with open(os.path.join(dest, ".psprecomp-ndk"), "w") as f:
        f.write(v["ndk"]["version"] + "\n")
    try:
        os.remove(zp)
    except OSError:
        pass
    return dest


def check_prebuilt(adir, die):
    """The prebuilt core/libraries and the NDK that compiles the game must be the same release of the NDK."""
    v = load_versions(adir)
    ap = os.path.join(adir, "android.json")
    if not os.path.isfile(ap):
        die("this release has no Android build (android/android.json missing)")
    built = json.load(open(ap))
    if built.get("ndk") != v["ndk"]["version"] or built.get("api") != v["api"]:
        die("the release's Android core was built with NDK %s / API %s but this script pins NDK %s / API %s" %
            (built.get("ndk"), built.get("api"), v["ndk"]["version"], v["api"]))


def write_toolchain(adir, work):
    v = load_versions(adir)
    bd = os.path.join(work, "android", "build")
    os.makedirs(bd, exist_ok=True)
    tc = os.path.join(bd, "toolchain.cmake")
    ndk = os.path.join(adir, "ndk").replace("\\", "/")
    with open(tc, "w") as f:
        f.write('set(ANDROID_ABI %s)\nset(ANDROID_PLATFORM android-%d)\nset(ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES ON)\ninclude("%s/build/cmake/android.toolchain.cmake")\n'
                'set(PSPRECOMP_DEPS_CMAKE "%s/deps/deps.cmake")\n' % (v["abi"], v["api"], ndk, adir.replace("\\", "/")))
    return bd, tc


def pack_apk(adir, work, libmain, run, say, env):
    v = load_versions(adir)
    out_dir = os.path.join(work, "android")
    os.makedirs(out_dir, exist_ok=True)
    apk = os.path.join(out_dir, "dissidia.apk")
    exe = ".exe" if os.name == "nt" else ""
    stripped = os.path.join(out_dir, "libmain.so")
    if is_device():   # device_build() already links with -s
        shutil.copyfile(libmain, stripped)
    else:
        strip = os.path.join(adir, "ndk", "toolchains", "llvm", "prebuilt", v["ndk"]["files"][host_key()]["prebuilt"], "bin", "llvm-strip" + exe)
        run([strip, "-o", stripped, libmain], env=env)
    abi = v["abi"]
    run(([os.environ["PSPRECOMP_PY_LOADER"]] if os.environ.get("PSPRECOMP_PY_LOADER") else []) + [sys.executable, os.path.join(adir, "apk", "apk_pack.py"), "--skeleton", os.path.join(adir, "apk", "skeleton.apk"),
         "--key", os.path.join(adir, "apk", "debug_key.json"),
         "--lib", "lib/%s/libmain.so=%s" % (abi, stripped),
         "--lib", "lib/%s/libSDL2.so=%s" % (abi, os.path.join(adir, "deps", "deps", "lib", "libSDL2.so")),
         "--lib", "lib/%s/libc++_shared.so=%s" % (abi, os.path.join(adir, "libc++_shared.so"))]
        # libadrenotools' hook libraries (custom GPU drivers): they must sit in nativeLibraryDir, next to libmain.so
        + sum((["--lib", "lib/%s/%s=%s" % (abi, n, os.path.join(adir, "deps", "deps", "lib", n))]
               for n in ("libhook_impl.so", "libmain_hook.so", "libfile_redirect_hook.so", "libgsl_alloc_hook.so")
               if os.path.isfile(os.path.join(adir, "deps", "deps", "lib", n))), [])
        + (["--lib", "assets/shaders.pack=" + os.path.join(adir, "shaders.pack")] if os.path.isfile(os.path.join(adir, "shaders.pack")) else [])   # shaders compiled at start ("Preparing graphics")
        + ["-o", apk], env=env)
    return apk


# --------------------------------------------------------------------------------------------------------------- on the phone

# The cold batches' flags (measured, see games/dissidia/docs/native-build.md "Fast build"): -O1 compiles as slowly as -O2 on this code (huge straight-line
# functions: the time is in the inliner and the back end), -O0 is slower still (41 MB objects); -Os with no inlining is the cheapest and the smallest.
COLD_OPT = "-Os -fno-inline"
HOT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hot_funcs.txt")


def build_profile(work):
    """("fast" | "full", the cold files' -O level) for the phone's own build. PSPRECOMP_BUILD_PROFILE, else <work>/build_profile.txt
    (written by the setup app), else fast. PSPRECOMP_COLD_OPT (-O1 / -O0) is for measuring."""
    p = os.environ.get("PSPRECOMP_BUILD_PROFILE", "").strip().lower()
    if not p:
        try:
            p = open(os.path.join(work, "build_profile.txt")).read().strip().lower()
        except OSError:
            p = ""
    if p != "full":
        p = "fast"
    cold = os.environ.get("PSPRECOMP_COLD_OPT", COLD_OPT)
    return p, (cold if cold in ("-O0", "-O1", "-Os", COLD_OPT) else COLD_OPT)


def hot_functions():
    """tools/hot_funcs.txt: {group: set of entry addresses} of the functions that run a lot; group = out (the EBOOT) or an overlay name.
    No file = nothing known: every batch is hot (the same as Full)."""
    d = {}
    try:
        for l in open(HOT_FILE):
            t = l.split()
            if len(t) == 2 and not l.startswith("#"):
                d.setdefault(t[0], set()).add(int(t[1], 16))
    except OSError:
        return None
    return d or None


def src_is_batch(src):
    return os.path.basename(src).startswith("batch_")


_FUNC_RE = re.compile(rb"^RECOMP_FUNC void \w+\(.*\{ /\* 0x([0-9A-Fa-f]{8}) \*/", re.M)


def batch_is_hot(src, work, hot):
    """Does this generated batch file hold a function of the hot list? (by the entry addresses in the file: batch numbers can change)"""
    if hot is None:
        return True
    parts = os.path.normpath(src).split(os.sep)
    group = parts[-3] if len(parts) > 3 and parts[-3] != "output" and parts[-2] == "generated" and parts[-4] == "overlays" else "out"
    want = hot.get(group, ())
    with open(src, "rb") as f:
        return any(int(m.group(1), 16) in want for m in _FUNC_RE.finditer(f.read()))


MB = 1024 * 1024


def free_mb(path):
    """Free MB on the volume of `path` (the nearest existing parent); None when unknown."""
    p = os.path.abspath(path)
    while p and not os.path.exists(p) and os.path.dirname(p) != p:
        p = os.path.dirname(p)
    try:
        return shutil.disk_usage(p).free // MB
    except OSError:
        return None


def pick_zig_cache(base, need_mb, say):
    """Where zig keeps its cache while it compiles: the work folder's, unless that drive is nearly full and another folder the app
    can write to has the room (PSPRECOMP_ZIG_CACHE, the app's folder on an SD card). The cache is emptied after the build."""
    default = os.path.join(base, "zig-cache")
    cands = [default]
    if os.environ.get("PSPRECOMP_ZIG_CACHE"):
        cands.insert(0, os.environ["PSPRECOMP_ZIG_CACHE"])
    import glob
    for d in sorted(glob.glob("/storage/*/Android/data/org.psprecomp.setup")):
        cands.append(os.path.join(d, "zig-cache"))
    free = {}
    for c in cands:
        parent = os.path.dirname(c)
        try:
            os.makedirs(parent, exist_ok=True)
        except OSError:
            continue
        f = free_mb(parent)
        if f is not None and os.access(parent, os.W_OK):
            free[c] = f
    if not free:
        return default
    best = max(free, key=lambda c: free[c])
    if free.get(default, 0) >= need_mb or free[best] <= free.get(default, 0):
        return default
    say("The work folder's drive has only %d MB free (the compile needs about %d MB): zig's cache goes to %s (%d MB free)" % (
        free.get(default, 0), need_mb, best, free[best]))
    return best


def device_build(adir, work, jobs, say, tag, die, jobs_fixed=False):
    """Compile + link the generated game ON the phone with the zig that ships in the setup app (zig is clang + lld in one static binary).

    The prebuilt core (adir/sdk) and libraries (adir/deps) come from the NDK, so the game is compiled against the NDK's headers and
    libc++ (`__ndk1`) exactly like the PC build; only the compiler front end differs. Findings (RP6, 2026-11-13):
      - the NDK's libc++ headers must come with -I (zig puts its own include dir before -isystem ones);
      - `zig cc` will not link for Android ("unable to provide libc"): link with `zig ld.lld` and the NDK's crt files / libraries;
      - lld must run with --threads=1 (the parallel linker crashes on Android).
    Layout of adir (the setup app's payload): ndk/<SYSROOT_REL>, ndk/clang-rt/{libclang_rt.builtins-aarch64-android.a,libunwind.a,libatomic.a},
    zig-lib/ (zig's lib/ include + std), sdk/, deps/. The zig binary: $PSPRECOMP_ZIG (the app's libzig.so) or adir/bin/zig."""
    v = load_versions(adir)
    zig = os.environ.get("PSPRECOMP_ZIG") or os.path.join(adir, "bin", "zig")
    if not os.path.isfile(zig):
        die("the compiler (zig) was not found: %s" % zig)
    api = v["api"]
    sysroot = os.path.join(adir, "ndk", SYSROOT_REL)
    lib = sysroot + "/usr/lib/aarch64-linux-android"
    rt = os.path.join(adir, "ndk", "clang-rt")
    for need in (sysroot + "/usr/include/c++/v1", lib + "/%d/crtbegin_so.o" % api, rt + "/libclang_rt.builtins-aarch64-android.a",
                 os.path.join(adir, "sdk", "lib", "libpsprecomp_core.a")):
        if not os.path.exists(need):
            die("incomplete Android part: %s is missing" % need)
    base = os.path.join(work, "android")
    objdir = os.path.join(base, "obj")
    os.makedirs(objdir, exist_ok=True)
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=os.path.join(base, "zig-cache"), HOME=base,
               ZIG_LIB_DIR=os.path.join(adir, "zig-lib"))
    # zig keeps a second copy of every object it compiles in its cache folder, and never removes one: it grew with every update
    # (the Shield install failed with "CacheCheckFailed" on a full disk). The objects are kept in objdir by content (below), so
    # zig's cache is only needed while a compile runs: emptied before the build (a stopped one leaves it) and after it.
    if not os.environ.get("PSPRECOMP_KEEP_ZIG_CACHE"):
        shutil.rmtree(env["ZIG_GLOBAL_CACHE_DIR"], ignore_errors=True)
    # a build that was killed leaves the half-written object it was compiling (<name>.part): discarded here. A finished object is only ever put in place by
    # os.replace of a complete .part (below), so everything else in objdir is whole and is reused: a resumed build compiles only the missing files.
    for dp, _, fns in os.walk(objdir):
        for fn in fns:
            if fn.endswith(".part"):
                try:
                    os.remove(os.path.join(dp, fn))
                except OSError:
                    pass
    deps = os.path.join(adir, "deps", "deps")
    out = os.path.join(work, "output")
    ovl = os.path.join(work, "overlays")
    oi = ["-I" + out + "/include", "-I" + out]
    # (source, include flags): the same sources the SDK's CMakeLists compiles
    srcs = []
    import glob
    for f in sorted(glob.glob(out + "/generated/batch_*.cpp")) + [os.path.join(out, n) for n in
            ("dispatch.cpp", "data_sections.cpp", "init_array.cpp", "mid_entries.cpp", "syscall_table.cpp")]:
        srcs.append((f, oi))
    for name in ("battle", "menu", "extra", "supacc", "xmods"):
        d = os.path.join(ovl, name)
        inc = ["-I" + d] + oi
        files = sorted(glob.glob(d + "/generated/batch_*.cpp")) + [d + "/dispatch.cpp", d + "/mid_entries.cpp"]
        meta = os.path.join(d, "overlay.txt")
        if os.path.isfile(meta) and open(meta).read().startswith("0 "):
            files += [d + "/data_sections.cpp", d + "/syscall_table.cpp"]
        srcs += [(f, inc) for f in files]
    main_cpp = os.path.join(adir, "sdk", "src", "main.cpp")
    srcs.append((main_cpp, ["-I" + os.path.join(adir, "sdk", "include")] + oi +
                 ["-isystem", deps + "/include/SDL2", "-isystem", deps + "/include/freetype2", "-isystem", deps + "/include"]))
    flags = ["-target", "aarch64-linux-android", "-nostdinc++", "-I" + sysroot + "/usr/include/c++/v1",
             "-isystem", sysroot + "/usr/include/aarch64-linux-android", "-isystem", sysroot + "/usr/include",
             "-DANDROID", "-U__ANDROID_API__", "-D__ANDROID_API__=%d" % api, "-fdata-sections", "-ffunction-sections", "-funwind-tables",
             "-fstack-protector-strong", "-D_FORTIFY_SOURCE=2", "-std=gnu++17", "-fPIC", "-O2", "-fno-lto", "-fno-strict-aliasing", "-Wno-everything",
             "-ffp-contract=off"]   # lock-step online play: no fused multiply-add (clang for ARM fuses a * b + c by default), same rounding as the PC build
    # Build profile: "full" = -O2 everywhere; "fast" (the phone's default, chosen in the setup app) = -O2 only for the batch files that hold
    # code that runs a lot (tools/hot_funcs.txt: measured during a battle and the menus), the cold batches with -O1: a shorter install
    # for a slightly slower game. The flags are part of the object's key, so switching the profile compiles only the files that differ.
    profile, cold_opt = build_profile(work)
    hot_set = hot_functions() if profile == "fast" else None
    opt_of = {}   # source -> "-O2" / cold_opt
    hot_n = [0, 0]

    def flags_for(src):
        if src not in opt_of:
            hot = hot_set is None or not src_is_batch(src) or batch_is_hot(src, work, hot_set)
            opt_of[src] = "-O2" if hot else cold_opt
            if src_is_batch(src):
                hot_n[0 if hot else 1] += 1
        out = []
        for t in flags:
            out += cold_opt.split() if (t == "-O2" and opt_of[src] != "-O2") else [t]
        return out

    # Objects are kept by what they were compiled from (objcache.py: the source's bytes, the generated headers it includes, the compiler
    # and the flags without folder paths), so an update compiles only the files whose code changed. Before, the object names carried a
    # hash of the flags and of zig's path, which are inside the release folder: every update compiled all files again.
    # What is not generated code (the runtime's main.cpp), and everything with PSPRECOMP_NO_OBJCACHE=1, keeps the old rule: an object
    # named after the source's path and the flags, compiled again when the source is newer.
    import objcache
    import threading
    import time as _time
    try:
        zig_ver = subprocess.run([zig, "version"], env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, timeout=60).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        zig_ver = ""
    cid = "zig %s size %d; ndk %s; api %s" % (zig_ver or "?", os.path.getsize(zig), v.get("ndk", {}).get("version", "?"), api)
    use_cache = not os.environ.get("PSPRECOMP_NO_OBJCACHE")
    old_stamp = {}
    info = {}   # source -> (object file, the parts of its key or None when it is not cached)

    def obj_of(src):
        if src not in info:
            parts = objcache.key_parts(work, src, flags_for(src), cid) if use_cache else None
            info[src] = (objcache.entry(objdir, objcache.key_of(parts)) if parts else
                         os.path.join(objdir, hashlib.sha1(src.encode()).hexdigest()[:10] + "_" + os.path.basename(src) + "." + hashlib.sha256((" ".join(flags_for(src)) + zig).encode()).hexdigest()[:12] + ".o"), parts)
        return info[src][0]

    def up_to_date(src):
        o = obj_of(src)
        return os.path.isfile(o) and (info[src][1] is not None or os.path.getmtime(o) >= os.path.getmtime(src))

    todo = [(s, inc) for s, inc in srcs if not up_to_date(s)]
    say("Build: %s (%s)" % ("Fast: -O2 for %d batch files that run a lot, '%s' for the other %d" % (hot_n[0], cold_opt, hot_n[1]) if profile == "fast" else "Full: -O2 for every file",
                            "chosen in the setup app"))
    reasons = {}   # why the cached files of this build are compiled (the part of the key that changed)
    for s, _ in todo:
        if info[s][1]:
            r = objcache.explain(objcache.recall(objdir, work, s), info[s][1])
            reasons[r] = reasons.get(r, 0) + 1
    say("Compiling %d of %d files with zig; %d are unchanged since the last build%s" % (
        len(todo), len(srcs), len(srcs) - len(todo), " (why: %s)" % objcache.reasons_text(reasons) if reasons else ""))
    # Space: measured 26 Nov, one object is ~0.85 MB and zig keeps a second copy while compiling (its cache, emptied after the build);
    # the link and the APK need ~200 MB more. Checked BEFORE the first compile, so a full phone says so in words (not "CacheCheckFailed").
    need_mb = int(len(todo) * 2 + 200)
    if todo and not os.environ.get("PSPRECOMP_SKIP_SPACE_CHECK"):
        env["ZIG_GLOBAL_CACHE_DIR"] = pick_zig_cache(base, need_mb, say)
        shutil.rmtree(env["ZIG_GLOBAL_CACHE_DIR"], ignore_errors=True)
        same_drive = os.stat(base).st_dev == os.stat(os.path.dirname(env["ZIG_GLOBAL_CACHE_DIR"])).st_dev
        base_need = int(len(todo) * (2 if same_drive else 1) + 200)   # the objects and the link here, plus the cache when it is on this drive
        free_work = free_mb(base)
        say("Space: about %d MB needed here for the compile, %s MB free%s" % (base_need, free_work, "" if same_drive else " (zig's cache is on another drive)"))
        if free_work is not None and free_work < base_need:
            die("not enough free disk space in %s: the compile needs about %d MB (objects and the compiler's cache) and %d MB are free" % (base, base_need, free_work))
    done = [0]
    errors = []
    # How many files compile at once. Before: at most 3 (psp_build.py), whatever the phone. Now every core may be used, but a job is
    # only started while the memory that is FREE right now covers it (objcache.may_start): a phone is killed, not slowed, when it
    # runs out.
    #  - the memory a job needs is measured (the peak of each finished compile, wait4); until the first one finishes it is taken as
    #    1.5 GB (the old rule's figure). The largest files are compiled first, so the worst case is known early;
    #  - a new job needs: free memory >= estimate x (1 + jobs started in the last 20 s, which have not grown yet) + 800 MB kept free;
    #  - one job always runs. `--jobs N` (jobs_fixed) or PSPRECOMP_JOBS_FIXED=1 keeps a plain fixed count.
    cpus = os.cpu_count() or 2
    fixed = jobs_fixed or bool(os.environ.get("PSPRECOMP_JOBS_FIXED")) or objcache.mem_free_kb() is None
    max_jobs = max(1, jobs if fixed else cpus)
    cond = threading.Condition()
    st = {"running": 0, "starts": [], "peak_kb": 0, "most": 0}

    def may_start():
        if st["running"] == 0:
            return True
        if st["running"] >= max_jobs:
            return False
        if fixed:
            return True
        now = _time.time()
        return objcache.may_start(st["running"], sum(1 for t in st["starts"] if now - t < objcache.YOUNG_S),
                                  objcache.estimate_kb(st["peak_kb"]), objcache.mem_free_kb())

    def compile_one(item):
        src, inc = item
        if errors:   # one file failed: the rest is not started (a full disk fails every one of them)
            return
        o = obj_of(src)
        os.makedirs(os.path.dirname(o), exist_ok=True)
        part = o + ".part"
        with cond:
            while not may_start():
                cond.wait(1.0)
            st["running"] += 1
            st["most"] = max(st["most"], st["running"])
            at_once = st["running"]
            t0 = _time.time()
            st["starts"].append(t0)
        out, rc, peak = "", 1, 0
        try:
            pr = subprocess.Popen([zig, "c++"] + flags_for(src) + inc + ["-c", src, "-o", part], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            out = pr.stdout.read().decode("utf-8", "replace")
            try:
                _, status, ru = os.wait4(pr.pid, 0)
                rc = os.waitstatus_to_exitcode(status) if hasattr(os, "waitstatus_to_exitcode") else (status >> 8)
                peak = int(ru.ru_maxrss)   # KB on Linux / Android: the largest of zig and the clang it runs
                pr.returncode = rc
            except (AttributeError, ChildProcessError, OSError):
                rc = pr.wait()
        except OSError as e:
            out = str(e)
        finally:
            with cond:
                st["running"] -= 1
                st["starts"].remove(t0)
                st["peak_kb"] = max(st["peak_kb"], peak)
                cond.notify_all()
        if rc != 0:
            hint = ""
            if "CacheCheckFailed" in out or "NoSpaceLeft" in out or "No space left" in out:
                free = shutil.disk_usage(base).free // (1024 * 1024)
                hint = ("The compiler could not write its files (%d MB free in %s). " % (free, base) +
                        ("The storage is nearly full: free some space and run the install again. " if free < 1024 else
                         "The storage is not full, so the compiler could not use its own cache folder (zig-cache) there. ") +
                        "What is already compiled is kept.\n")
            errors.append("%s%s\n%s" % (hint, src, out[-1500:]))
            return
        os.replace(part, o)
        if info[src][1]:
            objcache.remember(objdir, work, src, info[src][1])
        done[0] += 1
        tag("PROGRESS", str(100 * done[0] // max(1, len(todo))))
        say("[%d/%d] %s (%.0f s%s, %d at once)" % (done[0], len(todo), os.path.basename(src), _time.time() - t0,
                                                 ", %d MB" % (peak // 1024) if peak else "", at_once))

    if todo:
        todo.sort(key=lambda it: -os.path.getsize(it[0]))   # the largest first: the memory a job needs is learned from the worst case
        t_all = _time.time()
        with concurrent.futures.ThreadPoolExecutor(max_workers=max_jobs) as ex:
            list(ex.map(compile_one, todo))
        say("Compiled %d files in %.0f s: up to %d at once (%d cores), largest job %d MB%s" % (
            done[0], _time.time() - t_all, st["most"], cpus, st["peak_kb"] // 1024, " (fixed job count)" if fixed else ""))
    if errors:
        die("compiling failed:\n" + errors[0])
    say("Linking libmain.so")
    tag("PROGRESS", "99")
    objs = [obj_of(s) for s, _ in srcs if s != main_cpp]
    cl = lib + "/%d" % api
    dl = deps + "/lib"
    # what the core needs besides SDL2 / FreeType / zlib: the Android part lists it (android/build_deps.py link.txt); an older part without the file is the plain set
    link_txt = os.path.join(adir, "deps", "link.txt")
    names = [l.strip() for l in open(link_txt)] if os.path.isfile(link_txt) else ["libavcodec.a", "libavutil.a", "-lm"]
    extra_libs = [n if n.startswith("-l") else dl + "/" + n for n in names if n]
    libmain = os.path.join(base, "build", "libmain.so")
    os.makedirs(os.path.dirname(libmain), exist_ok=True)
    cmd = [zig, "ld.lld", "-m", "aarch64linux", "-shared", "-z", "noexecstack", "-z", "relro", "-z", "now", "-z", "max-page-size=16384",
           "--hash-style=gnu", "--eh-frame-hdr", "--build-id=sha1", "--gc-sections", "--threads=1", "-s", "--no-rosegment", "--no-undefined",
           "--fatal-warnings", "-soname", "libmain.so", "-o", libmain, cl + "/crtbegin_so.o", obj_of(main_cpp),
           "--start-group", os.path.join(adir, "sdk", "lib", "libpsprecomp_core.a")] + objs + ["--end-group",
           dl + "/libSDL2.so", dl + "/libfreetype.a", cl + "/libz.so", "-L" + cl, "-L" + lib, "-lGLESv3", "-llog", "-landroid"] + extra_libs + [
           lib + "/libc++_static.a", lib + "/libc++abi.a",
           rt + "/libunwind.a", rt + "/libatomic.a", "-lm", "-ldl", "-lc", rt + "/libclang_rt.builtins-aarch64-android.a", cl + "/crtend_so.o"]
    r = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    if r.returncode != 0:
        die("linking failed:\n" + "\n".join(l[:300] for l in r.stdout.splitlines()[:12]))
    used = set(o for o, _ in info.values())   # the objects of this build stay; older ones (another version's code, the old naming) are removed
    removed = 0
    for dp, _, fns in os.walk(objdir):
        for fn in fns:
            fp = os.path.join(dp, fn)
            if fp not in used and fn.endswith((".o", ".part", ".d")):
                try:
                    os.remove(fp)
                    removed += 1
                except OSError:
                    pass
    if removed:
        say("Removed %d object files of an older build" % removed)
    if not os.environ.get("PSPRECOMP_KEEP_ZIG_CACHE"):
        shutil.rmtree(env["ZIG_GLOBAL_CACHE_DIR"], ignore_errors=True)
    return libmain
