# X3 OTA interrupted-download recovery

## Observed build2001 failure

The device reported running `orbitink-x3-nightly/x3-zh-TW/build=2001`
(runtime 1.3.3), downloading 5,907,424 bytes:

```text
DOWNLOAD TRUNCATED/TLS_READ_ERROR HTTP 206 hop2
release-assets.githubusercontent.com resumes 1 heap 46860/27636
Bytes: 49152/5907424
```

The retained build2001 application descriptor identifies source `9f78899`.
Its `OtaFetch.cpp` is identical to published build2101 source
`3dbd949bb7501f9fc6ecd451c7135114ddec074b`. Both pin freeink-sdk
`566fce3d4fe45fdcf76853be788e4d497547b592`.

The compiled SDK `SecureClient::read` calls `wolfSSL_get_error` after a
nonpositive `wolfSSL_read`. WANT_READ/WANT_WRITE map to **0**, with the TLS
session retained. ZERO_RETURN maps to 0 with the session disconnected; other
errors map to **-1**, also disconnected. A raw negative `wolfSSL_read` can
mean would-block, but the fetcher receives the wrapper result. Treating the
wrapper's -1 as another transient read would spin on a dead session.
The regression test compiles the exact pinned wrapper read implementation,
including WANT and fatal-code cases, rather than substituting these semantics.

The decisive pinned wrapper code is:

```cpp
int SecureClient::read(uint8_t* buf, size_t size) {
  if (!_connected) return -1;
  auto* ssl = static_cast<WOLFSSL*>(_ssl);
  const int n = wolfSSL_read(ssl, buf, size);
  if (n > 0) return n;

  const int err = wolfSSL_get_error(ssl, n);
  if (isWantIo(err)) return 0;
  if (err == WOLFSSL_ERROR_ZERO_RETURN) {
    _connected = false;
    return 0;
  }
  _connected = false;
  return -1;
}
```

Here `isWantIo` accepts only `WOLFSSL_ERROR_WANT_READ` and
`WOLFSSL_ERROR_WANT_WRITE` (2/3), not callback error numbers. For example,
callback WANT_READ and fatal wolfCrypt MP_MEM both have value -2; accepting
callback codes as wrapper errors would conceal actual failures.

The old continuation condition required each failed attempt to deliver new
bytes. HTTP206 means a continuation header and its exact Content-Range were
accepted. `resumes 1` with an incomplete body means that first continuation
delivered zero new bytes, so the progress condition stopped recovery before
the nominal limit of three resumes. An executable harness using production
`OtaFetch.cpp` reproduces this exact 49,152-byte failure.

This proves why recovery stopped, not why TLS first failed. The wrapper does
not expose the underlying wolfSSL error. TLS record/peer/TCP failure and
resource pressure remain possible. The reported heap numbers are sampled
after the hop's client and parser are destroyed; they cannot establish a
TLS-session heap low-water mark or prove OOM. No SDK change or TLS memory
expansion is part of this fix.

## Bounded downloader policy

Firmware transport interruption in connect, request write, response header or
response body can start a fresh request from the manifest's original URL,
obtaining new signed redirect Locations. It never reuses a failed TLS session
or a possibly expired asset URL. Manifest fetching remains single-attempt.

- At most three extra requests (four attempts total), each with at most four
  redirects. Every extra request consumes that same budget, even at offset zero.
- One consecutive attempt with no delivered bytes can recover; a second
  consecutive no-progress attempt stops. Any delivered bytes reset that streak,
  without resetting the total request budget.
- Offset zero requires a fresh HTTP200. A partial download requires HTTP206
  with exact offset, total, last byte and remaining Content-Length.
- Parser/HTTP/origin/range/length/sink rejection is terminal. A missing header
  because the transport failed is distinguished from a malformed header.
- Wrapper zero waits while the connection can still supply bytes, with the
  existing 20-second idle deadline and watchdog feeds. Wrapper -1 tears down
  the session. Clean disconnects follow the same bounded continuation policy.

The existing fixed 4 KiB body buffer, 512-byte header reads, fixed parser/URL
buffers, host allowlist, HTTPS requirement and redirect cap are retained.
Only one TLS client is live at a time. Retrying does not grow the staging
buffer or allocate a whole-image buffer. No retry loop is unbounded by
attempt count; phase idle deadlines and the SDK's existing connect deadlines
are unchanged. A peer that keeps sending within the idle deadline is still
subject to the existing header-byte/line and signed body-size limits.

Failure reports now include the final attempt's delivered bytes, consecutive
no-progress count, wrapper read result and retry stop reason. `Client read`
is **not** a wolfSSL error code. Heap remains a post-teardown observation.
Signed manifest ECDSA, signed size/SHA-256, image validation and inactive-slot
flash rules are unchanged. Existing TLS peer verification remains disabled;
authenticity comes from the signed manifest/image hash, as documented in
[x3-ota.md](x3-ota.md).

## Recovery for a device still running build2001

The installed downloader cannot acquire this fix merely because a new
downloader has been built. It must first successfully install an application
image. Build2101 also contains the old downloader.

1. A limited manual retry of the existing signed build2101 OTA is reasonable
   on a stable connection. A failed download aborts the SD upload session;
   another user-initiated attempt starts at byte zero with fresh redirects.
   There is no cross-operation persistent resume. The size/hash/image gates
   run before flash, but the same interruption can recur; success is not
   guaranteed. Repeatedly retrying a deterministic failure is not a repair.
2. If it recurs, use the existing **manual web update**. Start the device's
   web server, open its Firmware link (`/update`) from a desktop/phone on the
   local network, and upload the trusted application `firmware.bin`. This
   bypasses the X3's outbound GitHub/wolfSSL download. Check the staged
   size/SHA-256, request apply, then physically confirm on the device
   (Cancel is the default). The full flow and checks are in
   [x3-ota.md](x3-ota.md#manual-web-update).
3. Alternatively copy the trusted application `.bin` onto SD via USB file
   transfer or a card reader. In build2001, Settings' system section has
   **SD firmware update** (`STR_SD_FIRMWARE_UPDATE`); its firmware picker
   starts at `/` and lists `.bin` files. Select the application image and
   physically confirm. This also bypasses outbound TLS. Use the application
   image, not a merged full-flash/partition/bootloader image.

The already published build2101 artifact recorded for this incident is:

```text
https://github.com/SemonCat/OrbitInk/releases/download/orbitink-b2101/firmware.bin
size:   5907424 bytes
sha256: 1d67a3cc4c41421616f178114c6438ef87b586fa1c1fa0c54c9c2f24bea29de6
```

The retained artifact's signed manifest verifies with the committed public
key. Compare the downloaded application against that size/hash before manual
upload or SD installation: manual selection validates image structure but
does **not** authenticate an image's publisher. These application-only paths
do not reset books, fonts or settings. Keep the installed SD fonts. This
documents a recovery route, not a claim that a device has been updated.

This scoped candidate has not been published, signed or flashed. Its guarded
local build is validation only, with the checkout's existing build metadata;
it must not be relabeled as published build2101. The parent chooses any future
release build number and delivery. Installing existing2101 gives its published
features but does not give the bounded recovery fix described here.

## Regression checks

```sh
python3 -m unittest scripts.test_ota_fetch scripts.test_ota_download_resume -v
cmake --build build/test -j 6
ctest --test-dir build/test --output-on-failure
```

The fetch harness compiles actual production fetch/parser code against clocked
Client doubles. It exercises the reported redirect/range failure, recovery
after an empty continuation, repeated no-progress, deadline/watchdog behavior,
WANT waits in headers/body, header interruptions at zero/nonzero offsets,
clean disconnects, connect/write failures, progress resetting the streak,
total request exhaustion, and policy/range/length/sink rejections.
These are deterministic transport simulations, not an X3 network acceptance
test. The firmware-update simulator covers staging and confirmation; it does
not implement real TLS or flash hardware.
