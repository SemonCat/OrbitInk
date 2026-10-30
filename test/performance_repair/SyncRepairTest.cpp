#include <gtest/gtest.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <CrossPointState.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <KOReaderDocumentId.h>
#include <ProgressPullPolicy.h>
#include <WiFi.h>
#include <activities/reader/EpubReaderUtils.h>

#include <fstream>
#include <sstream>

#include "ProgressSyncDeferred.h"
#include "ProgressSyncNetwork.h"
#include "ProgressSyncPosition.h"

namespace {
class SyncRepair : public testing::Test {
 protected:
  void SetUp() override {
    HalStorage::resetTestFiles();
    APP_STATE = {};
    Epub::defaultCachedPages.reset();
    Epub::compatibleLayout = true;
    Epub::partialLayout = false;
    WiFi = {};
    gpio = {};
    KOReaderDocumentId::reads = 0;
    KOReaderDocumentId::identity = std::string(32, 'a');
    EpubReaderUtils::haveProgress = false;
    EpubReaderUtils::saveWorks = true;
    EpubReaderUtils::saves = 0;
    EpubReaderUtils::timestamp = 0;
    EpubReaderUtils::legacy = false;
  }
  static DeferredProgress remote(bool start = false) {
    DeferredProgress progress;
    progress.provider = 2;
    progress.path = "/book.epub";
    progress.document = KOReaderDocumentId::identity;
    progress.xpath = start ? "/body/DocFragment[2]/body" : "/body/DocFragment[2]/body/p[2]/text().1";
    progress.percentage = 0.7f;
    progress.timestamp = 1800000000;
    return progress;
  }
};

TEST_F(SyncRepair, SharedPositionCacheInvalidatesIdentityPageDenominatorAndLayout) {
  auto book = std::make_shared<Epub>("/book.epub", "/cache");
  const CrossPointPosition position{1, 4, 10};
  HalStorage::registerTestFile("/cache/sections/1.bin", std::vector<uint8_t>(64, 1));
  const std::string identity(32, 'a');
  const auto first = ProgressSyncPosition::resolve(book, position, identity, nullptr);
  ASSERT_FALSE(first.xpath.empty());
  ASSERT_GT(book->streamCalls(), 0);
  const int streams = book->streamCalls();
  const auto warm = ProgressSyncPosition::resolve(book, position, identity, nullptr);
  EXPECT_EQ(warm.xpath, first.xpath);
  EXPECT_NEAR(warm.percentage, first.percentage, 0.000001f);  // JSON's serialized decimal precision.
  EXPECT_EQ(book->streamCalls(), streams);
  ProgressSyncPosition::resolve(book, position, std::string(32, 'b'), nullptr);
  EXPECT_GT(book->streamCalls(), streams);
  int previous = book->streamCalls();
  ProgressSyncPosition::resolve(book, {1, 5, 10}, std::string(32, 'b'), nullptr);
  EXPECT_GT(book->streamCalls(), previous);
  previous = book->streamCalls();
  ProgressSyncPosition::resolve(book, {1, 5, 20}, std::string(32, 'b'), nullptr);
  EXPECT_GT(book->streamCalls(), previous);
  previous = book->streamCalls();
  HalStorage::registerTestFile("/cache/sections/1.bin", std::vector<uint8_t>(64, 2));
  ProgressSyncPosition::resolve(book, {1, 5, 20}, std::string(32, 'b'), nullptr);
  EXPECT_GT(book->streamCalls(), previous);
}

TEST_F(SyncRepair, TornPositionFileFallsBackAndUnverifiedTempNeverBecomesLive) {
  auto book = std::make_shared<Epub>("/book.epub", "/cache");
  HalStorage::registerTestFile("/cache/sync_position.json", {'{'});
  HalStorage::registerTestFile("/cache/sync_position.json.tmp", {'{', '}'});
  const auto position = ProgressSyncPosition::resolve(book, {1, 2, 10}, std::string(32, 'a'), nullptr);
  EXPECT_FALSE(position.xpath.empty());
  EXPECT_GT(book->streamCalls(), 0);
  EXPECT_FALSE(Storage.exists("/cache/sync_position.json.tmp"));
  const auto streams = book->streamCalls();
  ProgressSyncPosition::resolve(book, {1, 2, 10}, std::string(32, 'a'), nullptr);
  EXPECT_EQ(book->streamCalls(), streams);
}

TEST_F(SyncRepair, TwoProvidersShareOneAssociationIdentityAndTeardown) {
  {
    ProgressSyncNetwork::Batch batch("/book.epub");
    ASSERT_TRUE(ProgressSyncNetwork::connect());
    const auto first = ProgressSyncNetwork::binaryDocument("/book.epub");
    ProgressSyncNetwork::disconnect();
    EXPECT_EQ(WiFi.stops, 0);
    ASSERT_TRUE(ProgressSyncNetwork::connect());
    EXPECT_EQ(ProgressSyncNetwork::binaryDocument("/book.epub"), first);
    EXPECT_EQ(KOReaderDocumentId::reads, 1);
    EXPECT_EQ(WiFi.associations, 1);
  }
  EXPECT_EQ(WiFi.stops, 1);
  EXPECT_FALSE(ProgressSyncNetwork::active());
}

TEST_F(SyncRepair, FailedAssociationIsSharedButNextEventRetries) {
  WiFi.succeeds = false;
  {
    ProgressSyncNetwork::Batch batch("/book.epub");
    EXPECT_FALSE(ProgressSyncNetwork::connect());
    EXPECT_FALSE(ProgressSyncNetwork::connect());
    EXPECT_EQ(WiFi.associations, 1);
  }
  WiFi.succeeds = true;
  {
    ProgressSyncNetwork::Batch batch("/book.epub");
    EXPECT_TRUE(ProgressSyncNetwork::connect());
    EXPECT_EQ(WiFi.associations, 2);
  }
  EXPECT_EQ(WiFi.stops, 2);
}

TEST_F(SyncRepair, UnusedBatchLeavesExistingRadioAloneAndButtonAbortIsShared) {
  WiFi.connected = true;
  { ProgressSyncNetwork::Batch batch("/book.epub"); }
  EXPECT_TRUE(WiFi.connected);
  EXPECT_EQ(WiFi.stops, 0);
  WiFi.succeeds = false;
  gpio.pressed = true;
  const auto before = millis();
  {
    ProgressSyncNetwork::Batch batch("/book.epub");
    EXPECT_FALSE(ProgressSyncNetwork::connect());
    EXPECT_FALSE(ProgressSyncNetwork::connect());
    EXPECT_EQ(millis(), before);
    EXPECT_EQ(WiFi.associations, 1);
  }
}

TEST_F(SyncRepair, InteriorAnchorWaitsForCompleteCompatibleLayoutAndSurvivesSaveFailure) {
  GfxRenderer renderer;
  auto candidate = remote();
  ASSERT_TRUE(ProgressSyncDeferred::stage(candidate));
  auto book = std::make_shared<Epub>("/book.epub", "/cache");
  EXPECT_EQ(ProgressSyncDeferred::readerSpine(book), 1);
  EXPECT_FALSE(ProgressSyncDeferred::applyForBook("/book.epub", renderer));
  EXPECT_EQ(EpubReaderUtils::saves, 0);
  Epub::defaultCachedPages = 20;
  Epub::partialLayout = true;
  EXPECT_FALSE(ProgressSyncDeferred::applyForBook("/book.epub", renderer));
  Epub::partialLayout = false;
  Epub::compatibleLayout = false;
  EXPECT_FALSE(ProgressSyncDeferred::applyForBook("/book.epub", renderer));
  EXPECT_TRUE(APP_STATE.deferredProgress.valid());
  Epub::compatibleLayout = true;
  EpubReaderUtils::saveWorks = false;
  EXPECT_FALSE(ProgressSyncDeferred::applyForBook("/book.epub", renderer));
  EXPECT_TRUE(APP_STATE.deferredProgress.valid());
  EpubReaderUtils::saveWorks = true;
  EXPECT_TRUE(ProgressSyncDeferred::applyForBook("/book.epub", renderer));
  EXPECT_EQ(EpubReaderUtils::spine, 1);
  EXPECT_EQ(EpubReaderUtils::pages, 20);
  EXPECT_GT(EpubReaderUtils::page, 0);
  EXPECT_FALSE(APP_STATE.deferredProgress.valid());
}

TEST_F(SyncRepair, ChapterStartUsesUnknownDenominatorWithoutLayoutAndKeepsChronology) {
  GfxRenderer renderer;
  ASSERT_TRUE(ProgressSyncDeferred::stage(remote(true)));
  EXPECT_TRUE(ProgressSyncDeferred::applyForBook("/book.epub", renderer));
  EXPECT_EQ(EpubReaderUtils::spine, 1);
  EXPECT_EQ(EpubReaderUtils::page, 0);
  EXPECT_EQ(EpubReaderUtils::pages, 0);
  EpubReaderUtils::timestamp += 1;
  ASSERT_TRUE(ProgressSyncDeferred::stage(remote()));
  EXPECT_FALSE(ProgressSyncDeferred::applyForBook("/book.epub", renderer));
  EXPECT_FALSE(APP_STATE.deferredProgress.valid());
}

TEST_F(SyncRepair, UnknownChapterStartParticipatesInProviderTimestampOrderingWithoutXhtml) {
  GfxRenderer renderer;
  const auto candidate = remote(true);
  ASSERT_TRUE(ProgressSyncDeferred::stage(candidate));
  ASSERT_TRUE(ProgressSyncDeferred::applyForBook("/book.epub", renderer));
  auto book = std::make_shared<Epub>("/book.epub", "/cache");
  CrossPointPosition position{EpubReaderUtils::spine, EpubReaderUtils::page, EpubReaderUtils::pages};
  const auto fraction = ProgressSyncPosition::chapterProgress(position, book->getSpineItemsCount());
  ASSERT_TRUE(fraction.has_value());
  EXPECT_EQ(*fraction, 0.0f);
  const auto mapped = ProgressSyncPosition::resolve(book, position, candidate.document, &renderer);
  EXPECT_EQ(mapped.xpath, candidate.xpath);
  EXPECT_EQ(book->streamCalls(), 0);
  EXPECT_EQ(book->calculateProgress(position.spineIndex, *fraction), mapped.percentage);
  using namespace ProgressPullPolicy;
  DecisionInput input{true, EpubReaderUtils::timestamp, RemoteState::Found, candidate.timestamp - 1};
  EXPECT_EQ(decide(input), SyncAction::PushLocal);
  input.remoteTimestamp = candidate.timestamp + 1;
  EXPECT_EQ(decide(input), SyncAction::PullRemote);
  input.remoteTimestamp = candidate.timestamp;
  input.localSameAsRemote = true;
  EXPECT_EQ(decide(input), SyncAction::AlreadySynced);
  input.remoteState = RemoteState::Missing;
  EXPECT_EQ(decide(input), SyncAction::PushLocal);
  input.remoteState = RemoteState::Unavailable;
  EXPECT_EQ(decide(input), SyncAction::Abort);
}

TEST_F(SyncRepair, LocalChapterFractionRejectsInvalidRecordsAndPreservesKnownDenominators) {
  EXPECT_EQ(ProgressSyncPosition::chapterProgress({1, 4, 10}, 2), 0.4f);
  EXPECT_EQ(ProgressSyncPosition::chapterProgress({1, 0, 0}, 2), 0.0f);
  for (const CrossPointPosition invalid : {CrossPointPosition{-1, 0, 1}, {2, 0, 1}, {0, -1, 1},
                                           {0, 0, -1}, {0, 1, 0}, {0, 10, 10}}) {
    EXPECT_FALSE(ProgressSyncPosition::chapterProgress(invalid, 2).has_value());
  }
}

TEST_F(SyncRepair, AutomaticMappingUsesTheSamePageStartBoundaryAsTheReader) {
  GfxRenderer renderer;
  auto book = std::make_shared<Epub>("/book.epub", "/cache");
  Epub::defaultCachedPages = 2;
  CrossPointPosition readerPosition{0, 1, 2};
  // The LUT stores the paragraph at page completion; the preceding entry starts page 1.
  readerPosition.paragraphIndex = 1;
  readerPosition.hasParagraphIndex = true;
  const auto expected = ProgressMapper::toSavedProgress(book, readerPosition);
  const auto actual = ProgressSyncPosition::resolve(book, {0, 1, 2}, std::string(32, 'a'), &renderer);
  ASSERT_NE(expected.xpath.find("/p[1]"), std::string::npos);
  EXPECT_EQ(actual.xpath, expected.xpath);
  EXPECT_EQ(actual.percentage, expected.percentage);
  const auto streams = book->streamCalls();
  EXPECT_EQ(ProgressSyncPosition::resolve(book, {0, 1, 2}, std::string(32, 'a'), &renderer).xpath, expected.xpath);
  EXPECT_EQ(book->streamCalls(), streams);
  HalFile cache;
  ASSERT_TRUE(Storage.openFileForRead("Test", "/cache/sync_position.json", cache));
  JsonDocument old;
  ASSERT_FALSE(deserializeJson(old, cache));
  cache.close();
  old["version"] = 1;
  old["xpath"] = "/body/DocFragment[1]/body/p[2]";
  std::string stale;
  serializeJson(old, stale);
  HalStorage::registerTestFile("/cache/sync_position.json", {stale.begin(), stale.end()});
  EXPECT_EQ(ProgressSyncPosition::resolve(book, {0, 1, 2}, std::string(32, 'a'), &renderer).xpath, expected.xpath);
  EXPECT_GT(book->streamCalls(), streams);
}

TEST(SyncReaderContract, EveryProviderEntryUsesSharedLocalPositionValidation) {
  for (const std::string path : {"/src/util/KOReaderAutoSync.cpp", "/src/util/BookOrbitAutoSync.cpp"}) {
    std::ifstream file(std::string(REPO_ROOT) + path);
    const std::string source((std::istreambuf_iterator<char>(file)), {});
    size_t count = 0;
    size_t offset = 0;
    while ((offset = source.find("ProgressSyncPosition::chapterProgress(", offset)) != std::string::npos) {
      ++count;
      ++offset;
    }
    EXPECT_EQ(count, path.find("KOReader") != std::string::npos ? 1u : 2u);
    EXPECT_EQ(source.find("static_cast<float>(pageNumber) / pageCount"), std::string::npos);
    if (path.find("KOReader") != std::string::npos) {
      EXPECT_NE(source.find("usesCrossPointSyncServer() && pageCount > 0"), std::string::npos);
    }
  }
}

TEST_F(SyncRepair, RichChapterStartIgnoresIncompatibleCachedDenominator) {
  GfxRenderer renderer;
  auto candidate = remote(true);
  candidate.provider = 1;
  candidate.hasRichPosition = true;
  candidate.richSpineIndex = 1;
  candidate.richTotalPages = 20;
  Epub::defaultCachedPages = 90;
  Epub::compatibleLayout = false;
  ASSERT_TRUE(ProgressSyncDeferred::stage(candidate));
  EXPECT_TRUE(ProgressSyncDeferred::applyForBook("/book.epub", renderer));
  EXPECT_EQ(EpubReaderUtils::pages, 0);
}

TEST_F(SyncRepair, IdentityMismatchDiscardsHandoffWithoutSavingAndStageRollbackIsAtomic) {
  APP_STATE.saveWorks = false;
  EXPECT_FALSE(ProgressSyncDeferred::stage(remote()));
  EXPECT_FALSE(APP_STATE.deferredProgress.valid());
  APP_STATE.saveWorks = true;
  ASSERT_TRUE(ProgressSyncDeferred::stage(remote()));
  KOReaderDocumentId::identity = std::string(32, 'b');
  GfxRenderer renderer;
  EXPECT_FALSE(ProgressSyncDeferred::applyForBook("/book.epub", renderer));
  EXPECT_FALSE(APP_STATE.deferredProgress.valid());
  EXPECT_EQ(EpubReaderUtils::saves, 0);
}

TEST(SyncReaderContract, PendingAnchorUsesBudgetedBuildAndSuppressesPlaceholderSaves) {
  std::ifstream file(std::string(REPO_ROOT) + "/src/activities/reader/EpubReaderActivity.cpp");
  const std::string source((std::istreambuf_iterator<char>(file)), {});
  ASSERT_NE(source.find("buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK, BACKGROUND_BUILD_BUDGET_MS)"), std::string::npos);
  ASSERT_NE(source.find("while (!remotePositionPending && !section->isBuildComplete()"), std::string::npos);
  const auto flush = source.find("bool EpubReaderActivity::flushPendingProgress()");
  ASSERT_NE(flush, std::string::npos);
  EXPECT_LT(source.find("if (remotePositionPending)", flush), source.find("saveProgress(", flush));
  EXPECT_LT(source.find("quickResumeSkipRenderEligible = false", flush), source.find("saveProgress(", flush));
  const auto pressure = source.find("readerbuild::requiredHeapWaitExpired(");
  ASSERT_NE(pressure, std::string::npos);
  const auto pending = source.find("if (remotePositionPending) {", pressure);
  const auto failure = source.substr(pressure, pending - pressure);
  EXPECT_NE(failure.find("!buildTickHeapGate()"), std::string::npos);
  EXPECT_NE(failure.find("remoteBuildFailed = true"), std::string::npos);
  EXPECT_NE(failure.find("section.reset()"), std::string::npos);
  EXPECT_EQ(failure.find("ProgressSyncDeferred::clear"), std::string::npos);
}
}  // namespace
