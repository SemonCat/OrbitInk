#pragma once
#include <string>

class GfxRenderer;

// Independent bidirectional BookOrbit progress sync and stats/bookmark drain. Automatic paired
// events share a saved-network interval via ProgressSyncNetwork::Batch. Standalone calls still
// own their radio lifecycle and remain independent of KOReader credentials or availability.
//
// A no-op (no radio, no delay) unless BookOrbitCredentialStore has credentials configured --
// independent of KOReaderCredentialStore's own state. When only KOSync is configured (no
// BookOrbit), every function below returns immediately with zero network activity: that device's
// stats already drain opportunistically inside KOReaderAutoSync's own window (see its
// uploadBookOrbitStats()), and there is no BookOrbit progress endpoint to sync against.
//
// Provider composition: each transaction re-reads on-disk local progress and honors any durable
// remote handoff left by the other provider. Sequential transactions therefore converge on the
// furthest of local, KOSync, and BookOrbit without uploading a stale local position.
//
// Successful transactions here feed the SAME shared "last synced" timestamp and success notice as
// KOReaderAutoSync's own (see KOReaderAutoSync::recordProviderSuccess()), so the Home-screen
// status readout reflects the most recent successful sync from either provider.
namespace BookOrbitAutoSync {

// Sleep/close/open wrappers use the same GET/compare/pull-or-push transaction. Remote mapping runs
// only after the network session and TLS transport have been destroyed.
void syncOnSleep(GfxRenderer& renderer);

// Timer-wake transaction. Safe before renderer/display initialization: remote-ahead
// positions are persisted for later layout-aware application and no stats/bookmark work runs.
void syncHeadlessly(const std::string& epubPath);

// Call once the reader activity has been destroyed.
void syncOnClose(GfxRenderer& renderer);

// Synchronize a freshly opened EPUB before constructing its reader activity.
void syncOnOpen(const std::string& epubPath, GfxRenderer& renderer);

// Outcome of an interactive manual sync (syncNow() below), for a settings-screen UI to report.
// Unlike the silent push/pull triggers above, a manual action needs to tell the user which of
// three things happened: nothing to do, a position was pulled in, or one was pushed out.
enum class SyncNowResult {
  NO_CREDENTIALS,   // BookOrbitCredentialStore has no username/password/server URL configured.
  NO_OPEN_BOOK,     // APP_STATE.openEpubPath is empty or not an EPUB.
  NO_WIFI,          // No saved WiFi network, or the connect attempt failed/timed out/was aborted.
  NETWORK_ERROR,    // Transport, auth, server, JSON, or heap failure -- see errorDetail.
  ALREADY_SYNCED,   // Remote and local already agree; nothing to pull or push.
  PULLED,           // Remote was further ahead; local position was updated from it.
  PUSHED,           // Local was further ahead; remote was updated from it.
  CHRONOLOGY_UNAVAILABLE,  // No safe timestamp comparison was possible; progress was preserved.
  LOCAL_LOAD_FAILED  // Could not load/build the EPUB cache needed to compare positions.
};

// Interactive, single-direction-resolving sync for a settings-screen "Sync Now" action: one WiFi
// connect, one GET, a single comparison against the CURRENTLY on-disk local position, then either
// a pull (apply remote to local) or a push (upload local to remote) -- never both, since after
// either one the two sides already agree. Drains queued stats on the same session regardless of
// which direction fired. On any failure returning NETWORK_ERROR, `errorDetail` (out-param) is set
// to a human-readable string from BookOrbitSyncClient::errorString(); left empty otherwise.
SyncNowResult syncNow(const std::string& epubPath, GfxRenderer& renderer, std::string& errorDetail);

}  // namespace BookOrbitAutoSync
