#!/usr/bin/env python3
"""Put the game's files where the Android game app reads them (run ON the phone by the setup app).

  android_local_install.py <workdir> [--dest DIR] [--only NAME,NAME] [--clean]

The game app (org.psprecomp.game) looks in <storage>/Android/media/<package>/game when its own Android/data folder holds no game (no other app may
write to Android/data since Android 11; runtime/src/psp_android.cpp pick_workdir). The setup app has "all files access", so it can write there.
Saves in ms0/ that already exist at the destination are kept. Prints `[psp-build] PROGRESS n` lines. Python 3 standard library only.

One copy of the big files only (the phone has little room): disc0 is extracted straight into <dest>/disc0 (setup_from_iso.py, discdir.py), so
there is normally nothing of it to copy here; a disc0 left in the work folder (an install of an older version) and voice_jp / movie_jp are
MOVED (copied and deleted file by file, so a stop leaves at most one file twice; a resumed run continues). Texture packs are moved as before.
mods/ and ms0/ stay in the work folder (a rebuild reads the mods; the saves are kept). With --clean, once everything is in place, what the
build left and can be made again is deleted (see clean()); the compiled objects stay (a quick update needs them).
PSPRECOMP_DISC_HOME_OLD=1: the old copy of everything. PSPRECOMP_KEEP_BUILD=1: no clean-up.
"""
import argparse
import glob
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import discdir  # noqa: E402

DEFAULT_DEST = "/sdcard/Android/media/org.psprecomp.game/game"
PARTS = ["disc0", "ms0", "voice_jp", "movie_jp", "mods"]
MOVED = ("disc0", "voice_jp", "movie_jp")   # one copy only: the work folder's is deleted file by file
MB = 1 << 20


def clean(work, say=lambda m: print(m, flush=True)):
    """Deletes what the build left behind and a later build makes again (measured on a phone: output 128 MB, overlays 68 MB, analysis 40 MB,
    EBOOT copies 9 MB, android/libmain.so + build/ ~30 MB, zig's cache, tmp). Kept: android/obj (the compiled objects, ~400 MB: without them an
    update compiles every file again, 10 to 20 minutes), android/dissidia.apk (installed from the setup app afterwards), EBOOT.enc.bin (the
    ISO's own EBOOT: only the ISO has it), mods/, ms0/, the logs and settings. psp_build.py makes the rest again: the analysis from the
    shipped files + the disc, output/ and overlays/ by the recompile (a missing output re-runs it whatever its stamp says), EBOOT.dec.elf from
    the disc's SYSDIR/EBOOT.BIN (discdir.eboot_elf)."""
    freed = 0
    targets = [os.path.join(work, d) for d in ("output", "overlays", ".recompile.stamp", "eboot_image.bin", "EBOOT.dec.elf", ".zig-cache",
                                               os.path.join("android", "zig-cache"), os.path.join("android", "build"), os.path.join("android", "libmain.so"))]
    targets += glob.glob(os.path.join(work, "analysis*.json*")) + glob.glob(os.path.join(work, "OVL_*_APP.bin"))
    tmp = os.path.join(work, "tmp")
    targets += [os.path.join(tmp, n) for n in os.listdir(tmp)] if os.path.isdir(tmp) else []
    for t in targets:
        if not os.path.lexists(t):
            continue
        if os.path.isdir(t) and not os.path.islink(t):
            for dp, _, fns in os.walk(t):
                for fn in fns:
                    try:
                        freed += os.path.getsize(os.path.join(dp, fn))
                    except OSError:
                        pass
            shutil.rmtree(t, ignore_errors=True)
        else:
            try:
                freed += os.path.getsize(t)
                os.remove(t)
            except OSError:
                pass
    say("[psp-build] CLEAN freed %d MB of build leftovers (compiled objects kept for quick updates)" % (freed // MB))
    return freed


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("workdir")
    ap.add_argument("--dest", default=DEFAULT_DEST)
    ap.add_argument("--only")
    ap.add_argument("--clean", action="store_true", help="delete what the build left and can be made again, once the files are in place")
    a = ap.parse_args()
    work = os.path.abspath(a.workdir)
    old = os.environ.get("PSPRECOMP_DISC_HOME_OLD") == "1"
    parts = a.only.split(",") if a.only else PARTS
    files = []
    # Texture packs (mods/texpacks/<id>) are MOVED to the game's folder: the work folder keeps no second copy (they are large), and a pack
    # deleted in the game's settings does not come back with the next extras change. A pack of the same id there is replaced.
    packs_src = os.path.join(work, "mods", "texpacks")
    packs = [d for d in (os.listdir(packs_src) if os.path.isdir(packs_src) else []) if not d.startswith(".")]
    for name in parts:
        src = os.path.join(work, name)
        for dp, dns, fns in os.walk(src):
            if os.path.abspath(dp) == os.path.abspath(packs_src):
                dns[:] = [d for d in dns if not d.startswith(".")]   # an unfinished install (.new-*, .unpack-*)
            for fn in fns:
                files.append((name, os.path.join(dp, fn)))
    # the game's disc files: here already (extracted there), or moved now from an older install's work/disc0
    disc_there = discdir.has_game(os.path.join(a.dest, "disc0"))
    if not any(n == "disc0" for n, _ in files) and not disc_there:
        print("[psp-build] ERROR the game's disc files are in neither %s nor %s: nothing to install (the game file is needed again: pick your .iso / .cso)" % (
            os.path.join(work, "disc0"), os.path.join(a.dest, "disc0")), flush=True)
        sys.exit(1)
    total = sum(os.path.getsize(p) for _, p in files) or 1
    done = 0
    last = -1
    try:
        os.makedirs(a.dest, exist_ok=True)
        # Keep the game's files (thousands of texture PNGs) out of the gallery: Android's media scanner indexes Android/media, and a
        # .nomedia file in the package folder hides it and everything below (the setup app also rescans it).
        open(os.path.join(os.path.dirname(os.path.abspath(a.dest)), ".nomedia"), "a").close()
        discdir.purge_part(a.dest)   # a copy that was cut off
        for pid in packs:
            shutil.rmtree(os.path.join(a.dest, "mods", "texpacks", pid), ignore_errors=True)
        for name, p in files:
            rel = os.path.relpath(p, work)
            d = os.path.join(a.dest, rel)
            sz = os.path.getsize(p)
            move = name in MOVED and not old
            keep = (name == "ms0" and os.path.exists(d)) or (os.path.exists(d) and os.path.getsize(d) == sz and (move or os.path.getmtime(d) >= os.path.getmtime(p)))
            if keep:
                if move:
                    os.remove(p)   # the game's folder has it: the work folder's copy goes
            elif move:
                discdir.move_file(p, d)
            else:
                os.makedirs(os.path.dirname(d), exist_ok=True)
                shutil.copyfile(p, d)
            done += sz
            pct = 100 * done // total
            if pct != last:
                last = pct
                print("[psp-build] PROGRESS %d" % pct, flush=True)
        for name in MOVED:   # the emptied folders
            if not old:
                for dp, _, _ in os.walk(os.path.join(work, name), topdown=False):
                    try:
                        os.rmdir(dp)
                    except OSError:
                        pass
    except OSError as e:
        print("[psp-build] ERROR cannot write %s: %s (is \"all files access\" allowed for this app, and is there free space?)" % (a.dest, e), flush=True)
        sys.exit(1)
    for pid in packs:   # in place now: drop the work folder's copy
        try:
            info = json.load(open(os.path.join(packs_src, pid, ".source.json")))
        except (OSError, ValueError):
            info = {}
        shutil.rmtree(os.path.join(packs_src, pid), ignore_errors=True)
        if info.get("migrated_from") == "textures":   # the old single pack, moved into texpacks: the game's old copy is the same pack
            shutil.rmtree(os.path.join(a.dest, "mods", "textures"), ignore_errors=True)
    if a.clean and os.environ.get("PSPRECOMP_KEEP_BUILD") != "1" and not old:
        if discdir.has_game(os.path.join(a.dest, "disc0")):   # the disc files are what the build is made again from
            clean(work)
        else:
            print("[psp-build] not cleaning: the game's disc files are not in %s" % a.dest, flush=True)
    print("[psp-build] DONE %s" % a.dest, flush=True)


if __name__ == "__main__":
    main()
