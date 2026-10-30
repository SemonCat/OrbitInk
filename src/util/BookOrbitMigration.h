#pragma once

// One-shot upgrade from e51b383's opportunistic BookOrbit detection (a capability probe of
// whatever server generic KOSync happened to be pointed at) to an explicit, standalone
// BookOrbitCredentialStore. See docs/bookorbit-feature-matrix.md for the full rationale.
//
// Without this, a user who was already getting page-stats uploads via the old opportunistic path
// would silently lose them the moment this firmware version boots: BookOrbitStats now reads
// BOOKORBIT_STORE, which starts out unconfigured, so captureEnabled()/drainPossible() both read
// false until the user re-enters credentials they had already typed once (into
// KOReaderCredentialStore) and forgotten they were BookOrbit-specific.
namespace BookOrbitMigration {

/**
 * Runs the migration check until it reaches a durable terminal state, gated by
 * CrossPointState::boMigrationAttempted. Call after APP_STATE, KOREADER_STORE, and BOOKORBIT_STORE
 * have all been loaded from disk (see src/main.cpp's setup()). Retryable save, directory-scan, and
 * queue-rewrite failures deliberately leave the latch false so the next boot resumes; a completed
 * migration, genuine no-op, or deliberate privacy-preserving abandonment sets it.
 *
 * A fresh attempt is a no-op when BookOrbitCredentialStore already contains independently
 * configured credentials, KOReaderCredentialStore has no credentials, or the legacy capability
 * cache was never confirmed BookOrbit / no longer matches the current KOSync identity. An
 * in-progress attempt is different: boCredentialsMigrated and boMigrationTargetOwner bind it to
 * the exact account whose credentials this function copied, so it may resume only while the
 * current BookOrbit owner still matches that durable target.
 *
 * Otherwise: copies the KOSync username/password and ALREADY-NORMALIZED getBaseUrl() (not the
 * raw, unnormalized getServerUrl()) into BookOrbitCredentialStore, then -- only if the two
 * stores' normalization disagrees on the resulting base URL, which the official BookOrbit
 * server's fixed "/api/v1/koreader" routing means should not happen for a genuinely-confirmed
 * detection -- rewrites the owner fingerprint on every book's queued bo_stats.bin so already-
 * captured, not-yet-uploaded reading events are not silently orphaned by BookOrbitStatsQueue's
 * existing "different owner: discard and replace" rule.
 *
 * Safe to retry, including a resume mid-way through: the credential copy is guarded by its own
 * durable marker and bound target rather than by BookOrbitCredentialStore::hasCredentials(),
 * which cannot distinguish a copied account from an unrelated one. For compatibility with the
 * immediately preceding build that wrote boCredentialsMigrated but had no target field, a missing
 * target is inferred only when the current RAW BookOrbit values still exactly match the KOSync
 * credentials this migration would have copied; any mismatch is abandoned without touching the
 * queue. A failed credential save also restores the prior in-memory values, and capability-cache
 * writes plus stats capture/drain are deferred while the durable migration remains pending.
 */
void runOnce();

}  // namespace BookOrbitMigration
