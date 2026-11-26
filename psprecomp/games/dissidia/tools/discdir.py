"""Where the extracted disc files (disc0/) of an install are.

On the desktop: <work>/disc0. On the phone (the setup app sets PSPRECOMP_DISC_HOME to <game folder>/disc0, the folder the game app reads) the
disc files live ONLY there: one copy, not one in the work folder and one in the game's. Every tool that reads the disc asks disc_dir(work).
A work folder that still holds disc0/ (an install of an older version, or an install that was cut off while its files were being moved) is
adopted first: its files are moved into the game folder (a file already there with the same size is kept, the work folder's copy deleted).
PSPRECOMP_DISC_HOME_OLD=1: the old layout (disc0 in the work folder, copied to the game folder at the end). Python 3 standard library only.
"""
import os
import shutil

ENV = "PSPRECOMP_DISC_HOME"
PKG = ("PSP_GAME", "USRDIR", "DATA", "PACKAGE.BIN")


def home():
    """The disc folder outside the work folder ('' = the work folder's own disc0)."""
    if os.environ.get("PSPRECOMP_DISC_HOME_OLD") == "1":
        return ""
    v = os.environ.get(ENV, "")
    return os.path.abspath(v) if v else ""


def has_game(d):
    return os.path.isfile(os.path.join(d, *PKG))


def purge_part(root):
    """Half-copied files of a move that was cut off."""
    for dp, _, fns in os.walk(root):
        for fn in fns:
            if fn.endswith(".part"):
                try:
                    os.remove(os.path.join(dp, fn))
                except OSError:
                    pass


def move_file(s, d):
    """Move one file across file systems: copy to <d>.part, rename, delete the source (at most one file is ever stored twice)."""
    os.makedirs(os.path.dirname(d), exist_ok=True)
    try:
        os.replace(s, d)   # the same file system: a rename
        return
    except OSError:
        pass
    tmp = d + ".part"
    shutil.copyfile(s, tmp)
    os.replace(tmp, d)
    os.remove(s)


def adopt(work, say=print):
    """Moves a leftover <work>/disc0 into the disc home. Returns the number of files moved or dropped."""
    h = home()
    src = os.path.join(work, "disc0")
    if not h or not os.path.isdir(src):
        return 0
    n = 0
    say("Moving the game files from %s to %s (one copy only)" % (src, h))
    try:
        purge_part(h)
        for dp, _, fns in os.walk(src):
            rel = os.path.relpath(dp, src)
            for fn in fns:
                s = os.path.join(dp, fn)
                d = os.path.normpath(os.path.join(h, rel, fn))
                if os.path.isfile(d) and os.path.getsize(d) == os.path.getsize(s):
                    os.remove(s)
                else:
                    move_file(s, d)
                n += 1
    except OSError as e:
        raise SystemExit("cannot move the game files to %s: %s (is \"all files access\" allowed for this app, and is there free space?)" % (h, e))
    for dp, dns, fns in os.walk(src, topdown=False):
        try:
            os.rmdir(dp)
        except OSError:
            pass
    return n


def disc_dir(work, say=print):
    """The folder that holds PSP_GAME/ for this install."""
    h = home()
    legacy = os.path.join(work, "disc0")
    if not h:
        return legacy
    if os.path.isdir(legacy):
        adopt(work, say)
    return h


def prepare(d):
    """The folder for a fresh extraction on the phone: made, and hidden from the gallery (Android's media scanner indexes Android/media; a
    .nomedia file in the package folder hides everything below). A write error is explained in words."""
    try:
        os.makedirs(d, exist_ok=True)
        open(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(d))), ".nomedia"), "a").close()
    except OSError as e:
        raise SystemExit("cannot write %s: %s (is \"all files access\" allowed for this app?)" % (d, e))


def eboot_elf(work, say=print):
    """<work>/EBOOT.dec.elf, made again from the disc's decrypted EBOOT (disc0/PSP_GAME/SYSDIR/EBOOT.BIN is the same file) when the
    build cleanup removed it. '' when it cannot be had."""
    p = os.path.join(work, "EBOOT.dec.elf")
    if os.path.isfile(p):
        return p
    src = os.path.join(disc_dir(work, say), "PSP_GAME", "SYSDIR", "EBOOT.BIN")
    try:
        with open(src, "rb") as f:
            if f.read(4) != b"\x7fELF":
                return ""
        shutil.copyfile(src, p)
    except OSError:
        return ""
    say("EBOOT.dec.elf made again from the disc's EBOOT.BIN")
    return p
