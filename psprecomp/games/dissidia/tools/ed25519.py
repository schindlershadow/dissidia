#!/usr/bin/env python3
"""Ed25519 signatures (RFC 8032), pure Python 3 standard library, for signing and checking release manifests.

Not constant-time and slow (tens of milliseconds per operation), which is fine for one 2 KB manifest and no secret ever
leaves the release machine. The code follows the RFC's reference implementation.

  ed25519.py keygen                      -> prints "seed <64 hex>" and "public <64 hex>"
  ed25519.py sign SEED_HEX FILE          -> writes the 64-byte signature as hex to stdout
  ed25519.py verify PUBLIC_HEX FILE SIG_HEX_FILE   -> exit 0 when valid
"""
import hashlib
import os
import sys

P = 2 ** 255 - 19
L = 2 ** 252 + 27742317777372353535851937790883648493
D = -121665 * pow(121666, P - 2, P) % P
I = pow(2, (P - 1) // 4, P)


def _sha512(m):
    return hashlib.sha512(m).digest()


def _inv(x):
    return pow(x, P - 2, P)


def _recover_x(y, sign):
    if y >= P:
        return None
    x2 = (y * y - 1) * _inv(D * y * y + 1) % P
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P != 0:
        x = x * I % P
    if (x * x - x2) % P != 0:
        return None
    if (x & 1) != sign:
        x = P - x
    return x


_GY = 4 * _inv(5) % P
_GX = _recover_x(_GY, 0)
G = (_GX, _GY, 1, _GX * _GY % P)   # extended coordinates (X, Y, Z, T)
ZERO = (0, 1, 1, 0)


def _add(p, q):
    a = (p[1] - p[0]) * (q[1] - q[0]) % P
    b = (p[1] + p[0]) * (q[1] + q[0]) % P
    c = 2 * p[3] * q[3] * D % P
    d = 2 * p[2] * q[2] % P
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % P, g * h % P, f * g % P, e * h % P)


def _mul(s, p):
    q = ZERO
    while s > 0:
        if s & 1:
            q = _add(q, p)
        p = _add(p, p)
        s >>= 1
    return q


def _equal(p, q):
    return (p[0] * q[2] - q[0] * p[2]) % P == 0 and (p[1] * q[2] - q[1] * p[2]) % P == 0


def _compress(p):
    zi = _inv(p[2])
    x, y = p[0] * zi % P, p[1] * zi % P
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def _decompress(b):
    if len(b) != 32:
        return None
    y = int.from_bytes(b, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    if x is None:
        return None
    return (x, y, 1, x * y % P)


def _expand(seed):
    h = _sha512(seed)
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def public_key(seed):
    a, _ = _expand(seed)
    return _compress(_mul(a, G))


def sign(seed, msg):
    a, prefix = _expand(seed)
    pub = _compress(_mul(a, G))
    r = int.from_bytes(_sha512(prefix + msg), "little") % L
    rs = _compress(_mul(r, G))
    k = int.from_bytes(_sha512(rs + pub + msg), "little") % L
    s = (r + k * a) % L
    return rs + int.to_bytes(s, 32, "little")


def verify(pub, msg, sig):
    if len(pub) != 32 or len(sig) != 64:
        return False
    A = _decompress(pub)
    R = _decompress(sig[:32])
    if A is None or R is None:
        return False
    s = int.from_bytes(sig[32:], "little")
    if s >= L:
        return False
    k = int.from_bytes(_sha512(sig[:32] + pub + msg), "little") % L
    return _equal(_mul(s, G), _add(R, _mul(k, A)))


def _main(argv):
    if len(argv) >= 2 and argv[1] == "keygen":
        seed = os.urandom(32)
        print("seed", seed.hex())
        print("public", public_key(seed).hex())
        return 0
    if len(argv) == 4 and argv[1] == "sign":
        sys.stdout.write(sign(bytes.fromhex(argv[2]), open(argv[3], "rb").read()).hex() + "\n")
        return 0
    if len(argv) == 5 and argv[1] == "verify":
        ok = verify(bytes.fromhex(argv[2]), open(argv[3], "rb").read(), bytes.fromhex(open(argv[4]).read().strip()))
        print("valid" if ok else "INVALID")
        return 0 if ok else 1
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(_main(sys.argv))
