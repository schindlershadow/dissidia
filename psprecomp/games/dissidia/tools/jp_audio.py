#!/usr/bin/env python3
"""Extract the Japanese voices (and the movies that carry them) from an undub / Japanese disc image into an installed game.

  jp_audio.py --iso <undub.iso|cso> --work <workdir> [--dry-run] [--progress]

<workdir> is the install folder made by setup_from_iso.py (it holds disc0/ of the player's own disc). The undub disc is an
ordinary Dissidia 012 image whose voice files are Japanese. This compares its DATA/PACKAGE.BIN with the installed one and
writes only what differs and is audio:

  <workdir>/voice_jp/<asset path>        entries of the voice/en/* tree that differ (the game requests English paths; the
                                         loose-file layer serves these instead when the Voice setting is Japanese)
  <workdir>/voice_jp/by_crc/<crc>.bin    audio entries (RIFF / SSCF data) whose path is not in package_paths.txt but that differ
  <workdir>/movie_jp/<NAME>.PMF          DATA/MOVIE movies of the undub disc that differ from the installed ones

Nothing else is copied (menus, text and the other languages stay the player's own). The runtime offers a "Voice" row in the
settings menu once <workdir>/voice_jp exists (games/dissidia/runtime/hooks_io.cpp). Re-running skips files already extracted.
Reads the undub image in place (ISO or CSO), never extracts all of it. Python 3 standard library only.
"""
import argparse
import hashlib
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import package_extract as pe          # noqa: E402  (package_crc, the index layout)
import setup_from_iso as sfi          # noqa: E402  (Iso9660 / CSO reader)

SECTOR = 2048
AUDIO_MAGICS = (b"RIFF", b"SSCF")
PROGRESS = False


def say(msg):
    print(msg, flush=True)


def tag(kind, text=""):
    if PROGRESS:
        print("[jp-audio] %s %s" % (kind, text), flush=True)


def die(msg):
    tag("ERROR", msg)
    print("error: " + msg, file=sys.stderr, flush=True)
    sys.exit(1)


def parse_index(data):
    magic, tg, count, _ = struct.unpack_from("<I4sII", data, 0)
    if tg != b"pack":
        raise ValueError("not a PACKAGE_INFO.BIN")
    ent = {}
    for i in range(count):
        crc, sector, sf = struct.unpack_from("<III", data, 16 + 12 * i)
        ent[crc] = (sector, sf & 0x0FFFFFFF)
    return ent


def hash_range(f, base, off, size, chunk=4 << 20):
    h = hashlib.md5()
    f.seek(base + off)
    left = size
    while left > 0:
        b = f.read(min(chunk, left))
        if not b:
            raise IOError("short read")
        h.update(b)
        left -= len(b)
    return h.digest()


def copy_range(f, base, off, size, dest, chunk=4 << 20):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    tmp = dest + ".part"
    f.seek(base + off)
    left = size
    with open(tmp, "wb") as out:
        while left > 0:
            b = f.read(min(chunk, left))
            if not b:
                raise IOError("short read")
            out.write(b)
            left -= len(b)
    os.replace(tmp, dest)


def file_md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(4 << 20), b""):
            h.update(b)
    return h.digest()


def main():
    global PROGRESS
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--iso", required=True, help="the undub (or Japanese) disc image")
    ap.add_argument("--work", required=True, help="the install folder (has disc0/)")
    ap.add_argument("--dry-run", action="store_true", help="list what would be extracted, write nothing")
    ap.add_argument("--progress", action="store_true", help="machine-readable [jp-audio] lines")
    a = ap.parse_args()
    PROGRESS = a.progress

    data_dir = os.path.join(a.work, "disc0", "PSP_GAME", "USRDIR", "DATA")
    mine_info = os.path.join(data_dir, "PACKAGE_INFO.BIN")
    mine_pkg = os.path.join(data_dir, "PACKAGE.BIN")
    for p in (mine_info, mine_pkg):
        if not os.path.isfile(p):
            die("%s not found: install the game first (setup_from_iso.py)" % p)
    tag("STEP", "1/4 Opening the undub disc image")
    say("Opening %s" % a.iso)
    try:
        iso = sfi.Iso9660(a.iso)
    except Exception as e:   # noqa: BLE001
        die("cannot read %s as a disc image: %s" % (a.iso, e))
    rec_info = iso.find("PSP_GAME/USRDIR/DATA/PACKAGE_INFO.BIN")
    rec_pkg = iso.find("PSP_GAME/USRDIR/DATA/PACKAGE.BIN")
    if not rec_info or not rec_pkg:
        die("%s has no PSP_GAME/USRDIR/DATA/PACKAGE.BIN: not a Dissidia 012 disc image" % a.iso)
    iso.f.seek(rec_info["extent"] * SECTOR)
    undub = parse_index(iso.f.read(rec_info["size"]))
    mine = parse_index(open(mine_info, "rb").read())
    pkg_base = rec_pkg["extent"] * SECTOR
    say("undub disc: %s, %d package entries; installed: %d entries" % (iso.volume_id, len(undub), len(mine)))

    # crc -> every known path with that CRC (the index is keyed by a 32-bit CRC, and a few unrelated names collide: e.g.
    # voice/en/battle/voice_p_for100_00.scd shares its CRC with a menu image, so a plain dict lost Cecil's main voice bank)
    known = {}
    with open(os.path.join(HERE, "..", "package_paths.txt")) as f:
        for line in f:
            p = line.strip()
            if p:
                known.setdefault(pe.package_crc(p), []).append(p)

    out_voice = os.path.join(a.work, "voice_jp")
    out_movie = os.path.join(a.work, "movie_jp")
    candidates = []
    for crc, (sec, size) in undub.items():
        if crc not in mine:
            continue
        paths = known.get(crc)
        if paths is not None:
            voice = [q for q in paths if q.lower().replace("\\", "/").startswith("voice/en/")]
            # A name recovered for another folder can be a CRC collision (ce_battle_25.at3 shares its CRC with
            # dungeon/image/oq_mono_start.at3), so non-voice names are also decided by the entry's magic below.
            candidates.append((crc, voice[0] if voice else None, sec, size))
        else:
            candidates.append((crc, None, sec, size))   # unknown path: decided by its magic below
    tag("STEP", "2/4 Comparing the voice files")
    say("%d candidate entries to compare" % len(candidates))

    mine_f = open(mine_pkg, "rb")
    n_voice = n_skip = n_new = 0
    for i, (crc, path, sec, size) in enumerate(candidates):
        if i % 200 == 0:
            tag("PROGRESS", str(i * 70 // max(1, len(candidates))))
        if path is None:
            iso.f.seek(pkg_base + sec * SECTOR)
            if iso.f.read(4) not in AUDIO_MAGICS:
                continue
        msec, msize = mine[crc]
        if msize == size and hash_range(mine_f, 0, msec * SECTOR, msize) == hash_range(iso.f, pkg_base, sec * SECTOR, size):
            continue   # identical (same voice in both)
        rel = path.replace("\\", "/") if path else "by_crc/%08x.bin" % crc
        dest = os.path.join(out_voice, *rel.split("/"))
        if os.path.isfile(dest) and os.path.getsize(dest) == size:
            n_skip += 1
            continue
        n_voice += 1
        if a.dry_run:
            say("  would write %s (%d bytes)" % (rel, size))
        else:
            copy_range(iso.f, pkg_base, sec * SECTOR, size, dest)
    mine_f.close()

    tag("STEP", "3/4 Comparing the movies")
    n_movie = 0
    movie_dir_mine = os.path.join(data_dir, "MOVIE")
    movie_rec = iso.find("PSP_GAME/USRDIR/DATA/MOVIE")
    if movie_rec and movie_rec["is_dir"]:
        recs = [r for r in iso.listdir(movie_rec) if not r["is_dir"] and r["name"].upper().endswith(".PMF")]
        for j, r in enumerate(recs):
            tag("PROGRESS", str(70 + j * 30 // max(1, len(recs))))
            mine_path = None
            if os.path.isdir(movie_dir_mine):
                for fn in os.listdir(movie_dir_mine):
                    if fn.upper() == r["name"].upper():
                        mine_path = os.path.join(movie_dir_mine, fn)
            if mine_path and os.path.getsize(mine_path) == r["size"] and \
                    hash_range(iso.f, r["extent"] * SECTOR, 0, r["size"]) == file_md5(mine_path):
                continue
            dest = os.path.join(out_movie, r["name"])
            if os.path.isfile(dest) and os.path.getsize(dest) == r["size"]:
                n_skip += 1
                continue
            n_movie += 1
            if a.dry_run:
                say("  would write movie %s (%d bytes)" % (r["name"], r["size"]))
            else:
                copy_range(iso.f, r["extent"] * SECTOR, 0, r["size"], dest)
    else:
        say("the undub disc has no DATA/MOVIE folder: movies stay as they are")

    iso.close()
    tag("STEP", "4/4 Done")
    if n_voice + n_skip + n_movie == 0:
        die("nothing differs from your disc: this does not look like an undub / Japanese image")
    say("Japanese voices: %d voice files and %d movies %s (%d already there) in %s" % (
        n_voice, n_movie, "found" if a.dry_run else "extracted", n_skip, a.work))
    tag("PROGRESS", "100")
    tag("DONE", "%d voice, %d movies" % (n_voice, n_movie))
    return 0


if __name__ == "__main__":
    sys.exit(main())
