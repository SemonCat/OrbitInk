#!/usr/bin/env python3
"""Tests for scripts/make_ota_manifest.py. Uses throwaway keys in a temp dir; never the release key.

    python3 -m unittest scripts/test_make_ota_manifest.py
"""

import hashlib
import json
import os
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_ota_manifest as m  # noqa: E402

REPO = "SemonCat/BookOrbit-X3"


def fake_firmware(build=2, version="1.3.3", chip=0x0005, identities=1, size=200_000):
    img = bytearray(size)
    img[0] = 0xE9
    img[12:14] = struct.pack("<H", chip)
    pos = 4096
    for _ in range(identities):
        lit = f"CrossFade-OTA/bookorbit-x3/x3-zh-TW/build={build}\x00".encode()
        img[pos:pos + len(lit)] = lit
        pos += 256
    ua = f"CrossFade-ESP32-{version}\x00".encode()
    img[pos:pos + len(ua)] = ua
    return bytes(img)


class Keys:
    def __init__(self, tmp: Path):
        self.priv = tmp / "k.pem"
        subprocess.run(["openssl", "genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256",
                        "-out", str(self.priv)], check=True, capture_output=True)
        self.point = m.public_point(self.priv, private=True)
        self.header = tmp / "OtaPublicKey.h"
        self.header.write_text(m.pubkey_header(self.point))


class PureLogic(unittest.TestCase):
    def test_identity_read_from_firmware(self):
        ident = m.read_identity(fake_firmware(build=7))
        self.assertEqual(ident, {"channel": "bookorbit-x3", "target": "x3-zh-TW", "version": "1.3.3", "build": 7})

    def test_identity_rejections(self):
        with self.assertRaises(m.ManifestError):
            m.read_identity(fake_firmware(identities=0))
        # The same literal twice is still one identity (the linker may duplicate a string).
        self.assertEqual(m.read_identity(fake_firmware(identities=2, build=3))["build"], 3)
        with self.assertRaises(m.ManifestError):
            m.read_identity(fake_firmware(chip=0x0009))
        with self.assertRaises(m.ManifestError):
            m.read_identity(fake_firmware(build=0))
        with self.assertRaises(m.ManifestError):
            m.read_identity(b"\x00" + fake_firmware()[1:])
        with self.assertRaises(m.ManifestError):
            m.read_identity(fake_firmware(size=1000))

    def test_conflicting_identities_rejected(self):
        img = bytearray(fake_firmware(build=2))
        other = b"CrossFade-OTA/bookorbit-x3/x3-zh-TW/build=3\x00"
        img[9000:9000 + len(other)] = other
        with self.assertRaises(m.ManifestError):
            m.read_identity(bytes(img))

    def test_other_target_or_channel_rejected(self):
        for original, replacement in ((b"bookorbit-x3", b"stock-firmware"), (b"x3-zh-TW", b"x4-zh-TW")):
            with self.assertRaises(m.ManifestError):
                m.read_identity(fake_firmware().replace(original, replacement))

    def test_canonical_payload_exact_bytes(self):
        man = {"channel": "bookorbit-x3", "target": "x3-zh-TW", "version": "1.3.3", "build": 2, "size": 123456,
               "sha256": hashlib.sha256(b"fixture").hexdigest(),
               "url": "https://github.com/SemonCat/BookOrbit-X3/releases/download/x3-b2/firmware.bin"}
        # Same bytes test/firmware_update/OtaManifestTest.cpp CanonicalPayloadMatchesGenerator expects.
        self.assertEqual(m.canonical_payload(man), (
            "crossfade-ota-v1\nchannel=bookorbit-x3\ntarget=x3-zh-TW\nversion=1.3.3\nbuild=2\nsize=123456\n"
            "sha256=f16d05ec6b29248d2c61adb1e9263f78e4f7bace1b955014a2d17872cfe4064d\n"
            "url=https://github.com/SemonCat/BookOrbit-X3/releases/download/x3-b2/firmware.bin\n").encode())
        with self.assertRaises(m.ManifestError):
            m.canonical_payload({**man, "version": "1.3.3\nbuild=99"})

    def test_release_url_policy(self):
        self.assertEqual(m.release_url(REPO, "x3-b2"),
                         "https://github.com/SemonCat/BookOrbit-X3/releases/download/x3-b2/firmware.bin")
        for tag in ("..", "a/b", "x?y", ""):
            with self.assertRaises(m.ManifestError):
                m.release_url(REPO, tag)

    def test_der_raw_round_trip(self):
        for raw in (bytes(range(1, 65)), b"\x80" + bytes(31) + b"\x01" + bytes(30) + b"\x7f"):
            self.assertEqual(m.der_to_raw(m.raw_to_der(raw)), raw)
        with self.assertRaises(m.ManifestError):
            m.der_to_raw(b"\x30\x00")
        with self.assertRaises(m.ManifestError):
            m.der_to_raw(m.raw_to_der(bytes(64)))  # r = s = 0 out of range


class SignVerify(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.keys = Keys(self.dir)
        self.fw = self.dir / "firmware.bin"
        self.fw.write_bytes(fake_firmware(build=2))
        self.out = self.dir / "manifest.json"

    def tearDown(self):
        self.tmp.cleanup()

    def sign(self, key=None, header=None):
        return m.main(["sign", "--firmware", str(self.fw), "--key", str(key or self.keys.priv), "--tag", "x3-b2",
                       "--out", str(self.out), "--pubkey-header", str(header or self.keys.header)])

    def verify(self):
        return m.main(["verify", "--firmware", str(self.fw), "--manifest", str(self.out),
                       "--pubkey-header", str(self.keys.header)])

    def test_sign_then_verify(self):
        self.assertEqual(self.sign(), 0)
        man = json.loads(self.out.read_text())
        self.assertEqual(man["build"], 2)
        self.assertEqual(man["size"], self.fw.stat().st_size)
        self.assertEqual(man["sha256"], hashlib.sha256(self.fw.read_bytes()).hexdigest())
        self.assertEqual(len(man["sig"]), 128)
        self.assertLessEqual(len(self.out.read_bytes()), m.MAX_MANIFEST_BYTES)
        self.assertEqual(self.verify(), 0)

    def test_signing_key_must_match_committed_public_key(self):
        (self.dir / "other").mkdir()
        other = Keys(self.dir / "other")
        self.assertEqual(self.sign(key=other.priv), 1)
        self.assertFalse(self.out.exists())

    def test_tampered_manifest_rejected(self):
        self.assertEqual(self.sign(), 0)
        man = json.loads(self.out.read_text())
        for key, value in (("build", 3), ("size", man["size"] + 1), ("version", "1.3.4")):
            self.out.write_text(json.dumps({**man, key: value}))
            self.assertEqual(self.verify(), 1, key)
        sig = man["sig"]
        self.out.write_text(json.dumps({**man, "sig": ("0" if sig[0] != "0" else "1") + sig[1:]}))
        self.assertEqual(self.verify(), 1)

    def test_manifest_for_other_firmware_rejected(self):
        self.assertEqual(self.sign(), 0)
        self.fw.write_bytes(fake_firmware(build=2, size=210_000))
        self.assertEqual(self.verify(), 1)

    def test_generate_key_is_private_and_never_overwritten(self):
        path = self.dir / "keys" / "release.pem"
        self.assertEqual(m.main(["generate-key", str(path)]), 0)
        self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(path.parent.stat().st_mode), 0o700)
        before = path.read_bytes()
        self.assertEqual(m.main(["generate-key", str(path)]), 1)
        self.assertEqual(path.read_bytes(), before)


class CommittedKey(unittest.TestCase):
    def test_committed_header_parses(self):
        point = m.header_point(m.PUBKEY_HEADER.read_text())
        self.assertEqual(len(point), 65)
        self.assertEqual(point[0], 0x04)
        self.assertIn(m.pubkey_fingerprint(point), m.PUBKEY_HEADER.read_text())


if __name__ == "__main__":
    unittest.main()
