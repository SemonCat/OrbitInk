// Tests for the REAL src/util/BookOrbitMigration.cpp -- the one-shot upgrade from e51b383's
// opportunistic BookOrbit detection (a capability probe of whatever server generic KOSync
// happened to be pointed at) to an explicit, standalone BookOrbitCredentialStore.
//
// Compiles the production migration together with the real queue and identity sources against
// an in-memory read-write HalStorage stub and test-settable credential stores, so both halves of
// the migration are exercised end to end: the credential COPY (decision matrix below) and the
// queue OWNER REWRITE fallback (BookOrbitStatsQueue::rewriteOwner, exercised directly with its
// own red-capable coverage in test/bookorbit_stats/BookOrbitQueueTest.cpp -- this suite proves
// BookOrbitMigration.cpp actually reaches it when, and only when, the two stores' getBaseUrl()
// values genuinely disagree).

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "BookOrbitCredentialStore.h"
#include "BookOrbitOwnerId.h"
#include "BookOrbitStatsQueue.h"
#include "CrossPointState.h"
#include "HalStorage.h"
#include "KOReaderCredentialStore.h"
#include "util/BookOrbitMigration.h"

namespace {

constexpr char DOC_HASH[] = "0123456789abcdef0123456789abcdef";

BookOrbitStatEvent makeEvent(const uint32_t startTime) {
  BookOrbitStatEvent e;
  e.startTime = startTime;
  e.durationSeconds = 30;
  e.page = 500;
  e.totalPages = BookOrbitStatsQueue::PROGRESS_SCALE;
  return e;
}

// Seeds a legacy-owned queue file the way e51b383's BookOrbitStatsSync.cpp would have.
void seedLegacyQueue(const BookOrbitOwnerId& legacyOwner, const size_t count = 2) {
  std::vector<BookOrbitStatEvent> events;
  for (size_t i = 0; i < count; i++) events.push_back(makeEvent(1893456000u + static_cast<uint32_t>(i) * 60u));
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(DOC_HASH, legacyOwner, events.data(), events.size()));
}

// Puts APP_STATE/KOREADER_STORE into the exact state e51b383 would have left after confirming
// BookOrbit by capability probe: a KOSync server+account fingerprint recorded as confirmed.
BookOrbitOwnerId confirmLegacyDetection(const std::string& koSyncBaseUrl, const std::string& username,
                                        const std::string& password) {
  KOREADER_STORE.setTestCredentials(true, username, password, "deadbeefdeadbeefdeadbeefdeadbeef");
  KOREADER_STORE.setTestBaseUrl(koSyncBaseUrl);
  const BookOrbitOwnerId owner = BookOrbitOwnerId::derive(koSyncBaseUrl, username);
  APP_STATE.koStatsServerId = owner.toHex();
  APP_STATE.koStatsServerKind = 1;
  return owner;
}

class BookOrbitMigrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    testStorage().reset();
    KOREADER_STORE.resetToDefaults();
    KOREADER_STORE.setTestCredentials(false);  // no legacy KOSync config unless a test opts in
    BOOKORBIT_STORE.resetToDefaults();
    BOOKORBIT_STORE.setTestCredentials(false);  // unconfigured unless a test opts in
    APP_STATE.resetToDefaults();
  }
};

TEST_F(BookOrbitMigrationTest, HappyPathCopiesCredentialsAndTheNormalizedBaseUrl) {
  const BookOrbitOwnerId legacyOwner =
      confirmLegacyDetection("https://legacy.example.test/api/v1/koreader", "alice", "s3cret");

  BookOrbitMigration::runOnce();

  EXPECT_TRUE(BOOKORBIT_STORE.hasCredentials());
  EXPECT_EQ(BOOKORBIT_STORE.getUsername(), "alice");
  EXPECT_EQ(BOOKORBIT_STORE.getPassword(), "s3cret");
  EXPECT_EQ(BOOKORBIT_STORE.getBaseUrl(), "https://legacy.example.test/api/v1/koreader")
      << "the ALREADY-NORMALIZED KOSync base URL must be copied, not a re-derived one";
  EXPECT_TRUE(APP_STATE.boMigrationAttempted);

  // No normalization divergence here, so the owner is unchanged and no queue rewrite is needed.
  const BookOrbitOwnerId newOwner =
      BookOrbitOwnerId::derive(BOOKORBIT_STORE.getBaseUrl(), BOOKORBIT_STORE.getUsername());
  EXPECT_EQ(newOwner, legacyOwner);
  EXPECT_EQ(APP_STATE.koStatsServerId, legacyOwner.toHex());
  EXPECT_EQ(APP_STATE.koStatsServerKind, 0)
      << "the legacy probe predates the capability bits (bookmarkSync/catalogBulkManifest/"
      << "catalogDashboardSections); leaving kind==1 would falsely read as an already-confirmed, "
      << "capability-aware probe and skip re-probing them forever";
}

TEST_F(BookOrbitMigrationTest, AnAlreadyConfiguredBookOrbitStoreIsNeverOverwritten) {
  const BookOrbitOwnerId legacyOwner =
      confirmLegacyDetection("https://legacy.example.test/api/v1/koreader", "alice", "s3cret");
  seedLegacyQueue(legacyOwner, 2);
  BOOKORBIT_STORE.setTestCredentials(true, "bob", "different-pw", "cafebabecafebabecafebabecafebabe");
  BOOKORBIT_STORE.setTestBaseUrl("https://bobs-own-server.test/api/v1/koreader");

  BookOrbitMigration::runOnce();

  EXPECT_EQ(BOOKORBIT_STORE.getUsername(), "bob") << "an explicitly configured provider must never be overwritten";
  EXPECT_EQ(BOOKORBIT_STORE.getBaseUrl(), "https://bobs-own-server.test/api/v1/koreader");
  EXPECT_TRUE(APP_STATE.boMigrationAttempted);

  // The regression this guards: hasCredentials() alone cannot distinguish "bob's own account"
  // from "a resumed migration" -- the fix must still recognize this as the former and never
  // touch alice's legacy-owned queue, let alone reassign it to bob's fingerprint.
  const BookOrbitOwnerId bobOwner =
      BookOrbitOwnerId::derive(BOOKORBIT_STORE.getBaseUrl(), BOOKORBIT_STORE.getUsername());
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(DOC_HASH, legacyOwner), 2u)
      << "alice's queue must remain exactly where it was";
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(DOC_HASH, bobOwner), 0u)
      << "must never be reassigned to an unrelated account's fingerprint";
}

TEST_F(BookOrbitMigrationTest, AnInterruptedMultiBookMigrationResumesTheRemainingQueueRewrites) {
  // Reproduces the scenario a P0 review pass caught: a previous boot copied the credentials
  // (committing CrossPointState::boCredentialsMigrated) and rewrote book A's queue, then crashed
  // before reaching book B and before boMigrationAttempted could be saved. The next boot must
  // finish book B's rewrite rather than treating "BOOKORBIT_STORE already has credentials" as
  // proof there is nothing left to do.
  const std::string koSyncUrl = "https://legacy.example.test/api/v1/koreader/";  // trailing slash
  const BookOrbitOwnerId legacyOwner = confirmLegacyDetection(koSyncUrl, "alice", "s3cret");

  constexpr char DOC_HASH_A[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  constexpr char DOC_HASH_B[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  std::vector<BookOrbitStatEvent> events{makeEvent(1893456000u)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(DOC_HASH_A, legacyOwner, events.data(), events.size()));
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(DOC_HASH_B, legacyOwner, events.data(), events.size()));

  // Simulate the state left by the interrupted boot: credentials already copied and committed,
  // book A already rewritten to the new owner, book B still stranded under the legacy owner.
  BOOKORBIT_STORE.setCredentials("alice", "s3cret");
  BOOKORBIT_STORE.setServerUrl(koSyncUrl);
  const BookOrbitOwnerId newOwner = BookOrbitOwnerId::derive(BOOKORBIT_STORE.getBaseUrl(), "alice");
  ASSERT_NE(newOwner, legacyOwner) << "test setup must reproduce the owner-normalization divergence";
  ASSERT_TRUE(BookOrbitStatsQueue::rewriteOwner(DOC_HASH_A, legacyOwner, newOwner));
  APP_STATE.boCredentialsMigrated = true;
  ASSERT_FALSE(APP_STATE.boMigrationAttempted) << "the crash happened before this could be saved";

  BookOrbitMigration::runOnce();

  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(DOC_HASH_A, newOwner), 1u) << "already-migrated book must stay migrated";
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(DOC_HASH_B, legacyOwner), 0u)
      << "book B must no longer be stranded under the legacy owner";
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(DOC_HASH_B, newOwner), 1u)
      << "book B's events must have survived the resumed migration";
  EXPECT_TRUE(APP_STATE.boMigrationAttempted);
}

TEST_F(BookOrbitMigrationTest, NoLegacyKoSyncCredentialsIsANoOp) {
  APP_STATE.koStatsServerKind = 1;  // pathological: confirmed with no credentials at all
  BookOrbitMigration::runOnce();

  EXPECT_FALSE(BOOKORBIT_STORE.hasCredentials());
  EXPECT_TRUE(APP_STATE.boMigrationAttempted);
}

TEST_F(BookOrbitMigrationTest, AnUnconfirmedLegacyDetectionIsANoOp) {
  KOREADER_STORE.setTestCredentials(true, "alice", "s3cret", "deadbeefdeadbeefdeadbeefdeadbeef");
  KOREADER_STORE.setTestBaseUrl("https://legacy.example.test/api/v1/koreader");
  APP_STATE.koStatsServerKind = 0;  // never confirmed BookOrbit

  BookOrbitMigration::runOnce();

  EXPECT_FALSE(BOOKORBIT_STORE.hasCredentials()) << "nothing to migrate without a confirmed detection";
  EXPECT_TRUE(APP_STATE.boMigrationAttempted);
}

TEST_F(BookOrbitMigrationTest, AStaleLegacyOwnerFromASinceChangedServerIsNotMigrated) {
  // The confirmation was recorded against a DIFFERENT server/account than KOSync is configured
  // for now -- the user repointed KOSync elsewhere after the probe. Migrating it would hand the
  // new server credentials that were never configured for it.
  confirmLegacyDetection("https://old-server.example.test/api/v1/koreader", "alice", "s3cret");
  KOREADER_STORE.setTestBaseUrl("https://new-server.example.test/api/v1/koreader");

  BookOrbitMigration::runOnce();

  EXPECT_FALSE(BOOKORBIT_STORE.hasCredentials());
  EXPECT_TRUE(APP_STATE.boMigrationAttempted);
}

TEST_F(BookOrbitMigrationTest, RunsExactlyOnceEvenIfCredentialsAreClearedAfterwards) {
  confirmLegacyDetection("https://legacy.example.test/api/v1/koreader", "alice", "s3cret");
  BookOrbitMigration::runOnce();
  ASSERT_TRUE(BOOKORBIT_STORE.hasCredentials());

  // The user deliberately clears BookOrbit credentials after migrating (e.g. to stop using it).
  BOOKORBIT_STORE.clearCredentials();
  ASSERT_FALSE(BOOKORBIT_STORE.hasCredentials());

  BookOrbitMigration::runOnce();  // must be a no-op: the flag is already set

  EXPECT_FALSE(BOOKORBIT_STORE.hasCredentials())
      << "a cleared BookOrbit config must not be silently repopulated from the legacy detection";
}

TEST_F(BookOrbitMigrationTest, MigrationStillAppliesWhenPersistingTheFlagFails) {
  // Safe-retry property: every step besides the final flag-save is idempotent, so even if the
  // flag itself fails to persist, the credential copy this boot performed is not undone or
  // duplicated -- a later boot simply finds BOOKORBIT_STORE already configured and skips ahead.
  confirmLegacyDetection("https://legacy.example.test/api/v1/koreader", "alice", "s3cret");
  APP_STATE.saveShouldFail = true;

  BookOrbitMigration::runOnce();

  EXPECT_TRUE(BOOKORBIT_STORE.hasCredentials()) << "the copy must still happen even if persisting the flag fails";
}

TEST_F(BookOrbitMigrationTest, OwnerDivergenceTriggersAQueueRewriteAndCredentialsStillMigrate) {
  // The defensive fallback: BookOrbitCredentialStore's own normalization (here, a trailing-slash
  // strip -- see the stub's setServerUrl()) can legitimately produce a getBaseUrl() that differs
  // from KOReaderCredentialStore's, even for what was functionally the same server. When that
  // happens, already-queued, not-yet-uploaded reading events must not be silently orphaned.
  const std::string koSyncUrl = "https://legacy.example.test/api/v1/koreader/";  // trailing slash
  const BookOrbitOwnerId legacyOwner = confirmLegacyDetection(koSyncUrl, "alice", "s3cret");
  seedLegacyQueue(legacyOwner, 3);
  ASSERT_EQ(BookOrbitStatsQueue::queuedCount(DOC_HASH, legacyOwner), 3u);

  BookOrbitMigration::runOnce();

  ASSERT_TRUE(BOOKORBIT_STORE.hasCredentials());
  const std::string newBaseUrl = BOOKORBIT_STORE.getBaseUrl();
  ASSERT_NE(newBaseUrl, koSyncUrl) << "the stub's setServerUrl() must have actually stripped the trailing slash";
  const BookOrbitOwnerId newOwner = BookOrbitOwnerId::derive(newBaseUrl, BOOKORBIT_STORE.getUsername());
  ASSERT_NE(newOwner, legacyOwner) << "test setup must actually produce a genuine owner divergence";

  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(DOC_HASH, legacyOwner), 0u) << "no longer readable under the old owner";
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(DOC_HASH, newOwner), 3u)
      << "queued reading events must survive the owner-normalization divergence";
  EXPECT_EQ(APP_STATE.koStatsServerId, newOwner.toHex());
}

TEST_F(BookOrbitMigrationTest, CredentialSaveFailureDoesNotLatchAnIncompleteMigrationAndRetriesNextBoot) {
  // Regression: BOOKORBIT_STORE.saveToFile()'s return value used to be ignored entirely, so a
  // failed write still let boMigrationAttempted latch at the end -- permanently abandoning the
  // migration with no credentials ever durably saved, and no way to retry. The failed copy must
  // also be rolled back IN MEMORY: otherwise the not-durable account becomes operational for the
  // rest of this boot and can capture/sync against queues the migration has not reconciled yet.
  confirmLegacyDetection("https://legacy.example.test/api/v1/koreader", "alice", "s3cret");
  BOOKORBIT_STORE.setTestCredentials(false, "draft-user", "");
  BOOKORBIT_STORE.setTestBaseUrl("https://draft.example.test");
  BOOKORBIT_STORE.saveShouldFail = true;

  BookOrbitMigration::runOnce();

  EXPECT_FALSE(APP_STATE.boMigrationAttempted) << "must not latch while the credential save never durably succeeded";
  EXPECT_FALSE(APP_STATE.boCredentialsMigrated);
  EXPECT_FALSE(BOOKORBIT_STORE.hasCredentials()) << "the failed copy must not become active for this boot";
  EXPECT_EQ(BOOKORBIT_STORE.getUsername(), "draft-user");
  EXPECT_TRUE(BOOKORBIT_STORE.getPassword().empty());
  EXPECT_EQ(BOOKORBIT_STORE.getServerUrl(), "https://draft.example.test")
      << "rollback must restore the exact raw pre-migration URL, not a normalized substitute";

  // On a later boot the write succeeds; the migration must actually complete then.
  BOOKORBIT_STORE.saveShouldFail = false;
  BookOrbitMigration::runOnce();

  EXPECT_TRUE(APP_STATE.boMigrationAttempted);
  EXPECT_TRUE(APP_STATE.boCredentialsMigrated);
  EXPECT_TRUE(BOOKORBIT_STORE.hasCredentials());
  EXPECT_EQ(BOOKORBIT_STORE.getUsername(), "alice");
}

TEST_F(BookOrbitMigrationTest, AQueueRewriteFailureDoesNotLatchAnIncompleteMigrationAndRetriesNextBoot) {
  // Regression: migrateQueueOwners()'s per-book failures were only logged; the caller had no way
  // to know a book was left stranded under the legacy owner, so boMigrationAttempted latched
  // anyway and that book's queued events were never re-attempted.
  const std::string koSyncUrl = "https://legacy.example.test/api/v1/koreader/";  // trailing slash
  const BookOrbitOwnerId legacyOwner = confirmLegacyDetection(koSyncUrl, "alice", "s3cret");
  seedLegacyQueue(legacyOwner, 2);

  testStorage().failRenames = true;  // rewriteOwner()'s tmp->dest promote needs a rename to succeed
  BookOrbitMigration::runOnce();

  EXPECT_FALSE(APP_STATE.boMigrationAttempted) << "must not latch while a book's queue rewrite failed";
  EXPECT_TRUE(APP_STATE.boCredentialsMigrated) << "the credential copy itself already succeeded and is durable";
  EXPECT_TRUE(BOOKORBIT_STORE.hasCredentials());
  const BookOrbitOwnerId newOwner =
      BookOrbitOwnerId::derive(BOOKORBIT_STORE.getBaseUrl(), BOOKORBIT_STORE.getUsername());
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(DOC_HASH, legacyOwner), 2u) << "left exactly where it was, not lost";

  testStorage().failRenames = false;
  BookOrbitMigration::runOnce();

  EXPECT_TRUE(APP_STATE.boMigrationAttempted);
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(DOC_HASH, legacyOwner), 0u);
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(DOC_HASH, newOwner), 2u) << "retried and completed on the later boot";
}

TEST_F(BookOrbitMigrationTest, AResetBetweenTheCredentialSaveAndItsMarkerResumesRatherThanAbandoning) {
  // Regression: hasCredentials() alone cannot distinguish "an earlier boot's own copy, whose
  // saveToFile() succeeded but crashed before boCredentialsMigrated could be persisted" from "an
  // unrelated account the user independently configured". The old code treated both the same way
  // (break, abandon), which for the FIRST case meant the queue rewrite and koStatsServerId update
  // never happened even though the credentials were already exactly right.
  //
  // No trailing slash here (contrast the divergence-rewrite tests above): this test isolates the
  // resume-vs-abandon decision, which must not depend on the two stores' base-URL normalization
  // agreeing or disagreeing.
  const std::string koSyncUrl = "https://legacy.example.test/api/v1/koreader";
  const BookOrbitOwnerId legacyOwner = confirmLegacyDetection(koSyncUrl, "alice", "s3cret");
  seedLegacyQueue(legacyOwner, 1);

  // Simulate exactly what a prior boot's successful-but-unmarked copy left behind.
  BOOKORBIT_STORE.setCredentials("alice", "s3cret");
  BOOKORBIT_STORE.setServerUrl(koSyncUrl);
  ASSERT_FALSE(APP_STATE.boCredentialsMigrated) << "the crash happened before this could be saved";

  BookOrbitMigration::runOnce();

  EXPECT_TRUE(APP_STATE.boMigrationAttempted);
  EXPECT_TRUE(APP_STATE.boCredentialsMigrated)
      << "must recognize this as its own earlier copy and resume, not abandon it as a stranger's account";
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(DOC_HASH, legacyOwner), 1u)
      << "no normalization divergence here, so the queue never needed to move";
  EXPECT_EQ(APP_STATE.koStatsServerId, legacyOwner.toHex());
}

TEST_F(BookOrbitMigrationTest, CredentialsChangedSinceMigrationStartedAbandonsWithoutTouchingTheLegacyQueue) {
  // Regression: the migration marker was not bound to the account it started FOR. If the user
  // reconfigures BookOrbitCredentialStore between an interrupted attempt and its retry, the queue
  // rewrite must not run against the NEW (unrelated) account using the OLD legacy-detection owner.
  const std::string koSyncUrl = "https://legacy.example.test/api/v1/koreader";
  const BookOrbitOwnerId legacyOwner = confirmLegacyDetection(koSyncUrl, "alice", "s3cret");
  seedLegacyQueue(legacyOwner, 2);

  // Simulate a prior boot that copied credentials, recorded the target owner it was for, but was
  // interrupted before the queue rewrite / boMigrationAttempted commit.
  BOOKORBIT_STORE.setCredentials("alice", "s3cret");
  BOOKORBIT_STORE.setServerUrl(koSyncUrl);
  APP_STATE.boCredentialsMigrated = true;
  APP_STATE.boMigrationTargetOwner =
      BookOrbitOwnerId::derive(BOOKORBIT_STORE.getBaseUrl(), BOOKORBIT_STORE.getUsername()).toHex();

  // Between that boot and this one, the user pointed BookOrbit at an unrelated account.
  BOOKORBIT_STORE.setCredentials("mallory", "different-pw");
  BOOKORBIT_STORE.setServerUrl("https://unrelated-server.test/api/v1/koreader");
  const BookOrbitOwnerId malloryOwner =
      BookOrbitOwnerId::derive(BOOKORBIT_STORE.getBaseUrl(), BOOKORBIT_STORE.getUsername());

  BookOrbitMigration::runOnce();

  EXPECT_TRUE(APP_STATE.boMigrationAttempted) << "must abandon (not retry forever) once the mismatch is detected";
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(DOC_HASH, legacyOwner), 2u) << "alice's legacy queue must be untouched";
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(DOC_HASH, malloryOwner), 0u)
      << "must never be re-owned to an account this migration was never for";
}

TEST_F(BookOrbitMigrationTest, UpgradingFromABuildWithNoRecordedTargetOwnerSelfHealsRatherThanAbandoning) {
  // boMigrationTargetOwner did not exist before this fix. A state.json from that build can have
  // boCredentialsMigrated already true with no recorded target. When the BookOrbit credentials
  // still byte-match the KOSync values that migration copied, the target is provable and can be
  // recorded before proceeding rather than abandoning intact work.
  const std::string koSyncUrl = "https://legacy.example.test/api/v1/koreader/";  // trailing slash
  const BookOrbitOwnerId legacyOwner = confirmLegacyDetection(koSyncUrl, "alice", "s3cret");
  seedLegacyQueue(legacyOwner, 2);

  BOOKORBIT_STORE.setCredentials("alice", "s3cret");
  BOOKORBIT_STORE.setServerUrl(koSyncUrl);
  APP_STATE.boCredentialsMigrated = true;
  ASSERT_TRUE(APP_STATE.boMigrationTargetOwner.empty()) << "simulating a pre-fix state.json";

  BookOrbitMigration::runOnce();

  EXPECT_TRUE(APP_STATE.boMigrationAttempted);
  const BookOrbitOwnerId newOwner =
      BookOrbitOwnerId::derive(BOOKORBIT_STORE.getBaseUrl(), BOOKORBIT_STORE.getUsername());
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(DOC_HASH, newOwner), 2u)
      << "the migration must have proceeded normally, not been abandoned";
  EXPECT_FALSE(APP_STATE.boMigrationTargetOwner.empty()) << "must self-heal by recording the current owner";
}

TEST_F(BookOrbitMigrationTest, PreBindingStateWithChangedCredentialsAbandonsWithoutReowningTheLegacyQueue) {
  // The build immediately before boMigrationTargetOwner existed already had a durable
  // boCredentialsMigrated marker and could reset between saving that marker and rewriting the
  // queue. With no target recorded, current credentials are safe to infer only when they still
  // byte-match the KOSync values the migration originally copied. A different BookOrbit account
  // is ambiguous and must be abandoned, never adopted as the migration target.
  const std::string koSyncUrl = "https://legacy.example.test/api/v1/koreader/";
  const BookOrbitOwnerId legacyOwner = confirmLegacyDetection(koSyncUrl, "alice", "s3cret");
  seedLegacyQueue(legacyOwner, 2);

  APP_STATE.boCredentialsMigrated = true;
  ASSERT_TRUE(APP_STATE.boMigrationTargetOwner.empty()) << "simulating the pre-binding state format";
  BOOKORBIT_STORE.setCredentials("mallory", "different-pw");
  BOOKORBIT_STORE.setServerUrl("https://unrelated-server.test/api/v1/koreader");
  const BookOrbitOwnerId malloryOwner =
      BookOrbitOwnerId::derive(BOOKORBIT_STORE.getBaseUrl(), BOOKORBIT_STORE.getUsername());

  BookOrbitMigration::runOnce();

  EXPECT_TRUE(APP_STATE.boMigrationAttempted) << "the ambiguous old state must be abandoned, not retried forever";
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(DOC_HASH, legacyOwner), 2u)
      << "the legacy account's queue must remain untouched";
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(DOC_HASH, malloryOwner), 0u)
      << "an unrelated current account must never become an inferred migration target";
}

TEST_F(BookOrbitMigrationTest, DiscoversAndDedupesTempAndBackupQueueStatesAlongsideTheLiveFile) {
  // Regression: the scan only recognized ".bin", so a book whose ONLY on-disk state was a
  // ".bin.tmp" or ".bin.bak" (left by an interrupted BookOrbitStatsQueue replacement) was never
  // discovered and its events were silently left under the legacy owner forever.
  const std::string koSyncUrl = "https://legacy.example.test/api/v1/koreader/";  // trailing slash
  const BookOrbitOwnerId legacyOwner = confirmLegacyDetection(koSyncUrl, "alice", "s3cret");

  constexpr char hashA[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";  // only a live .bin
  constexpr char hashB[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";  // only a .bin.bak (no live file)
  seedLegacyQueue(legacyOwner, 1);                              // uses DOC_HASH, distinct from hashA/hashB below
  {
    std::vector<BookOrbitStatEvent> events{makeEvent(1893456000u)};
    ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(hashA, legacyOwner, events.data(), events.size()));
  }
  {
    std::vector<BookOrbitStatEvent> events{makeEvent(1893456100u)};
    ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(hashB, legacyOwner, events.data(), events.size()));
  }
  // Simulate an interrupted replacement for book B: only the backup survives, no live file.
  const std::string liveB = BookOrbitStatsQueue::filePath(hashB);
  const std::string bakB = BookOrbitStatsQueue::backupPath(hashB);
  auto it = testStorage().files.find(liveB);
  ASSERT_NE(it, testStorage().files.end());
  testStorage().files[bakB] = it->second;
  testStorage().files.erase(it);
  ASSERT_FALSE(Storage.exists(liveB.c_str()));
  ASSERT_TRUE(Storage.exists(bakB.c_str()));

  BookOrbitMigration::runOnce();

  const BookOrbitOwnerId newOwner =
      BookOrbitOwnerId::derive(BOOKORBIT_STORE.getBaseUrl(), BOOKORBIT_STORE.getUsername());
  ASSERT_NE(newOwner, legacyOwner) << "test setup must produce a genuine owner divergence";
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(DOC_HASH, newOwner), 1u);
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(hashA, newOwner), 1u);
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(hashB, newOwner), 1u)
      << "a book whose only on-disk state was a .bak must still be discovered and re-owned";
}

TEST_F(BookOrbitMigrationTest, AnUnreadableQueueDirectoryIsNotTreatedAsAbsentAndRetriesOnTheNextBoot) {
  // Regression: an unreadable-but-existing directory (a transient SD error) was treated the same
  // as a genuinely absent one -- reported success with the directory unscanned, permanently
  // stranding whatever it held under the legacy owner.
  const std::string koSyncUrl = "https://legacy.example.test/api/v1/koreader/";  // trailing slash
  const BookOrbitOwnerId legacyOwner = confirmLegacyDetection(koSyncUrl, "alice", "s3cret");
  seedLegacyQueue(legacyOwner, 2);

  const std::string dirPath = BookOrbitStatsQueue::storageDirectory();
  ASSERT_TRUE(Storage.exists(dirPath.c_str())) << "the directory genuinely exists (it holds the seeded queue file)";
  testStorage().failOpenDir = true;  // transient failure to OPEN it, not absence

  BookOrbitMigration::runOnce();

  EXPECT_FALSE(APP_STATE.boMigrationAttempted) << "must not latch complete over an unscanned directory";
  EXPECT_TRUE(APP_STATE.boCredentialsMigrated) << "the credential copy itself already succeeded and is durable";
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(DOC_HASH, legacyOwner), 2u) << "left exactly where it was, unmigrated";

  // On a later boot the directory becomes readable again; the migration must actually complete.
  testStorage().failOpenDir = false;
  BookOrbitMigration::runOnce();

  EXPECT_TRUE(APP_STATE.boMigrationAttempted);
  const BookOrbitOwnerId newOwner =
      BookOrbitOwnerId::derive(BOOKORBIT_STORE.getBaseUrl(), BOOKORBIT_STORE.getUsername());
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(DOC_HASH, newOwner), 2u) << "retried and completed on the later boot";
}

TEST_F(BookOrbitMigrationTest, AnInconclusiveQueueDirectoryProbeRemainsRetryable) {
  const std::string koSyncUrl = "https://legacy.example.test/api/v1/koreader/";
  const BookOrbitOwnerId legacyOwner = confirmLegacyDetection(koSyncUrl, "alice", "s3cret");
  seedLegacyQueue(legacyOwner, 2);

  const std::string dirPath = BookOrbitStatsQueue::storageDirectory();
  testStorage().failPathProbes.insert(dirPath);
  BookOrbitMigration::runOnce();

  EXPECT_FALSE(APP_STATE.boMigrationAttempted) << "an I/O error is not proof that the queue directory is absent";
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(DOC_HASH, legacyOwner), 2u);

  testStorage().failPathProbes.clear();
  BookOrbitMigration::runOnce();
  const BookOrbitOwnerId newOwner =
      BookOrbitOwnerId::derive(BOOKORBIT_STORE.getBaseUrl(), BOOKORBIT_STORE.getUsername());
  EXPECT_TRUE(APP_STATE.boMigrationAttempted);
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(DOC_HASH, newOwner), 2u);
}

TEST_F(BookOrbitMigrationTest, APartialDirectoryEnumerationRemainsRetryable) {
  const std::string koSyncUrl = "https://legacy.example.test/api/v1/koreader/";
  const BookOrbitOwnerId legacyOwner = confirmLegacyDetection(koSyncUrl, "alice", "s3cret");
  constexpr char secondHash[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  seedLegacyQueue(legacyOwner, 1);
  std::vector<BookOrbitStatEvent> events{makeEvent(1893456120u)};
  ASSERT_TRUE(BookOrbitStatsQueue::appendBatch(secondHash, legacyOwner, events.data(), events.size()));

  testStorage().failDirectoryReadAfter = 1;
  BookOrbitMigration::runOnce();

  EXPECT_FALSE(APP_STATE.boMigrationAttempted) << "a read failure is not clean end-of-directory";
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(DOC_HASH, legacyOwner), 1u);
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(secondHash, legacyOwner), 1u)
      << "the scan must finish before any owner rewrite begins";

  testStorage().failDirectoryReadAfter = SIZE_MAX;
  BookOrbitMigration::runOnce();
  const BookOrbitOwnerId newOwner =
      BookOrbitOwnerId::derive(BOOKORBIT_STORE.getBaseUrl(), BOOKORBIT_STORE.getUsername());
  EXPECT_TRUE(APP_STATE.boMigrationAttempted);
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(DOC_HASH, newOwner), 1u);
  EXPECT_EQ(BookOrbitStatsQueue::queuedCount(secondHash, newOwner), 1u);
}

TEST_F(BookOrbitMigrationTest, NoQueueFileYetIsHandledCleanly) {
  // The common case for a freshly confirmed BookOrbit detection that had not yet captured any
  // events: the migration's directory scan must not fail or fabricate a queue.
  confirmLegacyDetection("https://legacy.example.test/api/v1/koreader", "alice", "s3cret");

  BookOrbitMigration::runOnce();

  EXPECT_TRUE(BOOKORBIT_STORE.hasCredentials());
}

}  // namespace

// ---- Firmware-only symbols, supplied for the host link ----------------------------------------
// Same rationale as BookOrbitSyncPolicyTest.cpp: BookOrbitOwnerId::derive(url, user) and
// bookOrbitFirmwareSha256() live in the mbedTLS translation unit, which cannot build here.
namespace {
void hostDigest(const uint8_t* data, const size_t len, uint8_t out[32]) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    h ^= data[i];
    h *= 16777619u;
  }
  for (int i = 0; i < 32; i++) {
    h ^= static_cast<uint32_t>(i + 1) * 2654435761u;
    h *= 16777619u;
    out[i] = static_cast<uint8_t>((h >> 13) & 0xFF);
  }
}
}  // namespace

BookOrbitSha256Fn bookOrbitFirmwareSha256() { return &hostDigest; }

BookOrbitOwnerId BookOrbitOwnerId::derive(const std::string& baseUrl, const std::string& username) {
  return derive(baseUrl, username, &hostDigest);
}
