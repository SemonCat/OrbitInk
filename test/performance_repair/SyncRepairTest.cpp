#include <gtest/gtest.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <CrossPointState.h>
#include <Epub/SectionMetadata.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <KOReaderDocumentId.h>
#include <ProgressPullPolicy.h>
#include <WiFi.h>
#include <activities/reader/EpubReaderUtils.h>

#include <cstring>
#include <fstream>
#include <limits>
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
  static void finalizedSection(int spine = 0) {
    constexpr uint16_t count = 2;
    constexpr uint32_t lutOffset = 128;
    std::vector<uint8_t> bytes(lutOffset + sizeof(count) * (count + 1), 0);
    bytes[0] = sectionmetadata::FILE_VERSION;
    std::memcpy(bytes.data() + sectionmetadata::HEADER_SIZE - sizeof(uint32_t) * 4 - sizeof(count), &count, sizeof(count));
    std::memcpy(bytes.data() + sectionmetadata::HEADER_SIZE - sizeof(uint32_t) * 2, &lutOffset, sizeof(lutOffset));
    std::memcpy(bytes.data() + lutOffset, &count, sizeof(count));
    for (uint16_t i = 0; i < count; ++i) {
      const uint16_t paragraph = i + 1;
      std::memcpy(bytes.data() + lutOffset + sizeof(count) * (i + 1), &paragraph, sizeof(paragraph));
    }
    HalStorage::registerTestFile("/cache/sections/" + std::to_string(spine) + ".bin", std::move(bytes));
  }
};

TEST_F(SyncRepair, SharedPositionCacheInvalidatesIdentityPageDenominatorAndLayout) {
  auto book = std::make_shared<Epub>("/book.epub", "/cache");
  const CrossPointPosition position{1, 4, 10};
  finalizedSection(1);
  const std::string identity(32, 'a');
  const auto first = ProgressSyncPosition::resolve(book, position, identity);
  ASSERT_FALSE(first.xpath.empty());
  ASSERT_GT(book->streamCalls(), 0);
  const int streams = book->streamCalls();
  const auto warm = ProgressSyncPosition::resolve(book, position, identity);
  EXPECT_EQ(warm.xpath, first.xpath);
  EXPECT_NEAR(warm.percentage, first.percentage, 0.000001f);  // JSON's serialized decimal precision.
  EXPECT_EQ(book->streamCalls(), streams);
  ProgressSyncPosition::resolve(book, position, std::string(32, 'b'));
  EXPECT_GT(book->streamCalls(), streams);
  int previous = book->streamCalls();
  ProgressSyncPosition::resolve(book, {1, 5, 10}, std::string(32, 'b'));
  EXPECT_GT(book->streamCalls(), previous);
  previous = book->streamCalls();
  ProgressSyncPosition::resolve(book, {1, 5, 20}, std::string(32, 'b'));
  EXPECT_GT(book->streamCalls(), previous);
  previous = book->streamCalls();
  auto changedLayout = HalStorage::readTestFile("/cache/sections/1.bin");
  changedLayout[1] = 42;  // Different font ID, same finalized page-count metadata.
  HalStorage::registerTestFile("/cache/sections/1.bin", std::move(changedLayout));
  ProgressSyncPosition::resolve(book, {1, 5, 20}, std::string(32, 'b'));
  EXPECT_GT(book->streamCalls(), previous);
}

TEST_F(SyncRepair, TornPositionFileFallsBackAndUnverifiedTempNeverBecomesLive) {
  auto book = std::make_shared<Epub>("/book.epub", "/cache");
  HalStorage::registerTestFile("/cache/sync_position.json", {'{'});
  HalStorage::registerTestFile("/cache/sync_position.json.tmp", {'{', '}'});
  const auto position = ProgressSyncPosition::resolve(book, {1, 2, 10}, std::string(32, 'a'));
  EXPECT_FALSE(position.xpath.empty());
  EXPECT_GT(book->streamCalls(), 0);
  EXPECT_FALSE(Storage.exists("/cache/sync_position.json.tmp"));
  const auto streams = book->streamCalls();
  ProgressSyncPosition::resolve(book, {1, 2, 10}, std::string(32, 'a'));
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
  const auto mapped = ProgressSyncPosition::resolve(book, position, candidate.document);
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
  auto book = std::make_shared<Epub>("/book.epub", "/cache");
  Epub::defaultCachedPages = 2;
  finalizedSection();
  CrossPointPosition readerPosition{0, 1, 2};
  // The LUT stores the paragraph at page completion; the preceding entry starts page 1.
  readerPosition.paragraphIndex = 1;
  readerPosition.hasParagraphIndex = true;
  const auto expected = ProgressMapper::toSavedProgress(book, readerPosition);
  const auto actual = ProgressSyncPosition::resolve(book, {0, 1, 2}, std::string(32, 'a'));
  ASSERT_NE(expected.xpath.find("/p[1]"), std::string::npos);
  EXPECT_EQ(actual.xpath, expected.xpath);
  EXPECT_EQ(actual.percentage, expected.percentage);
  const auto streams = book->streamCalls();
  EXPECT_EQ(ProgressSyncPosition::resolve(book, {0, 1, 2}, std::string(32, 'a')).xpath, expected.xpath);
  EXPECT_EQ(book->streamCalls(), streams);
  HalFile cache;
  ASSERT_TRUE(Storage.openFileForRead("Test", "/cache/sync_position.json", cache));
  JsonDocument old;
  ASSERT_FALSE(deserializeJson(old, cache));
  cache.close();
  for (int version : {1, 2}) {
    old["version"] = version;
    old["xpath"] = "/body/DocFragment[1]/body/p[2]";
    std::string stale;
    serializeJson(old, stale);
    HalStorage::registerTestFile("/cache/sync_position.json", {stale.begin(), stale.end()});
    EXPECT_EQ(ProgressSyncPosition::resolve(book, {0, 1, 2}, std::string(32, 'a')).xpath, expected.xpath);
    EXPECT_GT(book->streamCalls(), streams);
  }
}

TEST_F(SyncRepair, HeadlessCacheMissCannotReplaceTheReadersParagraphBoundary) {
  auto book = std::make_shared<Epub>("/book.epub", "/cache");
  Epub::defaultCachedPages = 2;
  finalizedSection();
  CrossPointPosition readerPosition{0, 1, 2};
  readerPosition.paragraphIndex = 1;
  readerPosition.hasParagraphIndex = true;
  const auto expected = ProgressMapper::toSavedProgress(book, readerPosition);
  const auto headless = ProgressSyncPosition::resolve(book, {0, 1, 2}, std::string(32, 'a'));
  EXPECT_EQ(headless.xpath, expected.xpath);
  const auto streams = book->streamCalls();
  EXPECT_EQ(ProgressSyncPosition::resolve(book, {0, 1, 2}, std::string(32, 'a')).xpath, expected.xpath);
  EXPECT_EQ(book->streamCalls(), streams);
}

TEST_F(SyncRepair, FailedParagraphMetadataReadRetriesBeforeCachingTheAnchor) {
  auto book = std::make_shared<Epub>("/book.epub", "/cache");
  for (const size_t failAt : {size_t{19}, size_t{128}}) {
    SCOPED_TRACE(failAt);
    finalizedSection();
    Storage.remove("/cache/sync_position.json");
    HalStorage::setReadFailure("/cache/sections/0.bin", failAt);
    const auto writes = HalStorage::writeOpenAttempts("/cache/sync_position.json.tmp");
    const auto coarse = ProgressSyncPosition::resolve(book, {0, 1, 2}, std::string(32, 'a'));
    EXPECT_NE(coarse.xpath.find("/p[2]"), std::string::npos);
    EXPECT_EQ(HalStorage::writeOpenAttempts("/cache/sync_position.json.tmp"), writes);
    const auto streams = book->streamCalls();
    HalStorage::setReadFailure("/cache/sections/0.bin", std::numeric_limits<size_t>::max());
    const auto repaired = ProgressSyncPosition::resolve(book, {0, 1, 2}, std::string(32, 'a'));
    EXPECT_NE(repaired.xpath.find("/p[1]"), std::string::npos);
    EXPECT_GT(book->streamCalls(), streams);
    const auto warmStreams = book->streamCalls();
    EXPECT_EQ(ProgressSyncPosition::resolve(book, {0, 1, 2}, std::string(32, 'a')).xpath, repaired.xpath);
    EXPECT_EQ(book->streamCalls(), warmStreams);
  }
}

TEST_F(SyncRepair, SectionMetadataValidatesFinalizationBoundsAndIoFailures) {
  finalizedSection();
  auto file = Storage.open("/cache/sections/0.bin");
  EXPECT_EQ(sectionmetadata::pageCount(file), 2);
  EXPECT_EQ(sectionmetadata::paragraphIndexForPage(file, 0), 1);
  EXPECT_EQ(sectionmetadata::paragraphIndexForPage(file, 1), 2);
  EXPECT_FALSE(sectionmetadata::paragraphIndexForPage(file, 2));
  HalStorage::setReadFailure("/cache/sections/0.bin", 128);
  EXPECT_FALSE(sectionmetadata::paragraphIndexForPage(file, 0));
  HalStorage::setReadFailure("/cache/sections/0.bin", std::numeric_limits<size_t>::max());
  HalStorage::setSeekFailure("/cache/sections/0.bin", true);
  EXPECT_FALSE(sectionmetadata::pageCount(file));
  EXPECT_FALSE(sectionmetadata::paragraphIndexForPage(file, 0));
  for (const uint8_t version : {uint8_t{0}, uint8_t{0xFE - (sectionmetadata::FILE_VERSION - 28)}}) {
    auto bytes = HalStorage::readTestFile("/cache/sections/0.bin");
    bytes[0] = version;
    HalStorage::registerTestFile("/cache/sections/0.bin", std::move(bytes));
    auto incomplete = Storage.open("/cache/sections/0.bin");
    EXPECT_FALSE(sectionmetadata::pageCount(incomplete));
  }
  auto bytes = HalStorage::readTestFile("/cache/sections/0.bin");
  bytes.resize(sectionmetadata::HEADER_SIZE + 1);
  HalStorage::registerTestFile("/cache/sections/0.bin", std::move(bytes));
  auto truncated = Storage.open("/cache/sections/0.bin");
  EXPECT_FALSE(sectionmetadata::paragraphIndexForPage(truncated, 0));
}

TEST(SyncReaderContract, EveryProviderEntryUsesSharedLocalPositionValidation) {
  for (const std::string path : {"/src/util/BookOrbitAutoSync.cpp"}) {
    std::ifstream file(std::string(REPO_ROOT) + path);
    const std::string source((std::istreambuf_iterator<char>(file)), {});
    size_t count = 0;
    size_t offset = 0;
    while ((offset = source.find("ProgressSyncPosition::chapterProgress(", offset)) != std::string::npos) {
      ++count;
      ++offset;
    }
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(source.find("static_cast<float>(pageNumber) / pageCount"), std::string::npos);

  }
}

TEST_F(SyncRepair, RetiredProviderHandoffIsIgnoredWithoutChangingSavedProgress) {
  auto candidate = remote();
  candidate.provider = 1;
  ASSERT_TRUE(ProgressSyncDeferred::stage(candidate));
  DeferredProgress out;
  EXPECT_FALSE(ProgressSyncDeferred::pendingForBook("/book.epub", out));
  GfxRenderer renderer;
  EXPECT_FALSE(ProgressSyncDeferred::applyForBook("/book.epub", renderer));
  EXPECT_EQ(EpubReaderUtils::saves, 0);
  EXPECT_TRUE(APP_STATE.deferredProgress.valid());
}

TEST_F(SyncRepair, BookOrbitChapterStartIgnoresIncompatibleCachedDenominator) {
  GfxRenderer renderer;
  auto candidate = remote(true);
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
