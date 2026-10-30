#pragma once
#include <CalendarTime.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

/**
 * Wire types for BookOrbit's two-way bookmark exchange (POST .../plugin/bookmarks/exchange,
 * .../exchange-ack).
 *
 * Independently reimplemented from the official BookOrbit server's documented behavior (see
 * docs/bookorbit-feature-matrix.md's Phase 4 architecture section for what was and wasn't
 * verifiable from that AGPL source): the server keys each bookmark by md5(datetime | pos), where
 * `pos` is a KOReader-style xpointer and `datetime` is a "YYYY-MM-DD HH:MM:SS" string neither side
 * interprets as a timezone -- only its exact bytes matter, since the identity depends on them
 * matching byte-for-byte across syncs.
 *
 * Identity direction is asymmetric by construction, matching the annotation exchange this
 * codebase has not yet built (Phase 5+): for a bookmark this device creates, THIS DEVICE mints the
 * datetime once (at first sync, not at creation -- no RTC read needed on the hot bookmark-toggle
 * path) and never changes it afterward. For a bookmark the server wants applied here (created on
 * the web), the identity convention INVERTS: this device mints its OWN datetime when it applies
 * the entry and reports {key, datetime, pos} back in the acknowledgment -- without that triplet
 * the server has nothing to link its own copy to, since it never sent a datetime of its own for a
 * bookmark it created.
 */

// A local bookmark on its way to the server.
struct BookOrbitBookmark {
  char datetime[20] = {};  // "YYYY-MM-DD HH:MM:SS", minted once and never changed
  std::string pos;         // xpointer of the bookmarked page (BookmarkEntry::xpath, reused as-is)
  uint16_t pageno = 0;      // chapter-relative page number, 0 when unknown -- optional on the wire
};

/**
 * A change the server wants applied locally: a bookmark created on the web, or a deletion.
 *
 * `serverId` is what the acknowledgment names, and the server keeps the entry pending until
 * ackBookmarks() names it -- so an entry this device cannot place yet is simply left off the next
 * acknowledgment rather than reported as done, and the server re-offers it next sync.
 */
struct BookOrbitIncomingBookmark {
  uint32_t serverId = 0;
  std::string key;    // deletion target: the md5 this device itself reported for the local entry
  std::string pos;    // add: xpointer of the bookmarked page
  std::string title;  // add: the server's label, bounded on parse (see BookOrbitBookmarkSync.cpp)
  uint16_t pageno = 0;
  bool deleted = false;
};

/**
 * One acknowledgment line for an applied server-originated change.
 *
 * An applied add must carry the LOCAL identity triplet this device just minted for it -- the
 * server has nothing else to link its own bookmark to. `failed` marks an entry this device can
 * never place (an empty position); an entry that might resolve later (e.g. the clock was
 * unavailable this round) is simply left out of the batch and re-offered next sync, per the class
 * comment above.
 */
struct BookOrbitBookmarkAck {
  uint32_t serverId = 0;
  bool failed = false;
  std::string key;       // 32 lowercase hex characters
  char datetime[20] = {};
  std::string pos;
};

// One row of the device's key set: the identity digest plus the datetime it was built from, which
// the server validates as a pair and rejects the whole request (HTTP 400) if they don't match.
struct BookOrbitBookmarkKeyRow {
  static constexpr size_t DIGEST_SIZE = 33;  // 32 hex characters plus a terminator
  char k[DIGEST_SIZE] = {};
  char dt[20] = {};
};

// Upload batch bound: how many NEW/unsynced local bookmarks go out in one exchange request. A
// book with more than this many unsynced bookmarks simply spreads across further sync rounds --
// see BookOrbitBookmarkSync::buildOutgoing's own doc comment.
inline constexpr size_t BOOKORBIT_BOOKMARK_BATCH = 16;

// Bound on the device's key set sent in one request, independent of BOOKORBIT_BOOKMARK_BATCH:
// the key set describes EVERY bookmark this device holds (for the server's deletion diff), which
// can be far larger than one batch of new uploads. A book with more bookmarks than this sends a
// truncated (keysComplete=false) set rather than growing the request body without bound -- see
// BookOrbitBookmarkSync::buildOutgoing.
inline constexpr size_t BOOKORBIT_BOOKMARK_MAX_KEYS = 500;

// Wire-side bounds on incoming server text, applied at parse time in BookOrbitSyncClient.cpp.
inline constexpr size_t BOOKORBIT_BOOKMARK_POS_MAX_CHARS = 256;
inline constexpr size_t BOOKORBIT_BOOKMARK_TITLE_MAX_CHARS = 128;

/**
 * Format UTC epoch seconds as the server's datetime string. No inverse (parse) function exists
 * here: unlike the annotation exchange (Phase 5+), a bookmark's datetime is never read back off
 * the wire and reinterpreted -- an incoming bookmark's identity is always minted fresh locally
 * (see the class comment above), so only the forward direction is ever needed.
 *
 * Deliberately no timezone interpretation, matching the server's own "device datetime" semantics:
 * this only has to round-trip to the exact bytes it produces, which is what the identity hash
 * needs -- meaning is irrelevant, byte-equality is not.
 */
inline bool bookOrbitFormatBookmarkDatetime(const uint32_t epochSeconds, char (&outDatetime)[20]) {
  const int64_t days = static_cast<int64_t>(epochSeconds / 86400u);
  int year = 0;
  unsigned month = 0, day = 0;
  CalendarTime::civilFromDays(days, year, month, day);
  if (year < 1970 || year > 2100) return false;
  const uint32_t secondsOfDay = epochSeconds % 86400u;
  const unsigned hour = secondsOfDay / 3600u;
  const unsigned minute = (secondsOfDay % 3600u) / 60u;
  const unsigned second = secondsOfDay % 60u;
  const int written = snprintf(outDatetime, sizeof(outDatetime), "%04d-%02u-%02u %02u:%02u:%02u", year, month, day,
                               hour, minute, second);
  return written == 19;  // "YYYY-MM-DD HH:MM:SS" is exactly 19 characters
}

/**
 * Compute the server's identity key for a bookmark: md5(datetime | pos), matching the reference
 * plugin's own construction.
 * @return false when either input is empty, leaving outKey untouched
 */
bool bookOrbitBookmarkKey(const char* datetime, const char* pos,
                         char (&outKey)[BookOrbitBookmarkKeyRow::DIGEST_SIZE]);
