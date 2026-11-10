#!/usr/bin/env python3
"""Check for and apply a new release of the installed runtime (the wizard runs this; the game's own files are never touched).

  update.py check --root RELEASE_DIR [--manifest URL]
  update.py apply --root RELEASE_DIR [--manifest URL]

RELEASE_DIR is the unpacked release (the folder with psprecomp_setup, release.json). The manifest is the manifest.json of a GitHub
Release (release/make_manifest.py); by default the URL written into release.json at build time (the fork's latest release).

check   prints exactly one line and exits 0 whatever happens (a failed check must never stop the game from starting):
          UPDATE version=<new> current=<installed> size=<bytes> rebuild=<0|1> relink=<0|1>
          NOUPDATE current=<installed>
          ERROR <message>
        rebuild=1: the translation inputs changed (recompiler, analysis, game tools, toolchain), the game has to be generated and
        compiled again; relink=1: only the prebuilt runtime core changed (a fast relink); both 0: files only.
apply   downloads the archive for this platform, verifies its sha256, replaces the release's files (the running wizard is moved
        aside on Windows), removes files the new release dropped and writes the new release.json last, so an interrupted update
        is simply run again. Prints "[update] PROGRESS <percent>" lines and "[update] DONE" or "[update] ERROR <message>".

The install's work folder (disc0, saves, settings.ini, mods) lives outside the release folder and is not touched.
Signed manifests: with game/update_key.pub present, <manifest url>.sig must verify (see load_manifest).
Opt-out: settings.ini "check_updates=0" in the work folder, or PSPRECOMP_UPDATE_CHECK=0 (checked by the wizard before it calls this).
Python 3 standard library only.
"""
import argparse
import hashlib
import json
import os
import shutil
import sys
import tarfile
import urllib.request
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ed25519  # noqa: E402

TIMEOUT = 8


def say(msg):
    print(msg, flush=True)


def fetch(url, timeout=TIMEOUT):
    req = urllib.request.Request(url, headers={"User-Agent": "psprecomp-updater/1"})
    return urllib.request.urlopen(req, timeout=timeout)


def load_json_url(url):
    with fetch(url) as r:
        return json.loads(r.read().decode("utf-8"))


def load_manifest(root, url):
    """The manifest at `url`. When the installed release carries game/update_key.pub (an Ed25519 public key, 64 hex
    characters) the manifest must also have a valid signature at <url>.sig (release/sign_manifest.py): a wrong or missing
    signature is an error, so a hijacked download host cannot push an update. Without a key file only the archive's sha256
    from the manifest protects the download (development builds)."""
    with fetch(url) as r:
        body = r.read()
    keyfile = os.path.join(root, "game", "update_key.pub")
    if os.path.isfile(keyfile):
        pub = bytes.fromhex(open(keyfile).read().strip())
        try:
            with fetch(url + ".sig") as r:
                sig = bytes.fromhex(r.read().decode("ascii").strip())
        except Exception as e:  # noqa: BLE001
            raise RuntimeError("manifest signature missing (%s)" % e)
        if not ed25519.verify(pub, body, sig):
            raise RuntimeError("manifest signature is not valid")
    return json.loads(body.decode("utf-8"))


def version_key(v):
    out = []
    for part in v.lstrip("v").replace("-", ".").split("."):
        out.append((0, int(part)) if part.isdigit() else (-1, part))
    return out


def installed(root):
    path = os.path.join(root, "release.json")
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def newer(manifest, cur):
    p = manifest.get("platforms", {}).get(cur["platform"])
    if not p:
        return None
    if manifest.get("min_wizard_version", 1) > 1:   # wizard version 1 is this one
        return None
    if version_key(manifest["version"]) <= version_key(cur["version"]):
        return None
    return p


def cmd_check(a):
    try:
        cur = installed(a.root)
        if not cur:
            say("ERROR %s has no release.json (not an installed release)" % a.root)
            return 0
        url = a.manifest or cur.get("manifest_url")
        if not url:
            say("ERROR no manifest url")
            return 0
        m = load_manifest(a.root, url)
        p = newer(m, cur)
        if not p:
            say("NOUPDATE current=%s" % cur["version"])
            return 0
        rebuild = int(p["rebuild_fingerprint"] != cur.get("rebuild_fingerprint"))
        relink = int(p["relink_fingerprint"] != cur.get("relink_fingerprint"))
        say("UPDATE version=%s current=%s size=%d rebuild=%d relink=%d" % (m["version"], cur["version"], p["size"], rebuild, relink))
    except Exception as e:  # noqa: BLE001 - offline, DNS, 404, bad JSON: all just "no answer"
        say("ERROR %s" % e)
    return 0


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def err(msg):
    say("[update] ERROR " + msg)
    return 1


def extract(archive, dest):
    os.makedirs(dest)
    if archive.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            for info in z.infolist():
                parts = [p for p in info.filename.split("/") if p]
                if ".." in parts or not parts[1:]:
                    continue
                t = os.path.join(dest, *parts[1:])
                if info.is_dir():
                    os.makedirs(t, exist_ok=True)
                    continue
                os.makedirs(os.path.dirname(t), exist_ok=True)
                with z.open(info) as s, open(t, "wb") as o:
                    shutil.copyfileobj(s, o)
                mode = (info.external_attr >> 16) & 0o777
                os.chmod(t, mode or 0o644)
    else:
        with tarfile.open(archive) as tf:
            for m in tf:
                parts = [p for p in m.name.split("/") if p]
                if ".." in parts or not parts[1:]:
                    continue
                t = os.path.join(dest, *parts[1:])
                if m.isdir():
                    os.makedirs(t, exist_ok=True)
                elif m.isfile():
                    os.makedirs(os.path.dirname(t), exist_ok=True)
                    with tf.extractfile(m) as s, open(t, "wb") as o:
                        shutil.copyfileobj(s, o)
                    os.chmod(t, m.mode & 0o777)


def place(src, dst):
    """Move src over dst; a running executable cannot be overwritten on Windows but can be renamed."""
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    try:
        os.replace(src, dst)
    except OSError:
        if os.path.exists(dst):
            old = dst + ".old"
            if os.path.exists(old):
                os.remove(old)
            os.replace(dst, old)
        os.replace(src, dst)


def cmd_apply(a):
    cur = installed(a.root)
    if not cur:
        return err("%s has no release.json" % a.root)
    try:
        m = load_manifest(a.root, a.manifest or cur["manifest_url"])
    except Exception as e:  # noqa: BLE001
        return err("cannot read the manifest: %s" % e)
    p = newer(m, cur)
    if not p:
        say("[update] DONE (already up to date)")
        return 0
    stage = os.path.join(a.root, ".update")
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    arch = os.path.join(stage, p["asset"])
    say("[update] STEP 1/3 Downloading %s" % p["asset"])
    try:
        with fetch(p["url"], 60) as r, open(arch, "wb") as f:
            total = int(r.headers.get("Content-Length") or p["size"])
            got = 0
            last = -1
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
                got += len(chunk)
                pc = min(100, got * 100 // max(1, total))
                if pc != last:
                    say("[update] PROGRESS %d" % pc)
                    last = pc
    except Exception as e:  # noqa: BLE001
        return err("download failed: %s" % e)
    say("[update] STEP 2/3 Checking the download")
    if sha256_file(arch) != p["sha256"]:
        return err("the downloaded file is damaged (checksum mismatch); nothing was changed")
    say("[update] STEP 3/3 Installing")
    new_dir = os.path.join(stage, "new")
    try:
        extract(arch, new_dir)
    except Exception as e:  # noqa: BLE001
        return err("cannot unpack the download: %s" % e)
    try:
        new_rj = json.load(open(os.path.join(new_dir, "release.json"), encoding="utf-8"))
    except (OSError, ValueError):
        return err("the download has no release.json")
    new_rj["manifest_url"] = cur.get("manifest_url", new_rj.get("manifest_url"))
    files = new_rj["files"]
    n = 0
    for rel, info in files.items():
        dst = os.path.join(a.root, *rel.split("/"))
        if os.path.isfile(dst) and sha256_file(dst) == info["sha256"]:
            continue
        place(os.path.join(new_dir, *rel.split("/")), dst)
        n += 1
    for rel in cur.get("files", {}):
        if rel not in files:
            try:
                os.remove(os.path.join(a.root, *rel.split("/")))
            except OSError:
                pass
    with open(os.path.join(a.root, "release.json.tmp"), "w", encoding="utf-8") as f:
        json.dump(new_rj, f, indent=1, sort_keys=True)
    place(os.path.join(a.root, "release.json.tmp"), os.path.join(a.root, "release.json"))
    shutil.rmtree(stage, ignore_errors=True)
    say("[update] %d files replaced" % n)
    say("[update] DONE")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("command", choices=["check", "apply"])
    ap.add_argument("--root", required=True)
    ap.add_argument("--manifest")
    a = ap.parse_args()
    a.root = os.path.abspath(a.root)
    return cmd_check(a) if a.command == "check" else cmd_apply(a)


if __name__ == "__main__":
    sys.exit(main())
