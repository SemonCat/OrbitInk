#!/usr/bin/env python3
"""Assemble only a fresh guarded release; never build, sign, push or publish.

The parent runs this after release_x3_zh_tw.py and make_ota_manifest.py sign.
Source/license assets are separate required inputs so they can be inspected before publication.
"""

import argparse
import hashlib
import json
import re
import tarfile
import zipfile
from pathlib import Path, PurePosixPath

import make_ota_manifest as manifest_tool
import release_x3_zh_tw as release
from release_sdk import digest

BASE = "279463be48edf7c1fd7d364060047eaf5de67ff9"
REQUIRED_NOTICES = {
    "project/LICENSE", "project/NOTICE", "freeink-sdk/LICENSE", "freeink-sdk/NOTICE",
    "dependencies/Arduino-wolfSSL/COPYING", "dependencies/Arduino-wolfSSL/LICENSING",
    "project/LICENSES/COPYING.GPL3",
}
NOTICE_NAME = re.compile(
    r"(?:licen[cs]e|licensing|copying|notice|copyright|ofl|ufl)"
    r"(?:[-_.][a-z0-9]+)*", re.IGNORECASE)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def load_firmware(record_path, head):
    if not re.fullmatch(r"[0-9a-f]{40}", head) or head == BASE:
        raise ValueError("expected a full new candidate HEAD, never the old delivery")
    record = json.loads(record_path.read_text())
    if record["commit"] != head:
        raise ValueError("release record is for another HEAD")
    artifact = Path(record["artifact"])
    if sha(artifact.read_bytes()) != record["tar_sha256"]:
        raise ValueError("guarded release tar hash mismatch")
    release.verify_release_tarball(artifact, record["firmware_sha256"])
    with tarfile.open(artifact, "r:gz") as tar:
        firmware = tar.extractfile("firmware.bin").read()
    if firmware.count(f"CrossFade-HEAD:{head}".encode() + b"\0") != 1:
        raise ValueError("firmware is not the expected full HEAD")
    return firmware, record


def source_notices(path, head, record):
    """Copy actual notices from the matching source snapshot, never generic substitutes."""
    notices = {}
    with tarfile.open(path, "r:gz") as source:
        provenance = source.getmember("SOURCE-PROVENANCE.json")
        if not provenance.isfile() or provenance.size > 65536:
            raise ValueError("invalid source provenance")
        source_record = json.load(source.extractfile(provenance))
        if (source_record["commit"] != head or source_record["sdk_key"] != record["sdk_key"]
                or source_record["firmware_sha256"] != record["firmware_sha256"]):
            raise ValueError("corresponding source is from another build")
        inputs = record.get("source_inputs")
        if (not isinstance(inputs, dict) or inputs.get("schema") != 1 or not inputs.get("files")
                or source_record.get("source_inputs_sha256") != digest(inputs)):
            raise ValueError("source inputs are not bound to the guarded release")
        captured = set()
        for member in source.getmembers():
            name = PurePosixPath(member.name)
            if name.is_absolute() or ".." in name.parts:
                raise ValueError("unsafe source archive path")
            if member.name in inputs["files"]:
                if (member.name in captured or not member.isfile()
                        or sha(source.extractfile(member).read()) != inputs["files"][member.name]):
                    raise ValueError(f"source input hash mismatch: {member.name}")
                captured.add(member.name)
            if not NOTICE_NAME.fullmatch(name.name):
                continue
            if not member.isfile() or not 0 < member.size <= 1024 * 1024:
                raise ValueError(f"invalid source notice: {member.name}")
            target = f"licenses/{name.as_posix()}"
            if target in notices:
                raise ValueError(f"duplicate source notice: {member.name}")
            notices[target] = source.extractfile(member).read()
        if captured != inputs["files"].keys():
            raise ValueError("source archive omits guarded build inputs")
    missing = {f"licenses/{name}" for name in REQUIRED_NOTICES} - notices.keys()
    if missing:
        raise ValueError(f"missing source license/notice files: {', '.join(sorted(missing))}")
    return notices


def assemble(args):
    firmware, record = load_firmware(args.release_record, args.head)
    manifest_bytes = args.manifest.read_bytes()
    if len(manifest_bytes) > manifest_tool.MAX_MANIFEST_BYTES:
        raise ValueError("manifest too large")
    manifest = json.loads(manifest_bytes)
    point = manifest_tool.header_point(manifest_tool.PUBKEY_HEADER.read_text())
    manifest_tool.verify_manifest(manifest, firmware, point)
    expected_prefix = f"https://github.com/{manifest_tool.DEFAULT_REPO}/releases/download/"
    if not manifest["url"].startswith(expected_prefix):
        raise ValueError("manifest is for another repository")
    notices = source_notices(args.source, args.head, record)
    readme = args.readme.read_text().replace("@HEAD@", args.head).replace(
        "@BUILD@", str(manifest["build"])).replace("@SHA256@", sha(firmware))
    if re.search(r"@[A-Z0-9_]+@", readme):
        raise ValueError("unfilled README placeholder")
    files = {
        "firmware.bin": firmware,
        "manifest.json": manifest_bytes,
        "README-zh-TW.md": readme.encode(),
        "IMPLEMENTATION.md": args.implementation.read_bytes(),
        "corresponding-source.tar.gz": args.source.read_bytes(),
    }
    files.update(notices)
    files["SHA256SUMS"] = "".join(
        f"{sha(data)}  {name}\n" for name, data in sorted(files.items())).encode()
    # Validate all inputs before creating anything. Exclusive create preserves previous deliveries.
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("xb") as output:
        with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for name, data in sorted(files.items()):
                info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                archive.writestr(info, data)
    with zipfile.ZipFile(args.out) as archive:
        if sorted(archive.namelist()) != sorted(files):
            raise ValueError("ZIP member list mismatch")
        for name, data in files.items():
            if archive.read(name) != data:
                raise ValueError(f"ZIP read-back failed: {name}")
    args.out.with_suffix(args.out.suffix + ".sha256").write_text(
        f"{sha(args.out.read_bytes())}  {args.out.name}\n")
    print(f"Prepared {args.out} (local assembly only; publication/license acceptance belongs to the parent)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("release-record", "manifest", "readme", "implementation", "source", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--head", required=True)
    try:
        assemble(parser.parse_args())
    except (ValueError, KeyError, OSError, tarfile.TarError, manifest_tool.ManifestError,
            release.ReleaseError) as exc:
        parser.exit(1, f"assemble_x3_ota: {exc}\n")


if __name__ == "__main__":
    main()
