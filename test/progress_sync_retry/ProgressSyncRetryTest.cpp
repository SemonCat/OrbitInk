#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

#include "BookOrbitSyncClient.h"
#include "BookOrbitResetPolicy.h"
#include "CrossPointState.h"
#include "DeferredProgress.h"
#include "KOReaderSyncClient.h"
#include "ProgressPullPolicy.h"
#include "ProgressSyncReceipt.h"
#include "ProgressSyncRetry.h"
#include "ProgressSyncNetwork.h"
#include "ProgressSyncSchedule.h"

using namespace ProgressSyncRetry;

TEST(ProgressSyncRetry, AllIntervalsUseUint64MicrosecondsAndInvalidDefaultsToOneHour) {
  using namespace ProgressSyncSchedule;
  EXPECT_EQ(intervalMicros(Off), 0u);
  EXPECT_EQ(intervalMicros(Minutes30), 1800000000ULL);
  EXPECT_EQ(intervalMicros(Hour1), 3600000000ULL);
  EXPECT_EQ(intervalMicros(Hours3), 10800000000ULL);
  EXPECT_EQ(intervalMicros(Hours6), 21600000000ULL);
  EXPECT_EQ(intervalMicros(Hours12), 43200000000ULL);
  EXPECT_EQ(intervalMicros(Hours24), 86400000000ULL);
  EXPECT_EQ(normalizeStored(0, false), Hour1) << "missing key upgrades to the communicated default";
  EXPECT_EQ(normalizeStored(7), Hour1);
  EXPECT_EQ(normalizeStored(UINT64_MAX), Hour1);
  EXPECT_EQ(intervalMicros(255), 3600000000ULL);
}

TEST(ProgressSyncRetry, ProvidersClearIndependentlyAndNoWorkStopsRetries) {
  State state;
  state.observe(Provider::KOSync, "/book.epub", Outcome::Retry);
  state.observe(Provider::BookOrbit, "/book.epub", Outcome::Retry);
  state.observe(Provider::KOSync, "/book.epub", Outcome::Confirmed);
  EXPECT_FALSE(state.pending(Provider::KOSync));
  EXPECT_TRUE(state.pending(Provider::BookOrbit));
  state.observe(Provider::BookOrbit, "/book.epub", Outcome::NoRetry);
  EXPECT_FALSE(state.pending(Provider::BookOrbit));
}

TEST(ProgressSyncRetry, DisabledProviderAndChangedBookCannotKeepHeartbeat) {
  State state;
  state.observe(Provider::KOSync, "/book.epub", Outcome::Retry);
  state.observe(Provider::BookOrbit, "/book.epub", Outcome::Retry);
  state.prune("/book.epub", static_cast<uint8_t>(Provider::BookOrbit));
  EXPECT_FALSE(state.pending(Provider::KOSync));
  EXPECT_TRUE(state.pending(Provider::BookOrbit));
  state.prune("/other.epub", 3);
  EXPECT_EQ(state.providers, 0u);
  state.observe(Provider::KOSync, "/other.epub", Outcome::Retry);
  EXPECT_FALSE(state.pending(Provider::BookOrbit));
}

TEST(ProgressSyncRetry, PeriodicEligibilitySupportsIndependentProviders) {
  using namespace ProgressSyncSchedule;
  EXPECT_EQ(enabledProviders(true, true, true, true, false), 1u);
  EXPECT_EQ(enabledProviders(true, true, false, false, true), 2u) << "BookOrbit-only must schedule";
  EXPECT_EQ(enabledProviders(true, true, true, false, true), 2u) << "KOSync auto-sync gating is independent";
  EXPECT_EQ(enabledProviders(true, true, true, true, true), 3u);
  EXPECT_EQ(enabledProviders(false, true, true, true, true), 0u);
  EXPECT_EQ(enabledProviders(true, false, true, true, true), 0u);
}

TEST(ProgressSyncRetry, SuccessAndNoPendingStillContinueConfiguredCadence) {
  using namespace ProgressSyncSchedule;
  State state;
  EXPECT_EQ(state.providers, 0u);
  EXPECT_EQ(sleepDelayMicros(Hour1, 2), 3600000000ULL);
  state.observe(Provider::BookOrbit, "/book.epub", Outcome::Retry);
  state.observe(Provider::BookOrbit, "/book.epub", Outcome::Confirmed);
  EXPECT_EQ(state.providers, 0u);
  EXPECT_EQ(sleepDelayMicros(Hours24, 2), 86400000000ULL);
  EXPECT_EQ(sleepDelayMicros(Off, 2), 0u) << "OFF suppresses periodic wake even after an old retry";
}

template <typename Client>
void checkErrors() {
  EXPECT_EQ(classify<Client>(Client::OK, 200), Outcome::Confirmed);
  EXPECT_EQ(classify<Client>(Client::NETWORK_ERROR, -1), Outcome::Retry);
  EXPECT_EQ(classify<Client>(Client::LOW_MEMORY, 0), Outcome::Retry);
  for (const int code : {408, 429, 500, 502, 503, 504}) {
    EXPECT_EQ(classify<Client>(Client::SERVER_ERROR, code), Outcome::Retry);
  }
  for (const int code : {301, 400, 401, 403, 404, 405, 413, 422, 501}) {
    EXPECT_EQ(classify<Client>(Client::SERVER_ERROR, code), Outcome::NoRetry);
  }
  EXPECT_EQ(classify<Client>(Client::AUTH_FAILED, 401), Outcome::NoRetry);
  EXPECT_EQ(classify<Client>(Client::NO_CREDENTIALS, 0), Outcome::NoRetry);
  EXPECT_EQ(classify<Client>(Client::JSON_ERROR, 200), Outcome::Retry);
  EXPECT_EQ(classify<Client>(Client::NOT_FOUND, 404), Outcome::NoRetry);
}

TEST(ProgressSyncRetry, BothClientsDistinguishTransientFromPermanent) {
  checkErrors<KOReaderSyncClient>();
  checkErrors<BookOrbitSyncClient>();
  EXPECT_EQ(classify<BookOrbitSyncClient>(BookOrbitSyncClient::NOT_BOOKORBIT, 200), Outcome::NoRetry);
}

std::string source(const char* path) {
  std::ifstream file(std::string(REPO_ROOT) + path);
  std::ostringstream out;
  out << file.rdbuf();
  return out.str();
}

// ESP reset causes and deep sleep cannot be linked into the host suite. Check the
// actual hardware route, including placement before the first display/error UI.
TEST(ProgressSyncRetry, TimerWakeRoutesBeforeDisplayAndButtonVerification) {
  const auto main = source("/src/main.cpp");
  const auto setup = main.substr(main.find("void setup()"));
  const auto route = setup.find("if (gpio.getWakeupReason() == HalGPIO::WakeupReason::Timer)");
  ASSERT_NE(route, std::string::npos);
  EXPECT_LT(route, setup.find("setupDisplayAndFonts("));
  EXPECT_LT(route, setup.find("gpio.verifyPowerButtonWakeup("));
  EXPECT_LT(route, setup.find("halTiltSensor.begin()"));
  const auto hal = source("/lib/hal/HalGPIO.cpp");
  EXPECT_NE(hal.find("wakeupCause == ESP_SLEEP_WAKEUP_TIMER"), std::string::npos);
  EXPECT_NE(hal.find("return WakeupReason::Timer;"), std::string::npos);
  EXPECT_NE(hal.find("wakeupCause == ESP_SLEEP_WAKEUP_GPIO || wakeupCause == ESP_SLEEP_WAKEUP_EXT1"),
            std::string::npos);
}

TEST(ProgressSyncRetry, HeadlessTimerPathDoesNotRenderAndAlwaysTearsRadioDown) {
  const auto main = source("/src/main.cpp");
  const auto start = main.find("static void synchronizeProgressHeadlessly()");
  ASSERT_NE(start, std::string::npos);
  const auto body = main.substr(start, main.find("\n}\n", start) - start);
  EXPECT_EQ(body.find("display."), std::string::npos);
  EXPECT_EQ(body.find("renderer."), std::string::npos);
  EXPECT_EQ(body.find("activityManager."), std::string::npos);
  EXPECT_NE(body.find("KOReaderAutoSync::syncHeadlessly(path)"), std::string::npos);
  EXPECT_NE(body.find("BookOrbitAutoSync::syncHeadlessly(path)"), std::string::npos);
  EXPECT_LT(body.find("WiFi.mode(WIFI_OFF)"), body.find("powerManager.startDeepSleep("));
  const auto hal = source("/lib/hal/HalPowerManager.cpp");
  EXPECT_LT(hal.find("esp_sleep_enable_timer_wakeup(retryDelayMicros)"),
            hal.find("freeink::PowerManager::deepSleepUntilPowerButton()"));
  EXPECT_NE(hal.find("esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER)"), std::string::npos);
}

TEST(ProgressSyncRetry, ReaderResumeSkipsNetworkSyncAndAppliesStagedBeforeSnapshotAndReader) {
  const auto main = source("/src/main.cpp");
  const auto declare = main.find("const bool resumeReaderOnBoot =");
  ASSERT_NE(declare, std::string::npos);
  const auto displaySetup = main.find("setupDisplayAndFonts(", declare);
  ASSERT_NE(displaySetup, std::string::npos);
  // A button/cold resume must originate no network transaction of its own -- it must only ever
  // consume whatever an earlier periodic/sleep/open/close sync already staged.
  const auto preDisplay = main.substr(declare, displaySetup - declare);
  EXPECT_EQ(preDisplay.find("syncHeadlessly"), std::string::npos);
  const auto apply = main.find("ProgressSyncDeferred::applyForBook(APP_STATE.openEpubPath, renderer)", displaySetup);
  const auto snapshotSwitch = main.find("switch (resume)", apply);
  const auto reader = main.find("activityManager.goToReader(path", snapshotSwitch);
  ASSERT_NE(apply, std::string::npos);
  ASSERT_NE(snapshotSwitch, std::string::npos);
  ASSERT_NE(reader, std::string::npos);
  EXPECT_LT(displaySetup, apply);
  EXPECT_LT(apply, snapshotSwitch);
  EXPECT_LT(snapshotSwitch, reader);
}

TEST(ProgressSyncRetry, StateSurvivesSleepAndLegacyFilesStartWithoutTimer) {
  auto& app = APP_STATE;
  JsonDocument legacy;
  legacy["openEpubPath"] = "/book.epub";
  ASSERT_TRUE(app.fromJson(legacy.as<JsonVariantConst>()));
  EXPECT_EQ(app.progressSyncRetry.providers, 0u);
  app.progressSyncRetry.observe(Provider::KOSync, app.openEpubPath, Outcome::Retry);
  app.progressSyncRetry.observe(Provider::BookOrbit, app.openEpubPath, Outcome::Retry);
  JsonDocument persisted;
  app.toJson(persisted);
  app.progressSyncRetry = {};
  ASSERT_TRUE(app.fromJson(persisted.as<JsonVariantConst>()));
  EXPECT_TRUE(app.progressSyncRetry.pending(Provider::KOSync));
  EXPECT_TRUE(app.progressSyncRetry.pending(Provider::BookOrbit));
  EXPECT_EQ(app.progressSyncRetry.path, "/book.epub");
  app.progressSyncRetry.observe(Provider::KOSync, app.openEpubPath, Outcome::Confirmed);
  app.toJson(persisted);
  app.progressSyncRetry = {};
  ASSERT_TRUE(app.fromJson(persisted.as<JsonVariantConst>()));
  EXPECT_FALSE(app.progressSyncRetry.pending(Provider::KOSync));
  EXPECT_TRUE(app.progressSyncRetry.pending(Provider::BookOrbit));
}

TEST(ProgressSyncRetry, ConfirmedTemporalDecisionClearsPendingUpload) {
  State state;
  state.observe(Provider::KOSync, "/book.epub", Outcome::Retry);
  state.observe(Provider::KOSync, "/book.epub", Outcome::Confirmed);
  EXPECT_EQ(state.providers, 0u);
}

TEST(ProgressSyncRetry, DeferredRemoteRoundTripsAcrossOpenBookChangesAndRejectsCorruptValues) {
  auto& app = APP_STATE;
  app.openEpubPath = "/book.epub";
  DeferredProgress staged;
  staged.provider = static_cast<uint8_t>(Provider::KOSync);
  staged.path = app.openEpubPath;
  staged.document = "0123456789abcdef0123456789abcdef";
  staged.xpath = "/body/DocFragment[2]/body/p[3]/text().4";
  staged.percentage = 0.75f;
  staged.timestamp = 1800000003ULL;
  staged.hasRichPosition = true;
  staged.richPctQ = 750000;
  staged.richSpineIndex = 1;
  staged.richPageNumber = 4;
  staged.richTotalPages = 12;
  staged.richHasParagraphIndex = true;
  staged.richParagraphIndex = 8;
  ASSERT_TRUE(staged.valid());
  app.deferredProgress = staged;

  JsonDocument persisted;
  app.toJson(persisted);
  app.deferredProgress.clear();
  ASSERT_TRUE(app.fromJson(persisted.as<JsonVariantConst>()));
  EXPECT_TRUE(app.deferredProgress.valid());
  EXPECT_EQ(app.deferredProgress.provider, static_cast<uint8_t>(Provider::KOSync));
  EXPECT_EQ(app.deferredProgress.path, "/book.epub");
  EXPECT_EQ(app.deferredProgress.document, "0123456789abcdef0123456789abcdef");
  EXPECT_EQ(app.deferredProgress.xpath, "/body/DocFragment[2]/body/p[3]/text().4");
  EXPECT_FLOAT_EQ(app.deferredProgress.percentage, 0.75f);
  EXPECT_EQ(app.deferredProgress.timestamp, 1800000003ULL);
  EXPECT_EQ(app.deferredProgress.richPctQ, 750000u);
  EXPECT_EQ(app.deferredProgress.richSpineIndex, 1u);
  EXPECT_EQ(app.deferredProgress.richPageNumber, 4u);
  EXPECT_EQ(app.deferredProgress.richTotalPages, 12u);
  EXPECT_TRUE(app.deferredProgress.richHasParagraphIndex);
  EXPECT_EQ(app.deferredProgress.richParagraphIndex, 8);

  // A library-open headless fetch may target the newly selected book before ReaderActivity has
  // updated openEpubPath. The durable handoff is bound to its own path/hash, not this old field.
  persisted["openEpubPath"] = "/other.epub";
  ASSERT_TRUE(app.fromJson(persisted.as<JsonVariantConst>()));
  EXPECT_TRUE(app.deferredProgress.valid());

  persisted["openEpubPath"] = "/book.epub";
  persisted["deferredProgressPercentage"] = 1.5;
  ASSERT_TRUE(app.fromJson(persisted.as<JsonVariantConst>()));
  EXPECT_FALSE(app.deferredProgress.valid());
}

TEST(ProgressSyncRetry, DeferredPlainBookOrbitAndLegacyEmptyStateRoundTripSafely) {
  auto& app = APP_STATE;
  JsonDocument legacy;
  legacy["openEpubPath"] = "/legacy.epub";
  ASSERT_TRUE(app.fromJson(legacy.as<JsonVariantConst>()));
  EXPECT_FALSE(app.deferredProgress.valid());

  DeferredProgress plain;
  plain.provider = static_cast<uint8_t>(Provider::BookOrbit);
  plain.path = "/book.epub";
  plain.document = "0123456789abcdef0123456789abcdef";
  plain.xpath = "/body/DocFragment[1]/body/p[2]";
  plain.percentage = 0.4f;
  plain.timestamp = 1800000001ULL;
  ASSERT_TRUE(plain.valid());
  app.deferredProgress = plain;
  JsonDocument persisted;
  app.toJson(persisted);
  app.deferredProgress.clear();
  ASSERT_TRUE(app.fromJson(persisted.as<JsonVariantConst>()));
  EXPECT_EQ(app.deferredProgress.provider, static_cast<uint8_t>(Provider::BookOrbit));
  EXPECT_FALSE(app.deferredProgress.hasRichPosition);
  EXPECT_FLOAT_EQ(app.deferredProgress.percentage, 0.4f);
}

TEST(ProgressSyncRetry, DeferredValidationRejectsUnknownProviderIdentityRangeAndMalformedRichData) {
  DeferredProgress value;
  value.provider = static_cast<uint8_t>(Provider::KOSync);
  value.path = "/book.epub";
  value.document = "0123456789abcdef0123456789abcdef";
  value.xpath = "/body/p[1]";
  value.percentage = 0.5f;
  value.timestamp = 1800000001ULL;
  EXPECT_TRUE(value.valid());

  for (const uint8_t provider : {static_cast<uint8_t>(0), static_cast<uint8_t>(3)}) {
    auto bad = value;
    bad.provider = provider;
    EXPECT_FALSE(bad.valid());
  }
  for (const std::string& missing : {std::string(), std::string("short")}) {
    auto bad = value;
    bad.document = missing;
    EXPECT_FALSE(bad.valid());
  }
  for (const float percentage : {-0.01f, 1.01f}) {
    auto bad = value;
    bad.percentage = percentage;
    EXPECT_FALSE(bad.valid());
  }
  auto rich = value;
  rich.hasRichPosition = true;
  rich.richPctQ = 1000001;
  EXPECT_FALSE(rich.valid());
  rich.richPctQ = 500000;
  rich.richTotalPages = 4;
  rich.richPageNumber = 4;
  EXPECT_FALSE(rich.valid());
  rich.richPageNumber = 3;
  EXPECT_TRUE(rich.valid());
  rich.provider = static_cast<uint8_t>(Provider::BookOrbit);
  EXPECT_FALSE(rich.valid());
}

TEST(ProgressSyncRetry, DeferredStateKeepsNewestProviderPositionEvenWhenLower) {
  DeferredProgress first;
  first.provider = static_cast<uint8_t>(Provider::BookOrbit);
  first.path = "/book.epub";
  first.document = "0123456789abcdef0123456789abcdef";
  first.xpath = "/body/DocFragment[1]";
  first.percentage = 0.8f;
  first.timestamp = 1800000001ULL;

  DeferredProgress lower = first;
  lower.provider = static_cast<uint8_t>(Provider::KOSync);
  lower.percentage = 0.5f;
  lower.timestamp = 1800000002ULL;
  DeferredProgress higher = lower;
  higher.percentage = 0.9f;
  higher.timestamp = 1800000003ULL;

  DeferredProgress state;
  EXPECT_TRUE(state.consider(first));
  EXPECT_TRUE(state.consider(lower));
  EXPECT_EQ(state.provider, static_cast<uint8_t>(Provider::KOSync));
  EXPECT_FLOAT_EQ(state.percentage, 0.5f);
  EXPECT_TRUE(state.consider(higher));
  EXPECT_EQ(state.provider, static_cast<uint8_t>(Provider::KOSync));
  EXPECT_FLOAT_EQ(state.percentage, 0.9f);
}

TEST(ProgressSyncRetry, StandingBookOrbitResetAcknowledgesThenLetsARealAdvanceEscape) {
  // The first reset has no marker yet, so normal timestamp ordering stages and durably applies it.
  auto decision = BookOrbitResetPolicy::decide(/*remoteIsWebReset=*/true, /*markerMatches=*/false,
                                               /*acknowledged=*/false, /*hasLocal=*/true,
                                               /*localIsStart=*/false, /*localTimestampValid=*/true);
  EXPECT_FALSE(decision.standingReset);

  // After durable apply, the repeated synthetic reset is acknowledged exactly once.
  decision = BookOrbitResetPolicy::decide(/*remoteIsWebReset=*/true, /*markerMatches=*/true,
                                          /*acknowledged=*/false, /*hasLocal=*/true,
                                          /*localIsStart=*/true, /*localTimestampValid=*/true);
  EXPECT_TRUE(decision.standingReset);
  EXPECT_TRUE(decision.acknowledgeStart);
  EXPECT_FALSE(decision.pushAdvance);
  EXPECT_FALSE(decision.holdUnknown);

  // BookOrbit still returns that standing reset after the ACK. A later real non-start page event
  // must be pushed regardless of the synthetic GET's fresh timestamp, ending the reset loop.
  decision = BookOrbitResetPolicy::decide(/*remoteIsWebReset=*/true, /*markerMatches=*/true,
                                          /*acknowledged=*/true, /*hasLocal=*/true,
                                          /*localIsStart=*/false, /*localTimestampValid=*/true);
  EXPECT_TRUE(decision.standingReset);
  EXPECT_FALSE(decision.acknowledgeStart);
  EXPECT_TRUE(decision.pushAdvance);
  EXPECT_FALSE(decision.holdUnknown);
}

TEST(ProgressSyncRetry, StandingBookOrbitResetHoldsAnAdvanceWithoutValidEventTime) {
  const auto decision = BookOrbitResetPolicy::decide(
      /*remoteIsWebReset=*/true, /*markerMatches=*/true, /*acknowledged=*/true, /*hasLocal=*/true,
      /*localIsStart=*/false, /*localTimestampValid=*/false);
  EXPECT_TRUE(decision.standingReset);
  EXPECT_FALSE(decision.pushAdvance);
  EXPECT_TRUE(decision.holdUnknown);
}

TEST(ProgressSyncRetry, BookOrbitResetMarkerRoundTripsAcrossReboot) {
  auto& app = APP_STATE;
  app.bookOrbitResetPath = "/book.epub";
  app.bookOrbitResetDocument = "0123456789abcdef0123456789abcdef";
  app.bookOrbitResetTimestamp = 1'800'000'001ULL;
  app.bookOrbitResetAcknowledged = true;
  JsonDocument persisted;
  app.toJson(persisted);

  app.bookOrbitResetPath.clear();
  app.bookOrbitResetDocument.clear();
  app.bookOrbitResetTimestamp = 0;
  app.bookOrbitResetAcknowledged = false;
  ASSERT_TRUE(app.fromJson(persisted.as<JsonVariantConst>()));
  EXPECT_EQ(app.bookOrbitResetPath, "/book.epub");
  EXPECT_EQ(app.bookOrbitResetDocument, "0123456789abcdef0123456789abcdef");
  EXPECT_EQ(app.bookOrbitResetTimestamp, 1'800'000'001ULL);
  EXPECT_TRUE(app.bookOrbitResetAcknowledged);
}

TEST(ProgressSyncRetry, ProviderHardwarePathsValidateBeforeWifiAndSuppressSideWork) {
  for (const auto* path : {"/src/util/KOReaderAutoSync.cpp", "/src/util/BookOrbitAutoSync.cpp"}) {
    const auto text = source(path);
    const auto start = text.find("ProgressSyncRetry::Outcome synchronizeProgress(");
    ASSERT_NE(start, std::string::npos);
    const auto body = text.substr(start, text.find("}  // namespace", start) - start);
    EXPECT_LT(body.find("bool hasLocal"), body.find("if (!connectToSavedWifi())"));
    EXPECT_LT(body.find("bool hasLocal"), body.find("epub->calculateProgress("));
    EXPECT_LT(body.find("epub.reset()"), body.find("session.getProgress("));
    EXPECT_NE(body.find("return finish(Outcome::Retry)"), std::string::npos);
    EXPECT_NE(body.find("return finish(Outcome::NoRetry)"), std::string::npos);
    EXPECT_NE(body.find("ProgressPullPolicy::decide("), std::string::npos);
    EXPECT_NE(body.find("ProgressSyncDeferred::stage(remoteCandidate)"), std::string::npos);
    EXPECT_NE(text.find("synchronizeProgress(epubPath, nullptr"), std::string::npos);
    EXPECT_EQ(text.find("scanNetworks("), std::string::npos);
    EXPECT_NE(text.find("ProgressSyncNetwork::connect()"), std::string::npos);
  }
  const auto network = source("/src/util/ProgressSyncNetwork.cpp");
  EXPECT_NE(network.find("millis() - start >= budgetMs"), std::string::npos);
  EXPECT_NE(source("/src/util/ProgressSyncNetwork.h").find("CONNECT_BUDGET_MS = 3500;"), std::string::npos);
  EXPECT_EQ(network.find("scanNetworks("), std::string::npos);
}

TEST(ProgressSyncRetry, DeferredApplyOccursAfterProviderTransactionReturnsAndInvalidatesSnapshot) {
  for (const auto* path : {"/src/util/KOReaderAutoSync.cpp", "/src/util/BookOrbitAutoSync.cpp"}) {
    const auto text = source(path);
    const auto wrapper = text.find("void syncOnOpen(");
    ASSERT_NE(wrapper, std::string::npos);
    const auto body = text.substr(wrapper, text.find("\n}", wrapper) - wrapper);
    EXPECT_LT(body.find("synchronizeProgress("), body.find("ProgressSyncDeferred::applyForBook("));
  }
  const auto deferred = source("/src/util/ProgressSyncDeferred.cpp");
  const auto stage = deferred.find("bool stage(");
  ASSERT_NE(stage, std::string::npos);
  const auto body = deferred.substr(stage, deferred.find("\n}", stage) - stage);
  EXPECT_NE(body.find("quickResumeSkipRenderEligible = false"), std::string::npos);
  EXPECT_NE(body.find("Storage.remove(SLEEP_FRAME_FILE)"), std::string::npos);
}

TEST(ProgressSyncReceipt, LineHasFixedFieldsSecondsAndUnknownMarkers) {
  ProgressSyncReceipt::Record r;
  r.trigger = ProgressSyncReceipt::Trigger::Timer;
  r.kosync = -1;
  r.bookOrbit = static_cast<int8_t>(Outcome::Retry);
  r.wifi = ProgressSyncReceipt::Wifi::Connected;
  r.wifiMs = 4210;
  r.gateChecks = 1;
  r.gateRejections = 1;
  r.gateMinFree = 30000;
  r.gateMinLargest = 19000;
  r.freeHeap = 31000;
  r.largestBlock = 19500;
  r.minFreeHeap = 29000;
  r.nextDelayMicros = ProgressSyncSchedule::intervalMicros(ProgressSyncSchedule::Hours24);
  r.usb = 0;
  r.cycle = 7;
  r.prevArmedMicros = ProgressSyncSchedule::intervalMicros(ProgressSyncSchedule::Hours24);
  char line[256];
  ASSERT_GT(ProgressSyncReceipt::format(r, line, sizeof(line)), 0u);
  EXPECT_STREQ(line,
               "at=- t=timer store=ok ko=- bo=retry wifi=ok/4210ms gate=1/1 gmin=30000/19000 "
               "heap=31000/19500/29000 next=86400s usb=0 cycle=7 prev=86400s\n");

  r.trigger = ProgressSyncReceipt::Trigger::Boot;
  r.storeFailed = true;
  r.hasTime = true;
  r.epochMinutes = 29891737;
  r.kosync = static_cast<int8_t>(Outcome::NoRetry);
  r.bookOrbit = static_cast<int8_t>(Outcome::Confirmed);
  r.wifi = ProgressSyncReceipt::Wifi::Failed;
  r.usb = -1;
  ASSERT_GT(ProgressSyncReceipt::format(r, line, sizeof(line)), 0u);
  EXPECT_EQ(std::string(line).rfind("at=29891737m t=boot store=fail ko=stop bo=ok wifi=fail/4210ms ", 0), 0u);
  EXPECT_NE(std::string(line).find(" usb=- "), std::string::npos);
  // Too small a buffer writes no partial line.
  EXPECT_EQ(ProgressSyncReceipt::format(r, line, 40), 0u);
}

// A timer wake must keep its cadence when a store exists but cannot be read, and must not mistake
// that for configuration. Behaviour is exercised end to end by scripts/sim_periodic_sync_smoke.py
// (S5/S7/S8); this pins the device-only parts the simulator cannot reach.
TEST(ProgressSyncRetry, HeadlessWakeReArmsRetainedDelayOnlyForUnreadableStores) {
  const auto main = source("/src/main.cpp");
  const auto start = main.find("static void synchronizeProgressHeadlessly()");
  ASSERT_NE(start, std::string::npos);
  const auto body = main.substr(start, main.find("\n}\n", start) - start);
  for (const char* store : {"APP_STATE.loadFromFile()", "loadedOrAbsent(SETTINGS)", "loadedOrAbsent(WIFI_STORE)",
                            "loadedOrAbsent(KOREADER_STORE)", "loadedOrAbsent(BOOKORBIT_STORE)"}) {
    EXPECT_LT(body.find(store), body.find("ProgressSyncRetry::sleepDelayMicros()")) << store;
  }
  EXPECT_NE(main.find("Storage.probePath(Store::getFilePath()) == HalStorage::PathStatus::Absent"), std::string::npos);
  EXPECT_NE(body.find("nextSyncDelayMicros = HalPowerManager::retainedTimerDelayMicros();"), std::string::npos);
  EXPECT_NE(body.find("ProgressSyncNetwork::UNATTENDED_CONNECT_BUDGET_MS"), std::string::npos);
  EXPECT_LT(body.find("ProgressSyncReceipt::commit(nextSyncDelayMicros)"), body.find("powerManager.startDeepSleep("));

  // RTC copy: cleared before arming, kept only for a timer the hardware accepted, on the X3 only.
  const auto hal = source("/lib/hal/HalPowerManager.cpp");
  EXPECT_NE(hal.find("RTC_DATA_ATTR uint64_t rtcArmedTimerDelayMicros"), std::string::npos);
  EXPECT_LT(hal.find("  rtcArmedTimerDelayMicros = 0;\n"), hal.find("rtcArmedTimerDelayMicros = retryDelayMicros;"));
  EXPECT_NE(hal.find("gpio.deviceIsX3() && retryDelayMicros != 0 && esp_sleep_enable_timer_wakeup(retryDelayMicros) "
                     "== ESP_OK"),
            std::string::npos);
}

TEST(ProgressSyncRetry, OnlyTheTimerRouteGetsTheLongConnectBudget) {
  static_assert(ProgressSyncNetwork::CONNECT_BUDGET_MS == 3500, "open/close/sleep latency budget is unchanged");
  static_assert(ProgressSyncNetwork::UNATTENDED_CONNECT_BUDGET_MS > ProgressSyncNetwork::CONNECT_BUDGET_MS);
  const auto main = source("/src/main.cpp");
  size_t uses = 0;
  for (size_t at = main.find("UNATTENDED_CONNECT_BUDGET_MS"); at != std::string::npos;
       at = main.find("UNATTENDED_CONNECT_BUDGET_MS", at + 1)) {
    uses++;
  }
  EXPECT_EQ(uses, 1u);
  const auto sleep = main.substr(main.find("void enterDeepSleep("));
  EXPECT_NE(sleep.find("ProgressSyncNetwork::Batch syncBatch(APP_STATE.openEpubPath, &renderer);"), std::string::npos);
}
