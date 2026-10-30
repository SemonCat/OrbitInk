import hashlib
import json
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import export_orbitink_source as export
import release_x3_zh_tw as release


class PublicSnapshot(unittest.TestCase):
    def test_exact_allowlisted_snapshot_has_no_history_local_state_or_agent_context(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "source"
            root.mkdir()
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            for name in ("src/main.cpp", "LICENSE", ".claude/session.md", "local-device.json"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(f"fixture {name}")
            (root / ".gitmodules").write_text(
                '[submodule "freeink-sdk"]\npath = freeink-sdk\nurl = https://example.invalid/sdk.git\n')
            (root / ".gitignore").write_text(".pio/\n.release-sdk/\nplatformio.local.ini\n")
            subprocess.run(["git", "add", "."], cwd=root, check=True)
            subprocess.run(["git", "update-index", "--add", "--cacheinfo",
                            "160000," + "a" * 40 + ",freeink-sdk"], cwd=root, check=True)
            (root / "freeink-sdk").mkdir()
            subprocess.run(["git", "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                            "commit", "-qm", "fixture"], cwd=root, check=True)
            head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
            output = Path(temporary) / "source.tar.gz"
            with patch.object(export, "PROJECT_PATHS", ["src", "LICENSE", ".gitmodules", ".gitignore"]):
                export.export(root, head, output)
                with tarfile.open(output) as archive:
                    names = archive.getnames()
                    self.assertNotIn(".claude/session.md", names)
                    self.assertNotIn("local-device.json", names)
                    self.assertFalse(any(name.startswith(".git/") for name in names))
                    receipt = json.load(archive.extractfile("PUBLIC-SOURCE-PROVENANCE.json"))
                    self.assertEqual(receipt["freeink_sdk_gitlink"], "a" * 40)
                    for name, digest in receipt["files"].items():
                        self.assertEqual(hashlib.sha256(archive.extractfile(name).read()).hexdigest(), digest)
                    public = Path(temporary) / "public"
                    public.mkdir()
                    # Fixtures contain only inspected regular files/directories.
                    for member in archive:
                        if member.isfile():
                            target = public / member.name
                            target.parent.mkdir(parents=True, exist_ok=True)
                            target.write_bytes(archive.extractfile(member).read())
                    subprocess.run(["git", "init", "-q", str(public)], check=True)
                    subprocess.run(["git", "add", "."], cwd=public, check=True)
                    subprocess.run(receipt["sdk_gitlink_import_argv"], cwd=public, check=True, shell=False)
                    subprocess.run(["git", "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                                    "commit", "-qm", "inspected snapshot"], cwd=public, check=True)
                    entry = subprocess.check_output(
                        ["git", "ls-tree", "HEAD", "freeink-sdk"], cwd=public, text=True)
                    self.assertEqual(entry, "160000 commit " + "a" * 40 + "\tfreeink-sdk\n")
                    status = subprocess.check_output(["git", "submodule", "status"], cwd=public, text=True)
                    self.assertTrue(status.startswith("-" + "a" * 40))
                    (public / "freeink-sdk").mkdir()
                    for name in ("platformio.local.ini", ".pio/build/generated", ".release-sdk/release.lock"):
                        path = public / name
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_text("generated CI state")
                    release.require_clean_release_tree(public)
                    (public / "src/unexpected.cpp").write_text("unreviewed source")
                    with self.assertRaises(release.ReleaseError):
                        release.require_clean_release_tree(public)
                with self.assertRaises(FileExistsError):
                    export.export(root, head, output)
                (root / "src/main.cpp").write_text("dirty source")
                with self.assertRaises(ValueError):
                    export.export(root, head, Path(temporary) / "bad.tar.gz")
