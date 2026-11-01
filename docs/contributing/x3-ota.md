# Custom X3 wireless firmware updates

The X3 zh-TW profile uses a public release-only repository, `SemonCat/BookOrbit-X3`.
The source origin `SemonCat/crossfade` remains private. Neither firmware nor the
release scripts contain a GitHub token. Do not publish the private Git history.

## Manual web update

Open the device's existing web server and follow the Firmware link (`/update`).
The page uploads an application `.bin` to SD staging, validates its ESP32-C3
header, segment layout, checksum and appended hash, then displays size/SHA-256.
It does not authenticate a manually selected image; obtain it from a trusted
source and compare the release checksum.

`POST /api/firmware/upload` accepts multipart file data. A fixed 4 KiB buffer
streams it into `/.crosspoint/fw_upload.part`. Only a complete valid image that
fits the next OTA partition becomes `/.crosspoint/fw_staged.bin`.
`GET /api/firmware` reports state, version, build and partition limit.
`POST /api/firmware/discard` deletes staging.
`POST /api/firmware/apply` requires the staged `sha256` form field and returns
202. It requests the existing on-device confirmation activity; the browser
cannot confirm or flash by itself.

Cancel is the default physical selection. No app partition is erased until
physical confirmation. The staged size and SHA-256 are rechecked before the
confirmation and again after it. The existing `FirmwareFlasher` then validates
the image, writes the inactive slot, and uses the existing boot-switch semantics.
Success shows a result and restarts after 1.5 seconds. Failure/decline removes
staging and returns to the server. The HTTP server is synchronous and is not
pumped during physical confirmation or flashing; callers can time out then.
There is no browser flash-progress stream during that interval; the device
shows installation progress.

Only the two staging paths are written/deleted by this feature. Books, fonts
and settings are not reset. Upload failure/disconnect removes the partial file;
a new upload replaces prior staging. Server startup and shutdown clear staging.
No automatic rollback guarantee is made: it depends on the installed bootloader
and hardware. Power loss and physical SD removal during flashing need device
acceptance.

## Signed custom channel

`platformio.ini` configures the HTTPS manifest URL and allowed release URL prefix
for `x3_zh_tw_custom`. An unconfigured custom build reports no update and never
falls back to the stock feed. The current endpoint is:

```text
https://github.com/SemonCat/BookOrbit-X3/releases/latest/download/manifest.json
```

The monotonic `crosspoint.x3_ota_build` orders releases independently of runtime
version 1.3.3. Increment it before each new release; do not reuse a published build.
The first channel-capable release has build 1; earlier firmware needs a manual
web or SD install before it can use this channel.

The manifest is at most 2,048 bytes. Its canonical fields are signed with
ECDSA P-256/SHA-256; see `lib/FirmwareUpdate/OtaManifest.h`. The public key is
compiled into `OtaPublicKey.h`. The signing key stays outside the checkout and
must be backed up by its owner. No key rotation mechanism is implemented.
Only the exact custom channel and target, a greater build, a fitting size, and a
firmware URL under the configured release prefix are accepted. The download must
match the signed size and SHA-256 and the shared image validator before flashing.

The custom fetcher holds fixed URL/header buffers, discards unused response
headers, reads body chunks of at most 4 KiB, requires Content-Length, rejects
Transfer-Encoding, allows four redirects, and enforces an idle timeout.
Allowed hosts are github.com, objects.githubusercontent.com and
release-assets.githubusercontent.com. It avoids SecureHttpClient's retained
header strings implicated in the previous redirect OOM.
TLS peer verification is disabled by the existing wolfSSL integration:
authenticity comes from the manifest signature and signed image hash, not
authenticated TLS. A network attacker can still deny or stall updates.
The build comparison is not a hardware-protected anti-rollback mechanism.

## Parent release sequence

Freeze and commit the candidate; finish independent P2 review and required repairs.
The parent alone runs `scripts/release_x3_zh_tw.py` from that clean HEAD.
Do not edit sources after that build starts. Build outputs from `.pio/` and
previous releases must never be substituted for the guarded result.

After the build, sign and verify its extracted `firmware.bin` with
`scripts/make_ota_manifest.py`. The signer reads channel/target/build/version from
the binary and checks that the supplied signing key matches the committed public
key. It never reads a GitHub credential.

`scripts/prepare_x3_ota_source.py` captures tracked product/build sources, the SDK
checkout, patched dependency sources, framework sources, SDK configs and a build
provenance record. It omits Git history, agent settings, local device state and
tool binaries. Inspect this archive and its licenses before publication; the
project's root MIT license does not replace the linked wolfSSL license.
The guarded build records the product/SDK Git archive member hashes, generated
source file hashes and SDK artifact inventory digest in `last-release.json`.
Capture verifies those inventories before
and after writing and checks each archived generated file's hash. Changed cached
sources require a fresh guarded build, not a relabeled source archive.
`build-inputs/project/platformio.ini` is the effective configuration used by that
build, including resolved local overrides; it does not depend on a missing
`platformio.local.ini`. Referenced extra scripts, partition and embedded input
files are captured with it. Absolute/outside-checkout references in those options
are rejected rather than silently distributed. Use these effective build inputs
when reconstructing the generated project, not just the root project's INI.
Old release records without these inventories are rejected. The assembler also
checks every recorded source member, including product and SDK trees, against
the guarded record; omitting or replacing those trees cannot pass on provenance
labels alone.

`scripts/assemble_x3_ota.py` consumes the guarded release record, signed manifest,
corresponding source archive, Chinese README and implementation report. It copies
the actual license/notice files from that same source archive into `licenses/`,
requiring the project, SDK and linked wolfSSL notices. In the current dependency,
wolfSSL's COPYING is GPLv2 and LICENSING allows GPLv2 or later, or a commercial
license. Inspect the exact final build's files and corresponding source;
root MIT does not override them.

The assembler verifies the new full HEAD and input bindings, writes a
deterministic ZIP with SHA256SUMS, and reads every member back. It never builds,
signs, pushes, creates repositories, changes visibility, or publishes. Notice
presence checks are not a determination of license compliance.

This firmware-only package omits all optional SD fonts and has no font input
arguments. Existing users must retain the seven installed GuanKiapTsingKhai sizes
(14/18/20/22/24/26/28) under `fonts/GuanKiapTsingKhai/`; firmware updates do not
replace them. The Chinese README must state this prerequisite. Unresolved source
font copyright/license and reserved-name questions prevent redistribution of
those optional assets, not this firmware-only packaging path. Built-in font
sources and their actual notices remain part of the corresponding build inputs.

## Verification

For the build2001 interrupted TLS download, bounded retry policy and practical
manual recovery, see [x3-ota-recovery.md](x3-ota-recovery.md).

```sh
cmake --build /tmp/x3ota-host -j 8
ctest --test-dir /tmp/x3ota-host --output-on-failure
python3 -m unittest discover -s scripts -p 'test_*ota*.py'
python3 -m unittest discover -s scripts -p 'test_release*.py'
CROSSFADE_SIM_ZH_TW_FONTS=/path/to/accepted/sd-assets/fonts \
  python3 scripts/sim_firmware_update_smoke.py
```

The simulator serves the firmware's actual HTML and HTTP handlers, validates SD
staging with the same code, and drives the real physical confirmation activity.
Its flasher intentionally returns WRITE_FAIL, displayed as
UNSUPPORTED_IN_SIMULATOR. It proves no real flash, boot switch, rollback, TLS
connection or successful firmware restart.
