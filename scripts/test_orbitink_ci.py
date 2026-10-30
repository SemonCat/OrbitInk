"""Channel CI safety and real throwaway-key assembly; never invokes GitHub or a release build."""

import json
import os
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import orbitink_ci as ci
import make_ota_manifest as manifest
import test_assemble_x3_ota as fixtures
from test_make_ota_manifest import Keys, fake_firmware

ENV = {"GITHUB_REPOSITORY": ci.REPOSITORY, "GITHUB_REF": "refs/heads/main",
       "GITHUB_EVENT_NAME": "workflow_dispatch", "GITHUB_ACTOR": "SemonCat",
       "GITHUB_SHA": fixtures.HEAD, "GITHUB_RUN_NUMBER": "8", "GITHUB_RUN_ATTEMPT": "1"}


class ChannelCi(unittest.TestCase):
    def test_global_sequence_and_bounds(self):
        self.assertGreater(ci.build_number(2, 1), ci.build_number(1, 99))
        self.assertGreater(ci.build_number(1, 2), ci.build_number(1, 1))
        self.assertLess(ci.build_number(9_999_980, 99), 1_000_000_000)
        for run, attempt in [(0, 1), (1, 0), (1, 100), (9_999_981, 1)]:
            with self.assertRaises(ValueError):
                ci.build_number(run, attempt)

    def test_event_boundary(self):
        ci.require_event(ENV)
        for key, value in [("GITHUB_REPOSITORY", "other/fork"), ("GITHUB_EVENT_NAME", "pull_request"),
                           ("GITHUB_REF", "refs/heads/topic"), ("GITHUB_EVENT_NAME", "push_target")]:
            with self.assertRaises(ValueError):
                ci.require_event({**ENV, key: value})

    def test_stable_requires_owner_deliberate_request_and_committed_version(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args = SimpleNamespace(channel="stable", version="1.3.3", confirm="RELEASE")
            with patch.object(ci, "ROOT", root), patch.object(ci.release, "read_crosspoint_version", return_value="1.3.3"):
                for env, version, confirm in [({**ENV, "GITHUB_ACTOR": "other"}, "1.3.3", "RELEASE"),
                                               (ENV, "1.3.4", "RELEASE"), (ENV, "1.3.3", "")]:
                    args.version, args.confirm = version, confirm
                    with patch.dict(os.environ, env, clear=True), self.assertRaises(ValueError):
                        ci.configure(args)
                    self.assertFalse((root / "platformio.local.ini").exists())
                args.version, args.confirm = "1.3.3", "RELEASE"
                with patch.dict(os.environ, {**ENV, "GITHUB_OUTPUT": str(root / "output")}, clear=True):
                    ci.configure(args)
                    self.assertIn("x3_ota_channel = bookorbit-x3", (root / "platformio.local.ini").read_text())
                    with self.assertRaises(FileExistsError):
                        ci.configure(args)

    def test_push_can_only_allocate_nightly(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args = SimpleNamespace(channel="stable", version="", confirm="")
            env = {**ENV, "GITHUB_EVENT_NAME": "push", "GITHUB_OUTPUT": str(root / "output")}
            with patch.object(ci, "ROOT", root), patch.dict(os.environ, env, clear=True), \
                    patch.object(ci.release, "read_crosspoint_version", return_value="1.3.3"):
                ci.configure(args)
            self.assertIn("x3_ota_channel = orbitink-x3-nightly", (root / "platformio.local.ini").read_text())

    def test_repeated_or_older_release_cannot_regress_channel(self):
        releases = [{"tagName": "orbitink-b1901"}, {"tagName": "nightly"}, {"tagName": "x3-ota-b1"}]
        ci.assert_new_build(1902, releases)
        for number in (1901, 1802):
            with self.assertRaises(ValueError):
                ci.assert_new_build(number, releases)

    def test_nightly_identity_and_uint32_build_are_not_relabelled(self):
        firmware = fake_firmware(build=4_000_000_001).replace(b"bookorbit-x3", b"orbitink-x3-nightly")
        identity = manifest.read_identity(firmware)
        self.assertEqual(identity["channel"], ci.CHANNELS["nightly"])
        self.assertEqual(identity["build"], 4_000_000_001)
        with self.assertRaises(manifest.ManifestError):
            manifest.read_identity(fake_firmware(build=4_294_967_296))

    def test_compiled_nightly_literal_matches_signer(self):
        root = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory() as temporary:
            cpp, binary = Path(temporary) / "identity.cpp", Path(temporary) / "identity"
            cpp.write_text('#define CROSSFADE_OTA_CHANNEL "orbitink-x3-nightly"\n'
                           '#define CROSSFADE_OTA_BUILD 1101\n#include "src/network/OtaChannel.h"\n'
                           '#include <cstdio>\nint main(){ puts(ota_channel::IDENTITY); }\n')
            subprocess.run(["c++", "-std=c++17", "-I", str(root), str(cpp), "-o", str(binary)], check=True)
            self.assertEqual(subprocess.check_output([str(binary)]).strip(),
                             b"CrossFade-OTA/orbitink-x3-nightly/x3-zh-TW/build=1101")

    def test_nightly_signature_covers_channel_and_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            key = Keys(root)
            firmware = fake_firmware(build=1101).replace(b"bookorbit-x3", b"orbitink-x3-nightly")
            (root / "firmware.bin").write_bytes(firmware)
            args = SimpleNamespace(firmware=str(root / "firmware.bin"), key=str(key.priv),
                                   tag="orbitink-b1101", repo=ci.LEGACY_REPOSITORY,
                                   out=str(root / "manifest.json"), pubkey_header=str(key.header))
            manifest.cmd_sign(args)
            signed = json.loads((root / "manifest.json").read_text())
            manifest.verify_manifest(signed, firmware, key.point)
            for field, value in [("channel", "bookorbit-x3"), ("build", 1102)]:
                with self.assertRaises(manifest.ManifestError):
                    manifest.verify_manifest({**signed, field: value}, firmware, key.point)

    def test_signing_job_rechecks_guarded_artifact_and_source_and_omits_fonts(self):
        fixture = fixtures.Assembly()
        fixture.setUp()
        try:
            directory = fixture.root / "ci"
            directory.mkdir()
            (directory / "guarded.tar.gz").write_bytes(fixture.tar.read_bytes())
            record = json.loads(fixture.record.read_text())
            record["artifact"] = "guarded.tar.gz"
            (directory / "record.json").write_text(json.dumps(record))
            (directory / "firmware.bin").write_bytes(fixture.fw)
            (directory / "corresponding-source.tar.gz").write_bytes(fixture.src.read_bytes())
            docs = fixture.root / "docs"
            (docs / "contributing").mkdir(parents=True)
            (docs / "OrbitInk-README-zh-TW.md").write_text("@HEAD@ @BUILD@ @SHA256@")
            (docs / "contributing/orbitink-channels.md").write_text("fixture documentation")
            args = SimpleNamespace(directory=directory, channel="stable", build=2)
            env = {**ENV, "OTA_SIGNING_KEY_PEM": fixture.key.priv.read_text()}
            with patch.dict(os.environ, env, clear=True), patch.object(ci, "ROOT", fixture.root):
                ci.package(args)
                self.assertNotIn("OTA_SIGNING_KEY_PEM", os.environ)
            signed = json.loads((directory / "manifest.json").read_text())
            manifest.verify_manifest(signed, fixture.fw, fixture.key.point)
            self.assertTrue(signed["url"].startswith("https://github.com/SemonCat/BookOrbit-X3/"))
            with zipfile.ZipFile(directory / "orbitink.zip") as archive:
                self.assertFalse(any(name.endswith(".cpfont") for name in archive.namelist()))
                self.assertIn("licenses/dependencies/Arduino-wolfSSL/COPYING", archive.namelist())
            self.assertNotIn("@HEAD@", (directory / "README-zh-TW.md").read_text())
            (directory / "firmware.bin").write_bytes(b"tampered")
            with patch.dict(os.environ, ENV, clear=True), self.assertRaisesRegex(ValueError, "differs"):
                ci.package(args)
        finally:
            fixture.tearDown()

    def test_publish_contract_uses_prerelease_and_legacy_signed_url(self):
        calls = []
        def fake_gh(*args):
            calls.append(args)
            return "[]" if args[:2] == ("release", "list") else ""
        args = SimpleNamespace(directory=Path("/fixture"), channel="nightly", build=1101)
        with patch.dict(os.environ, ENV, clear=True), patch.object(ci, "gh", fake_gh):
            ci.publish(args)
        create = next(call for call in calls if call[:3] == ("release", "create", "orbitink-b1101"))
        self.assertIn("--prerelease", create)
        self.assertIn("--latest=false", create)
        self.assertTrue(any(call[:3] == ("release", "upload", "nightly") for call in calls))
        with patch.dict(os.environ, {**ENV, "GITHUB_ACTOR": "other"}, clear=True), self.assertRaises(ValueError):
            ci.publish(SimpleNamespace(channel="stable"))


if __name__ == "__main__":
    unittest.main()
