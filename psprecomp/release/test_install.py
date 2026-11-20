#!/usr/bin/env python3
"""Release install test: install a just-built Linux release from the player's ISO, the way players do, and play a fight.

Run by the release workflow on the self-hosted runner that has the game files (job install-test, label dissidia-install), before
the release is published. Also usable by hand:

    release/test_install.py --setup DissidiaRecomp-Setup-X-linux-x86_64 --iso 012.iso [--d1-iso d1.iso] --work DIR --ms0 MS0

Modes (--modes, default both):
  desktop      the Linux install as players run it (system python3), with the Bartz moves extra when --d1-iso is given and, instead of
               our HD textures (not downloaded here), Add your own texture pack with a small test .zip (the pack two folders deep)
  android-sim  the same install with Python only runnable through a loader, as on Android: the release gets a non-executable
               python/python (a copy of this host's python3) and PSPRECOMP_TEST_PY_LOADER=ld.so makes the setup start it through
               the loader (setup/proc.cpp wrap_argv). A tool that starts sys.executable directly fails here instead of on phones
               (v0.1.21: xdelta_mod.py build-code).

After each install: the game binary must exist, and with --ms0 (a memory stick with Quick Battle unlocked) a Quick Battle is
played headless (tools/battle_test.py run --direct, screenshots in the work folder); the run log must reach the fight and have no
missing functions ("-> MISSING").

Exit code 0 = every mode passed. The output ends with one RESULT line per mode.
"""
import argparse
import glob
import json
import os
import shutil
import stat
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
TOOLS = os.path.join(REPO, "games", "dissidia", "tools")
LD_SO = "/lib64/ld-linux-x86-64.so.2"


def log(msg):
    print("[test_install] %s" % msg, flush=True)


def run(cmd, logfile, env=None, timeout=None):
    """Run `cmd`, streaming its output to `logfile` (and a short heartbeat to stdout). Returns the exit code."""
    log("$ " + " ".join(cmd))
    t0 = time.time()
    with open(logfile, "w") as f:
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, text=True, errors="replace")
        last = 0.0
        for line in p.stdout:
            f.write(line)
            if line.startswith("== ") or time.time() - last > 60:
                print("  " + line.rstrip()[:200], flush=True)
                last = time.time()
            if timeout and time.time() - t0 > timeout:
                p.kill()
                f.write("\n[test_install] timed out after %d s\n" % timeout)
                break
        rc = p.wait()
    log("exit %d after %d s" % (rc, time.time() - t0))
    return rc


def tail(path, n=40):
    try:
        with open(path, errors="replace") as f:
            return "".join(f.readlines()[-n:])
    except OSError:
        return ""


def unpack(setup, root, env):
    if os.path.isdir(root):
        shutil.rmtree(root)
    os.chmod(setup, os.stat(setup).st_mode | stat.S_IXUSR)
    rc = subprocess.call([setup, "--unpack", root], env=env, stdout=subprocess.DEVNULL)
    if rc != 0 or not os.path.isfile(os.path.join(root, "release.json")):
        raise SystemExit("cannot unpack %s (exit %d)" % (setup, rc))


def install(mode, a, root, env):
    """One headless install. Returns (ok, why, install dir)."""
    dest = os.path.join(a.work, "install_" + mode)
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    env = dict(env)
    py = os.path.join(root, "python", "python")
    if mode == "android-sim":
        if not os.path.exists(LD_SO):
            return False, "no %s on this host" % LD_SO, dest
        os.makedirs(os.path.dirname(py), exist_ok=True)
        shutil.copyfile(os.path.realpath(shutil.which("python3")), py)
        os.chmod(py, 0o644)   # not executable: only `ld.so python/python ...` runs it, as libpython.so on Android
        env["PSPRECOMP_TEST_PY_LOADER"] = LD_SO
    cmd = [a.setup_exe or os.path.join(root, "DissidiaRecomp"), "--root", root, "--headless", "--iso", a.iso, "--dir", dest]
    if mode == "desktop":
        if a.d1_iso:
            cmd += ["--extra", "d1_moves=1", "--extra", "d1_iso=" + a.d1_iso]
        pack = os.path.join(a.work, "own pack.zip")   # an own texture pack file (textures.ini two folders deep, as downloaded packs are)
        with zipfile.ZipFile(pack, "w") as z:
            z.writestr("Test Pack v1/ULUS10566/textures.ini", "[options]\nversion = 1\nhash = quick\n\n[hashes]\n")
            z.writestr("Test Pack v1/ULUS10566/README.txt", "release install test pack\n")
        cmd += ["--extra", "hd_textures=0", "--extra", "texture_add=1", "--extra", "texture_file=" + pack]
    logfile = os.path.join(a.work, "install_%s.log" % mode)
    try:
        rc = run(cmd, logfile, env=env, timeout=a.timeout)
    finally:
        if mode == "android-sim" and os.path.isdir(os.path.dirname(py)):
            shutil.rmtree(os.path.dirname(py))
    if rc != 0:
        return False, "install failed (exit %d); %s:\n%s" % (rc, logfile, tail(logfile)), dest
    packs = os.path.join(dest, "mods", "texpacks")   # several packs side by side (runtime/include/psp_texpacks.h)
    found = {}
    for pid in sorted(os.listdir(packs)) if os.path.isdir(packs) else []:
        try:
            found[pid] = json.load(open(os.path.join(packs, pid, ".source.json"))).get("pack")
        except (OSError, ValueError):
            found[pid] = None
    want = {"own-pack": "file"} if mode == "desktop" else {"recomp-hd": "ai"}
    if found != want or not all(os.path.isfile(os.path.join(packs, p, "textures.ini")) for p in want):
        return False, "installed, but the texture packs are %r, expected %r" % (found, want), dest
    return True, "installed (texture packs: %s)" % ", ".join(found), dest


def play(mode, a, dest, env):
    """A Quick Battle in the installed game (battle_test.py --direct). Returns (ok, why)."""
    if not a.ms0:
        return True, "no --ms0: fight skipped"
    shutil.copytree(a.ms0, os.path.join(dest, "ms0"), dirs_exist_ok=True)   # the install makes an empty ms0: add the test save
    out = os.path.join(a.work, "battle_" + mode)
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    env = dict(env, BT_GPU=os.environ.get("BT_GPU", "auto"))
    logfile = os.path.join(a.work, "battle_%s.log" % mode)
    rc = run([sys.executable, os.path.join(TOOLS, "battle_test.py"), "run", "--work", dest, "--out", out, "--shots", "5,15",
              "--timeout", "240"], logfile, env=env, timeout=400)
    runlog = os.path.join(out, "run_30.log")
    text = open(runlog, errors="replace").read() if os.path.isfile(runlog) else ""
    missing = [l for l in text.splitlines() if "-> MISSING" in l]
    shots = sorted(glob.glob(os.path.join(out, "battle_*.tga")))
    if rc != 0 or "in battle after" not in open(logfile, errors="replace").read():
        return False, "the fight did not start (exit %d); %s:\n%s" % (rc, logfile, tail(logfile))
    if missing:
        return False, "%d missing functions in %s, e.g. %s" % (len(missing), runlog, missing[0])
    if len(shots) < 2:
        return False, "no battle screenshots in %s" % out
    return True, "fight played (%s)" % ", ".join(os.path.basename(s) for s in shots)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--setup", required=True, help="the single-file Linux setup (DissidiaRecomp-Setup-*-linux-x86_64)")
    ap.add_argument("--iso", default=os.environ.get("DISSIDIA_TEST_ISO"), help="Dissidia 012 ISO (default $DISSIDIA_TEST_ISO)")
    ap.add_argument("--d1-iso", default=os.environ.get("DISSIDIA_TEST_D1_ISO"),
                    help="Dissidia (2008) USA ISO: also install the Bartz moves extra (default $DISSIDIA_TEST_D1_ISO)")
    ap.add_argument("--ms0", default=os.environ.get("DISSIDIA_TEST_MS0"),
                    help="memory stick folder with a save that has Quick Battle unlocked: play a fight (default $DISSIDIA_TEST_MS0)")
    ap.add_argument("--work", default=os.environ.get("DISSIDIA_TEST_WORK"), help="scratch folder (emptied first)")
    ap.add_argument("--modes", default="desktop,android-sim")
    ap.add_argument("--timeout", type=int, default=3600, help="per install, seconds")
    ap.add_argument("--setup-exe", help="run this setup program on the unpacked release instead of its own (testing a setup change)")
    ap.add_argument("--tools", help="copy this game/tools folder over the release's (testing a tools change before a release)")
    ap.add_argument("--keep", action="store_true", help="keep the installs (default: deleted after a pass)")
    a = ap.parse_args()
    if not a.iso or not os.path.isfile(a.iso):
        raise SystemExit("no Dissidia 012 ISO (--iso or $DISSIDIA_TEST_ISO)")
    if a.d1_iso and not os.path.isfile(a.d1_iso):
        raise SystemExit("--d1-iso %s not found" % a.d1_iso)
    if not a.work:
        raise SystemExit("no --work folder")
    a.work = os.path.abspath(a.work)
    a.setup = os.path.abspath(a.setup)
    if os.path.isdir(a.work):
        shutil.rmtree(a.work)
    os.makedirs(a.work)
    # Own HOME: the setup's data folder, settings and Steam detection stay inside the work folder, never the user's.
    home = os.path.join(a.work, "home")
    os.makedirs(home)
    env = dict(os.environ, HOME=home, XDG_DATA_HOME=os.path.join(home, ".local", "share"),
               XDG_CONFIG_HOME=os.path.join(home, ".config"), SDL_VIDEODRIVER="offscreen")
    env.pop("PSPRECOMP_PY_LOADER", None)
    root = os.path.join(a.work, "release")
    unpack(a.setup, root, env)
    if a.tools:
        shutil.copytree(a.tools, os.path.join(root, "game", "tools"), dirs_exist_ok=True)
    results = []
    for mode in [m for m in a.modes.split(",") if m]:
        log("===== %s =====" % mode)
        ok, why, dest = install(mode, a, root, env)
        if ok:
            ok, why = play(mode, a, dest, env)
        results.append((mode, ok, why))
        if ok and not a.keep:
            shutil.rmtree(dest, ignore_errors=True)
    print()
    for mode, ok, why in results:
        print("RESULT %s %s: %s" % (mode, "PASS" if ok else "FAIL", why))
    return 0 if all(ok for _, ok, _ in results) else 1


if __name__ == "__main__":
    sys.exit(main())
