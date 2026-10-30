#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "BookOrbitOwnerId.h"

// -----------------------------------------------------------------------------
// BookOrbit page-stats: shared types and protocol limits.
//
// BookOrbit (https://github.com/bookorbit/bookorbit) is a KOSync-compatible server that,
// beyond plain progress sync, ingests raw per-page reading events and clusters them
// server-side into reading sessions (time read, streaks, pace, reading DNA). Its own
// KOReader plugin posts those events to POST {base}/plugin/page-stats; this fork sends the
// same shape from the X3 so a CrossFade device feeds the same stats.
//
// Selectively ported, with attribution, from the MIT-licensed CrossInk-BookOrbit fork
// (https://github.com/agosez/CrossInk-Bookorbit, commit 92cc1be) -- payload shape and the
// basis-point position idea come from there. Everything else here is a rewrite for the X3's
// constraints: explicit little-endian record codec instead of raw struct persistence, a
// fixed 32-record RAM buffer instead of a 2000-event/32KB std::vector, and no WallClock/era
// machinery (the X3 has a battery-backed DS3231, so capture-time stamps are absolute).
//
// Server-side contract, verified against
// server/src/modules/koreader/dto/koreader-plugin.dto.ts and koreader-stats.service.ts:
//   - deviceId matches /^[A-Za-z0-9-]{1,100}$/, deviceModel <= 100 chars,
//     pluginVersion <= 20 chars (a longer value is rejected with HTTP 400).
//   - books <= 50 per request, <= 500 events per request in total.
//   - book.hash must be 32 hex characters (MD5); both of this fork's document-id methods
//     already produce exactly that (see KOReaderDocumentId).
//   - event.page >= 0, event.startTime >= 1 (Unix seconds), event.durationSeconds in
//     [0, 86400], event.totalPages >= 1.
//   - The server dedups on (user, book, device, page, startTime), so a retried batch must
//     carry the SAME timestamps it was captured with -- never re-stamped at upload time.
// -----------------------------------------------------------------------------

namespace BookOrbitLimits {
// Server caps. MAX_EVENTS_PER_REQUEST is the protocol ceiling, not what this device sends --
// see BookOrbitStatsQueue::MAX_EVENTS_PER_BATCH for the (far smaller) low-memory batch size.
constexpr size_t MAX_EVENTS_PER_REQUEST = 500;
constexpr size_t MAX_BOOKS_PER_REQUEST = 50;
constexpr uint32_t MAX_EVENT_DURATION_SECONDS = 86400;
constexpr size_t MAX_PLUGIN_VERSION_CHARS = 20;
constexpr size_t MAX_DEVICE_MODEL_CHARS = 100;

// Identifies this firmware to the server. Deliberately a short literal and NEVER derived from
// CROSSPOINT_VERSION: variant builds append suffixes (this branch's own build stamps
// "<ver>-x3-zh-tw"), and anything over 20 characters makes the server reject the whole upload
// with HTTP 400. Bump the numeric suffix if the payload shape changes.
constexpr char PLUGIN_VERSION[] = "crossfade-bo-1";
static_assert(sizeof(PLUGIN_VERSION) - 1 <= MAX_PLUGIN_VERSION_CHARS,
              "pluginVersion must fit the server's 20-character cap");

// The device_id sent with generic-KOSync PROGRESS sync (KOReaderSyncClient.cpp only -- see that
// file's own DEVICE_ID alias). Deliberately left alone there: it is part of the existing KOSync
// behaviour and changing it would orphan every device's server-side progress record on servers
// that key by it. BookOrbitSyncClient's own progress sync no longer uses this constant -- see
// BookOrbitProgress::deviceId's doc comment for why a per-installation id is used instead.
constexpr char DEVICE_ID[] = "crosspoint-reader";

// A persisted 128-bit random value identifies one installation. Thirty-two lower-case hex
// characters satisfy BookOrbit's device-id regex while keeping all of the first eight characters
// entropy-bearing (the server derives reading-session keys from that prefix).
constexpr size_t STATS_DEVICE_ID_ENTROPY_BYTES = 16;
constexpr size_t STATS_DEVICE_ID_CHARS = STATS_DEVICE_ID_ENTROPY_BYTES * 2;
}  // namespace BookOrbitLimits

/**
 * Per-installation identifier for BookOrbit operations.
 *
 * Distinct from BookOrbitLimits::DEVICE_ID above, which remains only in the sibling generic-
 * KOSync progress client. It was introduced for page stats, where BookOrbit derives a session id
 * from a PREFIX of this value -- `kor:${deviceId.slice(0, 8)}:` in koreader-stats.util.ts
 * buildDeviceSessionIdPrefix -- so two devices sharing an id, or merely sharing their first eight
 * characters, have their reading sessions merged into one on the server. BookOrbit progress,
 * bookmark, and catalog callers reuse the same stable installation identity for their own
 * verified per-device server state. It MUST NOT be deterministically recreated from hardware:
 * after a state reset/reinstall, reusing the prior id would inherit that old client's delivery,
 * tombstone, and reset-convergence state while the new local installation has none of its history.
 *
 * Pure and entropy-injected so its format and prefix uniqueness are host-testable. Firmware fills
 * the input from ESP's hardware RNG, then persists the returned id before any BookOrbit request.
 */
std::string bookOrbitStatsDeviceId(const uint8_t* entropy);

/** True only for the current persisted 128-bit lower-case-hex representation. */
bool bookOrbitStatsDeviceIdIsCurrent(const std::string& id);

/** Firmware RNG binding, kept out of the host-buildable formatter translation unit. */
void bookOrbitFirmwareRandom(uint8_t* out, size_t len);

/**
 * One queued per-page reading event.
 *
 * CrossPoint page numbers are per-chapter and layout-dependent (font size, margins and
 * orientation all change them), so a raw page index would be meaningless to a server
 * comparing devices. Events instead carry overall book position in basis points:
 * page in [0, 10000] against a fixed totalPages of 10000, so the server's own
 * `page / totalPages` (koreader-stats.util.ts computeClusterMetrics) reproduces the exact
 * reading fraction regardless of pagination engine.
 */
struct BookOrbitStatEvent {
  uint32_t startTime = 0;        // Page-read start, UTC epoch seconds. Stamped once at capture.
  uint16_t durationSeconds = 0;  // Dwell on the page. Bounded by the reader's idle threshold.
  uint16_t page = 0;             // Overall book position in basis points (0..PROGRESS_SCALE)
  uint16_t totalPages = 0;       // Always PROGRESS_SCALE; stored per record so it is self-describing
};

/** What the server did with one book's events, parsed from the page-stats response. */
struct BookOrbitStatsAck {
  bool matched = false;     // Our hash appeared in results[] -- the events were ingested
  bool unmatched = false;   // Our hash appeared in unmatched[] -- 2xx, but no library book matched
  uint32_t accepted = 0;    // Newly stored events
  uint32_t duplicates = 0;  // Events the server already had (dedup key hit)
};
