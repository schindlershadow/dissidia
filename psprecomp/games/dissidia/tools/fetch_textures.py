#!/usr/bin/env python3
"""Install an HD texture pack into an installed game (--pack):

  ai     (default) "Dissidia Recomp HD": the community pack below plus our own AI upscales, downloaded, about 1.3 GB.
  file   the player's own pack archive (--zip FILE, also "fd:N" from the Android picker): a .zip, or on a PC a .7z / .rar that 7-Zip
         (7z / 7za / 7zz) or bsdtar (Windows' tar.exe) unpacks. The folder holding its textures.ini (a PPSSPP-style pack; any depth, the
         one named after the disc's id first) is installed as it is; a pack of only id-named PNGs (<16 hex digits>.png) also works.
         Nothing is downloaded or added, and the archive is never deleted.

  fetch_textures.py <workdir> [--pack ai|file] [--ref main|<commit>] [--zip FILE] [--keep-zip] [--no-ids] [--ids-zip FILE]

Several packs can be installed side by side: each goes to <workdir>/mods/texpacks/<id> ("recomp-hd" for ours, a name made from the
archive's file name for an own pack; installing the same one again replaces it), with .source.json ("name", "installed") and
PACK_CREDITS.txt. The game applies one, picked in Settings > Graphics > Texture Pack (runtime/include/psp_texpacks.h): the newest
until the player picks another. A pack an older release installed to <workdir>/mods/textures is moved in first (it stays installed).
The rest of this text is about the "ai" pack.

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
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile

REPO = "Niosa/D012FFRT"
CREDITS = {
    "file": "HD textures: your own texture pack, installed from %s.\n"
            "Its own credits and license, if it came with any, are in this folder. The artwork belongs to Square Enix.\n",
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


def open_pack(path):
    """The player's archive as a seekable binary file: a path, or "fd:N" (the Android picker's descriptor, duplicated so ours can be
    closed without closing the app's)."""
    if path.startswith("fd:"):
        return os.fdopen(os.dup(int(path[3:].split(":")[0])), "rb")
    return open(path, "rb")


def pack_root(names, did):
    """Of the archive's file names (forward slashes), the folder the pack is in: the one holding textures.ini (the one named after the
    disc id first, then the shallowest), "" for the top. None when there is no textures.ini."""
    ini = [n for n in names if n.split("/")[-1].lower() == "textures.ini" and "__MACOSX/" not in n]
    if not ini:
        return None
    ini.sort(key=lambda n: (0 if did and ("/" + did + "/") in ("/" + n) else 1, n.count("/"), n))
    return ini[0][: -len(ini[0].split("/")[-1])]


def archive_tool():
    """A program that unpacks .7z / .rar: [argv prefix...] to which the archive and the destination are added, or None."""
    for t in ("7z", "7za", "7zz"):
        exe = shutil.which(t)
        if exe:
            return lambda src, dst, exe=exe: [exe, "x", "-y", "-bd", "-o" + dst, src]
    for t in ("bsdtar",) + (("tar",) if os.name == "nt" else ()):   # Windows 10+ tar.exe is bsdtar: reads .7z and .rar
        exe = shutil.which(t)
        if exe:
            return lambda src, dst, exe=exe: [exe, "-x", "-f", src, "-C", dst]
    return None


def unpack_own_pack(src, tmp, did):
    """Installs the player's pack archive into the (empty) folder tmp. Returns (files, what) or dies with a message for the player."""
    with open_pack(src) as f:
        head = f.read(8)
        f.seek(0)
        if head.startswith(b"PK"):
            try:
                z = zipfile.ZipFile(f)
            except zipfile.BadZipFile as e:
                die("the texture pack file is damaged or not a .zip (%s)" % e)
            files = [i for i in z.infolist() if not i.is_dir()]
            names = [i.filename.replace("\\", "/") for i in files]
            root = pack_root(names, did)
            n = 0
            for k, (info, name) in enumerate(zip(files, names)):
                parts = [p for p in name.split("/") if p]
                if not parts or ".." in parts or name.startswith("/") or "__MACOSX" in parts:
                    continue
                if root is not None:
                    if not name.startswith(root):
                        continue
                    rel = [p for p in name[len(root):].split("/") if p]
                elif ID_RE.match(parts[-1].lower()):
                    rel = [parts[-1].lower()]                 # a pack of id-named PNGs: flat, as the runtime looks them up
                else:
                    continue
                t = os.path.join(tmp, *rel)
                os.makedirs(os.path.dirname(t), exist_ok=True)
                with z.open(info) as s, open(t, "wb") as o:
                    shutil.copyfileobj(s, o)
                n += 1
                if k % 50 == 0:
                    say("[textures] PROGRESS %d" % (k * 100 // max(1, len(files))))
            return n, "zip"
        if not (head.startswith(b"7z\xbc\xaf\x27\x1c") or head.startswith(b"Rar!")):
            die("the texture pack file is not a .zip, .7z or .rar")
        kind = "7z" if head.startswith(b"7z") else "rar"
        tool = archive_tool()
        if src.startswith("fd:") or tool is None:
            die("a .%s pack can not be unpacked here%s: unpack it yourself, zip the folder again and pick the .zip"
                % (kind, "" if src.startswith("fd:") else " (install 7-Zip, 7-zip.org, to use it directly)"))
    raw = tempfile.mkdtemp(prefix=".unpack-", dir=os.path.dirname(tmp))
    try:
        say("[textures] Unpacking the .%s with %s" % (kind, os.path.basename(tool(src, raw)[0])))
        r = subprocess.run(tool(src, raw), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
        if r.returncode != 0:
            die("could not unpack the .%s: %s" % (kind, r.stdout.strip()[-300:]))
        found = []
        for dp, _, fns in os.walk(raw):
            for fn in fns:
                found.append(os.path.relpath(os.path.join(dp, fn), raw).replace(os.sep, "/"))
        root = pack_root(found, did)
        n = 0
        real_raw = os.path.realpath(raw)
        for name in found:
            full = os.path.realpath(os.path.join(raw, name))
            if not full.startswith(real_raw + os.sep) or "__MACOSX" in name.split("/"):
                continue
            if root is not None:
                if not name.startswith(root):
                    continue
                rel = [p for p in name[len(root):].split("/") if p]
            elif ID_RE.match(name.split("/")[-1].lower()):
                rel = [name.split("/")[-1].lower()]
            else:
                continue
            t = os.path.join(tmp, *rel)
            os.makedirs(os.path.dirname(t), exist_ok=True)
            shutil.move(full, t)
            n += 1
        return n, kind
    finally:
        shutil.rmtree(raw, ignore_errors=True)


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
    ap.add_argument("--pack", choices=["ai", "file"], default="ai", help="which pack (default ai); file = the player's own archive (--zip)")
    ap.add_argument("--ref", default=None, help="branch or commit of the pack (default: main)")
    ap.add_argument("--zip", help="use this archive instead of downloading (--pack file: the player's pack, a path or fd:N)")
    ap.add_argument("--keep-zip", action="store_true")
    ap.add_argument("--consume-zips", action="store_true", help="delete the archives given with --zip / --ids-zip once unpacked (the Android app downloads them)")
    ap.add_argument("--no-ids", action="store_true", help="skip the extra textures (%s)" % IDS_REPO)
    ap.add_argument("--ids-zip", help="use this archive of %s instead of downloading" % IDS_REPO)
    ap.add_argument("--name", help="--pack file: the archive's file name when --zip is fd:N (the Android picker); names the pack")
    a = ap.parse_args()
    work = os.path.abspath(a.workdir)
    if not os.path.isdir(os.path.join(work, "disc0")):
        die("%s is not an install folder (no disc0/)" % work)
    did = disc_id(work)
    mods = os.path.join(work, "mods")
    os.makedirs(mods, exist_ok=True)
    migrate_old_pack(mods)
    if a.pack == "file":
        return install_own_pack(a, work, mods, None, did)
    dest = os.path.join(mods, "texpacks", "recomp-hd")
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    if did and did != "ULUS10566":
        say("note: this pack was made for the US release (ULUS10566); your disc is %s. Most textures are shared; the rest stay original." % did)
    REPO_ = REPO
    if a.ref is None:
        a.ref = "main"
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
        say("[textures] STEP 2/3 Downloading %s (about 200 MB; our own upscales follow, about 1.1 GB)" % url)
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
    tmp = os.path.join(os.path.dirname(dest), ".new-" + os.path.basename(dest))   # hidden: the game skips dot folders
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
        json.dump({"pack": a.pack, "name": "Dissidia Recomp HD", "installed": int(time.time()), "repo": REPO_, "ref": sha, "files": n, "disc": did,
                   "ids_repo": IDS_REPO, "ids_ref": ids_sha, "ids_files": n_ids}, f)
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


def migrate_old_pack(mods):
    """An older release kept its one pack in mods/textures: move it into mods/texpacks (it stays installed, as one of the packs)."""
    old = os.path.join(mods, "textures")
    if not os.path.isdir(old) or not (os.path.isfile(os.path.join(old, "textures.ini")) or os.path.isfile(os.path.join(old, ".source.json"))):
        return
    info = {}
    try:
        info = json.load(open(os.path.join(old, ".source.json")))
    except (OSError, ValueError):
        pass
    pid = "recomp-hd" if info.get("pack") == "ai" else "earlier-pack"
    if info.get("pack") == "file" and info.get("archive"):
        pid = pack_id(info["archive"])
    target = os.path.join(mods, "texpacks", pid)
    if os.path.exists(target):
        pid += "-old"
        target = os.path.join(mods, "texpacks", pid)
    os.makedirs(os.path.dirname(target), exist_ok=True)
    os.replace(old, target)
    if not info.get("name"):
        info["name"] = "Dissidia Recomp HD" if info.get("pack") == "ai" else (info.get("archive") or "Pack installed earlier")
        info.setdefault("installed", int(os.path.getmtime(target)))
        info["migrated_from"] = "textures"
        with open(os.path.join(target, ".source.json"), "w") as f:
            json.dump(info, f)
    say("[textures] moved the pack installed earlier (mods/textures) to mods/texpacks/%s" % pid)


def pack_id(file_name):
    """A folder name for an own pack, from its archive's file name: "My Pack v2 (US).zip" -> "my-pack-v2-us"."""
    base = re.sub(r"\.(zip|7z|rar)$", "", os.path.basename(file_name), flags=re.I)
    pid = re.sub(r"[^a-z0-9]+", "-", base.lower()).strip("-")[:48].strip("-")
    return pid if pid and pid not in ("recomp-hd", "textures") else "my-pack"


def install_own_pack(a, work, mods, dest, did):
    """--pack file: the player's archive, unpacked as it is (no download, none of our upscales on top: they would win over the pack)."""
    if not a.zip:
        die("--pack file needs --zip FILE (the texture pack archive)")
    name = a.name or (os.path.basename(a.zip) if not a.zip.startswith("fd:") else "Your texture pack")
    dest = os.path.join(mods, "texpacks", pack_id(name))   # the same archive again replaces its pack; another one is added
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    say("[textures] STEP 1/1 Unpacking your texture pack (%s)" % name)
    tmp = os.path.join(os.path.dirname(dest), ".new-" + os.path.basename(dest))   # hidden: the game skips dot folders
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    try:
        n, kind = unpack_own_pack(a.zip, tmp, did)
    except SystemExit:
        shutil.rmtree(tmp, ignore_errors=True)
        raise
    except OSError as e:
        shutil.rmtree(tmp, ignore_errors=True)
        die("could not read the texture pack file: %s" % e)
    pngs = sum(1 for dp, _, fns in os.walk(tmp) for fn in fns if fn.lower().endswith(".png"))
    if not os.path.isfile(os.path.join(tmp, "textures.ini")) and not any(ID_RE.match(f) for f in os.listdir(tmp)):
        shutil.rmtree(tmp, ignore_errors=True)
        die("the archive has no textures.ini (and no PNG files named by texture ids): not a texture pack the game can use")
    with open(os.path.join(tmp, "PACK_CREDITS.txt"), "w") as f:
        f.write(CREDITS["file"] % name)
    with open(os.path.join(tmp, ".source.json"), "w") as f:
        json.dump({"pack": "file", "name": re.sub(r"\.(zip|7z|rar)$", "", name, flags=re.I), "installed": int(time.time()), "archive": name,
                   "format": kind, "files": n, "disc": did}, f)
    shutil.rmtree(dest, ignore_errors=True)
    os.replace(tmp, dest)
    say("HD texture pack installed: %d files (%d PNG) from %s in %s" % (n, pngs, name, dest))
    say("[textures] PROGRESS 100")
    say("[textures] DONE")
    return 0


if __name__ == "__main__":
    sys.exit(main())
