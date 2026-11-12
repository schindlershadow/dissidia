#!/usr/bin/env python3
"""Update android/shaders/ from a device: the game records every shader source it asks for in <game folder>/vk_cache/src/
(runtime/src/vk/psp_vk_glsl.h); this copies the ones the list does not have yet. Run before every Android build so the shader pack keeps up
with what the game really uses (the build scripts call it; it does nothing, quietly, when no device with the game is reachable).

  sync_shader_list.py [--serial SERIAL] [--prune] [--quiet]

--prune also deletes list entries the device did not record (clear the device's vk_cache/src first, play through the scenes you care about,
then prune: it drops shaders of code that no longer exists).
"""
import argparse, os, shutil, subprocess, sys, tempfile

PKG = "org.psprecomp.game"
SRC = "/sdcard/Android/data/%s/files/game/vk_cache/src" % PKG


def adb(serial, *args):
    cmd = ["adb"] + (["-s", serial] if serial else []) + list(args)
    return subprocess.run(cmd, capture_output=True, text=True, timeout=120)


def find_serial():
    try:
        r = subprocess.run(["adb", "devices"], capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired):
        return None
    for line in r.stdout.splitlines()[1:]:
        f = line.split()
        if len(f) == 2 and f[1] == "device":
            if adb(f[0], "shell", "pm list packages %s" % PKG).stdout.find(PKG) >= 0:
                return f[0]
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--serial")
    ap.add_argument("--prune", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()
    say = (lambda *x: None) if a.quiet else print
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "shaders")
    os.makedirs(out, exist_ok=True)
    serial = a.serial or find_serial()
    if not serial:
        say("sync_shader_list: no device with %s reachable over adb; the list stays as it is (%d shaders)" % (PKG, len(os.listdir(out))))
        return 0
    with tempfile.TemporaryDirectory() as tmp:
        r = adb(serial, "pull", SRC + "/.", tmp)
        got = [n for n in os.listdir(tmp) if n.endswith(".glsl")]
        if r.returncode != 0 or not got:
            say("sync_shader_list: %s has no recorded shaders (%s)" % (serial, (r.stderr or r.stdout).strip()[:120]))
            return 0
        new = 0
        for n in got:
            dst = os.path.join(out, n)
            if not os.path.exists(dst):
                shutil.copy2(os.path.join(tmp, n), dst)
                new += 1
        pruned = 0
        if a.prune:
            for n in os.listdir(out):
                if n.endswith(".glsl") and n not in got:
                    os.remove(os.path.join(out, n))
                    pruned += 1
    say("sync_shader_list: %s recorded %d shaders, %d new%s; the list has %d" % (
        serial, len(got), new, ", %d pruned" % pruned if a.prune else "", len([n for n in os.listdir(out) if n.endswith(".glsl")])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
