#!/usr/bin/env python3
"""Make the single-file release: the wizard program with the whole release folder appended (setup/payload.h).

  make_single.py --wizard FILE --release-dir DIR --version V -o OUT

OUT = wizard bytes + zip of DIR's contents (paths relative to DIR, unix modes and symlinks kept) + 48-byte trailer
(version[32], zip size u64 LE, "PSPRPAY1"). The wizard unpacks the zip into a per-user folder on its first start and runs from
there, so the player downloads and starts one file. DIR must contain the plain wizard (psprecomp_setup[.exe]) too.
"""
import argparse
import os
import stat
import struct
import sys
import zipfile


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--wizard", required=True)
    ap.add_argument("--release-dir", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("-o", "--output", required=True)
    a = ap.parse_args()
    ver = a.version.encode()
    if len(ver) > 32:
        sys.exit("version too long")
    tmp = a.output + ".zip.tmp"
    root = os.path.abspath(a.release_dir)
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for dp, dns, fns in os.walk(root):
            dns.sort()
            for name in sorted(dns) + sorted(fns):
                p = os.path.join(dp, name)
                rel = os.path.relpath(p, root).replace("\\", "/")
                if os.path.islink(p):
                    zi = zipfile.ZipInfo(rel)
                    zi.create_system = 3
                    zi.external_attr = (stat.S_IFLNK | 0o777) << 16
                    z.writestr(zi, os.readlink(p))
                elif os.path.isfile(p):
                    z.write(p, rel)
    size = os.path.getsize(tmp)
    with open(a.output, "wb") as out:
        for src in (a.wizard, tmp):
            with open(src, "rb") as f:
                while True:
                    b = f.read(1 << 20)
                    if not b:
                        break
                    out.write(b)
        out.write(ver.ljust(32, b"\0") + struct.pack("<Q", size) + b"PSPRPAY1")
    os.remove(tmp)
    os.chmod(a.output, 0o755)
    print("single-file release %s: %.0f MB (payload %.0f MB)" % (a.output, os.path.getsize(a.output) / 1e6, size / 1e6))


if __name__ == "__main__":
    main()
