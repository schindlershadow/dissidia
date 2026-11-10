#!/usr/bin/env python3
"""Sign a release manifest with Ed25519 so installed copies can tell it is genuine (update.py checks <manifest url>.sig).

  sign_manifest.py keygen                         print a new key pair: keep the seed secret, commit the public key
  sign_manifest.py sign manifest.json             writes manifest.json.sig; the seed comes from $RELEASE_SIGNING_KEY (64 hex)

One-time setup (see docs/installer.md "Signed manifests"):
  1. python3 release/sign_manifest.py keygen
  2. put the "public" value in games/dissidia/update_key.pub (committed; releases ship it as game/update_key.pub)
  3. save the "seed" value as the GitHub Actions secret RELEASE_SIGNING_KEY
Releases built after that are signed by the workflow, and installs made from them refuse any manifest without a valid signature.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "games", "dissidia", "tools"))
import ed25519  # noqa: E402


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "keygen":
        seed = os.urandom(32)
        print("seed   ", seed.hex())
        print("public ", ed25519.public_key(seed).hex())
        return 0
    if len(sys.argv) == 3 and sys.argv[1] == "sign":
        seed_hex = os.environ.get("RELEASE_SIGNING_KEY", "").strip()
        if len(seed_hex) != 64:
            sys.exit("RELEASE_SIGNING_KEY must hold the 64 hex characters of the seed")
        seed = bytes.fromhex(seed_hex)
        repo = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
        keyfile = os.path.join(repo, "games", "dissidia", "update_key.pub")
        if not os.path.isfile(keyfile):
            sys.exit("games/dissidia/update_key.pub is missing: releases would ship without the key that checks this signature")
        if bytes.fromhex(open(keyfile).read().strip()) != ed25519.public_key(seed):
            sys.exit("RELEASE_SIGNING_KEY does not belong to games/dissidia/update_key.pub")
        body = open(sys.argv[2], "rb").read()
        sig = ed25519.sign(seed, body)
        assert ed25519.verify(ed25519.public_key(seed), body, sig)
        open(sys.argv[2] + ".sig", "w").write(sig.hex() + "\n")
        print("signed", sys.argv[2])
        return 0
    sys.exit(__doc__)


if __name__ == "__main__":
    sys.exit(main())
