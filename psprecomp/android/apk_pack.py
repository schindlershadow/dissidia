#!/usr/bin/env python3
"""Put libraries into an APK skeleton and sign it (APK Signature Scheme v2), in pure Python 3 (standard library only): no Java, aapt2,
zipalign or apksigner on the player's machine.

  apk_pack.py --skeleton skeleton.apk --lib NAME=PATH [--lib ...] [--key debug_key.json] -o out.apk

--lib lib/arm64-v8a/libmain.so=/path/to/libmain.so adds that file under that name. The skeleton is an unsigned, zip-aligned APK holding the
manifest, the compiled resources and classes.dex (android/make_skeleton.py builds it at release time); entries are appended after it so its
alignment is kept. The signing key (debug_key.json: RSA 2048 numbers + the self-signed certificate) is a debug key that is public on purpose:
it only identifies APKs built on this machine from the player's own disc.

Format reference: https://source.android.com/docs/security/features/apksigning/v2
"""
import argparse
import base64
import hashlib
import json
import os
import shutil
import struct
import sys
import zipfile

SIG_ALGO_RSA_PKCS1_SHA256 = 0x0103
V2_BLOCK_ID = 0x7109871A
CHUNK = 1024 * 1024

# DER prefix of DigestInfo for SHA-256 (RFC 8017 9.2)
_SHA256_PREFIX = bytes.fromhex("3031300d060960864801650304020105000420")


def _u32(v):
    return struct.pack("<I", v)


def _u64(v):
    return struct.pack("<Q", v)


def _lp(b):
    """length-prefixed (uint32) byte string"""
    return _u32(len(b)) + b


def rsa_sign_pkcs1_sha256(n, d, message):
    k = (n.bit_length() + 7) // 8
    t = _SHA256_PREFIX + hashlib.sha256(message).digest()
    em = b"\x00\x01" + b"\xff" * (k - len(t) - 3) + b"\x00" + t
    return pow(int.from_bytes(em, "big"), d, n).to_bytes(k, "big")


def _chunk_digests(data, out):
    for off in range(0, len(data), CHUNK):
        c = data[off:off + CHUNK]
        out.append(hashlib.sha256(b"\xa5" + _u32(len(c)) + c).digest())


def content_digest(entries, cd, eocd):
    chunks = []
    for sec in (entries, cd, eocd):
        _chunk_digests(sec, chunks)
    return hashlib.sha256(b"\x5a" + _u32(len(chunks)) + b"".join(chunks)).digest()


def find_eocd(data):
    i = data.rfind(b"PK\x05\x06")
    if i < 0 or len(data) - i != 22:   # no archive comment
        raise ValueError("no plain end-of-central-directory record")
    return i


def sign_apk_bytes(data, key):
    eocd_at = find_eocd(data)
    cd_size, cd_off = struct.unpack_from("<II", data, eocd_at + 12)
    entries, cd = data[:cd_off], data[cd_off:cd_off + cd_size]
    eocd = bytearray(data[eocd_at:])
    struct.pack_into("<I", eocd, 16, cd_off)   # digest is computed with the CD offset = start of the signing block (= old CD offset)
    digest = content_digest(entries, cd, bytes(eocd))
    cert = base64.b64decode(key["cert_der_b64"])
    spki = base64.b64decode(key["spki_der_b64"])
    n, d = int(key["n"], 16), int(key["d"], 16)
    digests = _lp(_u32(SIG_ALGO_RSA_PKCS1_SHA256) + _lp(digest))
    signed_data = _lp(digests) + _lp(_lp(cert)) + _lp(b"")   # digests, certificates, additional attributes
    signature = rsa_sign_pkcs1_sha256(n, d, signed_data)
    signer = _lp(signed_data) + _lp(_lp(_u32(SIG_ALGO_RSA_PKCS1_SHA256) + _lp(signature))) + _lp(spki)
    v2_value = _lp(_lp(signer))
    pair = _u64(4 + len(v2_value)) + _u32(V2_BLOCK_ID) + v2_value
    block_body = pair
    block = _u64(len(block_body) + 8 + 16) + block_body + _u64(len(block_body) + 8 + 16) + b"APK Sig Block 42"
    struct.pack_into("<I", eocd, 16, cd_off + len(block))
    return entries + block + cd + bytes(eocd)


def build(skeleton, libs, out, key):
    tmp = out + ".unsigned"
    shutil.copyfile(skeleton, tmp)
    with zipfile.ZipFile(tmp, "a", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for name, path in libs:
            z.write(path, name)
    data = open(tmp, "rb").read()
    os.remove(tmp)
    signed = sign_apk_bytes(data, key)
    with open(out, "wb") as f:
        f.write(signed)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--skeleton", required=True)
    ap.add_argument("--lib", action="append", default=[], metavar="NAME=PATH")
    ap.add_argument("--key", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "debug_key.json"))
    ap.add_argument("-o", "--output", required=True)
    a = ap.parse_args()
    libs = []
    for l in a.lib:
        name, _, path = l.partition("=")
        if not path or not os.path.isfile(path):
            sys.exit("--lib %s: file not found" % l)
        libs.append((name, path))
    build(a.skeleton, libs, a.output, json.load(open(a.key)))
    print("built %s (%.1f MB)" % (a.output, os.path.getsize(a.output) / 1e6))


if __name__ == "__main__":
    main()
