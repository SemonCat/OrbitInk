#!/usr/bin/env python3
"""Build a guarded X3 zh-TW release using a verified incremental SDK cache.

Use --cold to reconstruct the private SDK. No global packages are modified.
See docs/contributing/x3-release.md for cache provenance and verification.
"""

from __future__ import annotations

import argparse
import configparser
import dataclasses
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import gzip
import io
import json
import time
import threading
import fcntl
import signal

from release_sdk import SdkCache, digest, effective_config, sha, trusted, protected_command
import x3_ota_source_inputs as ota_sources
from pathlib import Path
from typing import Optional

ENV_NAME = "x3_zh_tw_custom"
REPO_ROOT = Path(__file__).resolve().parent.parent

# --- esp_app_desc_t layout (esp_app_format/include/esp_app_desc.h). Total
# struct size is a fixed 256 bytes; offsets below are load-bearing constants,
# not guesses -- confirmed against a real build's firmware.bin during
# development of this script.
APP_DESC_MAGIC = 0xABCD5432
APP_DESC_MAGIC_BYTES = struct.pack("<I", APP_DESC_MAGIC)
APP_DESC_SIZE = 256
APP_DESC_VERSION_OFFSET = 16
APP_DESC_VERSION_LEN = 32
APP_DESC_PROJECT_NAME_OFFSET = 48
APP_DESC_PROJECT_NAME_LEN = 32


@dataclasses.dataclass
class AppDescriptor:
    version: str
    project_name: str


class ReleaseError(Exception):
    """Fatal, user-facing failure. Message is printed as-is; no traceback."""


# --------------------------------------------------------------------------
# Pure logic (covered by scripts/test_release_x3_zh_tw.py without needing a
# firmware compile)
# --------------------------------------------------------------------------


def parse_app_descriptor(data: bytes) -> AppDescriptor:
    """Locate and parse the esp_app_desc_t embedded in a firmware image.

    Raises ReleaseError if the magic word is missing, truncated, or appears
    more than once (which would make "the" descriptor ambiguous).
    """
    count = data.count(APP_DESC_MAGIC_BYTES)
    if count == 0:
        raise ReleaseError(
            "No ESP app descriptor (magic word "
            f"0x{APP_DESC_MAGIC:08X}) found in firmware image; "
            "cannot validate embedded version."
        )
    if count > 1:
        raise ReleaseError(
            f"ESP app descriptor magic word appears {count} times in firmware "
            "image; refusing to guess which one is authoritative."
        )
    idx = data.find(APP_DESC_MAGIC_BYTES)
    if idx != 32:
        raise ReleaseError(f"ESP descriptor must begin at image offset 32, found {idx}")
    if idx + APP_DESC_SIZE > len(data):
        raise ReleaseError("ESP app descriptor is truncated at end of image.")

    def read_cstr(offset: int, length: int) -> str:
        raw = data[idx + offset : idx + offset + length]
        return raw.split(b"\x00", 1)[0].decode("utf-8", errors="replace")

    return AppDescriptor(
        version=read_cstr(APP_DESC_VERSION_OFFSET, APP_DESC_VERSION_LEN),
        project_name=read_cstr(
            APP_DESC_PROJECT_NAME_OFFSET, APP_DESC_PROJECT_NAME_LEN
        ),
    )


def validate_descriptor_version(descriptor_version: str, expected_describe: str) -> None:
    """Raise ReleaseError unless the embedded version exactly matches the
    freshly-computed `git describe --always --tags --dirty` for current HEAD.
    """
    if "dirty" in descriptor_version:
        raise ReleaseError(
            f"Embedded app descriptor version '{descriptor_version}' is dirty; "
            "refusing to package a build made from an uncommitted tree."
        )
    if descriptor_version != expected_describe:
        raise ReleaseError(
            "Embedded app descriptor version does not match current HEAD -- "
            "this is exactly the stale-descriptor bug this script exists to "
            f"catch.\n  embedded:  {descriptor_version!r}\n"
            f"  expected:  {expected_describe!r}\n"
            "Investigate project descriptor compilation before shipping."
        )



# HttpDownloader.cpp builds its HTTP User-Agent as the adjacent string-literal
# concatenation `"CrossFade-ESP32-" CROSSPOINT_VERSION`, which the compiler
# folds into one combined, null-terminated .rodata literal. Anchoring the
# binary check to this specific, distinctive prefix (rather than searching
# for the bare version number anywhere in the image) is deliberate: the ESP
# app descriptor's own `version` field is an *unrelated* git-describe string
# (see AppDescriptor/parse_app_descriptor above) that, once this repo grows a
# "v1.3.3" tag, will legitimately contain substrings like "1.3.3-1-g<sha>" --
# a bare `"1.3.3-"` substring search would misfire on that and reject an
# otherwise-correct build. "CrossFade-ESP32-1.3.3-..." never occurs for a
# correctly-built plain version, so this cannot collide with it.
RUNTIME_VERSION_TAG_PREFIX = "CrossFade-ESP32-"


def check_runtime_version_embedded(binary: bytes, plain_version: str) -> None:
    """Raise ReleaseError unless `plain_version` (e.g. "1.3.3") is embedded as
    an exact, unsuffixed CROSSPOINT_VERSION C string literal, anchored via the
    `CrossFade-ESP32-` User-Agent prefix (src/network/HttpDownloader.cpp) so a
    same-prefix substring from an unrelated part of the image (e.g. the ESP
    app descriptor's own git-describe version field) cannot cause a false
    positive or false negative.
    """
    versions = set(re.findall(rb"CrossFade-ESP32-([^\x00]*)\x00", binary))
    if versions != {plain_version.encode("ascii")}:
        raise ReleaseError(f"Runtime version literals do not exactly match {plain_version!r}: {versions!r}")


def validate_x3_zh_tw_build_flag(project_dir: Path) -> None:
    """Statically check platformio.ini's own text (not the binary): the
    x3_zh_tw_custom env must define CROSSPOINT_VERSION as exactly the plain,
    unsuffixed `${crosspoint.version}` macro. This is an independent,
    source-level guard against a regression in the ini itself -- catching it
    here fails fast, before spending minutes on a build the binary-level
    check would reject anyway.
    """
    ini_path = project_dir / "platformio.ini"
    if not ini_path.is_file():
        raise ReleaseError(f"platformio.ini not found at {ini_path}")
    text = ini_path.read_text(encoding="utf-8")
    section_match = re.search(
        rf"^\[env:{re.escape(ENV_NAME)}\]\n(.*?)(?=^\[|\Z)",
        text,
        re.MULTILINE | re.DOTALL,
    )
    if not section_match:
        raise ReleaseError(f"[env:{ENV_NAME}] section not found in platformio.ini")
    section_text = section_match.group(1)
    version_lines = [
        line.strip()
        for line in section_text.splitlines()
        if line.strip().startswith("-DCROSSPOINT_VERSION=")
    ]
    expected = '-DCROSSPOINT_VERSION=\\"${crosspoint.version}\\"'
    if not version_lines:
        raise ReleaseError(
            f"No CROSSPOINT_VERSION build flag found in [env:{ENV_NAME}] in "
            "platformio.ini."
        )
    if len(version_lines) > 1:
        raise ReleaseError(
            f"Multiple CROSSPOINT_VERSION build flags found in "
            f"[env:{ENV_NAME}]: {version_lines!r}"
        )
    if version_lines[0] != expected:
        raise ReleaseError(
            f"[env:{ENV_NAME}]'s CROSSPOINT_VERSION build flag is "
            f"{version_lines[0]!r}, expected exactly {expected!r} (the plain "
            "product version, with no locale/device suffix)."
        )


def extract_short_sha(describe_output: str) -> Optional[str]:
    """Pull the short SHA out of a `git describe --always --tags --dirty`
    string, whether it's a tag-relative form ("v1.3.2-25-gb9840cc[-dirty]")
    or the bare `--always` fallback ("b9840cc[-dirty]")."""
    tag_relative = re.match(r"^.*-g([0-9a-f]{4,40})(?:-dirty)?$", describe_output)
    if tag_relative:
        return tag_relative.group(1)
    bare = re.match(r"^([0-9a-f]{4,40})(?:-dirty)?$", describe_output)
    if bare:
        return bare.group(1)
    return None


def read_crosspoint_version(project_dir: Path) -> str:
    ini_path = project_dir / "platformio.ini"
    if not ini_path.is_file():
        raise ReleaseError(f"platformio.ini not found at {ini_path}")
    config = configparser.ConfigParser()
    config.read(ini_path, encoding="utf-8")
    if not config.has_option("crosspoint", "version"):
        raise ReleaseError("No [crosspoint] version option in platformio.ini")
    return config.get("crosspoint", "version").strip()


# The only untracked path allowed to exist during a release build: this
# repo's own pre-existing test/build_host/ CMake build-output tree. It is
# untracked but deliberately *not* gitignored (it's the user's own local
# host-test build directory), so it always shows up in `git status`; it is
# also structurally incapable of feeding into any PlatformIO env's build
# (nothing under test/ is a build input), unlike an untracked source or
# config file dropped anywhere else in the tree.
ALLOWED_UNTRACKED_EXACT = ("test/build_host",)
ALLOWED_UNTRACKED_PREFIXES = ("test/build_host/",)


def _is_allowed_untracked_path(path: str) -> bool:
    return path in ALLOWED_UNTRACKED_EXACT or any(
        path.startswith(prefix) for prefix in ALLOWED_UNTRACKED_PREFIXES
    )


def parse_porcelain_z(output: str) -> list:
    """Parse `git status --porcelain=v1 -z` output into a list of
    {"xy": str, "path": str, "orig_path": Optional[str]} dicts.

    -z is used deliberately instead of plain --porcelain: v1's default output
    quotes/escapes paths containing spaces, unicode, or other special
    characters (making naive line-splitting on " -> " or fixed offsets
    unreliable), while -z prints each path verbatim, NUL-terminated, and
    represents a rename/copy as two consecutive NUL-terminated fields (new
    path, then original path) instead of embedding an arrow in the text.
    """
    tokens = iter(output.split("\x00"))
    entries = []
    for token in tokens:
        if not token:
            continue
        xy = token[:2]
        path = token[3:]
        orig_path = None
        if "R" in xy or "C" in xy:
            orig_path = next(tokens, None)
            if not orig_path:
                raise ReleaseError("Truncated rename/copy record in Git status")
        entries.append({"xy": xy, "path": path, "orig_path": orig_path})
    return entries


def find_release_blocking_paths(entries: list) -> list:
    """Given parsed porcelain=v1 -z entries (tracked changes, staged changes,
    and untracked files alike -- this deliberately does not exempt untracked
    files in general, only the specific allowed path below), return every
    path that must block a release build."""
    blocking = []
    for entry in entries:
        candidates = [entry["path"]]
        if entry["orig_path"] is not None:
            candidates.append(entry["orig_path"])
        for candidate in candidates:
            if candidate and not (entry["xy"] == "??" and _is_allowed_untracked_path(candidate)):
                blocking.append(candidate)
    return blocking


def artifact_basename(version: str, short_sha: str) -> str:
    return f"crossfade-x3-zh-tw-v{version}-{short_sha}"


CHECKSUM_LINE_RE = re.compile(r"([0-9a-f]{64})  firmware\.bin")


def verify_release_tarball(tar_path: Path, expected_firmware_sha256: str) -> None:
    """Re-open the just-written tarball from disk (never trust the in-memory
    bytes used to build it) and verify it end to end: it contains exactly
    firmware.bin and CHECKSUMS.sha256 -- both regular files, nothing else,
    no path traversal or unexpected member types -- and the checksum file's
    recorded hash matches both firmware.bin's actual extracted bytes and the
    hash computed before packaging.
    """
    if not tar_path.is_file():
        raise ReleaseError(f"Release tarball missing: {tar_path}")
    with tarfile.open(tar_path, "r:gz") as tar:
        members = tar.getmembers()
        names = sorted(m.name for m in members)
        if names != ["CHECKSUMS.sha256", "firmware.bin"]:
            raise ReleaseError(
                f"Release tarball {tar_path} must contain exactly "
                f"firmware.bin and CHECKSUMS.sha256; found {names!r}."
            )
        for member in members:
            if not member.isfile():
                raise ReleaseError(
                    f"Release tarball {tar_path} member {member.name!r} is "
                    f"not a regular file (type code {member.type!r})."
                )
        firmware_fileobj = tar.extractfile("firmware.bin")
        checksums_fileobj = tar.extractfile("CHECKSUMS.sha256")
        if firmware_fileobj is None or checksums_fileobj is None:
            raise ReleaseError(
                f"Release tarball {tar_path}: could not extract member data."
            )
        firmware_bytes = firmware_fileobj.read()
        checksums_text = checksums_fileobj.read().decode("utf-8", errors="replace")

    checksum_lines = [line for line in checksums_text.splitlines() if line.strip()]
    if len(checksum_lines) != 1:
        raise ReleaseError(
            f"CHECKSUMS.sha256 inside {tar_path} must have exactly one "
            f"line; found {len(checksum_lines)}: {checksum_lines!r}"
        )
    match = CHECKSUM_LINE_RE.fullmatch(checksum_lines[0])
    if not match:
        raise ReleaseError(
            f"CHECKSUMS.sha256 inside {tar_path} has unexpected format: "
            f"{checksum_lines[0]!r}"
        )
    recorded_sha256 = match.group(1)
    actual_sha256 = hashlib.sha256(firmware_bytes).hexdigest()
    if actual_sha256 != recorded_sha256:
        raise ReleaseError(
            f"firmware.bin inside {tar_path} does not match its own "
            f"recorded checksum.\n  recorded: {recorded_sha256}\n"
            f"  actual:   {actual_sha256}"
        )
    if actual_sha256 != expected_firmware_sha256:
        raise ReleaseError(
            f"firmware.bin inside {tar_path} does not match the firmware "
            "that was validated before packaging.\n"
            f"  expected: {expected_firmware_sha256}\n"
            f"  actual:   {actual_sha256}"
        )


SIZE_RAM_RE = re.compile(r"RAM:.*?(\d+)\s+bytes\s+from\s+(\d+)\s+bytes")
SIZE_FLASH_RE = re.compile(r"Flash:.*?(\d+)\s+bytes\s+from\s+(\d+)\s+bytes")


def parse_size_summary(build_output: str) -> Optional[dict]:
    """Best-effort extraction of PlatformIO's RAM/Flash usage lines.

    The build log for this env contains more than one such summary (a
    bootloader.elf size check, and -- because of the custom_sdkconfig
    two-pass mechanism, visible in the log as "Original Arduino
    idf_component.yml restored" -- potentially more than one app build
    pass too). The *last* RAM line and *last* Flash line in the log
    correspond to the final firmware.bin actually produced, so this takes
    the last match of each rather than the first. Returns None if no
    matching lines are present at all (older pio output, -s builds).
    """
    ram_matches = list(SIZE_RAM_RE.finditer(build_output))
    flash_matches = list(SIZE_FLASH_RE.finditer(build_output))
    if not ram_matches or not flash_matches:
        return None
    ram = ram_matches[-1]
    flash = flash_matches[-1]
    return {
        "ram_used": int(ram.group(1)),
        "ram_total": int(ram.group(2)),
        "flash_used": int(flash.group(1)),
        "flash_total": int(flash.group(2)),
    }


# --------------------------------------------------------------------------
# Process / filesystem plumbing
# --------------------------------------------------------------------------


def run_git(args: list, cwd: Path) -> str:
    result = subprocess.run(
        ["git", *args], cwd=cwd, capture_output=True, text=True
    )
    if result.returncode != 0:
        raise ReleaseError(
            f"git {' '.join(args)} failed (exit {result.returncode}): "
            f"{result.stderr.strip()}"
        )
    return result.stdout.strip()


def find_pio() -> str:
    on_path = shutil.which("pio")
    if on_path:
        return on_path
    candidates = [
        Path.home() / ".platformio" / "penv" / "bin" / "pio",
        Path.home() / ".platformio" / "penv" / "Scripts" / "pio.exe",
    ]
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    raise ReleaseError(
        "Could not locate the 'pio' executable.\n"
        "  Checked: PATH, and the standard PlatformIO venv locations:\n"
        + "\n".join(f"    {c}" for c in candidates)
        + "\nInstall PlatformIO (`pip install platformio`) or add it to PATH."
    )


def require_clean_release_tree(repo_root: Path) -> None:
    """Refuse to build a release unless the tree is completely clean: no
    staged or unstaged changes to tracked files, AND no untracked files
    either -- an untracked source or config file can change PlatformIO's
    build output just as much as a tracked one, so this cannot simply
    exempt untracked paths in general (`--untracked-files=no` would). The
    sole exception is this repo's own pre-existing test/build_host/ tree
    (see ALLOWED_UNTRACKED_* above)."""
    result = subprocess.run(
        ["git", "status", "--porcelain=v1", "-z", "--untracked-files=all", "--ignore-submodules=none"],
        cwd=repo_root,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise ReleaseError(f"git status failed: {result.stderr.strip()}")
    entries = parse_porcelain_z(result.stdout)
    blocking = find_release_blocking_paths(entries)
    if blocking:
        raise ReleaseError(
            "Release input is not clean; refusing to build. The following "
            "paths are uncommitted, staged, or untracked (only "
            "test/build_host/ is allowed to be untracked):\n"
            + "\n".join(f"  {p}" for p in sorted(set(blocking)))
        )


def release_snapshot(root):
    require_clean_release_tree(root)
    head = run_git(["rev-parse", "HEAD"], root)
    describe = run_git(["describe", "--always", "--tags", "--dirty"], root)
    if run_git(["rev-parse", describe + "^{commit}"], root) != head:
        raise ReleaseError("Git describe does not resolve to build-start HEAD")
    pointer = run_git(["rev-parse", "HEAD:freeink-sdk"], root)
    checkout = run_git(["rev-parse", "HEAD"], root / "freeink-sdk")
    if checkout != pointer:
        raise ReleaseError("freeink-sdk checkout differs from recorded pointer")
    require_clean_release_tree(root / "freeink-sdk")
    submodules = run_git(["submodule", "status", "--recursive"], root)
    if any(line.startswith(("+", "-", "U")) for line in submodules.splitlines()):
        raise ReleaseError("Submodule checkout mismatch or uninitialized submodule")
    config_paths = set(root.glob("platformio*.ini")) | set(root.glob("sdkconfig*"))
    configs = {str(p.relative_to(root)): sha(p) for p in config_paths if p.is_file()}
    return dict(head=head, describe=describe, pointer=pointer, checkout=checkout,
                submodules=submodules, configs=configs)


def verify_snapshot(root, expected):
    if release_snapshot(root) != expected:
        raise ReleaseError("Root HEAD/config/submodule changed during build")


def run_build(pio, project, environment, root, snapshot, log_path):
    start = time.monotonic()
    failure = []
    stop = threading.Event()
    def monitor():
        while not stop.wait(1):
            try:
                verify_snapshot(root, snapshot)
            except Exception as error:
                failure.append(error)
                os.killpg(proc.pid, signal.SIGTERM)
                return
    command = protected_command([pio, "run", "-e", ENV_NAME],
                                [Path.home() / ".platformio", root / "freeink-sdk"])
    proc = subprocess.Popen(command, cwd=project, env=environment,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, bufsize=1, start_new_session=True)
    watcher = threading.Thread(target=monitor, daemon=True)
    watcher.start()
    lines = []
    try:
        with log_path.open("w") as log:
            for line in proc.stdout:
                print(line, end="", flush=True)
                log.write(line)
                lines.append(line)
        proc.wait()
    finally:
        stop.set()
        watcher.join()
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGTERM)
            proc.wait()
        proc.stdout.close()
    if failure:
        raise ReleaseError(f"Release inputs changed during build: {failure[0]}")
    verify_snapshot(root, snapshot)
    if proc.returncode:
        raise ReleaseError(f"pio run failed (exit {proc.returncode}); see {log_path}")
    elapsed = time.monotonic() - start
    print(f"[release] Build elapsed: {elapsed:.2f}s")
    return "".join(lines), elapsed


def verify_firmware(binary, elf, snapshot, version, nm):
    descriptor = parse_app_descriptor(binary)
    validate_descriptor_version(descriptor.version, snapshot["describe"])
    check_runtime_version_embedded(binary, version)
    if binary[32 + 144:32 + 176] != hashlib.sha256(elf.read_bytes()).digest():
        raise ReleaseError("Embedded ELF SHA256 does not match firmware.elf")
    if binary.count(f"CrossFade-HEAD:{snapshot['head']}".encode() + b"\0") != 1:
        raise ReleaseError("Firmware full HEAD identity does not match build-start HEAD")
    symbols = subprocess.check_output([str(nm), "-g", "--defined-only", str(elf)], text=True)
    matches = re.findall(r"^[0-9a-f]+ ([A-Za-z]) esp_app_desc$", symbols, re.M)
    if matches != ["R"]:
        raise ReleaseError(f"Expected exactly one strong project esp_app_desc, got {matches}")
    return descriptor


def package_firmware(tar_path, binary):
    checksum = hashlib.sha256(binary).hexdigest()
    with tar_path.open("wb") as output:
        with gzip.GzipFile(filename="", mode="wb", fileobj=output, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.USTAR_FORMAT) as tar:
                for name, content in (("firmware.bin", binary),
                                      ("CHECKSUMS.sha256", f"{checksum}  firmware.bin\n".encode())):
                    info = tarfile.TarInfo(name)
                    info.size, info.mode, info.mtime = len(content), 0o644, 0
                    info.uid = info.gid = 0
                    info.uname = info.gname = ""
                    tar.addfile(info, io.BytesIO(content))
    verify_release_tarball(tar_path, checksum)


def build_release(root, cold=False):
    started = time.monotonic()
    snapshot = release_snapshot(root)
    version = read_crosspoint_version(root)
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise ReleaseError("Product version must be plain X.Y.Z")
    validate_x3_zh_tw_build_flag(root)
    pio = find_pio()
    config = effective_config(root, pio)
    cache = SdkCache(root, pio, config)
    print("[release] Hashing SDK inputs and cached artifacts", flush=True)
    inputs = cache.inputs()
    key = digest(inputs)
    manifest = cache.load()
    cold = cold or not trusted(manifest, key, cache.artifacts())
    environment = cache.environment()
    environment.update(CROSSFADE_RELEASE_ROOT=str(root), CROSSFADE_RELEASE_HEAD=snapshot["head"])
    sdk_elapsed = 0
    if cold:
        print("[release] No matching trusted SDK: controlled cold rebuild", flush=True)
        cache.prepare_cold()
        output, sdk_elapsed = run_build(pio, cache.project, environment, root, snapshot,
                                        cache.state / "cold.log")
        if "Copied compiled" not in output or "IDF libraries to Arduino framework" not in output:
            raise ReleaseError("Cold build did not prove custom SDK compilation/copy completed")
    else:
        print("[release] Verified SDK manifest: warm incremental build", flush=True)
    cache.prepare_app()
    output, app_elapsed = run_build(pio, cache.project, environment, root, snapshot,
                                    cache.state / "app.log")
    build_dir = cache.project / ".pio/build" / ENV_NAME
    binary = (build_dir / "firmware.bin").read_bytes()
    nm = cache.core / "packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-nm"
    descriptor = verify_firmware(binary, build_dir / "firmware.elf", snapshot, version, nm)
    verify_snapshot(root, snapshot)
    if effective_config(root, pio) != config or cache.inputs() != inputs:
        raise ReleaseError("Effective configuration or SDK inputs changed during build")
    if cold:
        cache.seal(key, inputs)
    elif not trusted(manifest, key, cache.artifacts()):
        cache.manifest_path.unlink(missing_ok=True)
        raise ReleaseError("SDK artifacts changed during warm build; refusing package")
    # Bind generated/patched sources and the effective app configuration to this binary.
    source_inputs = ota_sources.capture(cache)
    cache.digests.save()
    dist = root / "dist"
    dist.mkdir(exist_ok=True)
    short = snapshot["head"][:7]
    tar_path = dist / (artifact_basename(version, short) + ".tar.gz")
    package_firmware(tar_path, binary)
    verify_snapshot(root, snapshot)
    report = dict(commit=snapshot["head"], describe=descriptor.version,
                  runtime_version=version, sdk_key=key, cold=cold,
                  sdk_seconds=sdk_elapsed, app_seconds=app_elapsed,
                  total_seconds=round(time.monotonic() - started, 2),
                  source_inputs=source_inputs,
                  firmware_sha256=hashlib.sha256(binary).hexdigest(),
                  tar_sha256=sha(tar_path), artifact=str(tar_path), size=parse_size_summary(output))
    (cache.state / "last-release.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cold", action="store_true", help="Rebuild the private SDK from seed inputs")
    args = parser.parse_args()
    state = REPO_ROOT / ".release-sdk"
    state.mkdir(exist_ok=True)
    try:
        with (state / "release.lock").open("w") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise ReleaseError("Another release build owns the project SDK cache")
            build_release(REPO_ROOT, args.cold)
        return 0
    except (ReleaseError, RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print(f"[release] ERROR: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
