#!/usr/bin/env python3
"""
Focused unit tests for scripts/release_x3_zh_tw.py's parsing/validation logic.
These do not invoke PlatformIO or compile any firmware; they exercise the
pure functions directly against synthetic byte blobs and strings.

Run with:
    python3 scripts/test_release_x3_zh_tw.py
(stdlib unittest only -- no pytest dependency, matching the rest of scripts/.)
"""

import subprocess
import struct
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import release_x3_zh_tw as rel  # noqa: E402


def _init_git_repo(root: Path) -> None:
    """Minimal, isolated git repo for tests of git-state-dependent logic
    (tracked/ignored checks, status parsing). Never touches the real
    crossfade repo."""
    subprocess.run(["git", "init", "-q"], cwd=root, check=True)
    subprocess.run(
        ["git", "config", "user.email", "test@example.invalid"], cwd=root, check=True
    )
    subprocess.run(["git", "config", "user.name", "Test"], cwd=root, check=True)


def _git_add(root: Path, *rel_paths: str) -> None:
    subprocess.run(["git", "add", "--", *rel_paths], cwd=root, check=True)


def make_app_desc_blob(
    version: str = "v1.3.2-25-gb9840cc",
    project_name: str = "crossfade",
    prefix: bytes = b"\0" * 32,
    suffix: bytes = b"",
) -> bytes:
    """Build a synthetic esp_app_desc_t-shaped blob for testing, matching the
    real struct layout (see esp_app_desc.h): magic_word, secure_version,
    reserv1[2], version[32], project_name[32], time[16], date[16],
    idf_ver[32], app_elf_sha256[32], min/max_efuse+mmu+reserv3[4],
    reserv2[18].
    """

    def fixed(s: str, length: int) -> bytes:
        raw = s.encode("ascii")
        if len(raw) >= length:
            raise ValueError("field too long for test fixture")
        return raw + b"\x00" * (length - len(raw))

    body = b"".join(
        [
            struct.pack("<I", rel.APP_DESC_MAGIC),
            struct.pack("<I", 0),  # secure_version
            struct.pack("<II", 0, 0),  # reserv1[2]
            fixed(version, 32),
            fixed(project_name, 32),
            fixed("12:00:00", 16),
            fixed("Jan  1 2026", 16),
            fixed("5.5.2.260206", 32),
            b"\x00" * 32,  # app_elf_sha256
            struct.pack("<HH", 0, 0),  # min/max efuse
            struct.pack("<B", 0) + b"\x00" * 3,  # mmu_page_size + reserv3
            b"\x00" * (18 * 4),  # reserv2
        ]
    )
    assert len(body) == rel.APP_DESC_SIZE, len(body)
    return prefix + body + suffix


class ParseAppDescriptorTests(unittest.TestCase):
    def test_parses_version_and_project_name(self):
        blob = make_app_desc_blob(version="v1.3.2-25-gb9840cc", project_name="crossfade")
        desc = rel.parse_app_descriptor(blob)
        self.assertEqual(desc.version, "v1.3.2-25-gb9840cc")
        self.assertEqual(desc.project_name, "crossfade")

    def test_finds_descriptor_amid_other_bytes(self):
        blob = make_app_desc_blob()
        padded = b"\xff" * 4096 + blob + b"\xee" * 4096
        with self.assertRaises(rel.ReleaseError):
            rel.parse_app_descriptor(padded)

    def test_missing_magic_raises(self):
        with self.assertRaises(rel.ReleaseError):
            rel.parse_app_descriptor(b"\x00" * 1024)

    def test_duplicate_magic_raises(self):
        blob = make_app_desc_blob()
        doubled = blob + blob
        with self.assertRaises(rel.ReleaseError):
            rel.parse_app_descriptor(doubled)

    def test_truncated_descriptor_raises(self):
        blob = make_app_desc_blob()[:100]
        with self.assertRaises(rel.ReleaseError):
            rel.parse_app_descriptor(blob)


class ValidateDescriptorVersionTests(unittest.TestCase):
    def test_matching_clean_version_passes(self):
        rel.validate_descriptor_version(
            "v1.3.2-25-gb9840cc", "v1.3.2-25-gb9840cc"
        )  # no raise

    def test_dirty_version_rejected(self):
        with self.assertRaises(rel.ReleaseError):
            rel.validate_descriptor_version(
                "v1.3.2-25-gb9840cc-dirty", "v1.3.2-25-gb9840cc-dirty"
            )

    def test_stale_sha_rejected(self):
        # Exactly the observed real-world bug: an older commit's descriptor
        # embedded despite HEAD having moved on.
        with self.assertRaises(rel.ReleaseError):
            rel.validate_descriptor_version(
                "v1.3.2-23-g82403f4", "v1.3.2-25-gb9840cc"
            )


class RuntimeVersionEmbeddedTests(unittest.TestCase):
    def test_exact_plain_version_passes(self):
        binary = b"junk\x00" + b"CrossFade-ESP32-1.3.3\x00" + b"more junk"
        rel.check_runtime_version_embedded(binary, "1.3.3")  # no raise

    def test_missing_version_rejected(self):
        with self.assertRaises(rel.ReleaseError):
            rel.check_runtime_version_embedded(b"nothing here", "1.3.3")

    def test_bare_version_without_tag_prefix_rejected(self):
        # The bare version number alone, without the CrossFade-ESP32- tag,
        # must not satisfy the check -- it isn't proof CROSSPOINT_VERSION
        # was actually built in as the runtime version.
        binary = b"junk\x00" + b"1.3.3\x00" + b"more junk"
        with self.assertRaises(rel.ReleaseError):
            rel.check_runtime_version_embedded(binary, "1.3.3")

    def test_suffixed_version_rejected(self):
        binary = b"junk\x00" + b"CrossFade-ESP32-1.3.3-x3-zh-tw\x00" + b"more junk"
        with self.assertRaises(rel.ReleaseError):
            rel.check_runtime_version_embedded(binary, "1.3.3")

    def test_mixed_runtime_versions_rejected(self):
        # Multiple runtime identities indicate mixed/stale objects.
        binary = b"CrossFade-ESP32-1.3.3\x00" + b"CrossFade-ESP32-1.3.30-other\x00"
        with self.assertRaises(rel.ReleaseError):
            rel.check_runtime_version_embedded(binary, "1.3.3")

    def test_unrelated_app_descriptor_git_describe_string_does_not_misfire(self):
        # Regression test: once this repo grows a "v1.3.3" tag, the ESP app
        # descriptor's own (unrelated) `git describe` field will legitimately
        # contain a substring like "1.3.3-1-gabcdef0". A bare `"1.3.3-"`
        # substring search (the old, broken check) would misfire on this and
        # falsely reject an otherwise-correct build. The CrossFade-ESP32-
        # anchored check must not be fooled by it.
        binary = (
            b"junk before "
            + b"v1.3.3-1-gabcdef0\x00"  # unrelated app descriptor version field
            + b" junk between "
            + b"CrossFade-ESP32-1.3.3\x00"  # the actual runtime literal
            + b" junk after"
        )
        rel.check_runtime_version_embedded(binary, "1.3.3")  # must not raise


class ValidateX3ZhTwBuildFlagTests(unittest.TestCase):
    INI_TEMPLATE = """\
[crosspoint]
version = 1.3.3

[base]
build_flags =
  -DSOME_BASE_FLAG=1

[env:x3_zh_tw_custom]
extends = base
build_flags =
  ${{base.build_flags}}
  -DFREEINK_DEVICE_X3=1
  -DCROSSFADE_CUSTOM_ZH_TW=1
  ; Runtime CROSSPOINT_VERSION is the plain product version -- no suffix.
  {version_flag}
  -DENABLE_SERIAL_LOG

[env:sticky]
extends = base
build_flags =
  ${{base.build_flags}}
  -DCROSSPOINT_VERSION=\\"${{crosspoint.version}}-sticky\\"
"""

    def _write_ini(self, tmp: Path, version_flag: str) -> Path:
        ini_path = tmp / "platformio.ini"
        ini_path.write_text(
            self.INI_TEMPLATE.format(version_flag=version_flag), encoding="utf-8"
        )
        return ini_path

    def test_plain_flag_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            self._write_ini(
                Path(tmp), r'-DCROSSPOINT_VERSION=\"${crosspoint.version}\"'
            )
            rel.validate_x3_zh_tw_build_flag(Path(tmp))  # no raise

    def test_suffixed_flag_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            self._write_ini(
                Path(tmp),
                r'-DCROSSPOINT_VERSION=\"${crosspoint.version}-x3-zh-tw\"',
            )
            with self.assertRaises(rel.ReleaseError):
                rel.validate_x3_zh_tw_build_flag(Path(tmp))

    def test_missing_flag_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            self._write_ini(Path(tmp), "; no CROSSPOINT_VERSION flag here")
            with self.assertRaises(rel.ReleaseError):
                rel.validate_x3_zh_tw_build_flag(Path(tmp))

    def test_explanatory_comment_mentioning_the_macro_is_not_mistaken_for_a_flag(self):
        # A comment line containing the word CROSSPOINT_VERSION (like the one
        # actually present in platformio.ini today) must not be counted as a
        # second, conflicting build-flag line.
        with tempfile.TemporaryDirectory() as tmp:
            self._write_ini(
                Path(tmp), r'-DCROSSPOINT_VERSION=\"${crosspoint.version}\"'
            )
            rel.validate_x3_zh_tw_build_flag(Path(tmp))  # no raise

    def test_missing_ini_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(rel.ReleaseError):
                rel.validate_x3_zh_tw_build_flag(Path(tmp))


class ExtractShortShaTests(unittest.TestCase):
    def test_tag_relative_form(self):
        self.assertEqual(
            rel.extract_short_sha("v1.3.2-25-gb9840cc"), "b9840cc"
        )

    def test_tag_relative_dirty_form(self):
        self.assertEqual(
            rel.extract_short_sha("v1.3.2-25-gb9840cc-dirty"), "b9840cc"
        )

    def test_bare_always_fallback_form(self):
        self.assertEqual(rel.extract_short_sha("b9840cc"), "b9840cc")

    def test_unparseable_form_returns_none(self):
        self.assertIsNone(rel.extract_short_sha("not-a-describe-output!!"))


class ParsePorcelainZTests(unittest.TestCase):
    def test_multiple_ordinary_entries_are_not_skipped(self):
        entries = rel.parse_porcelain_z(
            "?? test/build_host/cache\0 M src/second.cpp\0?? src/third.cpp\0 M platformio.ini\0")
        self.assertEqual([e["path"] for e in entries],
                         ["test/build_host/cache", "src/second.cpp", "src/third.cpp", "platformio.ini"])
        self.assertEqual(rel.find_release_blocking_paths(entries),
                         ["src/second.cpp", "src/third.cpp", "platformio.ini"])

    def test_rename_consumes_only_its_original_path(self):
        entries = rel.parse_porcelain_z(" M first\0R  new\0old\0?? last\0")
        self.assertEqual([e["path"] for e in entries], ["first", "new", "last"])
        self.assertEqual(entries[1]["orig_path"], "old")

    def test_truncated_rename_is_rejected(self):
        with self.assertRaises(rel.ReleaseError):
            rel.parse_porcelain_z("R  new\0")

    def test_empty_output_is_clean(self):
        self.assertEqual(rel.parse_porcelain_z(""), [])

    def test_modified_tracked_file(self):
        entries = rel.parse_porcelain_z(" M platformio.ini\x00")
        self.assertEqual(
            entries, [{"xy": " M", "path": "platformio.ini", "orig_path": None}]
        )

    def test_untracked_file(self):
        entries = rel.parse_porcelain_z("?? scripts/new_file.py\x00")
        self.assertEqual(
            entries,
            [{"xy": "??", "path": "scripts/new_file.py", "orig_path": None}],
        )

    def test_rename_pairs_two_nul_fields_correctly(self):
        # -z represents a rename as two consecutive NUL-terminated fields:
        # new path, then original path (no " -> " arrow in the text).
        entries = rel.parse_porcelain_z("R  new/path.py\x00old/path.py\x00")
        self.assertEqual(
            entries,
            [
                {
                    "xy": "R ",
                    "path": "new/path.py",
                    "orig_path": "old/path.py",
                }
            ],
        )

    def test_multiple_entries(self):
        output = " M platformio.ini\x00?? scripts/new_file.py\x00"
        entries = rel.parse_porcelain_z(output)
        self.assertEqual(len(entries), 2)
        self.assertEqual(entries[0]["path"], "platformio.ini")
        self.assertEqual(entries[1]["path"], "scripts/new_file.py")

    def test_path_with_space_is_not_mis_split(self):
        # -z never quotes/escapes the path, unlike default --porcelain -- a
        # space in the path must not be mistaken for a field separator.
        entries = rel.parse_porcelain_z("?? my dir/my file.txt\x00")
        self.assertEqual(entries[0]["path"], "my dir/my file.txt")


class FindReleaseBlockingPathsTests(unittest.TestCase):
    def test_clean_tree_blocks_nothing(self):
        self.assertEqual(rel.find_release_blocking_paths([]), [])

    def test_modified_tracked_file_blocks(self):
        entries = [{"xy": " M", "path": "platformio.ini", "orig_path": None}]
        self.assertEqual(
            rel.find_release_blocking_paths(entries), ["platformio.ini"]
        )

    def test_untracked_source_file_blocks(self):
        # The core of this fix: an untracked file anywhere else in the tree
        # must block the release, not be silently ignored.
        entries = [
            {"xy": "??", "path": "src/EvilInject.cpp", "orig_path": None}
        ]
        self.assertEqual(
            rel.find_release_blocking_paths(entries), ["src/EvilInject.cpp"]
        )

    def test_test_build_host_directory_marker_allowed(self):
        entries = [{"xy": "??", "path": "test/build_host/", "orig_path": None}]
        self.assertEqual(rel.find_release_blocking_paths(entries), [])

    def test_test_build_host_individual_files_allowed(self):
        entries = [
            {
                "xy": "??",
                "path": "test/build_host/CMakeCache.txt",
                "orig_path": None,
            },
            {
                "xy": "??",
                "path": "test/build_host/_deps/googletest-src/x.cc",
                "orig_path": None,
            },
        ]
        self.assertEqual(rel.find_release_blocking_paths(entries), [])

    def test_lookalike_path_outside_allowed_tree_still_blocks(self):
        # "test/build_host_extra/" is not "test/build_host/" or a path under
        # it -- a naive prefix check without the trailing slash could be
        # fooled by this.
        entries = [
            {"xy": "??", "path": "test/build_host_extra/x", "orig_path": None}
        ]
        self.assertEqual(
            rel.find_release_blocking_paths(entries),
            ["test/build_host_extra/x"],
        )

    def test_rename_out_of_build_host_blocks_on_new_path(self):
        entries = [
            {
                "xy": "R ",
                "path": "src/Moved.cpp",
                "orig_path": "test/build_host/whatever.txt",
            }
        ]
        self.assertEqual(
            rel.find_release_blocking_paths(entries), ["src/Moved.cpp", "test/build_host/whatever.txt"]
        )


class RequireCleanReleaseTreeTests(unittest.TestCase):
    def test_clean_repo_with_only_build_host_untracked_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo_root = Path(tmp)
            _init_git_repo(repo_root)
            (repo_root / "platformio.ini").write_text("[platformio]\n")
            _git_add(repo_root, "platformio.ini")
            subprocess.run(
                ["git", "commit", "-q", "-m", "init"], cwd=repo_root, check=True
            )
            build_host = repo_root / "test" / "build_host"
            build_host.mkdir(parents=True)
            (build_host / "CMakeCache.txt").write_text("stuff\n")

            rel.require_clean_release_tree(repo_root)  # no raise

    def test_untracked_source_file_blocks(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo_root = Path(tmp)
            _init_git_repo(repo_root)
            (repo_root / "platformio.ini").write_text("[platformio]\n")
            _git_add(repo_root, "platformio.ini")
            subprocess.run(
                ["git", "commit", "-q", "-m", "init"], cwd=repo_root, check=True
            )
            (repo_root / "src").mkdir()
            (repo_root / "src" / "Sneaky.cpp").write_text("// untracked\n")

            with self.assertRaises(rel.ReleaseError):
                rel.require_clean_release_tree(repo_root)

    def test_modified_tracked_file_blocks(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo_root = Path(tmp)
            _init_git_repo(repo_root)
            (repo_root / "platformio.ini").write_text("[platformio]\n")
            _git_add(repo_root, "platformio.ini")
            subprocess.run(
                ["git", "commit", "-q", "-m", "init"], cwd=repo_root, check=True
            )
            (repo_root / "platformio.ini").write_text("[platformio]\nchanged=1\n")

            with self.assertRaises(rel.ReleaseError):
                rel.require_clean_release_tree(repo_root)


class ArtifactBasenameTests(unittest.TestCase):
    def test_predictable_name(self):
        self.assertEqual(
            rel.artifact_basename("1.3.3", "b9840cc"),
            "crossfade-x3-zh-tw-v1.3.3-b9840cc",
        )


class ParseSizeSummaryTests(unittest.TestCase):
    def test_extracts_ram_and_flash(self):
        text = (
            "RAM:   [==        ]  23.4% (used 76832 bytes from 327680 bytes)\n"
            "Flash: [===       ]  34.5% (used 1085000 bytes from 3145728 bytes)\n"
        )
        summary = rel.parse_size_summary(text)
        self.assertEqual(
            summary,
            {
                "ram_used": 76832,
                "ram_total": 327680,
                "flash_used": 1085000,
                "flash_total": 3145728,
            },
        )

    def test_missing_lines_returns_none(self):
        self.assertIsNone(rel.parse_size_summary("no size info here"))

    def test_takes_last_summary_when_log_has_several(self):
        # Real build logs for this env contain more than one RAM:/Flash:
        # block -- a bootloader.elf size check, and (via the
        # custom_sdkconfig two-pass mechanism) potentially more than one
        # app build pass. Only the last one describes the firmware.bin
        # that actually gets shipped.
        text = (
            "RAM:   [==        ]  11.4% (used 37276 bytes from 327680 bytes)\n"
            "Flash: [==        ]  14.9% (used 978414 bytes from 6553600 bytes)\n"
            "*** Original Arduino \"idf_component.yml\" restored ***\n"
            "RAM:   [==        ]  15.4% (used 50508 bytes from 327680 bytes)\n"
            "Flash: [=========]  92.8% (used 6079721 bytes from 6553600 bytes)\n"
        )
        summary = rel.parse_size_summary(text)
        self.assertEqual(
            summary,
            {
                "ram_used": 50508,
                "ram_total": 327680,
                "flash_used": 6079721,
                "flash_total": 6553600,
            },
        )


class VerifyReleaseTarballTests(unittest.TestCase):
    def _make_tarball(self, tmp: Path, members: dict, name: str = "release.tar.gz") -> Path:
        """members: {arcname: bytes}. Writes each to a temp file then tars it."""
        tar_path = tmp / name
        with tarfile.open(tar_path, "w:gz") as tar:
            for arcname, content in members.items():
                src = tmp / f"__src_{arcname.replace('/', '_')}"
                src.write_bytes(content)
                tar.add(src, arcname=arcname)
        return tar_path

    def test_valid_tarball_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            firmware = b"fake firmware bytes"
            sha256 = rel.hashlib.sha256(firmware).hexdigest()
            tar_path = self._make_tarball(
                tmp_path,
                {
                    "firmware.bin": firmware,
                    "CHECKSUMS.sha256": f"{sha256}  firmware.bin\n".encode(),
                },
            )
            rel.verify_release_tarball(tar_path, sha256)  # no raise

    def test_missing_tarball_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(rel.ReleaseError):
                rel.verify_release_tarball(Path(tmp) / "nope.tar.gz", "a" * 64)

    def test_extra_member_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            firmware = b"fake firmware bytes"
            sha256 = rel.hashlib.sha256(firmware).hexdigest()
            tar_path = self._make_tarball(
                tmp_path,
                {
                    "firmware.bin": firmware,
                    "CHECKSUMS.sha256": f"{sha256}  firmware.bin\n".encode(),
                    "sneaky-extra-file.txt": b"unexpected",
                },
            )
            with self.assertRaises(rel.ReleaseError):
                rel.verify_release_tarball(tar_path, sha256)

    def test_missing_member_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            firmware = b"fake firmware bytes"
            sha256 = rel.hashlib.sha256(firmware).hexdigest()
            tar_path = self._make_tarball(tmp_path, {"firmware.bin": firmware})
            with self.assertRaises(rel.ReleaseError):
                rel.verify_release_tarball(tar_path, sha256)

    def test_checksum_mismatch_with_recorded_value_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            firmware = b"fake firmware bytes"
            wrong_sha256 = "0" * 64
            tar_path = self._make_tarball(
                tmp_path,
                {
                    "firmware.bin": firmware,
                    "CHECKSUMS.sha256": f"{wrong_sha256}  firmware.bin\n".encode(),
                },
            )
            with self.assertRaises(rel.ReleaseError):
                rel.verify_release_tarball(
                    tar_path, rel.hashlib.sha256(firmware).hexdigest()
                )

    def test_checksum_mismatch_with_pre_packaging_hash_rejected(self):
        # Internally-consistent tarball (recorded checksum matches the bytes
        # inside it) but the caller's pre-packaging hash disagrees -- e.g.
        # firmware.bin got swapped after hashing but before/while archiving.
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            firmware = b"fake firmware bytes"
            sha256 = rel.hashlib.sha256(firmware).hexdigest()
            tar_path = self._make_tarball(
                tmp_path,
                {
                    "firmware.bin": firmware,
                    "CHECKSUMS.sha256": f"{sha256}  firmware.bin\n".encode(),
                },
            )
            with self.assertRaises(rel.ReleaseError):
                rel.verify_release_tarball(tar_path, "f" * 64)

    def test_malformed_checksum_line_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            firmware = b"fake firmware bytes"
            tar_path = self._make_tarball(
                tmp_path,
                {
                    "firmware.bin": firmware,
                    "CHECKSUMS.sha256": b"not-a-valid-checksum-line\n",
                },
            )
            with self.assertRaises(rel.ReleaseError):
                rel.verify_release_tarball(
                    tar_path, rel.hashlib.sha256(firmware).hexdigest()
                )


if __name__ == "__main__":
    unittest.main()
