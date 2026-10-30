#!/usr/bin/env python3
"""Generate, sign and verify the custom X3 OTA manifest (lib/FirmwareUpdate/OtaManifest.h).

    # one-time: create the release signing key (private key never leaves this path)
    make_ota_manifest.py generate-key ~/.config/crossfade-ota/ota-signing-key.pem
    # print the C header carrying the PUBLIC key (commit as lib/FirmwareUpdate/OtaPublicKey.h)
    make_ota_manifest.py pubkey-header KEY.pem
    # per release, after scripts/release_x3_zh_tw.py produced firmware.bin
    make_ota_manifest.py sign --firmware firmware.bin --key KEY.pem --tag x3-b2 --out manifest.json
    make_ota_manifest.py verify --firmware firmware.bin --manifest manifest.json

Canonical signed payload (bytes, UTF-8/ASCII, each line ends with "\\n"):

    crossfade-ota-v1
    channel=<channel>
    target=<target>
    version=<version>
    build=<decimal>
    size=<decimal>
    sha256=<64 lowercase hex>
    url=<https://github.com/<repo>/releases/download/<tag>/firmware.bin>

Signature: ECDSA P-256 over SHA-256 of that payload, stored as "sig": 128 lowercase hex (r||s,
each 32 bytes big-endian). The device rebuilds the payload from the parsed JSON fields and
verifies it with the public key compiled into the firmware; JSON formatting is never signed.

channel / target / build / version are READ FROM firmware.bin (the CrossFade-OTA/... User-Agent
literal and the CrossFade-ESP32-<version> literal), never typed by hand, so a manifest cannot
describe a different build than the image it points to. Uses only the openssl CLI.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
PUBKEY_HEADER = REPO_ROOT / "lib" / "FirmwareUpdate" / "OtaPublicKey.h"
DEFAULT_REPO = "SemonCat/BookOrbit-X3"
MAX_MANIFEST_BYTES = 2048
MAX_URL_LEN = 256
ESP_MAGIC = 0xE9
ESP32C3_CHIP_ID = 0x0005
MIN_IMAGE = 64 * 1024
OTA_PARTITION = 0x640000  # partitions.csv app0/app1
# SubjectPublicKeyInfo DER prefix for id-ecPublicKey + prime256v1, followed by the 65-byte point.
P256_SPKI_PREFIX = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d030107034200")
P256_N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551

IDENTITY_RE = re.compile(rb"CrossFade-OTA/([a-z0-9-]{1,32})/([A-Za-z0-9-]{1,32})/build=([0-9]{1,10})\x00")
VERSION_RE = re.compile(rb"CrossFade-ESP32-([^\x00]*)\x00")
FIELD_RE = re.compile(r"^[A-Za-z0-9._+-]{1,32}$")
TAG_RE = re.compile(r"^[A-Za-z0-9._-]+$")
REPO_RE = re.compile(r"^[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$")


class ManifestError(Exception):
    pass


def openssl(*args: str, data: bytes | None = None) -> bytes:
    result = subprocess.run(["openssl", *args], input=data, capture_output=True, check=False)
    if result.returncode != 0:
        raise ManifestError(f"openssl {args[0]} failed: {result.stderr.decode(errors='replace').strip()}")
    return result.stdout


# ---------------------------------------------------------------- pure helpers (unit-tested)


def read_identity(firmware: bytes) -> dict:
    if len(firmware) < MIN_IMAGE or len(firmware) > OTA_PARTITION:
        raise ManifestError(f"firmware size {len(firmware)} outside [{MIN_IMAGE}, {OTA_PARTITION}]")
    if firmware[0] != ESP_MAGIC:
        raise ManifestError("firmware.bin is not an ESP app image (magic)")
    chip = int.from_bytes(firmware[12:14], "little")
    if chip != ESP32C3_CHIP_ID:
        raise ManifestError(f"firmware chip_id 0x{chip:04x} is not ESP32-C3")
    identities = set(IDENTITY_RE.findall(firmware))
    if len(identities) != 1:
        raise ManifestError(f"expected exactly one CrossFade-OTA identity literal, found {len(identities)}")
    channel, target, build = (x.decode("ascii") for x in identities.pop())
    if channel not in ("bookorbit-x3", "orbitink-x3-nightly") or target != "x3-zh-TW":
        raise ManifestError("firmware is not the custom BookOrbit X3 zh-TW channel")
    versions = set(VERSION_RE.findall(firmware))
    if len(versions) != 1:
        raise ManifestError(f"expected exactly one CrossFade-ESP32 version literal, found {versions!r}")
    version = versions.pop().decode("ascii")
    if not FIELD_RE.match(version):
        raise ManifestError(f"version {version!r} has characters the manifest does not allow")
    if not 1 <= int(build) <= 0xFFFFFFFF:
        raise ManifestError("firmware has no OTA build number (CROSSFADE_OTA_BUILD < 1)")
    return {"channel": channel, "target": target, "version": version, "build": int(build)}


def release_url(repo: str, tag: str) -> str:
    if not REPO_RE.match(repo) or not TAG_RE.match(tag) or tag in (".", ".."):
        raise ManifestError("repo/tag contain characters the device URL policy rejects")
    url = f"https://github.com/{repo}/releases/download/{tag}/firmware.bin"
    if len(url) > MAX_URL_LEN:
        raise ManifestError("firmware URL too long")
    return url


def canonical_payload(m: dict) -> bytes:
    for key in ("channel", "target", "version"):
        if not FIELD_RE.match(m[key]):
            raise ManifestError(f"{key} {m[key]!r} not allowed")
    if not re.fullmatch(r"[0-9a-f]{64}", m["sha256"]):
        raise ManifestError("sha256 must be 64 lowercase hex")
    return (
        "crossfade-ota-v1\n"
        f"channel={m['channel']}\ntarget={m['target']}\nversion={m['version']}\n"
        f"build={int(m['build'])}\nsize={int(m['size'])}\nsha256={m['sha256']}\nurl={m['url']}\n"
    ).encode("ascii")


def der_to_raw(der: bytes) -> bytes:
    """ECDSA-Sig-Value DER -> r||s (64 bytes)."""
    def need(i, n=1):
        if i + n > len(der):
            raise ManifestError("truncated DER signature")

    def read_len(buf, i):
        need(i)
        n = buf[i]
        i += 1
        if n & 0x80:
            k = n & 0x7F
            if k == 0 or k > 2:
                raise ManifestError("bad DER length")
            need(i, k)
            n = int.from_bytes(buf[i:i + k], "big")
            i += k
        return n, i

    if not der or der[0] != 0x30:
        raise ManifestError("signature is not a DER SEQUENCE")
    seq_len, i = read_len(der, 1)
    if i + seq_len != len(der):
        raise ManifestError("DER signature length mismatch")
    out = b""
    for _ in range(2):
        need(i)
        if der[i] != 0x02:
            raise ManifestError("DER INTEGER expected")
        n, i = read_len(der, i + 1)
        need(i, n)
        value = int.from_bytes(der[i:i + n], "big")
        i += n
        if not 0 < value < P256_N:
            raise ManifestError("signature scalar out of range")
        out += value.to_bytes(32, "big")
    if i != len(der):
        raise ManifestError("trailing bytes after DER signature")
    return out


def raw_to_der(raw: bytes) -> bytes:
    def integer(b: bytes) -> bytes:
        b = b.lstrip(b"\x00") or b"\x00"
        if b[0] & 0x80:
            b = b"\x00" + b
        return bytes([0x02, len(b)]) + b
    body = integer(raw[:32]) + integer(raw[32:])
    return bytes([0x30, len(body)]) + body


def public_point(key_path: Path, private: bool) -> bytes:
    args = ["pkey", "-in", str(key_path), "-pubout", "-outform", "DER"]
    if not private:
        args.insert(1, "-pubin")
    spki = openssl(*args)
    if not spki.startswith(P256_SPKI_PREFIX) or len(spki) != len(P256_SPKI_PREFIX) + 65 or spki[-65] != 0x04:
        raise ManifestError("key is not an uncompressed ECDSA P-256 key")
    return spki[-65:]


def pubkey_fingerprint(point: bytes) -> str:
    return hashlib.sha256(point).hexdigest()


def header_point(header_text: str) -> bytes:
    body = header_text.split("OTA_PUBLIC_KEY[", 1)[1].split("};", 1)[0]
    values = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body)]
    if len(values) != 65:
        raise ManifestError("OtaPublicKey.h does not hold a 65-byte point")
    return bytes(values)


def pubkey_header(point: bytes) -> str:
    rows = [", ".join(f"0x{b:02x}" for b in point[i:i + 13]) for i in range(0, 65, 13)]
    body = ",\n    ".join(rows)
    return (
        "#pragma once\n\n#include <cstdint>\n\n"
        "// Public verification key for the custom OTA channel (ECDSA P-256, uncompressed SEC1 point).\n"
        "// Generated by scripts/make_ota_manifest.py pubkey-header; the private key is held by the\n"
        "// release owner outside this repository and is never part of any build.\n"
        f"// SHA-256 fingerprint: {pubkey_fingerprint(point)}\n"
        "namespace ota_public_key {\n"
        f"inline constexpr uint8_t OTA_PUBLIC_KEY[65] = {{\n    {body}}};\n"
        "}  // namespace ota_public_key\n"
    )


def build_manifest(firmware: bytes, repo: str, tag: str) -> dict:
    ident = read_identity(firmware)
    return {
        "schema": 1,
        **ident,
        "size": len(firmware),
        "sha256": hashlib.sha256(firmware).hexdigest(),
        "url": release_url(repo, tag),
    }


def verify_manifest(manifest: dict, firmware: bytes, point: bytes) -> None:
    expected = build_manifest(firmware, manifest_repo(manifest["url"]), manifest_tag(manifest["url"]))
    for key, value in expected.items():
        if manifest.get(key) != value:
            raise ManifestError(f"manifest {key}={manifest.get(key)!r} does not match firmware ({value!r})")
    sig = manifest.get("sig", "")
    if not re.fullmatch(r"[0-9a-f]{128}", sig):
        raise ManifestError("sig must be 128 lowercase hex")
    with tempfile.TemporaryDirectory() as tmp:
        pub = Path(tmp) / "pub.der"
        pub.write_bytes(P256_SPKI_PREFIX + point)
        payload = Path(tmp) / "payload"
        payload.write_bytes(canonical_payload(manifest))
        der = Path(tmp) / "sig.der"
        der.write_bytes(raw_to_der(bytes.fromhex(sig)))
        openssl("dgst", "-sha256", "-verify", str(pub), "-keyform", "DER", "-signature", str(der), str(payload))
    if len(serialize(manifest)) > MAX_MANIFEST_BYTES:
        raise ManifestError("manifest exceeds the device's 2048-byte limit")


def manifest_repo(url: str) -> str:
    m = re.fullmatch(r"https://github\.com/([^/]+/[^/]+)/releases/download/([^/]+)/firmware\.bin", url)
    if not m:
        raise ManifestError(f"unexpected firmware url {url!r}")
    return m.group(1)


def manifest_tag(url: str) -> str:
    manifest_repo(url)
    return url.rsplit("/", 2)[-2]


def serialize(manifest: dict) -> bytes:
    return (json.dumps(manifest, separators=(",", ":"), sort_keys=False) + "\n").encode("ascii")


# ---------------------------------------------------------------- commands


def cmd_generate_key(args) -> None:
    path = Path(args.path).expanduser()
    if path.exists():
        raise ManifestError(f"{path} exists; refusing to overwrite a signing key")
    path.parent.mkdir(parents=True, exist_ok=True)
    os.chmod(path.parent, 0o700)
    old = os.umask(0o077)
    try:
        openssl("genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256", "-out", str(path))
    finally:
        os.umask(old)
    os.chmod(path, 0o600)
    print(f"key: {path} (mode 0600; contents not printed)")
    print(f"public key sha256 fingerprint: {pubkey_fingerprint(public_point(path, private=True))}")


def cmd_pubkey_header(args) -> None:
    sys.stdout.write(pubkey_header(public_point(Path(args.key).expanduser(), private=True)))


def cmd_sign(args) -> None:
    firmware = Path(args.firmware).read_bytes()
    key = Path(args.key).expanduser()
    point = public_point(key, private=True)
    committed = header_point(Path(args.pubkey_header).read_text())
    if point != committed:
        raise ManifestError("signing key does not match the public key compiled into the firmware")
    manifest = build_manifest(firmware, args.repo, args.tag)
    with tempfile.TemporaryDirectory() as tmp:
        payload = Path(tmp) / "payload"
        payload.write_bytes(canonical_payload(manifest))
        der = openssl("dgst", "-sha256", "-sign", str(key), str(payload))
    manifest["sig"] = der_to_raw(der).hex()
    verify_manifest(manifest, firmware, committed)
    out = Path(args.out)
    out.write_bytes(serialize(manifest))
    print(f"manifest: {out} build={manifest['build']} size={manifest['size']} sha256={manifest['sha256']}")


def cmd_verify(args) -> None:
    firmware = Path(args.firmware).read_bytes()
    raw = Path(args.manifest).read_bytes()
    if len(raw) > MAX_MANIFEST_BYTES:
        raise ManifestError("manifest exceeds the device's 2048-byte limit")
    manifest = json.loads(raw)
    point = header_point(Path(args.pubkey_header).read_text())
    verify_manifest(manifest, firmware, point)
    print(f"OK build={manifest['build']} sha256={manifest['sha256']} key={pubkey_fingerprint(point)}")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("generate-key")
    g.add_argument("path")
    g.set_defaults(func=cmd_generate_key)
    h = sub.add_parser("pubkey-header")
    h.add_argument("key")
    h.set_defaults(func=cmd_pubkey_header)
    s = sub.add_parser("sign")
    s.add_argument("--firmware", required=True)
    s.add_argument("--key", required=True)
    s.add_argument("--tag", required=True)
    s.add_argument("--repo", default=DEFAULT_REPO)
    s.add_argument("--out", required=True)
    s.add_argument("--pubkey-header", default=str(PUBKEY_HEADER))
    s.set_defaults(func=cmd_sign)
    v = sub.add_parser("verify")
    v.add_argument("--firmware", required=True)
    v.add_argument("--manifest", required=True)
    v.add_argument("--pubkey-header", default=str(PUBKEY_HEADER))
    v.set_defaults(func=cmd_verify)
    args = parser.parse_args(argv)
    try:
        args.func(args)
    except ManifestError as exc:
        print(f"make_ota_manifest: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
