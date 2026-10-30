"""Git/process/cache integration tests; fixtures never edit the real repository."""
import copy
import hashlib
import json
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch

import release_sdk as sdk
import release_x3_zh_tw as rel
from project_app_descriptor import descriptor_source
from test_release_x3_zh_tw import _init_git_repo, make_app_desc_blob


def git(root, *args):
    return subprocess.check_output(["git", *args], cwd=root, text=True).strip()


class GitIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / "root"
        self.root.mkdir()
        _init_git_repo(self.root)
        sub = Path(self.tmp.name) / "sub"
        sub.mkdir()
        _init_git_repo(sub)
        (sub / "source").write_text("submodule input")
        git(sub, "add", ".")
        git(sub, "commit", "-qm", "initial sub")
        git(self.root, "-c", "protocol.file.allow=always", "submodule", "add", "-q", str(sub), "freeink-sdk")
        (self.root / "platformio.ini").write_text("[crosspoint]\nversion = 1.3.3\n")
        (self.root / ".gitignore").write_text("sdkconfig*\n.release-sdk/\n")
        (self.root / "source.cpp").write_text("source")
        git(self.root, "add", ".")
        git(self.root, "commit", "-qm", "initial root")
        self.snapshot = rel.release_snapshot(self.root)

    def test_same_head_and_allowed_untracked_host_tree(self):
        path = self.root / "test/build_host"
        path.mkdir(parents=True)
        (path / "cache").write_text("preserved")
        rel.verify_snapshot(self.root, self.snapshot)

    def test_empty_commit_changes_identity(self):
        git(self.root, "commit", "--allow-empty", "-qm", "identity only")
        with self.assertRaises(rel.ReleaseError):
            rel.verify_snapshot(self.root, self.snapshot)

    def test_ordinary_source_commit_changes_identity(self):
        (self.root / "source.cpp").write_text("new source")
        git(self.root, "commit", "-qam", "source change")
        with self.assertRaises(rel.ReleaseError):
            rel.verify_snapshot(self.root, self.snapshot)

    def test_exact_tag_and_detached_head(self):
        git(self.root, "tag", "v1.3.3")
        git(self.root, "checkout", "--detach", "-q")
        current = rel.release_snapshot(self.root)
        self.assertEqual(current["describe"], "v1.3.3")
        rel.verify_snapshot(self.root, current)

    def test_dirty_source_and_untracked_source(self):
        for path in ("source.cpp", "injected.cpp"):
            with self.subTest(path=path):
                (self.root / path).write_text("dirty")
                with self.assertRaises(rel.ReleaseError):
                    rel.release_snapshot(self.root)
                if path == "source.cpp":
                    git(self.root, "restore", path)

    def test_tracked_modification_under_host_whitelist_rejected(self):
        path = self.root / "test/build_host/tracked"
        path.parent.mkdir(parents=True)
        path.write_text("tracked")
        git(self.root, "add", ".")
        git(self.root, "commit", "-qm", "tracked fixture")
        path.write_text("modified")
        with self.assertRaises(rel.ReleaseError):
            rel.release_snapshot(self.root)

    def test_submodule_dirty_and_pointer_mismatch(self):
        sub = self.root / "freeink-sdk"
        (sub / "source").write_text("changed")
        with self.assertRaises(rel.ReleaseError):
            rel.release_snapshot(self.root)
        git(sub, "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
            "commit", "-qam", "sub change")
        with self.assertRaises(rel.ReleaseError):
            rel.release_snapshot(self.root)

    def test_ignored_config_changed_during_build(self):
        (self.root / "sdkconfig.defaults").write_text("CONFIG_NEW=y")
        with self.assertRaises(rel.ReleaseError):
            rel.verify_snapshot(self.root, self.snapshot)

    def test_process_guard_rejects_mid_build_head_config_submodule_changes(self):
        # A real child process and Git operations exercise both watcher and final guard.
        for change in ("head", "config", "submodule"):
            with self.subTest(change=change):
                before = rel.release_snapshot(self.root)
                program = Path(self.tmp.name) / "fake-pio"
                program.write_text("#!/bin/sh\nsleep 2\n")
                program.chmod(0o755)
                def mutate():
                    time.sleep(.2)
                    if change == "head":
                        git(self.root, "commit", "--allow-empty", "-qm", "during build")
                    elif change == "config":
                        (self.root / "sdkconfig.defaults").write_text("changed")
                    else:
                        (self.root / "freeink-sdk/source").write_text("changed")
                worker = threading.Thread(target=mutate)
                worker.start()
                with self.assertRaises(rel.ReleaseError):
                    rel.run_build(str(program), self.root, {}, self.root, before,
                                  Path(self.tmp.name) / "build.log")
                worker.join()


class CacheTests(unittest.TestCase):
    def test_cold_preparation_uses_untrusted_target_seed_without_reinstall_stamp(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            seed = root / "seed"
            (seed / "platforms/espressif32").mkdir(parents=True)
            libs = seed / "packages/framework-arduinoespressif32-libs"
            for target in ("esp32c3", "esp32s3"):
                (libs / target).mkdir(parents=True)
                (libs / target / "vendor.a").write_text(target)
            (libs / "sdkconfig").write_text("untrusted old stamp")
            (seed / "packages/toolchain").mkdir()
            (seed / "packages/toolchain/gcc").write_text("compiler")
            (root / "partitions.csv").write_text("partitions")
            cache = sdk.SdkCache(root, "pio", {"env:" + sdk.ENV: {}})
            cache.seed = seed
            cache.prepare_cold()
            private_libs = cache.core / "packages/framework-arduinoespressif32-libs"
            self.assertTrue((private_libs / "esp32c3/vendor.a").is_file())
            self.assertFalse((private_libs / "esp32s3").exists())
            self.assertFalse((private_libs / "sdkconfig").exists())
            self.assertEqual((libs / "sdkconfig").read_text(), "untrusted old stamp")
            self.assertFalse(cache.manifest_path.exists())
            if sdk.sys.platform == "darwin":
                self.assertTrue((cache.core / "packages/toolchain").is_symlink())

    @unittest.skipUnless(sdk.sys.platform == "darwin", "macOS shared-tool protection")
    def test_shared_package_write_is_denied_through_symlink(self):
        with tempfile.TemporaryDirectory() as tmp:
            protected = Path(tmp) / "protected"
            protected.mkdir()
            original = protected / "source"
            original.write_text("unchanged")
            alias = Path(tmp) / "alias"
            alias.symlink_to(protected, target_is_directory=True)
            command = sdk.protected_command(
                [sdk.sys.executable, "-c", "import pathlib,sys;pathlib.Path(sys.argv[1]).write_text('changed')",
                 str(alias / "source")], [protected])
            result = subprocess.run(command, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(original.read_text(), "unchanged")

    def test_private_core_satisfies_builder_path_gate_and_overrides_global_routes(self):
        with tempfile.TemporaryDirectory() as tmp:
            cache = sdk.SdkCache(Path(tmp), "pio", {})
            with patch.dict(sdk.os.environ, {"PLATFORMIO_PACKAGES_DIR": "/global/packages"}):
                environment = cache.environment()
            self.assertEqual(cache.core, Path(tmp) / ".release-sdk/.platformio")
            self.assertEqual(environment["PLATFORMIO_PACKAGES_DIR"], str(cache.core / "packages"))
            self.assertEqual(environment["PLATFORMIO_CORE_DIR"], str(cache.core))

    def test_no_baseline_incomplete_and_corrupt_manifest(self):
        artifacts = {"lib.a": "abc"}
        for manifest in (None, {}, {"key": "key"}, {"schema": 1, "key": "key", "artifacts": artifacts}):
            self.assertFalse(sdk.trusted(manifest, "key", artifacts))

    def test_warm_key_change_and_stale_archive_injection(self):
        artifacts = {"lib.a": "abc"}
        manifest = dict(schema=sdk.SCHEMA, key="key", controlled_build=True, artifacts=artifacts)
        self.assertTrue(sdk.trusted(manifest, "key", artifacts))
        self.assertFalse(sdk.trusted(manifest, "toolchain changed", artifacts))
        self.assertFalse(sdk.trusted(manifest, "key", {"lib.a": "injected"}))
        self.assertFalse(sdk.trusted(manifest, "key", {}))

    def test_sdkconfig_and_toolchain_content_changes_invalidate_key(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "sdkconfig").write_text("CONFIG_TEST=y")
            (root / "gcc").write_text("toolchain 1")
            original = sdk.digest(sdk.inventory(root))
            for filename in ("sdkconfig", "gcc"):
                (root / filename).write_text("changed")
                self.assertNotEqual(sdk.digest(sdk.inventory(root)), original)

    def test_product_version_does_not_change_sdk_options(self):
        options = {"build_flags": ['-DCROSSPOINT_VERSION=\\"1.3.3\\"', '-DFREEINK_DEVICE_X3=1'],
                   "custom_sdkconfig": "CONFIG_TEST=y", "board": "esp32-c3-devkitm-1"}
        config = {"env:" + sdk.ENV: options}
        baseline = sdk.sdk_options(config)
        options["build_flags"][0] = '-DCROSSPOINT_VERSION=\\"1.3.4\\"'
        self.assertEqual(sdk.sdk_options(config), baseline)
        options["custom_sdkconfig"] = "CONFIG_TEST=n"
        self.assertNotEqual(sdk.sdk_options(config), baseline)

    def test_descriptor_source_changes_only_with_identity(self):
        a = descriptor_source("a" * 40, "v1.0.0", "5.5.2")
        self.assertEqual(a, descriptor_source("a" * 40, "v1.0.0", "5.5.2"))
        self.assertNotEqual(a, descriptor_source("b" * 40, "v1.0.0", "5.5.2"))
        self.assertNotEqual(a, descriptor_source("a" * 40, "v1.0.1", "5.5.2"))
        with self.assertRaises(ValueError):
            descriptor_source("a" * 40, "x" * 32, "5.5.2")

    def test_deterministic_tar_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            paths = [Path(tmp) / name for name in ("a.tar.gz", "b.tar.gz")]
            for path in paths:
                rel.package_firmware(path, b"firmware")
            self.assertEqual(paths[0].read_bytes(), paths[1].read_bytes())

    def test_stale_descriptor_and_elf_cache_rejected(self):
        head = "a" * 40
        snapshot = dict(head=head, describe="v1.3.3")
        with tempfile.TemporaryDirectory() as tmp:
            elf = Path(tmp) / "firmware.elf"
            elf.write_bytes(b"ELF")
            binary = bytearray(make_app_desc_blob(version="v1.3.3"))
            binary[176:208] = hashlib.sha256(b"ELF").digest()
            binary += b"CrossFade-ESP32-1.3.3\0CrossFade-HEAD:" + head.encode() + b"\0"
            with patch.object(rel.subprocess, "check_output", return_value="3c000020 R esp_app_desc\n"):
                rel.verify_firmware(binary, elf, snapshot, "1.3.3", "nm")
                elf.write_bytes(b"stale ELF")
                with self.assertRaises(rel.ReleaseError):
                    rel.verify_firmware(binary, elf, snapshot, "1.3.3", "nm")
            with self.assertRaises(rel.ReleaseError):
                rel.validate_descriptor_version("v1.3.2", snapshot["describe"])


if __name__ == "__main__":
    unittest.main()
