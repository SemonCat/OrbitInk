#include "BookOrbitMigration.h"

#include <BookOrbitCredentialStore.h>
#include <BookOrbitOwnerId.h>
#include <BookOrbitStatsQueue.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointState.h"

namespace BookOrbitMigration {

namespace {

// FAT/exFAT names are limited to 255 Unicode code points, each at most four UTF-8 bytes. Using
// the full bound lets an unrelated long directory entry be skipped rather than making getName()
// truncate/fail and leaving the scan result ambiguous.
constexpr size_t NAME_BUFFER_SIZE = 4 * 255 + 1;

// True when `name` ends in `suffix` and has at least one character before it.
bool endsWithSuffix(const std::string& name, const char* suffix) {
  const size_t suffixLen = strlen(suffix);
  return name.size() > suffixLen && name.compare(name.size() - suffixLen, suffixLen, suffix) == 0;
}

// Rewrites every book's queued stats file from `oldOwner` to `newOwner`. Only reached when the
// two stores' getBaseUrl() values normalize differently for what is otherwise the same migrated
// account -- see BookOrbitMigration.h's doc comment -- so this directory is expected to hold at
// most a handful of files (one per book with events still awaiting upload).
//
// Returns false when at least one book's rewrite failed, OR the directory itself could not be
// scanned, so the caller can leave the migration unmarked-complete and retry on a later boot --
// rewriteOwner() is a no-op for any book already carrying `newOwner` (see its own doc comment),
// so a retry only ever repeats the books that actually failed, never redoing already-migrated
// ones.
bool migrateQueueOwners(const BookOrbitOwnerId& oldOwner, const BookOrbitOwnerId& newOwner) {
  const std::string dirPath = BookOrbitStatsQueue::storageDirectory();
  // Absence and unreadability are NOT the same thing: the former means there is genuinely nothing
  // queued yet (the common case, safe to report success), the latter means this attempt learned
  // nothing about the directory's contents and must not report success over an unscanned queue --
  // doing so would let the caller mark migration complete while a book's events sit unmigrated
  // under the legacy owner forever.
  const HalStorage::PathStatus dirStatus = Storage.probePath(dirPath.c_str());
  if (dirStatus == HalStorage::PathStatus::Absent) return true;
  if (dirStatus == HalStorage::PathStatus::Error) {
    LOG_ERR("BOMigrate", "Could not determine whether the stats queue directory exists; will retry on next boot");
    return false;
  }
  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    LOG_ERR("BOMigrate", "Could not open the stats queue directory; will retry on next boot");
    return false;
  }
  dir.rewindDirectory();

  const auto nameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!nameBuffer) {
    LOG_ERR("BOMigrate", "OOM: %d bytes scanning stats queue directory", static_cast<int>(NAME_BUFFER_SIZE));
    return false;
  }

  // Recognizes the live queue file AND any temp/backup left by an interrupted replacement
  // (BookOrbitStatsQueue::tempPath()/backupPath()'s own suffixes): a document with only a
  // ".bin.tmp" or ".bin.bak" on disk right now (no live ".bin") would otherwise never be
  // discovered here and would be silently left under the legacy owner forever.
  // rewriteOwner() already resolves whichever of the three states is present via
  // recoverInterruptedReplacement(), so discovery only needs to find each hash once.
  constexpr char liveSuffix[] = ".bin";
  constexpr char tmpSuffix[] = ".bin.tmp";
  constexpr char bakSuffix[] = ".bin.bak";

  // This directory is documented above to hold at most a handful of files, so a small reserved
  // vector -- never reallocated in the common case -- is the right bound here, not a fixed-size
  // stack array that would silently drop hashes beyond its capacity.
  std::vector<std::string> hashes;
  hashes.reserve(8);

  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) continue;
    if (file.getName(nameBuffer.get(), NAME_BUFFER_SIZE) == 0) {
      LOG_ERR("BOMigrate", "Could not read a stats queue filename; will retry on next boot");
      return false;
    }
    const std::string name = nameBuffer.get();

    std::string documentHash;
    if (endsWithSuffix(name, bakSuffix)) {
      documentHash = name.substr(0, name.size() - strlen(bakSuffix));
    } else if (endsWithSuffix(name, tmpSuffix)) {
      documentHash = name.substr(0, name.size() - strlen(tmpSuffix));
    } else if (endsWithSuffix(name, liveSuffix)) {
      documentHash = name.substr(0, name.size() - strlen(liveSuffix));
    } else {
      continue;  // not a recognized stats queue state file
    }

    if (std::find(hashes.begin(), hashes.end(), documentHash) != hashes.end()) continue;  // already queued
    hashes.push_back(documentHash);
  }

  if (dir.getError() != 0) {
    LOG_ERR("BOMigrate", "Stats queue directory scan failed before end-of-directory; will retry on next boot");
    return false;
  }

  bool allOk = true;
  for (const auto& documentHash : hashes) {
    if (!BookOrbitStatsQueue::rewriteOwner(documentHash, oldOwner, newOwner)) {
      LOG_ERR("BOMigrate", "Could not migrate queued stats owner for one book; leaving it under the legacy owner");
      allOk = false;
    }
  }
  return allOk;
}

}  // namespace

void runOnce() {
  if (APP_STATE.boMigrationAttempted) return;

  // Becomes false the moment any step below needs a retry on a later boot. Guards the FINAL
  // boMigrationAttempted commit at the bottom: that flag must never latch while genuinely
  // incomplete work (a credential save that failed, a queue rewrite that failed) sits behind it,
  // or that work is silently abandoned forever -- see this function's own P1 review findings.
  bool complete = true;

  do {
    if (!KOREADER_STORE.hasCredentials()) break;  // no legacy KOSync config to have detected BookOrbit against
    if (APP_STATE.koStatsServerKind != 1) break;  // legacy probe never confirmed BookOrbit

    // The legacy confirmation is keyed to a fingerprint of KOSync's server+account AT THE TIME OF
    // THE PROBE. If the user has since repointed KOSync elsewhere, that confirmation no longer
    // describes the current KOSync configuration and must not be migrated as if it did.
    BookOrbitOwnerId legacyOwner;
    if (!BookOrbitOwnerId::fromHex(APP_STATE.koStatsServerId, legacyOwner)) break;
    const BookOrbitOwnerId expectedLegacyOwner =
        BookOrbitOwnerId::derive(KOREADER_STORE.getBaseUrl(), KOREADER_STORE.getUsername());
    if (legacyOwner != expectedLegacyOwner) break;

    const std::string expectedUsername = KOREADER_STORE.getUsername();
    const std::string expectedPassword = KOREADER_STORE.getPassword();
    const std::string expectedServerUrl = KOREADER_STORE.getBaseUrl();
    const auto matchesExpectedCredentialCopy = [&]() {
      return BOOKORBIT_STORE.hasCredentials() && BOOKORBIT_STORE.getUsername() == expectedUsername &&
             BOOKORBIT_STORE.getPassword() == expectedPassword && BOOKORBIT_STORE.getServerUrl() == expectedServerUrl;
    };

    // Two-phase commit, using OUR OWN durable marker rather than
    // BOOKORBIT_STORE.hasCredentials() to decide whether the copy already ran. hasCredentials()
    // is ambiguous: it cannot tell "this migration copied these credentials on an earlier,
    // interrupted boot" apart from "the user independently configured an unrelated BookOrbit
    // account" -- and the queue reconciliation below must run in the first case (to finish an
    // interrupted rewrite) but must NEVER run in the second (it would reassign a stranger
    // account's queue to the legacy owner's fingerprint). boCredentialsMigrated disambiguates
    // them precisely: only ever set by this function, immediately after its own copy is known to
    // have been durably saved.
    if (!APP_STATE.boCredentialsMigrated) {
      // Copy the ALREADY-NORMALIZED KOSync base URL (scheme resolved, trailing slashes stripped)
      // as BookOrbit's raw serverUrl input rather than KOReaderCredentialStore::getServerUrl()'s
      // raw, possibly-schemeless value. BookOrbitCredentialStore::getBaseUrl() then finds this
      // value already carries an explicit scheme and (per the official BookOrbit server's fixed
      // global "/api/v1" prefix -- confirmed against server/src/main.ts, not just this fork's own
      // convention) already ends in "/api/v1/koreader", so its strip-then-reappend normalization
      // is idempotent here: the resulting getBaseUrl() reproduces this exact string, byte for
      // byte.
      // A prior boot's copy can already be sitting here durably saved -- BOOKORBIT_STORE.saveToFile()
      // succeeded, but a reset landed before boCredentialsMigrated itself could be persisted. That
      // exact byte-for-byte match is the one case hasCredentials() alone cannot distinguish from an
      // independently configured account: recognize and resume it rather than abandoning the rest
      // of this migration (the queue rewrite and koStatsServerId update below) as if a stranger's
      // config were present.
      //
      // Compares getServerUrl() (the RAW value last passed to setServerUrl(), unnormalized) against
      // KOREADER_STORE's own base URL, not BOOKORBIT_STORE.getBaseUrl() against it -- the two
      // stores' getBaseUrl() normalization can legitimately disagree for the exact same underlying
      // server (that is what the owner-divergence rewrite a few lines below exists to handle), so
      // comparing normalized-to-normalized would report an exact prior copy as a mismatch and
      // abandon it every time normalization actually differs.
      const bool alreadyCopiedExactly = matchesExpectedCredentialCopy();
      if (BOOKORBIT_STORE.hasCredentials() && !alreadyCopiedExactly)
        break;  // independently configured; do not touch it

      if (!alreadyCopiedExactly) {
        // setCredentials()/setServerUrl() mutate the live singleton before saveToFile() has proved
        // those values durable. Preserve the exact prior (possibly only partially configured)
        // state so a failed write cannot make the migration's not-yet-committed account
        // operational for the rest of this boot -- capture/drain could otherwise run before the
        // legacy queue owner has been reconciled.
        const std::string priorUsername = BOOKORBIT_STORE.getUsername();
        const std::string priorPassword = BOOKORBIT_STORE.getPassword();
        const std::string priorServerUrl = BOOKORBIT_STORE.getServerUrl();
        BOOKORBIT_STORE.setCredentials(expectedUsername, expectedPassword);
        BOOKORBIT_STORE.setServerUrl(expectedServerUrl);
        if (!BOOKORBIT_STORE.saveToFile()) {
          LOG_ERR("BOMigrate", "Could not persist migrated BookOrbit credentials; will retry on next boot");
          BOOKORBIT_STORE.setCredentials(priorUsername, priorPassword);
          BOOKORBIT_STORE.setServerUrl(priorServerUrl);
          complete = false;
          break;
        }
      }
      // Bind this migration attempt to the exact account the copy above just produced, durably,
      // in the SAME commit as boCredentialsMigrated. Without this, a user who edits
      // BookOrbitCredentialStore's credentials between an interrupted attempt and its retry would
      // have the queue rewrite below run against the NEW (unrelated) account using the OLD
      // legacy-detection owner -- silently re-owning a stranger account's queue. Re-checked below
      // on every resume, including this same boot.
      APP_STATE.boMigrationTargetOwner =
          BookOrbitOwnerId::derive(BOOKORBIT_STORE.getBaseUrl(), BOOKORBIT_STORE.getUsername()).toHex();
      // Committed durably now, before the queue loop below, so an interruption partway through
      // that loop resumes it on the next boot instead of silently skipping the rest.
      APP_STATE.boCredentialsMigrated = true;
      if (!APP_STATE.saveToFile()) {
        LOG_ERR("BOMigrate", "Could not persist migration-in-progress marker; will retry on next boot");
        complete = false;
        break;
      }
      LOG_INF("BOMigrate", "Migrated BookOrbit credentials from legacy KOSync detection");
    }

    // Re-validate the bound target on EVERY resume, including the boot that just set it above:
    // BookOrbitCredentialStore may have been reconfigured since boMigrationTargetOwner was
    // recorded, and the queue rewrite below must never run against a now-unrelated account.
    BookOrbitOwnerId boundTarget;
    const bool haveBoundTarget = BookOrbitOwnerId::fromHex(APP_STATE.boMigrationTargetOwner, boundTarget);
    const BookOrbitOwnerId newOwner =
        BookOrbitOwnerId::derive(BOOKORBIT_STORE.getBaseUrl(), BOOKORBIT_STORE.getUsername());
    if (haveBoundTarget && boundTarget != newOwner) {
      LOG_ERR("BOMigrate",
              "BookOrbit credentials changed since this migration started; abandoning it without touching the "
              "legacy queue");
      break;  // complete stays true: a deliberate, safe abandonment, not a retryable failure
    }
    if (!haveBoundTarget) {
      // Upgrading from the immediately preceding build, which already persisted
      // boCredentialsMigrated before its queue rewrite but did not yet persist a target. That
      // build DID have an interruption window in which the user could reconfigure BookOrbit, so
      // only infer the missing target when the current raw values still byte-match the KOSync
      // credentials this migration copied. Anything else is ambiguous and must be abandoned --
      // privacy beats re-owning legacy reading history to the wrong account.
      if (!matchesExpectedCredentialCopy()) {
        LOG_ERR("BOMigrate",
                "Migration target is missing and BookOrbit credentials no longer match the legacy copy; "
                "abandoning without touching the queue");
        break;
      }
      APP_STATE.boMigrationTargetOwner = newOwner.toHex();
      if (!APP_STATE.saveToFile()) {
        LOG_ERR("BOMigrate", "Could not persist migration target owner; will retry on next boot");
        complete = false;
        break;
      }
    }

    if (newOwner != legacyOwner) {
      // Defensive fallback for the case the comment above argues should not occur in practice
      // (e.g. a non-standard self-hosted deployment that answered the probe at a different
      // path). Verified, not assumed: only a byte-for-byte owner mismatch triggers the rewrite.
      // Safe to call on every attempt, including a resumed one: rewriteOwner() is a no-op for
      // any book already carrying `newOwner`.
      LOG_INF("BOMigrate", "BookOrbit base URL normalization differs from the legacy detection; migrating queues");
      if (!migrateQueueOwners(legacyOwner, newOwner)) {
        LOG_ERR("BOMigrate", "Some queued stats could not be re-owned; will retry on next boot");
        complete = false;
        break;
      }
    }

    // koStatsServerId/koStatsServerKind now mean "BookOrbitCredentialStore's owner" going
    // forward (see CrossPointState.h's doc comment) -- no separate legacy field to retire.
    APP_STATE.koStatsServerId = newOwner.toHex();
    // The legacy e51b383 probe that set koStatsServerKind == 1 predates the
    // bookmarkSync/catalogBulkManifest/catalogDashboardSections capability bits
    // (BookOrbitSyncClient::Capabilities), so leaving it at 1 here would violate
    // CrossPointState.h's own documented invariant that those bits are confirmed ALONGSIDE
    // koStatsServerKind == 1 -- every migrated device would permanently read hasBookmarkSync as
    // false without ever re-probing. Reset to 0 (unconfirmed) so the very next sync performs a
    // real, capability-aware probe instead of trusting this pre-capability-bit confirmation.
    APP_STATE.koStatsServerKind = 0;
  } while (false);

  if (!complete) return;  // leave boMigrationAttempted false so this retries on the next boot

  APP_STATE.boMigrationAttempted = true;
  if (!APP_STATE.saveToFile()) {
    LOG_ERR("BOMigrate", "Could not persist migration-attempted flag; will retry on next boot");
  }
}

}  // namespace BookOrbitMigration
