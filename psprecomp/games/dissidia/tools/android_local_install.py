#!/usr/bin/env python3
"""Copy the game's files from the work folder to where the Android game app reads them (run ON the phone by the setup app).

  android_local_install.py <workdir> [--dest DIR] [--only NAME,NAME]

The game app (org.psprecomp.game) looks in <storage>/Android/media/<package>/game when its own Android/data folder holds no game (no other app may
write to Android/data since Android 11; runtime/src/psp_android.cpp pick_workdir). The setup app has "all files access", so it can write there.
Saves in ms0/ that already exist at the destination are kept. Prints `[psp-build] PROGRESS n` lines. Python 3 standard library only.
"""
import argparse
import os
import shutil
import sys

DEFAULT_DEST = "/sdcard/Android/media/org.psprecomp.game/game"
PARTS = ["disc0", "ms0", "voice_jp", "movie_jp", "mods"]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("workdir")
    ap.add_argument("--dest", default=DEFAULT_DEST)
    ap.add_argument("--only")
    a = ap.parse_args()
    work = os.path.abspath(a.workdir)
    parts = a.only.split(",") if a.only else PARTS
    files = []
    for name in parts:
        src = os.path.join(work, name)
        for dp, _, fns in os.walk(src):
            for fn in fns:
                files.append((name, os.path.join(dp, fn)))
    if not any(n == "disc0" for n, _ in files):
        print("[psp-build] ERROR %s has no disc0: nothing to copy" % work, flush=True)
        sys.exit(1)
    total = sum(os.path.getsize(p) for _, p in files) or 1
    done = 0
    last = -1
    try:
        os.makedirs(a.dest, exist_ok=True)
        for name, p in files:
            rel = os.path.relpath(p, work)
            d = os.path.join(a.dest, rel)
            sz = os.path.getsize(p)
            keep = (name == "ms0" and os.path.exists(d)) or (os.path.exists(d) and os.path.getsize(d) == sz and os.path.getmtime(d) >= os.path.getmtime(p))
            if not keep:
                os.makedirs(os.path.dirname(d), exist_ok=True)
                shutil.copyfile(p, d)
            done += sz
            pct = 100 * done // total
            if pct != last:
                last = pct
                print("[psp-build] PROGRESS %d" % pct, flush=True)
    except OSError as e:
        print("[psp-build] ERROR cannot write %s: %s (is \"all files access\" allowed for this app?)" % (a.dest, e), flush=True)
        sys.exit(1)
    print("[psp-build] DONE %s" % a.dest, flush=True)


if __name__ == "__main__":
    main()
