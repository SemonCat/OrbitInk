#pragma once
#include <BookOrbitSyncClient.h>

#include <string>

#include "BookOrbitBookmarkSyncPolicy.h"

/**
 * Two-way BookOrbit bookmark sync (POST .../plugin/bookmarks/exchange, .../exchange-ack).
 *
 * App-layer policy on top of BookOrbitSyncClient::Session's wire methods, mirroring
 * BookOrbitStatsSync.h's own split (protocol in lib/KOReaderSync, policy/persistence here) and
 * gated the same way (BOOKORBIT_STORE.hasCredentials() + a confirmed hasBookmarkSync capability,
 * via the same BookOrbitStats::ensureCapabilitiesConfirmed() both features now share).
 *
 * The actual decision logic (minting identities, building the outgoing batch, applying incoming
 * changes) lives in BookOrbitBookmarkSyncPolicy.h as pure, host-tested functions with no I/O --
 * see that header for their exact contracts. This file is the thin orchestrator that sequences
 * them against the real device (BookmarkFile, HalClock, an open BookOrbitSyncClient::Session) and
 * is deliberately NOT host-tested directly, matching KOReaderAutoSync.cpp/BookOrbitAutoSync.cpp's
 * own precedent: it depends directly on HalClock/HalStorage/CrossPointSettings, none of which
 * this project's host-test harness stands in for at this layer.
 *
 * DURABILITY MODEL -- there is no separate outbox/tombstone log; the book's own bookmark file
 * (BookmarkFile, via BookmarkEntry's boDatetime/boSynced fields) IS the durable state, which is
 * sufficient because a bookmark's identity, once minted, never changes:
 *  - Outgoing (local -> server): a bookmark's identity key is stable once boDatetime is minted,
 *    so resending it after an interrupted upload is a no-op server-side (same key, same
 *    upsert) -- no separate "pending upload" record is needed beyond boSynced=false.
 *  - Incoming (server -> local): every mutation to the bookmark list happens and is PERSISTED
 *    to BookmarkFile before the corresponding acknowledgment is ever sent (see sync()'s
 *    implementation). If the ack is lost, the server simply re-offers the same pending change
 *    next round; applying it again is a no-op (an add whose key already exists locally is
 *    recognized and not duplicated; a delete whose key is already absent locally is simply
 *    re-acked) -- see BookOrbitBookmarkSyncPolicy.h's applyIncoming() doc comment. This is what
 *    prevents a lost ack from ever resurrecting a deletion that already happened: the deletion is
 *    durable BEFORE the ack is attempted, so a retry can only re-confirm it, never undo it.
 *
 * ACCOUNT SCOPING: boSynced/boDatetime alone say "this identity is known to A server", never
 * WHICH one -- switching BookOrbit account/server does not clear them, since an identity, once
 * minted, must never change (see BookmarkEntry.h). BookmarkEntry::boOwner (a hex
 * BookOrbitOwnerId) is what buildOutgoing()/applyIncoming() actually check against the CURRENTLY
 * configured account (BookOrbitStats::currentOwner().toHex(), computed once per sync() call)
 * before treating anything as already-synced or already-applied -- an entry synced to a previous
 * account is transparently resent as if new, which server-side rows already tolerate: they are
 * scoped per account there too, so an unrecognized key from a re-synced identity is simply a new
 * row, never a collision.
 *
 * KNOWN COSMETIC GAP: a bookmark newly created here from an incoming server entry gets
 * percentage=0 and no computed spine/page hints, since resolving those needs a loaded Epub this
 * module deliberately does not load (a bidirectional sync round is not a reason to pay for an
 * EPUB parse). It corrects itself the next time the user opens that bookmark in the reader, the
 * same as any other display-only staleness in this codebase's bookmark list.
 */
namespace BookOrbitBookmarkSync {

using AbortCallback = BookOrbitSyncClient::AbortCallback;

/**
 * One full bidirectional exchange+ack round for one book, on the caller's already-open session --
 * shares the TLS connection BookOrbitAutoSync already opened for progress sync/stats drain rather
 * than opening a second one, since all three speak to the same BookOrbit server in the same
 * WiFi-up window.
 *
 * Every failure here is soft: this returns void and leaves BookmarkFile in whatever state the
 * pure steps in BookOrbitBookmarkSyncPolicy.h already produced, which is always internally
 * consistent for a later attempt to continue from (see this header's own durability-model
 * comment). Callers must not branch on it.
 *
 * `documentHash` must be KOReaderDocumentId::calculateForStats()'s binary partial MD5, matching
 * every other BookOrbit endpoint's document key (see BookOrbitCredentialStore.h's own doc comment
 * on why BookOrbit has no separate document-matching-method setting).
 */
void sync(BookOrbitSyncClient::Session& session, const std::string& epubPath, const std::string& documentHash,
          const AbortCallback& shouldAbort = nullptr);

}  // namespace BookOrbitBookmarkSync
