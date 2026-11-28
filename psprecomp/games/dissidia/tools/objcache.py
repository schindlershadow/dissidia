#!/usr/bin/env python3
"""Object cache for the generated game code: an update recompiles only the files whose code changed.

Every update runs the whole "Translating / compiling the game" step again, and before this cache every file was compiled again:
the Android build keyed its objects by the release folder (zig, NDK and SDK paths are in the flags), and on a PC the recompile
step rewrites all generated sources and the build folder is reconfigured from the new release. The mod code ships precompiled
(the core library), so most updates produce generated C++ that is byte for byte the same as before.

THE KEY of a generated source (work/output/... or work/overlays/<name>/...) is a hash of its parts (key_parts):
  - "source":   the source's bytes;
  - "headers":  the generated headers it includes (#include "..." lines, followed into those headers): recomp.h and its module's
                funcs.h. output/include/recomp_fingerprint.h holds the time of the translation and changes on every run, but only
                the runtime's main.cpp includes it;
  - "compiler": the compiler's identity: its --version text without the lines that name folders, its size, and an extra string
                (Android: the NDK version and API level; the phone: zig's version);
  - "flags":    the compiler flags without folder paths (-I / -isystem / -o / -MF ... and their values are left out, an absolute
                path in any other flag is reduced to its last component, a response file @file is read and treated the same).
So a new release folder or a new work folder does not change a key. Standard headers come with the compiler. Sources outside the
generated folders (the runtime's main.cpp) are not cached.

WHY A FILE WAS COMPILED: the parts of the last compile of every source are remembered (<cache>/why/), so a miss can say which
part changed (explain): "its code changed", "the header funcs.h changed", "the compiler changed", "the compiler flags changed".
The builds print the reasons; an update that compiles everything says why.

Objects live in <cache>/<2 hex>/<key>.o. The launcher notes which entry each object of the build folder uses (one small file per
object in <build>/objcache.manifest/); prune_build() after a build removes all other entries, so the cache holds one copy of the
game's objects (no growth across updates), also after a build that had nothing to do.

Two uses:
  - a library: android_build.device_build (the phone's own build loop; its object folder is the cache);
  - a compiler launcher for CMake (CMAKE_CXX_COMPILER_LAUNCHER="<python>;<this file>"), set up by psp_build.py with
    PSPRECOMP_OBJCACHE=<cache dir>, PSPRECOMP_OBJCACHE_WORK=<work dir>, PSPRECOMP_OBJCACHE_MANIFEST=<dir>,
    PSPRECOMP_OBJCACHE_BUILD=<id of this run>. Without them (or with PSPRECOMP_NO_OBJCACHE=1) it only runs the compiler.
    PSPRECOMP_OBJCACHE_LOG=1 prints one line per hit / miss. An error in the cache itself never fails a compile: the compiler runs.

The launcher is also the MEMORY GATE of the PC build (PSPRECOMP_OBJCACHE_GATE=1, set by psp_build.py when it chose the job count
itself): the build tool starts one compile per core, and each compile waits here until the memory that is free right now covers
it (may_start). What a compile needs is measured (the largest peak of this build, <cache>/jobs/peak_kb; FIRST_EST_KB until one is
known): Linux / Android / macOS from wait4, Windows from a job object.

Self-test: test_objcache.py (no game data, no compiler).
"""
import hashlib
import json
import os
import random
import re
import shutil
import subprocess
import sys
import time

VERSION = "objcache 1"   # part of every key: raise it when the meaning of a key changes
FIRST_EST_KB = 1536 * 1024   # what a compile is taken to need until one has been measured
RESERVE_KB = 800 * 1024      # memory the gate keeps free
YOUNG_S = 20.0               # a compile this young has not reached its memory yet: counted again in full

_digests = {}


# ------------------------------------------------------------------------------------------------------------------ the key
def file_digest(path):
    st = os.stat(path)
    k = (path, st.st_size, st.st_mtime_ns)
    d = _digests.get(k)
    if d is None:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        d = _digests[k] = h.hexdigest()
    return d


def _rel(work, path):
    return os.path.relpath(path, work).replace("\\", "/")


def module_root(work, src):
    """The generated module folder of a source (work/output or work/overlays/<name>), or None (not generated code)."""
    try:
        rel = os.path.relpath(os.path.abspath(src), os.path.abspath(work)).replace("\\", "/").split("/")
    except ValueError:   # another drive (Windows)
        return None
    if rel[0] == "output" and len(rel) > 1:
        return os.path.join(work, "output")
    if rel[0] == "overlays" and len(rel) > 2:
        return os.path.join(work, "overlays", rel[1])
    return None


_INC = re.compile(rb'^[ \t]*#[ \t]*include[ \t]*"([^"]+)"', re.M)


def local_includes(src, dirs, seen=None):
    """The headers a source includes with #include "..." that exist in its own folder or `dirs`, followed into those headers."""
    seen = set() if seen is None else seen
    try:
        with open(src, "rb") as f:
            data = f.read()
    except OSError:
        return seen
    for m in _INC.finditer(data):
        name = m.group(1).decode("utf-8", "replace")
        for d in [os.path.dirname(src)] + list(dirs):
            p = os.path.normpath(os.path.join(d, name))
            if os.path.isfile(p):
                if p not in seen:
                    seen.add(p)
                    local_includes(p, dirs, seen)
                break
    return seen


_PATH_ARGS = {"-I", "-isystem", "-iquote", "-idirafter", "-o", "-MF", "-MT", "-MQ", "-include", "--sysroot", "-isysroot", "-c"}
_PATH_PREFIXES = ("-I", "-isystem", "-iquote", "-idirafter", "-MF", "-MT", "-MQ", "--sysroot=", "-isysroot")


def normalize_flags(flags, _depth=0):
    """The flags that decide what the compiler produces, without folder paths."""
    out, skip = [], False
    for t in flags:
        if skip:
            skip = False
            continue
        if t in _PATH_ARGS:
            skip = True
            continue
        if t.startswith("@"):   # a response file (CMake on Windows puts the include folders into one): its flags count like the others
            inner = None
            if _depth < 2:
                try:
                    import shlex
                    with open(t[1:], errors="replace") as f:
                        inner = shlex.split(f.read())
                except (OSError, ValueError):
                    inner = None
            out += normalize_flags(inner, _depth + 1) if inner is not None else ["@" + os.path.basename(t)]
            continue
        if t in ("-MD", "-MMD") or t.startswith(_PATH_PREFIXES) or t.endswith((".cpp", ".cc", ".c", ".o", ".obj")):
            continue
        if "=" in t:
            a, b = t.split("=", 1)
            if os.path.isabs(b.strip("\"'")):
                t = a + "=" + os.path.basename(b.strip("\"'").rstrip("/\\"))
        elif os.path.isabs(t):
            t = os.path.basename(t.rstrip("/\\"))
        out.append(t)
    return out


def version_text(text):
    """A compiler's --version text without the lines that name folders: clang prints "InstalledDir: <folder>" (and "Configuration
    file: ..."), and that folder is inside the release, so it changes with every update."""
    keep = []
    for line in text.splitlines():
        line = line.strip()
        if not line or re.match(r"^[A-Za-z][A-Za-z ]*:\s*([A-Za-z]:[\\/]|/)", line):
            continue
        keep.append(line)
    return "\n".join(keep)


def compiler_id(compiler, cache_dir, extra=""):
    """Identity of a compiler command (a list): its --version text and size. Remembered in the cache folder (compilers.json) per
    file (path, size, time). None when the compiler does not answer: nothing is cached then."""
    exe = shutil.which(compiler[0]) or compiler[0]
    try:
        st = os.stat(exe)
    except OSError:
        return None
    memo_key = "%s|%s|%d|%d" % (os.path.realpath(exe), " ".join(compiler[1:]), st.st_size, st.st_mtime_ns)
    memo = os.path.join(cache_dir, "compilers.json")
    try:
        with open(memo) as f:
            known = json.load(f)
    except (OSError, ValueError):
        known = {}
    if memo_key not in known:
        try:
            r = subprocess.run(list(compiler) + ["--version"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace", timeout=120)
        except (OSError, subprocess.SubprocessError):
            return None
        text = version_text(r.stdout)
        if r.returncode != 0 or not text:
            return None
        known = {k: v for k, v in known.items() if os.path.exists(k.split("|", 1)[0])}   # compilers of releases that are gone
        known[memo_key] = "%s\nsize %d" % (text, st.st_size)
        try:
            os.makedirs(cache_dir, exist_ok=True)
            _write_atomic(memo, json.dumps(known, indent=1))
        except OSError:
            pass
    return known[memo_key] + "\n" + extra


def key_parts(work, src, flags, cid):
    """What an object of a generated source is made from, as short digests (see the top of the file), or None (not generated code)."""
    root = module_root(work, src)
    if root is None or not cid:
        return None
    out = os.path.join(work, "output")
    incs = local_includes(src, [root, os.path.join(root, "include"), os.path.join(out, "include"), out])
    return {"v": VERSION, "module": _rel(work, root),
            "compiler": hashlib.sha256(cid.encode()).hexdigest()[:16],
            "flags": hashlib.sha256("\0".join(normalize_flags(flags)).encode()).hexdigest()[:16],
            "source": file_digest(src),
            "headers": {_rel(work, p): file_digest(p) for p in sorted(incs)}}


def key_of(parts):
    return hashlib.sha256(json.dumps(parts, sort_keys=True).encode()).hexdigest()


def key(work, src, flags, cid):
    parts = key_parts(work, src, flags, cid)
    return key_of(parts) if parts else None


def explain(old, new):
    """Why a source is compiled again: the part of its key that differs from its last compile."""
    if not old:
        return "first build"
    if old.get("v") != new.get("v"):
        return "the cache format changed"
    if old.get("compiler") != new.get("compiler"):
        return "the compiler changed"
    if old.get("flags") != new.get("flags"):
        return "the compiler flags changed"
    oh, nh = old.get("headers") or {}, new.get("headers") or {}
    for name in sorted(set(oh) | set(nh)):
        if oh.get(name) != nh.get(name):
            return "the header %s changed" % os.path.basename(name)
    if old.get("source") != new.get("source"):
        return "its code changed"
    return "its object was not kept"


# ---------------------------------------------------------------------------------------------------------------- the store
def _write_atomic(path, text):
    tmp = "%s.%d.tmp" % (path, os.getpid())
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, path)


def entry(cache_dir, k, ext=".o"):
    return os.path.join(cache_dir, k[:2], k + ext)


def lookup(cache_dir, k):
    p = entry(cache_dir, k)
    return p if os.path.isfile(p) else None


def store(cache_dir, k, obj, dep=None):
    p = entry(cache_dir, k)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    tmp = "%s.%d.tmp" % (p, os.getpid())
    shutil.copyfile(obj, tmp)
    os.replace(tmp, p)
    if dep and os.path.isfile(dep):
        shutil.copyfile(dep, entry(cache_dir, k, ".d"))
    return p


def _why_file(cache_dir, work, src):
    return os.path.join(cache_dir, "why", hashlib.sha1(_rel(work, os.path.abspath(src)).encode("utf-8", "replace")).hexdigest()[:20] + ".json")


def recall(cache_dir, work, src):
    """The parts of the last compile of this source, or None."""
    try:
        with open(_why_file(cache_dir, work, src)) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def remember(cache_dir, work, src, parts):
    try:
        f = _why_file(cache_dir, work, src)
        os.makedirs(os.path.dirname(f), exist_ok=True)
        _write_atomic(f, json.dumps(parts, sort_keys=True))
    except OSError:
        pass


def note_object(obj, k, status, reason=""):
    """Launcher: this object of the build folder uses cache entry k (one small file per object: no shared file to append to)."""
    d = os.environ.get("PSPRECOMP_OBJCACHE_MANIFEST")
    if not d:
        return
    try:
        os.makedirs(d, exist_ok=True)
        a = os.path.abspath(obj)
        _write_atomic(os.path.join(d, hashlib.sha1(a.encode("utf-8", "replace")).hexdigest()[:20]),
                      json.dumps({"obj": a, "key": k, "status": status, "reason": reason, "build": os.environ.get("PSPRECOMP_OBJCACHE_BUILD", "")}))
    except OSError:
        pass


def read_manifest(d):
    """[{obj, key, status, reason, build}] of a build folder's manifest."""
    out = []
    try:
        names = os.listdir(d)
    except OSError:
        return out
    for n in names:
        if n.endswith(".tmp"):
            continue
        try:
            with open(os.path.join(d, n)) as f:
                e = json.load(f)
            if e.get("obj") and len(e.get("key", "")) == 64:
                e["file"] = os.path.join(d, n)
                out.append(e)
        except (OSError, ValueError):
            pass
    return out


def prune(cache_dir, keep_keys):
    """Removes every object entry whose key is not in keep_keys. Returns (kept, removed). Nothing is removed when keep_keys is empty."""
    kept = removed = 0
    keep_keys = set(keep_keys)
    if not keep_keys or not os.path.isdir(cache_dir):
        return kept, removed
    for d in os.listdir(cache_dir):
        sub = os.path.join(cache_dir, d)
        if len(d) != 2 or not os.path.isdir(sub):
            continue
        for n in os.listdir(sub):
            k, ext = os.path.splitext(n)
            if ext not in (".o", ".d", ".tmp"):
                continue
            if ext != ".tmp" and k in keep_keys:
                kept += ext == ".o"
                continue
            try:
                os.remove(os.path.join(sub, n))
                removed += ext == ".o"
            except OSError:
                pass
    return kept, removed


def prune_build(cache_dir, manifest, min_objects=0, build=None):
    """After a build: keeps the entries the build folder's objects use. Nothing is removed when the manifest lists fewer than
    min_objects objects: it is then not the whole build, and what it does not list may still be in use.
    Returns {"kept", "removed", "hit", "miss", "reasons": {reason: files}} (hit / miss / reasons: of the run `build`)."""
    res = {"kept": 0, "removed": 0, "hit": 0, "miss": 0, "reasons": {}}
    keep = set()
    for e in read_manifest(manifest):
        if not os.path.isfile(e["obj"]):
            try:
                os.remove(e["file"])
            except OSError:
                pass
            continue
        keep.add(e["key"])
        if build is not None and e.get("build") == build:
            if e.get("status") == "hit":
                res["hit"] += 1
            else:
                res["miss"] += 1
                r = e.get("reason") or "?"
                res["reasons"][r] = res["reasons"].get(r, 0) + 1
    if len(keep) and len(keep) >= min_objects:
        res["kept"], res["removed"] = prune(cache_dir, keep)
    else:
        res["kept"] = len(keep)
    return res


def reasons_text(reasons, limit=3):
    """{"the compiler changed": 468} -> "the compiler changed"; several -> "its code changed (3), first build (1)"."""
    items = sorted(reasons.items(), key=lambda kv: -kv[1])
    if len(items) == 1:
        return items[0][0]
    return ", ".join("%s (%d)" % kv for kv in items[:limit]) + (", ..." if len(items) > limit else "")


# ------------------------------------------------------------------------------------------------------------- memory gate
def mem_free_kb():
    """Physical memory free for use right now, in KB (Linux / Android: MemAvailable; Windows: GlobalMemoryStatusEx), or None."""
    try:
        if os.path.isfile("/proc/meminfo"):
            with open("/proc/meminfo") as f:
                m = re.search(r"MemAvailable:\s+(\d+)", f.read())
            return int(m.group(1)) if m else None
        if os.name == "nt":
            import ctypes

            class MS(ctypes.Structure):
                _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong)] + [(n, ctypes.c_ulonglong) for n in (
                    "ullTotalPhys", "ullAvailPhys", "ullTotalPageFile", "ullAvailPageFile", "ullTotalVirtual", "ullAvailVirtual", "ullAvailExtendedVirtual")]
            ms = MS()
            ms.dwLength = ctypes.sizeof(MS)
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(ms)):
                return int(ms.ullAvailPhys // 1024)
    except Exception:
        pass
    return None


def may_start(running, young, est_kb, avail_kb):
    """The rule of the gate (also android_build.device_build): one compile always runs; another needs the free memory to cover
    it and the compiles that are too young to have reached their memory."""
    if running == 0 or avail_kb is None:
        return True
    return avail_kb >= est_kb * (1 + young) + RESERVE_KB


def estimate_kb(peak_kb):
    """What the next compile is taken to need, from the largest peak measured so far (0 = none yet)."""
    return max(200 * 1024, int(peak_kb * 1.25)) if peak_kb else FIRST_EST_KB


def _pid_alive(pid):
    if os.name == "nt":
        try:
            import ctypes
            k32 = ctypes.windll.kernel32
            h = k32.OpenProcess(0x1000, False, int(pid))   # PROCESS_QUERY_LIMITED_INFORMATION
            if not h:
                return False
            code = ctypes.c_ulong(0)
            ok = k32.GetExitCodeProcess(h, ctypes.byref(code))
            k32.CloseHandle(h)
            return bool(ok) and code.value == 259   # STILL_ACTIVE
        except Exception:
            return True
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except OSError:
        pass
    return True


class Gate:
    """Cross-process: every running compile has a file <dir>/<pid>-<start time>; a short directory lock guards check + register."""

    def __init__(self, cache_dir):
        self.dir = os.path.join(cache_dir, "jobs")
        self.mine = None
        os.makedirs(self.dir, exist_ok=True)

    def _lock(self):
        lk = os.path.join(self.dir, "lock")
        for _ in range(400):
            try:
                os.mkdir(lk)
                return lk
            except OSError:   # held by another launcher (Windows also answers "access denied" while the folder is being removed)
                try:
                    if time.time() - os.path.getmtime(lk) > 10.0:   # left by a launcher that was killed
                        os.rmdir(lk)
                        continue
                except OSError:
                    pass
                time.sleep(0.02 + random.random() * 0.03)
        return None

    def _running(self):
        now, out = time.time(), []
        for n in os.listdir(self.dir):
            m = re.match(r"^(\d+)-(\d+(?:\.\d+)?)$", n)
            if not m:
                continue
            pid, t0 = int(m.group(1)), float(m.group(2))
            if now - t0 < 3600.0 and _pid_alive(pid):
                out.append(t0)
            else:
                try:
                    os.remove(os.path.join(self.dir, n))
                except OSError:
                    pass
        return out

    def peak_kb(self):
        try:
            with open(os.path.join(self.dir, "peak_kb")) as f:
                return int(f.read().strip())
        except (OSError, ValueError):
            return 0

    def enter(self):
        """Waits until this compile may start, then registers it. Returns the seconds waited."""
        waited = 0.0
        while True:
            lk = self._lock()   # without the lock (never free for 10 s) the rule is still applied
            try:
                starts = self._running()
                now = time.time()
                if waited > 1800.0 or may_start(len(starts), sum(1 for t in starts if now - t < YOUNG_S), estimate_kb(self.peak_kb()), mem_free_kb()):
                    self.mine = os.path.join(self.dir, "%d-%.3f" % (os.getpid(), now))
                    open(self.mine, "w").close()
                    return waited
            finally:
                if lk:
                    try:
                        os.rmdir(lk)
                    except OSError:
                        pass
            d = 0.4 + random.random() * 0.4
            time.sleep(d)
            waited += d

    def leave(self, peak_kb=0):
        if self.mine:
            try:
                os.remove(self.mine)
            except OSError:
                pass
            self.mine = None
        if peak_kb and peak_kb > self.peak_kb():
            try:
                _write_atomic(os.path.join(self.dir, "peak_kb"), str(int(peak_kb)))
            except OSError:
                pass


def gate_reset(cache_dir):
    """Start of a build: no compile is running and nothing is measured yet (the estimate is this build's own)."""
    shutil.rmtree(os.path.join(cache_dir, "jobs"), ignore_errors=True)


def _nt_job():
    """Windows: puts this process into a new job object, which the compiler it starts joins; its peak memory is the compile's
    (the wrapper, zig and clang together). Returns (kernel32, job handle) or None."""
    try:
        import ctypes
        from ctypes import wintypes
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.CreateJobObjectW.restype = wintypes.HANDLE
        k32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        k32.GetCurrentProcess.restype = wintypes.HANDLE
        k32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        k32.QueryInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p]
        job = k32.CreateJobObjectW(None, None)
        if not job:
            return None
        if not k32.AssignProcessToJobObject(job, k32.GetCurrentProcess()):
            return None
        return k32, job
    except Exception:
        return None


def _nt_job_peak_kb(k32, job):
    try:
        import ctypes
        from ctypes import wintypes

        class BASIC(ctypes.Structure):
            _fields_ = [("PerProcessUserTimeLimit", ctypes.c_longlong), ("PerJobUserTimeLimit", ctypes.c_longlong), ("LimitFlags", wintypes.DWORD),
                        ("MinimumWorkingSetSize", ctypes.c_size_t), ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                        ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD), ("SchedulingClass", wintypes.DWORD)]

        class IO(ctypes.Structure):
            _fields_ = [(n, ctypes.c_ulonglong) for n in ("ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                                                          "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]

        class EXT(ctypes.Structure):
            _fields_ = [("BasicLimitInformation", BASIC), ("IoInfo", IO), ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                        ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]
        info = EXT()
        if k32.QueryInformationJobObject(job, 9, ctypes.byref(info), ctypes.sizeof(info), None):   # JobObjectExtendedLimitInformation
            return int(info.PeakJobMemoryUsed // 1024)
    except Exception:
        pass
    return 0


def run_measured(argv):
    """Runs a command; returns (exit code, peak memory in KB, 0 when it is not known)."""
    job = _nt_job() if os.name == "nt" else None
    try:
        pr = subprocess.Popen(argv)
    except OSError as e:
        sys.stderr.write("%s: %s\n" % (argv[0], e))
        return 127, 0
    if hasattr(os, "wait4"):
        try:
            _, status, ru = os.wait4(pr.pid, 0)
            rc = os.waitstatus_to_exitcode(status) if hasattr(os, "waitstatus_to_exitcode") else (status >> 8)
            pr.returncode = rc
            peak = int(ru.ru_maxrss)
            return rc, (peak // 1024 if sys.platform == "darwin" else peak)   # macOS reports bytes
        except (ChildProcessError, OSError):
            pass
    rc = pr.wait()
    return rc, (_nt_job_peak_kb(*job) if job else 0)


# ---------------------------------------------------------------------------------------------------- compiler launcher (CMake)
def _arg_after(argv, name):
    for i, t in enumerate(argv[:-1]):
        if t == name:
            return argv[i + 1]
    return None


def _dep_escape(path):
    return path.replace(" ", "\\ ").replace("#", "\\#").replace("$", "$$")


def _serve_hit(cache_dir, k, argv, src, obj, dep):
    hit = lookup(cache_dir, k)
    if not hit:
        return False
    os.makedirs(os.path.dirname(os.path.abspath(obj)), exist_ok=True)
    shutil.copyfile(hit, obj)   # a fresh time stamp: newer than the source for ninja / make
    if dep:
        d = entry(cache_dir, k, ".d")
        text = ""
        if os.path.isfile(d):
            with open(d, errors="replace") as f:
                text = f.read()
        colon = text.find(": ")
        body = text[colon + 2:] if colon >= 0 else (_dep_escape(src) + "\n")
        with open(dep, "w") as f:
            f.write(_dep_escape(_arg_after(argv, "-MT") or obj) + ": " + body)
    return True


def launch(argv):
    """argv = the compiler command CMake runs (compiler first). Returns the exit code."""
    env = os.environ
    cache_dir, work = env.get("PSPRECOMP_OBJCACHE"), env.get("PSPRECOMP_OBJCACHE_WORK")
    src, obj, dep = _arg_after(argv, "-c"), _arg_after(argv, "-o"), _arg_after(argv, "-MF")
    log = env.get("PSPRECOMP_OBJCACHE_LOG")
    k = parts = None
    try:
        if cache_dir and work and src and obj and not env.get("PSPRECOMP_NO_OBJCACHE"):
            parts = key_parts(work, src, argv[1:], compiler_id([argv[0]], cache_dir, env.get("PSPRECOMP_OBJCACHE_ID", "")))
            k = key_of(parts) if parts else None
        if k and _serve_hit(cache_dir, k, argv, src, obj, dep):
            note_object(obj, k, "hit")
            if log:
                sys.stderr.write("[OBJCACHE] hit %s\n" % os.path.basename(src))
            return 0
    except Exception as e:   # the cache must never fail a build: compile as if it were not there
        sys.stderr.write("[OBJCACHE] not used for %s: %s\n" % (os.path.basename(src or "?"), e))
        k = None
    gate = None
    if cache_dir and src and env.get("PSPRECOMP_OBJCACHE_GATE"):
        try:
            gate = Gate(cache_dir)
            gate.enter()
        except Exception:
            gate = None
    peak = 0
    try:
        r, peak = run_measured(argv)
    finally:
        if gate:
            try:
                gate.leave(peak)
            except Exception:
                pass
    if r == 0 and k:
        try:
            if os.path.isfile(obj):
                store(cache_dir, k, obj, dep)
                reason = explain(recall(cache_dir, work, src), parts)
                remember(cache_dir, work, src, parts)
                note_object(obj, k, "miss", reason)
                if log:
                    sys.stderr.write("[OBJCACHE] miss %s: %s\n" % (os.path.basename(src), reason))
        except Exception as e:
            sys.stderr.write("[OBJCACHE] could not store %s: %s\n" % (os.path.basename(src), e))
    return r


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        sys.exit(2)
    sys.exit(launch(sys.argv[1:]))
