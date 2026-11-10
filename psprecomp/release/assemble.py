#!/usr/bin/env python3
"""Assemble a release folder (and its archive) from the built pieces.

  assemble.py --platform linux-x86_64|windows-x86_64 --version V --out DIR
              --wizard FILE --recompiler FILE --toolchain DIR --sdk DIR [--repo DIR] [--cache DIR] [--no-archive]

The result is what the wizard (setup/install.h "Layout") expects, flat:

  psprecomp_setup[.exe]   the wizard           bin/psprecomp[.exe]   the recompiler
  toolchain/              the portable compiler bundle (toolchain/build_bundle.py), trimmed
  sdk/                    prebuilt runtime core + headers + link project + toolchain.json
  game/                   games/dissidia: game.toml, overlay tomls, analysis/, features.ini, fx_shaders/, tools/*.py
  python/                 embedded Python (Windows only; Linux uses python3 from PATH)
  licenses/               third-party licenses
  release.json            version, platform, bundle identity, sha256 of every file (the updater diffs against it)

and writes <out>.tar.xz (Linux) or <out>.zip (Windows) next to it plus <archive>.sha256.
Nothing game-derived is in a release: the analysis metadata carries no game bytes (psp_build.py "hydrate" adds them from the
player's own files). Python 3 standard library only.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))


def die(msg):
    print("assemble: " + msg, file=sys.stderr)
    sys.exit(1)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def copy_tree(src, dst, skip=lambda rel: False):
    for dp, dns, fns in os.walk(src):
        rel_dir = os.path.relpath(dp, src)
        dns[:] = [d for d in dns if not skip(os.path.normpath(os.path.join(rel_dir, d)).replace("\\", "/"))]
        os.makedirs(os.path.join(dst, rel_dir), exist_ok=True)
        for fn in fns:
            rel = os.path.normpath(os.path.join(rel_dir, fn)).replace("\\", "/")
            if skip(rel):
                continue
            s = os.path.join(dp, fn)
            d = os.path.join(dst, rel)
            if os.path.islink(s):
                os.symlink(os.readlink(s), d)
            else:
                shutil.copy2(s, d)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--platform", required=True, choices=["linux-x86_64", "windows-x86_64"])
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--wizard", required=True)
    ap.add_argument("--recompiler", required=True)
    ap.add_argument("--toolchain", required=True)
    ap.add_argument("--sdk", required=True)
    ap.add_argument("--repo", default=REPO)
    ap.add_argument("--cache", default=None)
    ap.add_argument("--manifest-url", default=os.environ.get("PSPRECOMP_MANIFEST_URL") or
                    "https://github.com/%s/releases/latest/download/manifest.json" % (os.environ.get("GITHUB_REPOSITORY") or "schindlershadow/dissidia"))
    ap.add_argument("--no-archive", action="store_true")
    ap.add_argument("--android-part", default=None, help="the Android part of the release (release/build_android_sdk.py): shipped as android/ and switches the wizard's Android option on")
    a = ap.parse_args()
    win = a.platform.startswith("windows")
    exe = ".exe" if win else ""
    out = os.path.abspath(a.out)
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, "bin"))

    shutil.copy2(a.wizard, os.path.join(out, "psprecomp_setup" + exe))
    shutil.copy2(a.recompiler, os.path.join(out, "bin", "psprecomp" + exe))
    os.chmod(os.path.join(out, "psprecomp_setup" + exe), 0o755)
    os.chmod(os.path.join(out, "bin", "psprecomp" + exe), 0o755)

    # the bundle without what only developers read (cmake's help/manuals, zig's docs)
    def tc_skip(rel):
        parts = rel.split("/")
        if rel.startswith(("cmake/doc", "cmake/man", "zig/doc")) or rel.endswith(".pdb"):
            return True
        return len(parts) >= 4 and parts[0] == "cmake" and parts[1] == "share" and parts[2].startswith("cmake-") and parts[3] == "Help"
    copy_tree(a.toolchain, os.path.join(out, "toolchain"), tc_skip)
    tj = os.path.join(out, "toolchain", "toolchain.json")
    if not os.path.isfile(tj):
        die("%s is not a toolchain bundle (no toolchain.json)" % a.toolchain)
    meta = json.load(open(tj))
    if meta.get("platform") != a.platform:
        die("the toolchain is for %s, not %s" % (meta.get("platform"), a.platform))

    copy_tree(a.sdk, os.path.join(out, "sdk"))
    sdk_id = os.path.join(out, "sdk", "toolchain.json")
    if not os.path.isfile(sdk_id):
        die("the SDK has no toolchain.json: build the core with the bundle's toolchain.cmake")
    sdk_meta = json.load(open(sdk_id))
    for k in ("bundle_version", "platform", "zig_target", "zig"):
        if sdk_meta.get(k) != meta.get(k):
            die("the SDK was built with another toolchain bundle (%s differs: %s vs %s)" % (k, sdk_meta.get(k), meta.get(k)))

    game_src = os.path.join(a.repo, "games", "dissidia")

    def game_keep(rel):
        top = rel.split("/")[0]
        if top in ("analysis", "fx_shaders"):
            return True
        if rel in ("game.toml", "features.ini", "package_paths.txt", "README.md") or (rel.startswith("overlay_") and rel.endswith(".toml")):
            return True
        if top == "tools" and rel.count("/") == 1 and rel.endswith((".py", ".json", ".txt")) and not os.path.basename(rel).startswith("test_"):
            return True
        return False
    copy_tree(game_src, os.path.join(out, "game"), lambda rel: False)
    # (copy_tree cannot filter by "keep"; prune afterwards)
    for dp, dns, fns in os.walk(os.path.join(out, "game"), topdown=False):
        for fn in fns:
            rel = os.path.relpath(os.path.join(dp, fn), os.path.join(out, "game")).replace("\\", "/")
            if not game_keep(rel):
                os.remove(os.path.join(dp, fn))
        for d in dns:
            p = os.path.join(dp, d)
            if os.path.isdir(p) and not os.listdir(p):
                os.rmdir(p)
    for need in ("analysis/analysis.json.gz", "game.toml", "tools/psp_build.py", "tools/setup_from_iso.py"):
        if not os.path.isfile(os.path.join(out, "game", need)):
            die("game/%s is missing from the release" % need)

    if win:
        ver = json.load(open(os.path.join(a.repo, "toolchain", "versions.json")))
        ent = ver["python"]["files"][a.platform]
        cache = a.cache or out + ".cache"
        os.makedirs(cache, exist_ok=True)
        zp = os.path.join(cache, ent["url"].rsplit("/", 1)[1])
        if not os.path.isfile(zp):
            req = urllib.request.Request(ent["url"], headers={"User-Agent": "psprecomp-release/1"})
            with urllib.request.urlopen(req, timeout=120) as r, open(zp, "wb") as f:
                shutil.copyfileobj(r, f)
        if sha256(zp) != ent["sha256"]:
            os.remove(zp)
            die("python embed sha256 mismatch")
        with zipfile.ZipFile(zp) as z:
            z.extractall(os.path.join(out, "python"))
        # the embedded interpreter ignores the script's directory: put the tools folder on its path
        pth = [f for f in os.listdir(os.path.join(out, "python")) if f.endswith("._pth")][0]
        stem = pth[: -len("._pth")]
        with open(os.path.join(out, "python", pth), "w") as f:
            f.write("%s.zip\n.\n..\\game\\tools\nimport site\n" % stem)

    if a.android_part:
        copy_tree(a.android_part, os.path.join(out, "android"))
        fi = os.path.join(out, "game", "features.ini")
        txt = open(fi).read() if os.path.isfile(fi) else ""
        lines = [l for l in txt.splitlines() if not l.startswith("android=")] + ["android=1"]
        open(fi, "w").write("\n".join(lines) + "\n")
        for tool in ("android_build.py", "android_install.py"):   # (game/tools already has them when the whitelist includes *.py)
            if not os.path.isfile(os.path.join(out, "game", "tools", tool)):
                shutil.copy2(os.path.join(a.repo, "games", "dissidia", "tools", tool), os.path.join(out, "game", "tools", tool))

    os.makedirs(os.path.join(out, "licenses"))
    for src, name in ((os.path.join(a.repo, "LICENSE"), "psprecomp-LICENSE.txt"),
                      (os.path.join(a.repo, "setup", "third_party", "LICENSE-pfd.txt"), "portable-file-dialogs-LICENSE.txt"),
                      (os.path.join(a.repo, "setup", "third_party", "LICENSE-miniz.txt"), "miniz-LICENSE.txt"),
                      (os.path.join(out, "toolchain", "bin", "pspdecrypt-LICENSE.txt"), "pspdecrypt-LICENSE.txt")):
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(out, "licenses", name))

    # license texts of the bundled libraries, generated notices (incl. the FFmpeg source offer), the update signing key
    tlic = os.path.join(out, "toolchain", "licenses")
    if os.path.isdir(tlic):
        for fn in os.listdir(tlic):
            shutil.copy2(os.path.join(tlic, fn), os.path.join(out, "licenses", fn))
    subprocess.check_call([sys.executable, os.path.join(a.repo, "release", "third_party_notices.py"),
                           "--versions", os.path.join(a.repo, "toolchain", "versions.json"),
                           "-o", os.path.join(out, "licenses", "THIRD-PARTY-NOTICES.txt")])
    keyfile = os.path.join(a.repo, "games", "dissidia", "update_key.pub")
    if os.path.isfile(keyfile):
        shutil.copy2(keyfile, os.path.join(out, "game", "update_key.pub"))

    files = {}
    for dp, dns, fns in os.walk(out):
        for fn in sorted(fns):
            p = os.path.join(dp, fn)
            rel = os.path.relpath(p, out).replace("\\", "/")
            if not os.path.islink(p):
                files[rel] = {"sha256": sha256(p), "size": os.path.getsize(p)}
    def fingerprint(select):
        h = hashlib.sha256()
        for rel in sorted(files):
            if select(rel):
                h.update(rel.encode() + b"\0" + files[rel]["sha256"].encode() + b"\0")
        return h.hexdigest()

    # What the updater compares (see docs/installer.md "Updater"): if the translation inputs changed the game has to be
    # regenerated and compiled again; if only the prebuilt core changed it just has to be relinked.
    rebuild_fp = fingerprint(lambda r: r.startswith(("bin/psprecomp", "game/analysis/", "game/overlay_", "game/tools/"))
                             or r in ("game/game.toml", "toolchain/toolchain.json"))
    relink_fp = fingerprint(lambda r: r.startswith("sdk/"))
    rj = {"version": a.version, "platform": a.platform, "bundle_version": meta["bundle_version"], "zig_target": meta["zig_target"],
          "rebuild_fingerprint": rebuild_fp, "relink_fingerprint": relink_fp, "manifest_url": a.manifest_url, "files": files}
    json.dump(rj, open(os.path.join(out, "release.json"), "w"), indent=1, sort_keys=True)
    print("assembled %s: %d files, version %s" % (out, len(files), a.version))

    if a.no_archive:
        return
    base = os.path.basename(out.rstrip("/\\"))
    arch = out + (".zip" if win else ".tar.xz")
    if win:
        with zipfile.ZipFile(arch, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for dp, dns, fns in os.walk(out):
                for fn in fns:
                    p = os.path.join(dp, fn)
                    z.write(p, os.path.join(base, os.path.relpath(p, out)))
    else:
        with tarfile.open(arch, "w:xz", preset=6) as t:
            t.add(out, arcname=base)
    open(arch + ".sha256", "w").write("%s  %s\n" % (sha256(arch), os.path.basename(arch)))
    print("archive %s (%.0f MB)" % (arch, os.path.getsize(arch) / 1e6))


if __name__ == "__main__":
    main()
