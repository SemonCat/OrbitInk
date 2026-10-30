"""Exercise sealed source capture with tiny generated build fixtures, never a release build."""
import json
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import prepare_x3_ota_source as source
import release_x3_zh_tw as release
import x3_ota_source_inputs as inputs
from release_sdk import SdkCache, ENV, ini_text


class SourceCapture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for prefix in ("", "freeink-sdk/"):
            cwd = self.root / prefix
            cwd.mkdir(exist_ok=True)
            subprocess.run(["git", "init", "-q", str(cwd)], check=True)
            (cwd / "LICENSE").write_text("fixture license")
            (cwd / "NOTICE").write_text("fixture notice")
            (cwd / "platformio.ini").write_text("[platformio]\nextra_configs=platformio.local.ini\n")
            (cwd / "src").mkdir()
            (cwd / "src/main.cpp").write_text("fixture source")
            subprocess.run(["git", "add", "."], cwd=cwd, check=True)
            subprocess.run(["git", "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                            "commit", "-qm", "fixture"], cwd=cwd, check=True)
        self.head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=self.root, text=True).strip()
        self.options = {"extra_scripts": ["pre:scripts/local.py"], "board_build.partitions": "partitions.csv",
                        "build_flags": ["-DFROM_LOCAL_OVERRIDE=1"]}
        self.cache = SdkCache(self.root, None, {f"env:{ENV}": self.options})
        self.cache.project.mkdir(parents=True)
        for name in ("platformio.ini", "sdkconfig.defaults", f"sdkconfig.{ENV}", "partitions.csv"):
            (self.cache.project / name).write_text("fixture input\n")
        (self.cache.project / "platformio.ini").write_text(ini_text(self.options))
        (self.root / "scripts").mkdir()
        (self.root / "scripts/local.py").write_text("local_script_input = True\n")
        (self.cache.project / "scripts").symlink_to(self.root / "scripts", target_is_directory=True)
        self.paths = [
            self.cache.project / f".pio/libdeps/{ENV}/Arduino-wolfSSL/src/patched.c",
            self.cache.project / "managed_components/component/source.c",
        ]
        for package in ("framework-arduinoespressif32", "framework-arduinoespressif32-libs", "framework-espidf"):
            self.paths.append(self.cache.core / "packages" / package / "source.c")
        for path in self.paths:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("built source\n")
        self.cache.manifest_path.write_text(json.dumps({"key": "sdk", "artifacts": self.cache.artifacts()}))
        selection = patch.object(inputs, "PROJECT_PATHS", ["src", "platformio.ini", "LICENSE", "NOTICE"])
        selection.start()
        self.addCleanup(selection.stop)
        self.expected = inputs.capture(self.cache)
        self.record = self.root / "release.json"
        firmware = b"fixture firmware"
        artifact = self.root / "release.tar.gz"
        release.package_firmware(artifact, firmware)
        self.record.write_text(json.dumps({
            "commit": self.head, "sdk_key": "sdk", "artifact": str(artifact),
            "firmware_sha256": source.hashlib.sha256(firmware).hexdigest(),
            "source_inputs": self.expected,
        }))
        self.out = self.root / "source.tar.gz"
        snapshot = {"head": self.head, "checkout": "fixture-sdk"}
        for p in (patch.object(release, "release_snapshot", return_value=snapshot),
                  patch.object(release, "verify_snapshot"),
                  patch.object(source, "PROJECT_PATHS", ["src", "platformio.ini", "LICENSE", "NOTICE"])):
            p.start()
            self.addCleanup(p.stop)

    def prepare(self):
        source.prepare(self.root, self.record, self.head, self.out)

    def test_effective_local_config_and_referenced_script_are_archived(self):
        self.prepare()
        with tarfile.open(self.out) as tar:
            ini = tar.extractfile("build-inputs/project/platformio.ini").read()
            self.assertIn(b"-DFROM_LOCAL_OVERRIDE=1", ini)
            self.assertNotIn(b"extra_configs", ini)
            self.assertEqual(tar.extractfile("build-inputs/project/scripts/local.py").read(),
                             (self.root / "scripts/local.py").read_bytes())
            record = json.load(tar.extractfile("SOURCE-PROVENANCE.json"))
            self.assertEqual(record["source_inputs_sha256"], source.digest(self.expected))
            for name in ("project/src/main.cpp", "freeink-sdk/src/main.cpp"):
                data = tar.extractfile(name).read()
                self.assertEqual(source.hashlib.sha256(data).hexdigest(), self.expected["files"][name])

    def test_changed_dependency_managed_framework_and_effective_config_rejected(self):
        for path in [*self.paths, self.cache.project / "platformio.ini", self.root / "scripts/local.py"]:
            with self.subTest(path=path):
                before = path.read_bytes()
                path.write_bytes(before + b"changed")
                with self.assertRaisesRegex(ValueError, "differ"):
                    self.prepare()
                self.assertFalse(self.out.exists())
                path.write_bytes(before)

    def test_added_deleted_sources_rejected(self):
        path = self.paths[0]
        data = path.read_bytes()
        path.unlink()
        with self.assertRaises(ValueError):
            self.prepare()
        path.write_bytes(data)
        added = path.with_name("added.c")
        added.write_text("new input")
        with self.assertRaises(ValueError):
            self.prepare()
        self.assertFalse(self.out.exists())

    def test_binary_sdk_artifact_change_rejected_even_if_sources_same(self):
        archive = self.paths[-1].with_name("library.a")
        archive.write_bytes(b"changed compiled SDK")
        with self.assertRaisesRegex(ValueError, "differ"):
            self.prepare()
        self.assertFalse(self.out.exists())

    def test_old_release_without_inventory_rejected(self):
        record = json.loads(self.record.read_text())
        del record["source_inputs"]
        self.record.write_text(json.dumps(record))
        with self.assertRaisesRegex(ValueError, "differ"):
            self.prepare()

    def test_failed_capture_cleans_partial_and_existing_archive_is_preserved(self):
        with patch.object(source.subprocess, "check_output", side_effect=OSError("fixture I/O failure")):
            with self.assertRaises(OSError):
                self.prepare()
        self.assertFalse(self.out.exists())
        self.out.write_bytes(b"previous accepted archive")
        with self.assertRaises(FileExistsError):
            self.prepare()
        self.assertEqual(self.out.read_bytes(), b"previous accepted archive")

    def test_source_change_during_archive_is_rejected_and_cleaned(self):
        original = source.subprocess.check_output
        def mutate(*args, **kwargs):
            result = original(*args, **kwargs)
            self.paths[0].write_text("changed during capture")
            return result
        with patch.object(source.subprocess, "check_output", side_effect=mutate):
            with self.assertRaisesRegex(ValueError, "changed during capture"):
                self.prepare()
        self.assertFalse(self.out.exists())

    def test_external_referenced_script_is_not_silently_distributed(self):
        self.cache.config[f"env:{ENV}"]["extra_scripts"] = ["pre:/outside/private.py"]
        with self.assertRaisesRegex(ValueError, "unsupported source reference"):
            inputs.capture(self.cache)

    def test_guarded_build_records_source_inventory_in_release_record(self):
        from types import SimpleNamespace
        build = self.cache.project / f".pio/build/{ENV}"
        build.mkdir(parents=True)
        (build / "firmware.bin").write_bytes(b"fixture freshly built firmware")
        with (patch.object(release, "find_pio", return_value="fixture-pio"),
              patch.object(release, "validate_x3_zh_tw_build_flag"),
              patch.object(release, "read_crosspoint_version", return_value="1.3.3"),
              patch.object(release, "effective_config", return_value=self.cache.config),
              patch.object(release, "SdkCache", return_value=self.cache),
              patch.object(self.cache, "inputs", return_value={"sdk": "fixture"}),
              patch.object(self.cache, "prepare_app"),
              patch.object(release, "trusted", return_value=True),
              patch.object(release, "run_build", return_value=("", 0)),
              patch.object(release, "verify_firmware", return_value=SimpleNamespace(version="fixture"))):
            report = release.build_release(self.root)
        self.assertEqual(report["source_inputs"], self.expected)
        saved = json.loads((self.cache.state / "last-release.json").read_text())
        self.assertEqual(saved["source_inputs"], inputs.capture(self.cache))


if __name__ == "__main__":
    unittest.main()
