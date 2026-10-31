#!/usr/bin/env python3
"""Trusted OrbitInk release workflow helpers. No keys are generated or stored in source."""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace

import assemble_x3_ota as assembler
import make_ota_manifest as manifest
import prepare_x3_ota_source as source
import release_x3_zh_tw as release

CHANNELS = {"stable": "bookorbit-x3", "nightly": "orbitink-x3-nightly"}
REPOSITORY = "SemonCat/OrbitInk"
LEGACY_REPOSITORY = "SemonCat/BookOrbit-X3"
ROOT = Path(__file__).resolve().parent.parent


def build_number(run_number, attempt):
    """One workflow allocates both channels. Attempts have disjoint slots; never wrap uint32."""
    if not 1 <= run_number <= 9_999_980 or not 1 <= attempt <= 99:
        raise ValueError("CI build sequence exhausted or invalid")
    return 1000 + run_number * 100 + attempt


def require_event(env):
    if (env.get("GITHUB_REPOSITORY") != REPOSITORY or env.get("GITHUB_REF") != "refs/heads/main"
            or env.get("GITHUB_EVENT_NAME") not in {"push", "workflow_dispatch"}):
        raise ValueError("signing/publication only supports trusted OrbitInk main events")


def configure(args):
    require_event(os.environ)
    channel = "nightly" if os.environ["GITHUB_EVENT_NAME"] == "push" else args.channel
    version = release.read_crosspoint_version(ROOT)
    if channel == "stable":
        if (os.environ.get("GITHUB_ACTOR") != "SemonCat" or args.confirm != "RELEASE"
                or args.version != version):
            raise ValueError("stable requires owner dispatch, RELEASE, and the committed runtime version")
    number = build_number(int(os.environ["GITHUB_RUN_NUMBER"]), int(os.environ["GITHUB_RUN_ATTEMPT"]))
    local = ROOT / "platformio.local.ini"
    # CI has no personal overrides. Refuse to replace an operator's local configuration.
    with local.open("x") as output:
        output.write(f"[crosspoint]\nx3_ota_build = {number}\nx3_ota_channel = {CHANNELS[channel]}\n")
    with open(os.environ["GITHUB_OUTPUT"], "a") as output:
        output.write(f"channel={channel}\nbuild={number}\n")


def stage(args):
    record_path = ROOT / ".release-sdk/last-release.json"
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    firmware, record = assembler.load_firmware(record_path, head)
    ident = manifest.read_identity(firmware)
    if ident["channel"] != CHANNELS[args.channel] or ident["build"] != args.build:
        raise ValueError("guarded image does not match allocated channel/build")
    args.directory.mkdir(parents=True, exist_ok=False)
    source.prepare(ROOT, record_path, head, args.directory / "corresponding-source.tar.gz")
    # Bind the relocatable artifact to the unchanged guarded hash. Rechecked in the signing job.
    shutil.copyfile(record["artifact"], args.directory / "guarded.tar.gz")
    record["artifact"] = "guarded.tar.gz"
    (args.directory / "record.json").write_text(json.dumps(record, indent=2) + "\n")
    (args.directory / "firmware.bin").write_bytes(firmware)


def package(args):
    require_event(os.environ)
    head = os.environ["GITHUB_SHA"]
    record_path = args.directory / "record.json"
    record = json.loads(record_path.read_text())
    if record["artifact"] != "guarded.tar.gz":
        raise ValueError("invalid staged artifact path")
    record["artifact"] = str((args.directory / "guarded.tar.gz").resolve())
    # Work on a separate record; preserve the unsigned artifact receipt.
    signing_record = args.directory / "signing-record.json"
    signing_record.write_text(json.dumps(record, indent=2) + "\n")
    firmware, _ = assembler.load_firmware(signing_record, head)
    ident = manifest.read_identity(firmware)
    if ident["channel"] != CHANNELS[args.channel] or ident["build"] != args.build:
        raise ValueError("staged channel/build mismatch")
    if firmware != (args.directory / "firmware.bin").read_bytes():
        raise ValueError("staged firmware differs from guarded image")
    tag = f"orbitink-b{args.build}"
    with tempfile.TemporaryDirectory() as temporary:
        key = Path(temporary) / "signing.pem"
        # The secret is supplied to this step only, never to the build, PRs, artifacts or argv.
        key_data = os.environ.pop("OTA_SIGNING_KEY_PEM")
        fd = os.open(key, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        with os.fdopen(fd, "w") as output:
            output.write(key_data)
        del key_data
        manifest.cmd_sign(SimpleNamespace(
            firmware=str(args.directory / "firmware.bin"), key=str(key), tag=tag,
            repo=LEGACY_REPOSITORY, out=str(args.directory / "manifest.json"),
            pubkey_header=str(manifest.PUBKEY_HEADER)))
    assembler.assemble(SimpleNamespace(
        release_record=signing_record, head=head, manifest=args.directory / "manifest.json",
        source=args.directory / "corresponding-source.tar.gz", readme=ROOT / "docs/OrbitInk-README-zh-TW.md",
        implementation=ROOT / "docs/contributing/orbitink-channels.md",
        out=args.directory / "orbitink.zip"))
    names = ["firmware.bin", "manifest.json", "corresponding-source.tar.gz", "orbitink.zip"]
    readme = (ROOT / "docs/OrbitInk-README-zh-TW.md").read_text()
    readme = readme.replace("@HEAD@", head).replace("@BUILD@", str(args.build)).replace(
        "@SHA256@", hashlib.sha256(firmware).hexdigest())
    (args.directory / "README-zh-TW.md").write_text(readme)
    names.append("README-zh-TW.md")
    (args.directory / "SHA256SUMS").write_text("".join(
        f"{hashlib.sha256((args.directory / name).read_bytes()).hexdigest()}  {name}\n" for name in names))


def gh(*args):
    return subprocess.check_output(["gh", *args], text=True)


def assert_new_build(number, releases):
    # Immutable tags bind build ordering across BOTH channels, including older workflow reruns.
    builds = [int(m.group(1)) for item in releases
              if (m := re.fullmatch(r"orbitink-b([0-9]+)", item["tagName"]))]
    if builds and number <= max(builds):
        raise ValueError("stale/repeated workflow cannot publish or move a channel pointer backwards")


def publish(args):
    require_event(os.environ)
    if args.channel == "stable" and (os.environ["GITHUB_EVENT_NAME"] != "workflow_dispatch"
                                     or os.environ.get("GITHUB_ACTOR") != "SemonCat"):
        raise ValueError("stable publication requires an owner dispatch")
    # gh list paginates up to this explicit bound; refuse at the limit rather than silently omit.
    releases = json.loads(gh("release", "list", "--repo", REPOSITORY, "--limit", "10000",
                            "--json", "tagName"))
    if len(releases) >= 10000:
        raise ValueError("release inventory limit reached; parent must archive/adjust safely")
    assert_new_build(args.build, releases)
    tag = f"orbitink-b{args.build}"
    assets = [str(args.directory / name) for name in (
        "firmware.bin", "manifest.json", "corresponding-source.tar.gz", "orbitink.zip",
        "README-zh-TW.md", "SHA256SUMS")]
    # Publish the immutable release completely before advancing a channel endpoint.
    gh("release", "create", tag, *assets, "--repo", REPOSITORY, "--target", os.environ["GITHUB_SHA"],
       # Display only: nightly is titled exactly "nightly"; the unique tag carries the build.
       "--title", "nightly" if args.channel == "nightly" else f"OrbitInk {args.channel} build {args.build}",
       "--notes-file", str(args.directory / "README-zh-TW.md"),
       "--prerelease" if args.channel == "nightly" else "--latest",
       *(["--latest=false"] if args.channel == "nightly" else []))
    if args.channel == "nightly":
        if not any(item["tagName"] == "nightly" for item in releases):
            gh("release", "create", "nightly", "--repo", REPOSITORY, "--target", os.environ["GITHUB_SHA"],
               "--title", "OrbitInk nightly channel", "--notes", "Experimental signed update channel.",
               "--prerelease", "--latest=false")
        gh("release", "upload", "nightly", str(args.directory / "manifest.json"), "--clobber",
           "--repo", REPOSITORY)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["configure", "stage", "package", "publish"])
    parser.add_argument("--channel", choices=CHANNELS, default="nightly")
    parser.add_argument("--build", type=int)
    parser.add_argument("--version", default="")
    parser.add_argument("--confirm", default="")
    parser.add_argument("--directory", type=Path, default=ROOT / "dist/ci")
    args = parser.parse_args()
    try:
        globals()[args.command](args)
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError, manifest.ManifestError) as error:
        print(f"orbitink_ci: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
