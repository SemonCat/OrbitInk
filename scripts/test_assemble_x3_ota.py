"""Release assembly rejection/round-trip tests; uses throwaway keys and fake release fixtures."""

import argparse
import hashlib
import io
import json
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import assemble_x3_ota as a
import make_ota_manifest as m
import prepare_x3_ota_source as source
import release_x3_zh_tw as release
from test_make_ota_manifest import Keys, fake_firmware

HEAD = "a" * 40


class Assembly(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.key = Keys(self.root)
        self.fw = bytearray(fake_firmware())
        marker = f"CrossFade-HEAD:{HEAD}\0".encode()
        self.fw[10000:10000 + len(marker)] = marker
        self.fw = bytes(self.fw)
        self.tar = self.root / "release.tar.gz"
        release.package_firmware(self.tar, self.fw)
        self.record = self.root / "release.json"
        self.generated_inputs = {
            "build-inputs/project/platformio.ini": b"effective config\n",
            "project/src/main.cpp": b"application source fixture\n",
            "freeink-sdk/libs/driver.cpp": b"SDK source fixture\n",
        }
        self.source_inputs = {
            "schema": 1, "files": {name: a.sha(data) for name, data in self.generated_inputs.items()},
            "sdk_artifacts_sha256": "fixture",
        }
        self.record.write_text(json.dumps({
            "commit": HEAD, "artifact": str(self.tar), "sdk_key": "sdk",
            "source_inputs": self.source_inputs,
            "tar_sha256": a.sha(self.tar.read_bytes()), "firmware_sha256": a.sha(self.fw)}))
        fw = self.root / "firmware.bin"
        fw.write_bytes(self.fw)
        self.manifest = self.root / "manifest.json"
        self.assertEqual(m.main(["sign", "--firmware", str(fw), "--key", str(self.key.priv),
                                 "--tag", "x3-b2", "--out", str(self.manifest),
                                 "--pubkey-header", str(self.key.header)]), 0)
        self.src = self.root / "source.tar.gz"
        self.write_source(HEAD, "sdk")
        self.text = self.root / "notice.txt"
        self.text.write_text("fixture notice @HEAD@ @BUILD@ @SHA256@")
        self.out = self.root / "delivery.zip"
        self.args = argparse.Namespace(
            head=HEAD, release_record=self.record, manifest=self.manifest, source=self.src,
            readme=self.text, implementation=self.text, out=self.out)
        self.patches = [patch.object(m, "PUBKEY_HEADER", self.key.header)]
        for p in self.patches:
            p.start()

    def tearDown(self):
        for p in self.patches:
            p.stop()
        self.temp.cleanup()

    def write_source(self, head, sdk, omit=None, extra=None):
        data = json.dumps({"commit": head, "sdk_key": sdk, "firmware_sha256": a.sha(self.fw),
                           "source_inputs_sha256": a.digest(self.source_inputs)}).encode()
        members = {"SOURCE-PROVENANCE.json": data}
        members.update({name: data for name, data in self.generated_inputs.items() if name != omit})
        members.update({name: f"actual fixture notice: {name}\n".encode()
                        for name in a.REQUIRED_NOTICES if name != omit})
        members.update(extra or {})
        with tarfile.open(self.src, "w:gz") as tar:
            for name, content in sorted(members.items()):
                info = tarfile.TarInfo(name)
                info.size = len(content)
                tar.addfile(info, io.BytesIO(content))

    def test_round_trip_checksums_and_no_overwrite(self):
        self.write_source(HEAD, "sdk", extra={"project/lib/fonts/Ubuntu/UFL.txt": b"actual UFL fixture"})
        a.assemble(self.args)
        with zipfile.ZipFile(self.out) as z:
            self.assertEqual(z.read("firmware.bin"), self.fw)
            for line in z.read("SHA256SUMS").decode().splitlines():
                digest, name = line.split("  ", 1)
                self.assertEqual(hashlib.sha256(z.read(name)).hexdigest(), digest)
            self.assertFalse(any(name.endswith(".cpfont") or name.startswith("sd-assets/")
                                 or "FONT-LICENSE" in name for name in z.namelist()))
            for name in a.REQUIRED_NOTICES:
                self.assertEqual(z.read("licenses/" + name), f"actual fixture notice: {name}\n".encode())
            self.assertNotIn(b"@HEAD@", z.read("README-zh-TW.md"))
            self.assertEqual(z.read("licenses/project/lib/fonts/Ubuntu/UFL.txt"), b"actual UFL fixture")
            self.assertIn("licenses/project/LICENSES/COPYING.GPL3", z.namelist())
        with self.assertRaises(FileExistsError):
            a.assemble(self.args)

    def test_old_or_wrong_head_never_packaged(self):
        for head in (a.BASE, "b" * 40):
            with self.assertRaises(ValueError):
                a.load_firmware(self.record, head)

    def test_rejects_corrupt_tar(self):
        self.tar.write_bytes(b"wrong tar")
        with self.assertRaises(ValueError):
            a.assemble(self.args)
        self.assertFalse(self.out.exists())

    def test_rejects_other_source(self):
        self.write_source(HEAD, "old-sdk")
        with self.assertRaises(ValueError):
            a.assemble(self.args)
        self.assertFalse(self.out.exists())

    def test_rejects_tampered_manifest(self):
        manifest = json.loads(self.manifest.read_text())
        manifest["build"] += 1
        self.manifest.write_text(json.dumps(manifest))
        with self.assertRaises(m.ManifestError):
            a.assemble(self.args)
        self.assertFalse(self.out.exists())

    def test_rejects_missing_or_empty_binary_notices(self):
        for name in sorted(a.REQUIRED_NOTICES):
            for extra in ({}, {name: b""}):
                with self.subTest(name=name, empty=bool(extra)):
                    self.write_source(HEAD, "sdk", omit=name, extra=extra)
                    with self.assertRaises(ValueError):
                        a.assemble(self.args)
                    self.assertFalse(self.out.exists())

    def test_preserves_additional_dependency_notices(self):
        extra = {"dependencies/other/LICENSE.txt": b"dependency specific terms\n",
                 "project/lib/builtin/OFL.txt": b"actual built-in font copyright and terms\n"}
        self.write_source(HEAD, "sdk", extra=extra)
        a.assemble(self.args)
        with zipfile.ZipFile(self.out) as z:
            for name, data in extra.items():
                self.assertEqual(z.read("licenses/" + name), data)

    def test_rejects_unsafe_notice_path(self):
        self.write_source(HEAD, "sdk", extra={"../NOTICE": b"bad path"})
        with self.assertRaisesRegex(ValueError, "unsafe source archive path"):
            a.assemble(self.args)
        self.assertFalse(self.out.exists())

    def test_rejects_changed_or_missing_build_source(self):
        name = next(iter(self.generated_inputs))
        for extra in ({}, {name: b"changed after guarded build"}):
            self.write_source(HEAD, "sdk", omit=name, extra=extra)
            with self.assertRaises(ValueError):
                a.assemble(self.args)
            self.assertFalse(self.out.exists())

    def test_rejects_missing_or_replaced_application_and_sdk_sources(self):
        for name in ("project/src/main.cpp", "freeink-sdk/libs/driver.cpp"):
            for extra in ({}, {name: b"other version source"}):
                with self.subTest(name=name, tampered=bool(extra)):
                    self.write_source(HEAD, "sdk", omit=name, extra=extra)
                    with self.assertRaises(ValueError):
                        a.assemble(self.args)
                    self.assertFalse(self.out.exists())


class SourceSelection(unittest.TestCase):
    def test_excludes_git_build_binaries_and_does_not_follow_links(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name in ("COPYING", "src/file.c", ".git/config", "build/private.c", "lib.a", "firmware.bin"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("fixture")
            (root / "link").symlink_to(root / ".git", target_is_directory=True)
            self.assertEqual([name for name, _ in source.source_files(root)], ["COPYING", "src/file.c"])


if __name__ == "__main__":
    unittest.main()
