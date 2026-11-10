#!/usr/bin/env python3
"""Release notes: install instructions + the change log (commit subjects since the previous v* tag).

  make_notes.py --tag vX.Y.Z [-o notes.md]     (run inside the git checkout; needs the tags fetched)
"""
import argparse
import re
import subprocess
import sys

INSTALL = """## Install
- **Windows:** download `DissidiaRecomp-Setup-{v}-windows-x86_64.exe` and run it. It unpacks itself on the first start.
- **Linux:** download `DissidiaRecomp-Setup-{v}-linux-x86_64`, `chmod +x` it and run it.
- The `.tar.xz` / `.zip` archives hold the same files if you prefer to unpack them yourself.
- You need your own copy of the game (ISO or CSO). The wizard writes `setup.log` into the install folder; attach it to bug reports.
"""


def git(*a):
    return subprocess.run(["git", *a], capture_output=True, text=True).stdout.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True)
    ap.add_argument("-o", "--output", default="-")
    a = ap.parse_args()
    tags = [t for t in git("tag", "--list", "v*", "--sort=version:refname").split() if re.fullmatch(r"v\d+(\.\d+)*", t)]
    prev = None
    if a.tag in tags and tags.index(a.tag) > 0:
        prev = tags[tags.index(a.tag) - 1]
    rng = "%s..%s" % (prev, a.tag) if prev else a.tag
    subjects = [s for s in git("log", "--no-merges", "--pretty=%s", rng).splitlines() if s.strip()]
    out = INSTALL.format(v=a.tag.lstrip("v")) + "\n## Changes" + (" since %s" % prev if prev else "") + "\n"
    out += "\n".join("- " + s for s in subjects) if subjects else "- (no changes recorded)"
    out += "\n"
    if a.output == "-":
        sys.stdout.write(out)
    else:
        open(a.output, "w").write(out)


if __name__ == "__main__":
    main()
