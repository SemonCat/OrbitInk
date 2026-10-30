# OrbitInk update channels

OrbitInk is BookOrbit firmware for Xteink X3. This tree derives from CrossFade,
a CrossPoint fork, and also contains the merged CrossInk v1.6.1 base:
`a9898b6c85acfa7e5dbda9ebe255d3f23571afeb` merges
`9914146eeae7b46b300f475a16c32426fc02ec1f`. CrossInk is another
CrossPoint fork. Preserve LICENSE, NOTICE and component notices; the root MIT
license does not replace wolfSSL's GPL obligations.

The combined binary is distributed under GPLv3, selecting the later-version
option in the actual wolfSSL LICENSING file, compatible with the linked
Apache-2.0 JPEGDEC/PNGdec code. LICENSES/COPYING.GPL3 contains the actual GNU
GPLv3 text. Individual source/component licenses and copyright notices remain
in place, including WebSockets LGPL-2.1 and built-in font licenses. Parent must
inspect the full guarded corresponding-source and license set before release.
Installing a locally modified image uses the existing manual SD/web .bin path
and physical confirmation; the release signing private key is not needed for
that path. Hardware bootloader/recovery limitations still require acceptance.

## Device contract

Settings / System / OTA channel persists `otaChannel`: 0 stable (default),
1 nightly (experimental). Missing, invalid, negative, nonnumeric and oversized
stored values select stable. The setting does not flash or fetch anything.
The user must still select Check for updates and confirm installation.
Books, settings and existing fonts stay on SD.

Stable keeps the signed identity `bookorbit-x3`, target `x3-zh-TW`, canonical
signature format `crossfade-ota-v1` and the existing P-256 public key.
Nightly uses `orbitink-x3-nightly`, the same target and key. The signed
channel must exactly match the selected channel. Signing reads channel/build
from the binary; no command-line channel can relabel an image.

Same-channel updates require a strictly higher build. Explicitly selecting
the other channel allows its signed build even when older; the physical
confirmation then says it installs an older channel build and defaults to
Cancel. This exception uses the running image's compiled channel, never a
persistent maximum-nightly counter. After switching back to stable, ordinary
stable ordering applies again. No automatic updates or hardware antirollback
are claimed. A replayed valid older manifest on a selected different channel
can be offered until the user installs that channel; intentional downgrade
support necessarily permits this, with size/hash/signature and confirmation
still mandatory. Signed manifests authenticate downloads; existing TLS peer
verification remains disabled and is not claimed as authentication.

Stable endpoint remains
`https://github.com/SemonCat/BookOrbit-X3/releases/latest/download/manifest.json`.
Nightly endpoint is
`https://github.com/SemonCat/OrbitInk/releases/download/nightly/manifest.json`.
All newly signed firmware URLs intentionally retain
`https://github.com/SemonCat/BookOrbit-X3/releases/download/<tag>/firmware.bin`.
Installed build 1 accepts only that legacy URL prefix. A repository rename
redirect stays on the supported github.com/asset-host path; it does not
change signed payload bytes. Never reuse the old repository name for another
repository, which would break redirects. Parent must verify anonymous old-name
manifest/firmware downloads after rename, including signature, size, hash and
the device's four-redirect/1536-byte Location limit. No stock feed fallback.
New firmware can switch to the already published stable build 1; build 1 has
no channel setting and can subsequently update only via the stable endpoint.

## CI and release operations

One `orbitink.yml` workflow owns BOTH channels' build sequence:
`1000 + github.run_number * 100 + github.run_attempt` (attempts 1..99).
Builds must fit the supported bounded sequence; exhaustion fails closed.
Do not reset/rename/recreate this workflow or manually allocate overlapping
builds. An older rerun cannot publish behind any existing `orbitink-bN`
release. Unique immutable tags bind firmware URLs; no published firmware is
overwritten. GitHub jobs can finish out of order. Every main push builds an
unsigned nightly artifact; publication is serialized and a superseded older
build fails closed. GitHub may replace pending concurrency jobs; it does not
guarantee publication of every intermediate push.

Pushes to protected main build nightly. No PR event receives signing secrets.
Stable requires SemonCat's explicit workflow_dispatch, channel stable, the
exact committed runtime version, and confirmation `RELEASE`. No tag-triggered
stable release exists. Stable environment approval is also required by
repository configuration. Owner release requests which change the runtime
version first commit that version normally; CI never silently edits it.
Nightly releases are prereleases and explicitly not latest. Only deliberate
stable releases advance latest, keeping build 1 available until then.

Linux runner bootstrap uses pinned PlatformIO 6.1.18 and the pinned
pioarduino 55.03.37 platform from platformio.ini. `pio pkg install` seeds the
tools before the guarded cold build. The SDK cache physically copies packages
on Linux; macOS sandbox-exec is used only on macOS. The guarded script seals
actual SDK artifacts and corresponding build inputs. CI does not claim Linux
success until a real ubuntu-24.04 job finishes. A macOS development build and
host tests alone are insufficient evidence.

Unsigned build job: Python/host tests, package installation, guarded build,
source capture, relocatable guarded artifact receipt. Release job: checks out
the same commit without persisted credentials, verifies artifact/HEAD/image
and expected allocation, signs with the existing key, verifies that key against
the compiled public key, assembles firmware/source/actual licenses, publishes
immutable assets, then advances the nightly manifest pointer. Signing key
exists only in the isolated signing step's environment and a temporary 0600
file; it is not in build inputs, source, firmware, artifacts or command argv.

Parent prerequisites before enabling Actions:

1. Inspect the clean source snapshot and all applicable dependency/source
   notices. Keep private Git history and local ignored state private.
2. Rename BookOrbit-X3 to OrbitInk, preserving releases, and install inspected
   source as a new snapshot commit on its main. Keep upstream source notices.
3. Protect main against untrusted pushes and workflow edits. Configure
   `orbitink-nightly` and `orbitink-stable` environments restricted to main;
   stable additionally requires owner approval. Provision the SAME existing
   `OTA_SIGNING_KEY_PEM` in both environments by the parent's permitted secret
   input path. Never paste it into commands/chat/logs, generate a replacement,
   or expose it to pull requests.
4. Enable Actions with release-job contents write and artifact permissions.
   Require independent P2 review and parent acceptance before source activation.
5. First run a deliberate nightly dispatch on main and inspect Linux build,
   source licenses and anonymous signature/size/hash. No new stable release is
   authorized by the channel implementation request.

## Public source migration

Use a clean source snapshot on top of the existing public README commit;
do not push the private branch or its history. `scripts/export_orbitink_source.py`
exports an explicit tracked source allowlist at a frozen clean HEAD and records
every member hash and the freeink-sdk gitlink. Agent instructions, private
history, local device/settings files, platformio.local.ini, build outputs and
optional SD fonts are excluded. The export is preparation, not a secret scanner
or publication approval. Parent must inspect the exact archive before pushing.

After extracting the inspected snapshot into the clean public checkout, stage
its regular files and explicitly recreate the SDK gitlink before committing:

```sh
git add -- .gitmodules .gitignore src lib scripts sim test platformio.ini partitions.csv requirements.txt LICENSE LICENSES NOTICE README.md docs .github/workflows PUBLIC-SOURCE-PROVENANCE.json
python3 - <<'PY'
import json, re, subprocess
from pathlib import Path
receipt = json.loads(Path("PUBLIC-SOURCE-PROVENANCE.json").read_text())
sdk = receipt["freeink_sdk_gitlink"]
if not re.fullmatch(r"[0-9a-f]{40}", sdk):
    raise SystemExit("invalid SDK commit")
subprocess.run(["git", "update-index", "--add", "--cacheinfo",
                f"160000,{sdk},freeink-sdk"], check=True, shell=False)
entry = subprocess.check_output(["git", "ls-files", "--stage", "--", "freeink-sdk"], text=True)
if entry != f"160000 {sdk} 0\tfreeink-sdk\n":
    raise SystemExit("SDK gitlink was not staged correctly")
PY
git submodule update --init --recursive
# Inspect staged files, then make the parent's deliberate snapshot commit.
# After committing, verify:
git rev-parse HEAD:freeink-sdk
git submodule status --recursive
```

The committed SDK entry must exactly equal `freeink_sdk_gitlink` from the
inspected receipt. `.gitmodules` alone and copying SDK files as ordinary
files are insufficient. The exporter also emits the exact required index
command in its provenance and console output; the import regression test
reconstructs a fresh repository and verifies its committed mode-160000 entry.
The snapshot also includes the tracked `.gitignore`: CI's local allocation
override, `.pio` and `.release-sdk` state must stay ignored for the guarded
build to accept a fresh checkout. The import test exercises the actual
cleanliness guard with those files and confirms unexpected source is rejected.

The public source repository contains build scripts and the public SDK gitlink.
Each binary release additionally includes its exact product/SDK/dependency/
framework/managed-component/effective-config corresponding-source archive and
actual license notices, verified against that guarded binary. These archives
are still required: the public application tree alone omits patched generated
dependencies and is insufficient corresponding source for the linked firmware.
No optional GuanKiapTsingKhai SD font files are distributed; installed fonts
remain required for the existing Traditional Chinese profile.
