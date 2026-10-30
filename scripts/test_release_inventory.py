"""Digest reuse is allowed only for stable filesystem identity and metadata."""
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import release_sdk as sdk


class DigestInventoryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.inputs = self.root / "inputs"
        self.inputs.mkdir()
        self.file = self.inputs / "archive.a"
        self.file.write_bytes(b"original")
        self.cache = sdk.DigestCache(self.root / "digests.json")

    def test_unchanged_inventory_reuses_sha_in_memory_and_across_runs(self):
        with patch.object(sdk, "sha", wraps=sdk.sha) as hash_file:
            baseline = sdk.inventory(self.inputs, self.cache)
            self.assertEqual(hash_file.call_count, 1)
            self.assertEqual(sdk.inventory(self.inputs, self.cache), baseline)
            self.assertEqual(hash_file.call_count, 1)
            self.cache.save()
            reloaded = sdk.DigestCache(self.cache.path)
            self.assertEqual(sdk.inventory(self.inputs, reloaded), baseline)
            self.assertEqual(hash_file.call_count, 1)

    def test_same_length_change_with_restored_mtime_invalidates_via_ctime(self):
        before = sdk.inventory(self.inputs, self.cache)
        stat = self.file.stat()
        self.file.write_bytes(b"modified")
        os.utime(self.file, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        with patch.object(sdk, "sha", wraps=sdk.sha) as hash_file:
            self.assertNotEqual(sdk.inventory(self.inputs, self.cache), before)
            self.assertEqual(hash_file.call_count, 1)

    def test_every_stat_tuple_field_participates_in_invalidation(self):
        for field in range(5):
            with self.subTest(field=field):
                self.cache.hash(self.file)
                changed = sdk.DigestCache.signature(self.file)
                changed[field] += 1
                with patch.object(self.cache, "signature", return_value=changed):
                    with patch.object(sdk, "sha", wraps=sdk.sha) as hash_file:
                        self.cache.hash(self.file)
                        self.assertEqual(hash_file.call_count, 1)

    def test_new_deleted_and_renamed_files_change_inventory(self):
        baseline = sdk.inventory(self.inputs, self.cache)
        added = self.inputs / "new.a"
        added.write_bytes(b"new")
        with patch.object(sdk, "sha", wraps=sdk.sha) as hash_file:
            expanded = sdk.inventory(self.inputs, self.cache)
            self.assertEqual(hash_file.call_count, 1)
        self.assertNotEqual(expanded, baseline)
        added.unlink()
        self.assertEqual(sdk.inventory(self.inputs, self.cache), baseline)
        self.file.rename(self.inputs / "renamed.a")
        self.assertNotEqual(sdk.inventory(self.inputs, self.cache), baseline)

    def test_link_target_change_invalidates_even_with_same_contents(self):
        alternate = self.inputs / "alternate.a"
        alternate.write_bytes(self.file.read_bytes())
        link = self.inputs / "linked.a"
        link.symlink_to(self.file.name)
        before = sdk.inventory(self.inputs, self.cache)
        link.unlink()
        link.symlink_to(alternate.name)
        self.assertNotEqual(sdk.inventory(self.inputs, self.cache), before)

    def test_corrupt_persisted_cache_rehashes(self):
        self.cache.hash(self.file)
        self.cache.save()
        record = json.loads(self.cache.path.read_text())
        record["entries"][str(self.file)]["sha256"] = "0" * 64
        self.cache.path.write_text(json.dumps(record))
        with patch.object(sdk, "sha", wraps=sdk.sha) as hash_file:
            sdk.inventory(self.inputs, sdk.DigestCache(self.cache.path))
            self.assertEqual(hash_file.call_count, 1)

    def test_change_while_hashing_fails_closed(self):
        original = sdk.sha
        def mutate(path):
            value = original(path)
            self.file.write_bytes(b"changed during hashing")
            return value
        with patch.object(sdk, "sha", side_effect=mutate):
            with self.assertRaisesRegex(RuntimeError, "changed while"):
                self.cache.hash(self.file)


if __name__ == "__main__":
    unittest.main()
