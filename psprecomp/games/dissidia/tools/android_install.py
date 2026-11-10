#!/usr/bin/env python3
"""Install the Android build on a connected device with adb and copy the game's files to it.

  android_install.py <workdir> [--adb PATH] [--apk FILE] [--serial S] [--no-data] [--only NAME,NAME]

Needs adb (Android platform-tools, https://developer.android.com/tools/releases/platform-tools) in PATH, next to the release's android/
folder, or given with --adb, and a device with USB or wireless debugging on. Installs <workdir>/android/dissidia.apk, then copies the
work folder's disc0/ (the game), ms0/ (saves) and, when present, voice_jp/, movie_jp/ and mods/ into the app's private storage on the
device (<storage>/Android/data/org.psprecomp.game/files/game), makes them writable for the app and starts nothing.
Without adb: install the APK from a file manager and copy those folders to that path by hand (docs/android.md).
Python 3 standard library only.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tarfile

PACKAGE = "org.psprecomp.game"
DEVICE_DIR = "/sdcard/Android/data/%s/files/game" % PACKAGE
DEFAULT_PARTS = ["disc0", "ms0", "voice_jp", "movie_jp", "mods"]


def die(msg):
    print("[psp-build] ERROR " + msg, flush=True)   # the wizard shows this line
    print("error: " + msg, file=sys.stderr, flush=True)
    sys.exit(1)


def find_adb(explicit):
    if explicit:
        return explicit
    exe = "adb.exe" if os.name == "nt" else "adb"
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (shutil.which("adb"), os.path.join(here, "..", "..", "android", "platform-tools", exe)):
        if cand and os.path.isfile(cand):
            return cand
    die("adb not found: install Android platform-tools, or pass --adb PATH")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("workdir")
    ap.add_argument("--adb")
    ap.add_argument("--apk")
    ap.add_argument("--serial")
    ap.add_argument("--no-data", action="store_true", help="install the APK only")
    ap.add_argument("--only", help="comma-separated folders to copy (default: %s)" % ",".join(DEFAULT_PARTS))
    a = ap.parse_args()
    work = os.path.abspath(a.workdir)
    apk = a.apk or os.path.join(work, "android", "dissidia.apk")
    if not os.path.isfile(apk):
        die("%s not found: build the Android version first (the wizard's Android option, or psp_build.py --android)" % apk)
    adb = [find_adb(a.adb)] + (["-s", a.serial] if a.serial else [])

    def run(args, **kw):
        return subprocess.run(adb + args, **kw)

    out = run(["devices"], capture_output=True, text=True).stdout.splitlines()[1:]
    devs = [l.split()[0] for l in out if l.strip().endswith("device")]
    if not devs:
        die("no device: enable USB debugging (or wireless debugging and `adb pair` / `adb connect`) and allow this computer")
    if len(devs) > 1 and not a.serial:
        die("several devices (%s): pass --serial" % ", ".join(devs))
    print("Installing %s ..." % apk, flush=True)
    r = run(["install", "-r", apk], capture_output=True, text=True)
    print(r.stdout.strip(), flush=True)
    if r.returncode != 0 or "Success" not in r.stdout:
        if "INSTALL_FAILED_UPDATE_INCOMPATIBLE" in r.stdout + r.stderr:
            die("an app with the same name but another signature is installed: uninstall it first (this deletes its game copy on the device)")
        die("install failed: " + r.stderr.strip())
    if a.no_data:
        return
    parts = a.only.split(",") if a.only else DEFAULT_PARTS
    run(["shell", "mkdir", "-p", DEVICE_DIR])
    for name in parts:
        src = os.path.join(work, name)
        if not os.path.isdir(src):
            if name in ("disc0",):
                die("%s is missing: nothing to copy" % src)
            continue
        print("Copying %s ..." % name, flush=True)
        p = subprocess.Popen(adb + ["exec-in", "tar xf - -C %s" % DEVICE_DIR], stdin=subprocess.PIPE)
        with tarfile.open(fileobj=p.stdin, mode="w|") as t:
            t.add(src, arcname=name)
        p.stdin.close()
        if p.wait() != 0:
            die("copying %s failed (exec-in unsupported? update platform-tools)" % name)
    # files copied by adb belong to the shell user: make them writable for the app (saves, settings)
    run(["shell", "chmod", "-R", "a+rwX", DEVICE_DIR])
    print("Done. Start \"Dissidia 012\" on the device. Touch the screen to show the on-screen controls.", flush=True)


if __name__ == "__main__":
    main()
