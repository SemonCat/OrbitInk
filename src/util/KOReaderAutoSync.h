#pragma once
#include <cstddef>
#include <string>

class GfxRenderer;

// Automatic bidirectional KOSync for close/sleep/open/periodic routes. Reuses the same client code the manual
// "Sync Progress" reader-menu action does (KOReaderSyncClient, KOReaderDocumentId, ProgressMapper,
// KOReaderCredentialStore) -- this is not a second sync client, just headless callers of the
// existing one. Both entry points:
//  - No-op (no radio, no delay) unless a saved WiFi network and KOReader credentials both exist.
//  - Use a short, direct connect (last-connected SSID, no scan) bounded by a few seconds.
//  - Poll for aborts while connecting to WiFi and transferring response data. The TLS handshake
//    itself is bounded by SecureClient's transport timeout but cannot poll this callback.
//  - Failures (no WiFi, auth, network, server) stay silent. Successful open/close syncs queue one
//    short UI notice; sleep sync only updates the persisted last-success timestamp.
//  - Store-and-forward the KOReader progress/percentage fields verbatim on the pull side rather
//    than re-deriving them -- CrossPoint only computes its OWN side of a comparison, never the
//    remote's. See lib/KOReaderSync/ProgressMapper.h.
namespace KOReaderAutoSync {

// Sleep/close/open wrappers run the same GET/compare/pull-or-push transaction. They stage any
// remote-ahead position while the network session exists, then apply it only after that session and
// its TLS transport have been destroyed. Sleep no-ops unless the last activity was a reader.
void syncOnSleep(GfxRenderer& renderer);

// Timer-wake transaction. Safe before renderer/display initialization: remote-ahead
// positions are persisted for later layout-aware application and no display side effects run.
void syncHeadlessly(const std::string& epubPath);

// Call only after the reader activity has been destroyed on a reader -> destination transition.
void syncOnClose(GfxRenderer& renderer);

// Synchronize a freshly opened EPUB before constructing its reader activity.
void syncOnOpen(const std::string& epubPath, GfxRenderer& renderer);

// One-shot UI handoff consumed by the destination activity after automatic open/close sync.
bool consumeSuccessNotice();

// Compact status-bar age ("5m", "2h", "3d", then "MM/DD"). False means no valid persisted
// success timestamp or no RTC time is available.
bool formatLastSuccessRelative(char* output, size_t outputSize);

// Manual KOSync already owns its success screen, so record the timestamp without queuing a second
// popup. Call only after a confirmed successful server transaction.
void recordManualSuccess();

// Records a successful sync transaction from ANY provider -- KOSync's own push/pull below, or
// BookOrbitAutoSync's independent push/pull (src/util/BookOrbitAutoSync.h) -- against the same
// shared "last synced" timestamp and one-shot success notice consumeSuccessNotice()/
// formatLastSuccessRelative() expose. `queueNotice` mirrors this file's own push/pull call sites:
// true for close/open (visible transitions), false for the silent sleep path.
void recordProviderSuccess(bool queueNotice);

}  // namespace KOReaderAutoSync
