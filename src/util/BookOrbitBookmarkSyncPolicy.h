#pragma once
#include <BookOrbitBookmarks.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../BookmarkEntry.h"

/**
 * Pure decision logic for BookOrbit bookmark sync, split out from BookOrbitBookmarkSync.h so it
 * can be host-tested with no HalStorage/HalClock/CrossPointSettings/network dependency at all --
 * every function here takes plain data in and produces plain data out, with no I/O.
 *
 * See BookOrbitBookmarkSync.h for the orchestrating sync() that sequences these against the real
 * device (BookmarkFile, HalClock, an open BookOrbitSyncClient::Session) and for the durability
 * model these functions are designed to preserve.
 */
namespace BookOrbitBookmarkSync {

/**
 * True when a load failure must abort the whole sync round rather than proceed with an empty
 * bookmark vector.
 *
 * BookmarkFile::load()'s false return conflates two cases the caller must NOT treat the same:
 * "no bookmark file yet" (a book that has never had one -- an empty vector is authoritative and
 * safe to sync) and "file exists but failed to read/parse" (e.g. a partially written file after a
 * power loss; the true on-disk set may not be empty at all). Continuing with an empty vector in
 * the second case lets the incoming-apply path overwrite whatever bookmarks are actually still
 * intact, and lets an incoming delete get acknowledged against a phantom snapshot that never
 * reflected what is really there. `absenceConfirmed` must therefore come from a tri-state path
 * probe, not exists(): SdFat implements exists() as an attempted open, so false can mean either
 * genuinely absent or an I/O failure. Only a complete, error-free parent-directory scan proves
 * the empty set authoritative.
 */
inline bool loadFailureRequiresAbort(const bool loadOk, const bool absenceConfirmed) {
  return !loadOk && !absenceConfirmed;
}

/**
 * Mints boDatetime for every local bookmark that lacks one, using `nowEpochSeconds` as the one
 * shared "now" for this whole sync pass. Entries already carrying a boDatetime are never
 * touched -- BookmarkEntry.h's own doc comment: an identity must never change once minted.
 *
 * `clockAvailable=false` mints nothing and reports every unminted entry via `outAnyUnminted`, so
 * the caller knows the resulting key set cannot be complete this round.
 *
 * @return true when at least one entry was minted, telling the caller to persist BEFORE building
 *         the outgoing request -- a crash after minting but before the request still resends the
 *         exact identity next time, since it is already durable.
 */
bool mintMissingDatetimes(std::vector<BookmarkEntry>& bookmarks, bool clockAvailable, uint32_t nowEpochSeconds,
                          bool& outAnyUnminted);

/**
 * Builds the key set and the outgoing changes batch from the current bookmark list.
 *
 * `keys` includes every entry with a usable minted identity (a non-empty boDatetime AND xpath),
 * capped at `maxKeys`. `outKeysComplete` is false whenever the cap truncated the set, an entry
 * carried a boDatetime but could not be keyed (e.g. a corrupted empty xpath), or `anyUnminted`
 * (from mintMissingDatetimes) was true -- any of these means the device's true set could not be
 * fully represented this round, and the server must not read the gap as deletions.
 *
 * `changes`/`outChangeIndices` covers every entry that is minted but not yet confirmed synced TO
 * `currentOwnerHex` specifically (`boSynced && boOwner == currentOwnerHex`), capped at
 * `maxChanges`. An identity, once minted, never changes (BookmarkEntry.h's own doc comment) -- but
 * `boSynced` alone does not mean "the CURRENTLY configured server already has this": switching
 * BookOrbit account/server never clears boDatetime/boSynced, so without the owner check a
 * bookmark already synced to a PREVIOUS account would be silently skipped forever when the device
 * is later pointed at a different one. Resending the same identity to a different account is
 * safe: server-side bookmark rows are already scoped per account, so an unrecognized key is
 * simply treated as new there, never a collision. `outChangeIndices` names which `bookmarks[]`
 * slot produced each `changes[]` entry, so the caller can mark exactly those `boSynced=true` (and
 * `boOwner=currentOwnerHex`) after a confirmed upload.
 */
void buildOutgoing(const std::vector<BookmarkEntry>& bookmarks, bool anyUnminted, size_t maxKeys, size_t maxChanges,
                   const std::string& currentOwnerHex, std::vector<BookOrbitBookmarkKeyRow>& outKeys,
                   bool& outKeysComplete, std::vector<BookOrbitBookmark>& outChanges,
                   std::vector<size_t>& outChangeIndices);

/**
 * Applies the server's own incoming changes to `bookmarks` in place.
 *
 * A delete removes every local entry whose identity key matches AND whose `boOwner` is either
 * `currentOwnerHex` or empty (ordinarily at most one) and is ALWAYS acknowledged, whether or not
 * a local match was found -- an already-applied deletion from an earlier, un-acked round must
 * still be re-acked, which is exactly how a lost ack cannot resurrect it. The owner check exists
 * so an incoming delete from the currently configured account can never remove an entry that
 * actually belongs to a DIFFERENT, previously configured one, purely by key coincidence (in
 * practice near-impossible for a real digest collision, but cheap to guard regardless).
 *
 * An add first checks whether a local entry with the same identity key already exists (an
 * earlier, un-acked round applied it) -- if so, it is acknowledged again without being
 * duplicated. This key match usually cannot fire for an add, though: per BookOrbitBookmarks.h's
 * own class comment, the server has no key for a bookmark IT created until this device's ack
 * supplies one, so a lost ack causes the server to re-offer the SAME add with `key` still empty,
 * not a key that would simply fail to match. For that reason, when no key match is found, this
 * also checks for a local entry at the same `pos` (xpath) and treats a hit the same way --
 * without this fallback, every lost ack on an incoming add would mint a fresh identity and
 * duplicate the bookmark indefinitely. BOTH match paths additionally require the candidate's
 * `boOwner` to be `currentOwnerHex` or empty: without this, switching BookOrbit account and then
 * receiving an add that coincidentally lands on the same local xpath as a bookmark from the
 * PREVIOUS account would silently re-own that entry to the new account and ack it under the new
 * account's identity, corrupting which account it actually belongs to. Otherwise a new
 * BookmarkEntry is created, minting a device-local identity from `nowEpochSecondsOrZero` (per
 * BookOrbitBookmarks.h's class comment: for a bookmark created on the web, THIS DEVICE mints the
 * datetime, never the server's) and stamping `boOwner=currentOwnerHex`. `nowEpochSecondsOrZero=0`
 * means no usable clock this round: a genuinely new add is then left out of `bookmarks` and out of
 * the acknowledgment entirely (neither applied nor failed), so the server simply re-offers it once
 * a clock becomes available. An add with an empty `pos` can never be placed at all and is
 * acknowledged as `failed=true` immediately, regardless of clock availability.
 *
 * `outApplied`/`outDeletedIds` are exactly what the caller must pass to Session::ackBookmarks()
 * afterward. `outChanged` is true when `bookmarks` was modified, telling the caller whether
 * BookmarkFile::save() is needed -- callers MUST persist before sending the acknowledgment (see
 * BookOrbitBookmarkSync.h's own durability-model comment).
 */
void applyIncoming(std::vector<BookmarkEntry>& bookmarks, uint32_t nowEpochSecondsOrZero,
                   const std::string& currentOwnerHex, const std::vector<BookOrbitIncomingBookmark>& incoming,
                   std::vector<BookOrbitBookmarkAck>& outApplied, std::vector<uint32_t>& outDeletedIds,
                   bool& outChanged);

}  // namespace BookOrbitBookmarkSync
