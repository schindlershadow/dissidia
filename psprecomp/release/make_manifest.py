#!/usr/bin/env python3
"""Write manifest.json for a GitHub Release from the archives built by build_release.py.

  make_manifest.py --version V --repo owner/name --assets DIR [--tag vV] [-o manifest.json]

DIR holds psprecomp-<version>-<platform>.tar.xz / .zip (+ .sha256). The manifest is what the updater downloads first (a few KB):

  {"schema": 1, "version": "...", "min_wizard_version": 1, "published": "<utc>",
   "platforms": {"linux-x86_64": {"asset": name, "url": ..., "sha256": ..., "size": ..., "bundle_version": N,
                                  "rebuild_fingerprint": ..., "relink_fingerprint": ...}, ...}}

The fingerprints come from each archive's release.json: when rebuild_fingerprint differs from the installed one the game has to be
translated and compiled again, when only relink_fingerprint differs the new core is swapped in and relinked (fast), otherwise only
files are replaced. Python 3 standard library only.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import sys
import tarfile
import zipfile

WIZARD_VERSION = 1   # bump when an old wizard cannot apply a new release (the updater then asks to download the new wizard first)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_release_json(path):
    if path.endswith(".zip"):
        with zipfile.ZipFile(path) as z:
            name = next(n for n in z.namelist() if n.endswith("/release.json") and n.count("/") == 1)
            return json.loads(z.read(name))
    with tarfile.open(path) as t:
        name = next(n for n in t.getnames() if n.endswith("/release.json") and n.count("/") == 1)
        return json.loads(t.extractfile(name).read())


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--version", required=True)
    ap.add_argument("--repo", required=True)
    ap.add_argument("--assets", required=True)
    ap.add_argument("--tag")
    ap.add_argument("--base-url", help="download base instead of the GitHub release url (tests: file:///dir)")
    ap.add_argument("-o", "--output", default="manifest.json")
    a = ap.parse_args()
    tag = a.tag or "v" + a.version
    plats = {}
    for fn in sorted(os.listdir(a.assets)):
        m = re.fullmatch(r"psprecomp-(.+)-((?:linux|windows)-x86_64)\.(tar\.xz|zip)", fn)
        if not m:
            continue
        if m.group(1) != a.version:
            sys.exit("%s: version %s does not match --version %s" % (fn, m.group(1), a.version))
        path = os.path.join(a.assets, fn)
        rj = read_release_json(path)
        plats[m.group(2)] = {
            "asset": fn,
            "url": ((a.base_url.rstrip("/") + "/" + fn) if a.base_url else "https://github.com/%s/releases/download/%s/%s" % (a.repo, tag, fn)),
            "sha256": sha256(path),
            "size": os.path.getsize(path),
            "bundle_version": rj["bundle_version"],
            "rebuild_fingerprint": rj["rebuild_fingerprint"],
            "relink_fingerprint": rj["relink_fingerprint"],
        }
    if not plats:
        sys.exit("no release archives found in " + a.assets)
    manifest = {"schema": 1, "version": a.version, "min_wizard_version": WIZARD_VERSION,
                "published": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"), "platforms": plats}
    with open(a.output, "w") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
        f.write("\n")
    print("wrote %s: %s" % (a.output, ", ".join(sorted(plats))))


if __name__ == "__main__":
    main()
