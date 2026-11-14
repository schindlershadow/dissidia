#!/usr/bin/env python3
"""Download an HD texture pack into an installed game. Two packs are offered (--pack):

  liink  LiinkPK's Dissidia 012 HD Project (https://github.com/LiinkPK/Dissidia-012-HD-Textures): hand-retouched textures, about 0.8 GB.
         A PPSSPP texture pack (textures.ini with hash=quick + PNG files); unpacked into <workdir>/mods/textures as it is.
  ai     (the default of older versions) the community pack below plus our own AI upscales, about 1.3 GB.

  fetch_textures.py <workdir> [--pack liink|ai] [--ref main|<commit>] [--zip FILE] [--keep-zip] [--no-ids] [--ids-zip FILE]

Both packs are credited in <workdir>/mods/textures/PACK_CREDITS.txt (and in the installer). The rest of this text is about the "ai" pack.

The pack is Niosa/D012FFRT on GitHub (https://github.com/Niosa/D012FFRT, "HD Texture Mod for Dissidia 012", a PPSSPP texture pack:
textures.ini + PNG files). It is NOT part of this project and is not bundled in releases: this script downloads it to the player's
own machine from GitHub (the repository states no license, so check its terms and credits; a Credits.txt comes with it).
Unpacked into <workdir>/mods/textures, which is where the runtime looks for a pack (PSPRECOMP_TEXPACK overrides). The runtime reads the
PPSSPP `textures.ini` directly (docs/texture-packs.md, "PPSSPP-style packs"): nothing has to be converted.

Then it adds our own upscales from https://github.com/schindlershadow/dissidia-hd-textures (about 1.1 GB, 3,366 files named <id>.png): textures
the pack lacks and replacements for its pixel-repeated entries. They are unpacked next to it and the runtime looks them up first
(docs/texture-packs.md). If that download fails the pack alone is installed; --no-ids skips it.

The pack was made for the US release (ULUS10566); other releases share most textures, a warning is printed for them.
--zip FILE uses an already downloaded archive (offline installs, tests). Re-running replaces the pack with the newest one.
Python 3 standard library only.
"""
import argparse
import json
import os
import re
import shutil
import sys
import urllib.request
import zipfile

REPO = "Niosa/D012FFRT"
LIINK_REPO = "LiinkPK/Dissidia-012-HD-Textures"   # default branch: master
CREDITS = {
    "liink": "HD textures: Dissidia 012 HD Project by LiinkPK (https://github.com/LiinkPK/Dissidia-012-HD-Textures), hand-retouched.\n"
             "Further artwork credits are in Credits.txt and readme.md in this folder (ajb3art, unsplash.com).\n"
             "The artwork belongs to Square Enix; this is an unofficial fan project.\n",
    "ai": "HD textures: D012FFRT by Niosa (https://github.com/Niosa/D012FFRT), the community retexture project (Credits.txt in this folder),\n"
          "plus AI upscales of the remaining textures by schindlershadow (https://github.com/schindlershadow/dissidia-hd-textures),\n"
          "made with Real-ESRGAN by Xintao Wang et al. (BSD-3-Clause, https://github.com/xinntao/Real-ESRGAN).\n"
          "The artwork belongs to Square Enix; this is an unofficial fan project.\n",
}
IDS_REPO = "schindlershadow/dissidia-hd-textures"   # our own upscales, <16 hex digit id>.png (see its README)
ID_RE = re.compile(r"^[0-9a-f]{16}\.png$")
UA = {"User-Agent": "psprecomp-fetch-textures/1"}


def say(msg):
    print(msg, flush=True)


def die(msg):
    print("[textures] ERROR " + msg, flush=True)
    print("error: " + msg, file=sys.stderr, flush=True)
    sys.exit(1)


def http(url, timeout=60):
    return urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=timeout)


def disc_id(workdir):
    p = os.path.join(workdir, "disc0", "PSP_GAME", "PARAM.SFO")
    try:
        data = open(p, "rb").read()
    except OSError:
        return None
    m = re.search(rb"U[LC][A-Z]{2}\d{5}", data)
    return m.group(0).decode() if m else None


def fetch_id_textures(tmp, mods, a):
    """Our own upscales (textures/<id>.png in IDS_REPO) go next to the PPSSPP pack: flat files the runtime looks up first.
    Returns (count, ref) or (0, None) when skipped."""
    if a.no_ids:
        return 0, None
    zpath = a.ids_zip
    sha = "main"
    if not zpath:
        say("[textures] STEP 4/5 Looking up the extra textures")
        try:
            with http("https://api.github.com/repos/%s/commits/main" % IDS_REPO, 20) as r:
                sha = json.loads(r.read().decode())["sha"]
        except Exception as e:  # noqa: BLE001
            say("could not resolve main (%s); using the branch" % e)
        url = "https://codeload.github.com/%s/zip/%s" % (IDS_REPO, sha)
        zpath = os.path.join(mods, ".hdtextures.zip")
        say("[textures] STEP 4/5 Downloading the extra textures %s (about 1.1 GB)" % url)
        try:
            with http(url, 120) as r, open(zpath + ".part", "wb") as f:
                total = int(r.headers.get("Content-Length") or 0)
                got, last = 0, -1
                while True:
                    b = r.read(1 << 20)
                    if not b:
                        break
                    f.write(b)
                    got += len(b)
                    pc = got * 100 // total if total else min(99, got >> 24)
                    if pc != last:
                        say("[textures] PROGRESS %d" % pc)
                        last = pc
            os.replace(zpath + ".part", zpath)
        except Exception as e:  # noqa: BLE001 - the Niosa pack alone still works
            say("[textures] WARNING the extra textures could not be downloaded (%s); continuing without them" % e)
            try:
                os.remove(zpath + ".part")
            except OSError:
                pass
            return 0, None
    say("[textures] STEP 5/5 Unpacking the extra textures")
    n = 0
    try:
        with zipfile.ZipFile(zpath) as z:
            for info in z.infolist():
                parts = [p for p in info.filename.split("/") if p]
                if info.is_dir() or len(parts) != 3 or parts[1] != "textures" or not ID_RE.match(parts[2]):
                    continue                            # <top>/textures/<id>.png only
                with z.open(info) as s, open(os.path.join(tmp, parts[2]), "wb") as o:
                    shutil.copyfileobj(s, o)
                n += 1
                if n % 200 == 0:
                    say("[textures] PROGRESS %d" % min(99, n * 100 // 3400))
    except zipfile.BadZipFile as e:
        say("[textures] WARNING the extra textures archive is damaged (%s); continuing without them" % e)
        return 0, None
    if not a.ids_zip:
        try:
            os.remove(zpath)
        except OSError:
            pass
    return n, sha


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("workdir")
    ap.add_argument("--pack", choices=["liink", "ai"], default="ai", help="which pack (default ai, as before)")
    ap.add_argument("--ref", default=None, help="branch or commit of the pack (default: main, master for liink)")
    ap.add_argument("--zip", help="use this archive instead of downloading")
    ap.add_argument("--keep-zip", action="store_true")
    ap.add_argument("--consume-zips", action="store_true", help="delete the archives given with --zip / --ids-zip once unpacked (the Android app downloads them)")
    ap.add_argument("--no-ids", action="store_true", help="skip the extra textures (%s)" % IDS_REPO)
    ap.add_argument("--ids-zip", help="use this archive of %s instead of downloading" % IDS_REPO)
    a = ap.parse_args()
    work = os.path.abspath(a.workdir)
    if not os.path.isdir(os.path.join(work, "disc0")):
        die("%s is not an install folder (no disc0/)" % work)
    did = disc_id(work)
    if did and did != "ULUS10566":
        say("note: this pack was made for the US release (ULUS10566); your disc is %s. Most textures are shared; the rest stay original." % did)

    mods = os.path.join(work, "mods")
    dest = os.path.join(mods, "textures")
    os.makedirs(mods, exist_ok=True)
    if a.pack == "liink":
        a.no_ids = True
    REPO_ = LIINK_REPO if a.pack == "liink" else REPO
    if a.ref is None:
        a.ref = "master" if a.pack == "liink" else "main"
    zpath = a.zip
    sha = a.ref
    if not zpath:
        say("[textures] STEP 1/3 Looking up the newest pack version")
        try:
            with http("https://api.github.com/repos/%s/commits/%s" % (REPO_, a.ref), 20) as r:
                sha = json.loads(r.read().decode())["sha"]
        except Exception as e:  # noqa: BLE001 - rate limit / offline: fall back to the branch name
            say("could not resolve %s (%s); using the branch" % (a.ref, e))
        url = "https://codeload.github.com/%s/zip/%s" % (REPO_, sha)
        zpath = os.path.join(mods, ".d012ffrt.zip")
        say("[textures] STEP 2/3 Downloading %s (%s)" % (url, "about 0.8 GB" if a.pack == "liink" else "about 200 MB; our own upscales follow, about 1.1 GB"))
        try:
            with http(url, 120) as r, open(zpath + ".part", "wb") as f:
                total = int(r.headers.get("Content-Length") or 0)
                got = 0
                last = -1
                while True:
                    b = r.read(1 << 20)
                    if not b:
                        break
                    f.write(b)
                    got += len(b)
                    pc = got * 100 // total if total else min(99, got >> 22)
                    if pc != last:
                        say("[textures] PROGRESS %d" % pc)
                        last = pc
            os.replace(zpath + ".part", zpath)
        except Exception as e:  # noqa: BLE001
            die("download failed: %s" % e)
    say("[textures] STEP 3/3 Unpacking")
    tmp = dest + ".new"
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    n = 0
    try:
        with zipfile.ZipFile(zpath) as z:
            names = [i for i in z.infolist() if not i.is_dir()]
            for k, info in enumerate(names):
                parts = [p for p in info.filename.split("/") if p]
                if len(parts) < 2 or ".." in parts:
                    continue
                rel = parts[1:]                       # drop the "D012FFRT-<ref>/" top folder
                if rel[-1].lower().endswith((".psd", ".lnk", ".svg")) or rel[0] in (".github", ".gitignore"):
                    continue                          # sources and shortcuts: not used by the game
                t = os.path.join(tmp, *rel)
                os.makedirs(os.path.dirname(t), exist_ok=True)
                with z.open(info) as s, open(t, "wb") as o:
                    shutil.copyfileobj(s, o)
                n += 1
                if k % 50 == 0:
                    say("[textures] PROGRESS %d" % (k * 100 // max(1, len(names))))
    except zipfile.BadZipFile as e:
        shutil.rmtree(tmp, ignore_errors=True)
        die("the pack archive is damaged (%s); run again" % e)
    if not os.path.isfile(os.path.join(tmp, "textures.ini")):
        shutil.rmtree(tmp, ignore_errors=True)
        die("the archive has no textures.ini: not the expected texture pack")
    n_ids, ids_sha = fetch_id_textures(tmp, mods, a)
    with open(os.path.join(tmp, "PACK_CREDITS.txt"), "w") as f:
        f.write(CREDITS[a.pack])
    with open(os.path.join(tmp, ".source.json"), "w") as f:
        json.dump({"pack": a.pack, "repo": REPO_, "ref": sha, "files": n, "disc": did, "ids_repo": IDS_REPO, "ids_ref": ids_sha, "ids_files": n_ids}, f)
    shutil.rmtree(dest, ignore_errors=True)
    os.replace(tmp, dest)
    if not a.zip and not a.keep_zip:
        try:
            os.remove(zpath)
        except OSError:
            pass
    if a.consume_zips:
        for z in (a.zip, a.ids_zip):
            try:
                if z:
                    os.remove(z)
            except OSError:
                pass
    pngs = sum(1 for dp, _, fns in os.walk(dest) for fn in fns if fn.lower().endswith(".png"))
    say("HD texture pack installed: %d files (%d PNG, %d of them our own upscales) in %s" % (n + n_ids, pngs, n_ids, dest))
    say("[textures] PROGRESS 100")
    say("[textures] DONE")
    return 0


if __name__ == "__main__":
    sys.exit(main())
