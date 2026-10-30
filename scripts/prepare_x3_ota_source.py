#!/usr/bin/env python3
"""Capture current corresponding source/build inputs after the parent's guarded build.

No Git history, private agent configuration, local device state, credentials or tool binaries.
Includes app sources, SDK sources, the exact patched dependency sources, licenses, and SDK config.
The parent must inspect this archive and the distribution licenses before publishing it.
"""

import argparse
import configparser
import gzip
import hashlib
import io
import json
import subprocess
import tarfile
from pathlib import Path

import release_x3_zh_tw as release
from release_sdk import SdkCache, ENV, digest
from x3_ota_source_inputs import PROJECT_PATHS, source_files, tracked_sources, input_files, verify


def prepare(root, record_path, head, output):
    snapshot = release.release_snapshot(root)
    record = json.loads(record_path.read_text())
    if snapshot["head"] != head or record["commit"] != head:
        raise ValueError("checkout/release record does not match the frozen HEAD")
    state = root / ".release-sdk"
    sdk_manifest = json.loads((state / "manifest.json").read_text())
    if sdk_manifest["key"] != record["sdk_key"]:
        raise ValueError("SDK cache does not match the release")
    release.verify_release_tarball(Path(record["artifact"]), record["firmware_sha256"])
    # Read the actual generated INI, not today's root INI or omitted personal overrides.
    expected = record.get("source_inputs")
    ini = state / "project/platformio.ini"
    if (not isinstance(expected, dict)
            or expected.get("files", {}).get("build-inputs/project/platformio.ini") != release.sha(ini)):
        raise ValueError("source inputs or SDK artifacts differ from the guarded release")
    config = configparser.ConfigParser(interpolation=None)
    config.read(ini)
    cache = SdkCache(root, None, {f"env:{ENV}": dict(config[f"env:{ENV}"])})
    verify(cache, expected)
    sums = []

    def add(tar, name, data):
        entry = tarfile.TarInfo(name)
        entry.size = len(data)
        entry.mode = 0o644
        entry.mtime = 0
        tar.addfile(entry, io.BytesIO(data))
        sums.append(f"{hashlib.sha256(data).hexdigest()}  {name}\n")

    output.parent.mkdir(parents=True, exist_ok=True)
    raw = output.open("xb")
    try:
        with raw, gzip.GzipFile(fileobj=raw, mode="wb", filename="", mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w|") as tar:
                for name, data in tracked_sources(root):
                    if hashlib.sha256(data).hexdigest() != expected["files"].get(name):
                        raise ValueError(f"tracked source changed during capture: {name}")
                    add(tar, name, data)
                for name, path in sorted(input_files(cache).items()):
                    data = path.read_bytes()
                    if hashlib.sha256(data).hexdigest() != expected["files"].get(name):
                        raise ValueError(f"source changed during capture: {name}")
                    add(tar, name, data)
                provenance = {
                    "commit": head, "sdk_key": record["sdk_key"],
                    "firmware_sha256": record["firmware_sha256"],
                    "freeink_sdk_commit": snapshot["checkout"],
                    "project_paths": PROJECT_PATHS,
                    "source_inputs_sha256": digest(expected),
                    "note": "Source snapshot, no private Git history. Inspect licenses before publication.",
                }
                add(tar, "SOURCE-PROVENANCE.json", (json.dumps(provenance, indent=2) + "\n").encode())
                add(tar, "SOURCE-SHA256SUMS", "".join(sums).encode())
        verify(cache, expected)
        release.verify_snapshot(root, snapshot)
    except Exception:
        # A partial or invalid archive must not masquerade as a prepared source delivery.
        output.unlink(missing_ok=True)
        raise
    print(f"Prepared {output}; parent inspection and license acceptance remain required")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--release-record", type=Path, required=True)
    parser.add_argument("--head", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    try:
        prepare(args.root, args.release_record, args.head, args.out)
    except (ValueError, KeyError, OSError, configparser.Error,
            subprocess.CalledProcessError, release.ReleaseError) as exc:
        parser.exit(1, f"prepare_x3_ota_source: {exc}\n")


if __name__ == "__main__":
    main()
