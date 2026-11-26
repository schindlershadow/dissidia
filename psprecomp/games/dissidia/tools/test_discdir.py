#!/usr/bin/env python3
"""Tests of discdir.py and android_local_install.py (one copy of the disc on the phone, the build clean-up)."""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import discdir  # noqa: E402

PKG = "PSP_GAME/USRDIR/DATA/PACKAGE.BIN"


def wr(p, data=b"x"):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "wb").write(data)


def run_install(work, dest, *extra, env=None):
    e = dict(os.environ, PSPRECOMP_DISC_HOME=os.path.join(dest, "disc0"))
    e.pop("PSPRECOMP_DISC_HOME_OLD", None)
    e.pop("PSPRECOMP_KEEP_BUILD", None)
    e.update(env or {})
    return subprocess.run([sys.executable, os.path.join(HERE, "android_local_install.py"), work, "--dest", dest] + list(extra),
                          env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def leftovers(work):
    for n in ("output/include/recomp.h", "overlays/battle/dispatch.cpp", "analysis.json", "analysis_menu.json.grafted.json", "EBOOT.dec.elf",
              "eboot_image.bin", "OVL_BATTLE_APP.bin", ".recompile.stamp", "android/libmain.so", "android/build/libmain.so", "android/zig-cache/a", "tmp/t",
              "android/obj/keep.o", "android/dissidia.apk", "EBOOT.enc.bin", "mods/m.txt"):
        wr(os.path.join(work, n))


def main():
    d = tempfile.mkdtemp(prefix="discdir_test_")
    os.environ.pop("PSPRECOMP_DISC_HOME", None)
    # no home: the work folder's own disc0
    assert discdir.disc_dir(d + "/w") == d + "/w/disc0"
    # an older install: disc0 in the work folder, a half-copied game folder (one file the same size, one different, one missing)
    w, g = d + "/w", d + "/g"
    wr(w + "/disc0/" + PKG, b"pkg123")
    wr(w + "/disc0/PSP_GAME/SYSDIR/EBOOT.BIN", b"\x7fELF" + b"e" * 10)
    wr(w + "/disc0/PSP_GAME/USRDIR/a.bin", b"aaaa")
    wr(g + "/disc0/" + PKG, b"pkg123")
    wr(g + "/disc0/PSP_GAME/USRDIR/a.bin", b"zz")
    wr(g + "/disc0/PSP_GAME/USRDIR/b.bin.part", b"cut")
    os.environ["PSPRECOMP_DISC_HOME"] = g + "/disc0"
    assert discdir.disc_dir(w, lambda m: None) == g + "/disc0"
    assert not os.path.exists(w + "/disc0"), "the work folder's copy is gone"
    assert open(g + "/disc0/PSP_GAME/USRDIR/a.bin", "rb").read() == b"aaaa"
    assert not os.path.exists(g + "/disc0/PSP_GAME/USRDIR/b.bin.part")
    assert discdir.has_game(g + "/disc0")
    # the EBOOT is made again from the disc's decrypted EBOOT.BIN
    assert discdir.eboot_elf(w, lambda m: None) == w + "/EBOOT.dec.elf" and open(w + "/EBOOT.dec.elf", "rb").read(4) == b"\x7fELF"
    os.remove(w + "/EBOOT.dec.elf")
    # opt-out
    os.environ["PSPRECOMP_DISC_HOME_OLD"] = "1"
    assert discdir.disc_dir(w) == w + "/disc0"
    del os.environ["PSPRECOMP_DISC_HOME_OLD"]
    del os.environ["PSPRECOMP_DISC_HOME"]

    # the final step: old install (work/disc0 + voice_jp), clean-up
    w2, g2 = d + "/w2", d + "/g2"
    wr(w2 + "/disc0/" + PKG, b"pkgbig")
    wr(w2 + "/voice_jp/v.at3", b"voice")
    wr(w2 + "/ms0/PSP/SAVEDATA/x", b"save")
    leftovers(w2)
    r = run_install(w2, g2, "--clean")
    assert r.returncode == 0, r.stdout
    assert open(g2 + "/disc0/" + PKG, "rb").read() == b"pkgbig" and open(g2 + "/voice_jp/v.at3", "rb").read() == b"voice"
    assert not os.path.exists(w2 + "/disc0") and not os.path.exists(w2 + "/voice_jp"), "moved, not copied"
    assert open(g2 + "/mods/m.txt", "rb").read() == b"x" and os.path.exists(w2 + "/mods/m.txt"), "mods are copied and stay"
    for gone in ("output", "overlays", "analysis.json", "analysis_menu.json.grafted.json", "EBOOT.dec.elf", "eboot_image.bin", "OVL_BATTLE_APP.bin",
                 ".recompile.stamp", "android/libmain.so", "android/build", "android/zig-cache", "tmp/t"):
        assert not os.path.exists(os.path.join(w2, gone)), gone
    for kept in ("android/obj/keep.o", "android/dissidia.apk", "EBOOT.enc.bin", "mods/m.txt"):
        assert os.path.exists(os.path.join(w2, kept)), kept
    assert "CLEAN freed" in r.stdout
    # a second run (the game's disc already there, nothing in the work folder) works and cleans nothing more
    r = run_install(w2, g2, "--clean")
    assert r.returncode == 0, r.stdout
    # neither side has the disc: a clear error
    w3 = d + "/w3"
    leftovers(w3)
    r = run_install(w3, d + "/g3", "--clean")
    assert r.returncode != 0 and "pick your .iso" in r.stdout and os.path.exists(w3 + "/output/include/recomp.h"), r.stdout
    # opt-outs: KEEP_BUILD keeps the leftovers, DISC_HOME_OLD copies
    w4 = d + "/w4"
    wr(w4 + "/disc0/" + PKG, b"p")
    leftovers(w4)
    r = run_install(w4, d + "/g4", "--clean", env={"PSPRECOMP_KEEP_BUILD": "1"})
    assert r.returncode == 0 and os.path.exists(w4 + "/output/include/recomp.h") and not os.path.exists(w4 + "/disc0/" + PKG), r.stdout
    w5 = d + "/w5"
    wr(w5 + "/disc0/" + PKG, b"p")
    leftovers(w5)
    r = run_install(w5, d + "/g5", "--clean", env={"PSPRECOMP_DISC_HOME_OLD": "1"})
    assert r.returncode == 0 and os.path.exists(w5 + "/disc0/" + PKG) and os.path.exists(d + "/g5/disc0/" + PKG) and os.path.exists(w5 + "/output/include/recomp.h"), r.stdout
    print("test_discdir: all passed")


if __name__ == "__main__":
    main()
