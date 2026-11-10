#!/usr/bin/env python3
"""Tests of the manifest signature check in update.py and of ed25519.py (run: python3 test_update_signature.py)."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ed25519  # noqa: E402
import update  # noqa: E402


class T(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.root = os.path.join(self.d, "rel")
        os.makedirs(os.path.join(self.root, "game"))
        self.seed = bytes(range(32))
        self.manifest = os.path.join(self.d, "manifest.json")
        with open(self.manifest, "w") as f:
            f.write('{"version": "9.9.9", "platforms": {}}')
        self.url = "file://" + self.manifest

    def key(self, seed=None):
        with open(os.path.join(self.root, "game", "update_key.pub"), "w") as f:
            f.write(ed25519.public_key(seed or self.seed).hex() + "\n")

    def sign(self, seed=None):
        sig = ed25519.sign(seed or self.seed, open(self.manifest, "rb").read())
        with open(self.manifest + ".sig", "w") as f:
            f.write(sig.hex() + "\n")

    def test_rfc8032_vector(self):
        seed = bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
        self.assertEqual(ed25519.public_key(seed).hex(), "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")
        self.assertEqual(ed25519.sign(seed, b"").hex(), "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b")

    def test_no_key_file_needs_no_signature(self):
        self.assertEqual(update.load_manifest(self.root, self.url)["version"], "9.9.9")

    def test_valid_signature(self):
        self.key(); self.sign()
        self.assertEqual(update.load_manifest(self.root, self.url)["version"], "9.9.9")

    def test_missing_signature_is_refused(self):
        self.key()
        with self.assertRaises(RuntimeError):
            update.load_manifest(self.root, self.url)

    def test_tampered_manifest_is_refused(self):
        self.key(); self.sign()
        with open(self.manifest, "w") as f:
            f.write('{"version": "9.9.10", "platforms": {}}')
        with self.assertRaises(RuntimeError):
            update.load_manifest(self.root, self.url)

    def test_other_key_is_refused(self):
        self.key(bytes(32)); self.sign()
        with self.assertRaises(RuntimeError):
            update.load_manifest(self.root, self.url)


if __name__ == "__main__":
    unittest.main()
