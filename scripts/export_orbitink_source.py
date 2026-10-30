#!/usr/bin/env python3
"""Prepare an allowlisted source-only snapshot. Does not scan, push, rename or publish."""

import argparse
import hashlib
import io
import json
import subprocess
import tarfile
from pathlib import Path

from x3_ota_source_inputs import PROJECT_PATHS


def export(root, head, out):
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    status = subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=no"], cwd=root)
    if actual != head or status:
        raise ValueError("export requires exact clean committed HEAD")
    sdk = subprocess.check_output(["git", "rev-parse", f"{head}:freeink-sdk"], cwd=root, text=True).strip()
    data = subprocess.check_output(["git", "archive", "--format=tar", head, *PROJECT_PATHS], cwd=root)
    hashes = {}
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        for member in archive:
            if member.isfile():
                hashes[member.name] = hashlib.sha256(archive.extractfile(member).read()).hexdigest()
            elif not member.isdir():
                raise ValueError(f"unexpected non-regular source member: {member.name}")
    # No partial archive on failure; no overwrite of earlier receipts.
    out.parent.mkdir(parents=True, exist_ok=True)
    receipt = {"private_candidate": head, "freeink_sdk_gitlink": sdk,
               # Tar files cannot encode Git's mode-160000 index entry. The parent must recreate it.
               "sdk_gitlink_import_argv": ["git", "update-index", "--add", "--cacheinfo",
                                           f"160000,{sdk},freeink-sdk"],
               "files": hashes, "history_exported": False,
               "publication_gate": "parent inspection and acceptance required"}
    raw = (json.dumps(receipt, indent=2, sort_keys=True) + "\n").encode()
    with tarfile.open(fileobj=io.BytesIO(data)) as archive, out.open("xb") as stream:
        with tarfile.open(fileobj=stream, mode="w:gz") as output:
            for member in archive:
                output.addfile(member, archive.extractfile(member) if member.isfile() else None)
            info = tarfile.TarInfo("PUBLIC-SOURCE-PROVENANCE.json")
            info.size = len(raw)
            info.mode = 0o644
            output.addfile(info, io.BytesIO(raw))
    print(f"Prepared {out}; {len(hashes)} source files; parent inspection still required")
    print(f"Required in inspected import checkout: git update-index --add --cacheinfo 160000,{sdk},freeink-sdk")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--head", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    export(args.root, args.head, args.out)


if __name__ == "__main__":
    main()
