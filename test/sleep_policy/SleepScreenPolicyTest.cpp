#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>

#include "CrossPointSettings.h"

namespace {
std::string source(const char* path) {
  std::ifstream file(std::string(REPO_ROOT) + "/" + path);
  return {std::istreambuf_iterator<char>(file), {}};
}

TEST(SleepScreenPolicy, SelectedStaticModesIgnoreRetiredTimeoutPreference) {
  auto& settings = SETTINGS;
  for (const auto mode : {CrossPointSettings::DARK, CrossPointSettings::LIGHT, CrossPointSettings::CUSTOM,
                          CrossPointSettings::COVER, CrossPointSettings::BLANK}) {
    settings.sleepScreen = mode;
    for (const bool reader : {false, true}) {
      for (const uint8_t legacy : {0, 1}) {
        settings.quickResumeSleepScreen = legacy;
        const auto policy = settings.sleepScreenPolicy(reader);
        EXPECT_EQ(policy.screen, mode);
        EXPECT_EQ(policy.fromReader, reader);
        EXPECT_FALSE(policy.preserveView);
        EXPECT_FALSE(policy.resumeReader);
      }
    }
  }
}

TEST(SleepScreenPolicy, CoverCustomKeepsItsExplicitContextualBehavior) {
  auto& settings = SETTINGS;
  settings.sleepScreen = CrossPointSettings::COVER_CUSTOM;
  for (const uint8_t legacy : {0, 1}) {
    settings.quickResumeSleepScreen = legacy;
    EXPECT_EQ(settings.sleepScreenPolicy(true).screen, CrossPointSettings::COVER);
    EXPECT_EQ(settings.sleepScreenPolicy(false).screen, CrossPointSettings::CUSTOM);
    EXPECT_FALSE(settings.sleepScreenPolicy(true).resumeReader);
    EXPECT_FALSE(settings.sleepScreenPolicy(false).preserveView);
  }
}

TEST(SleepScreenPolicy, QuickResumeKeepsReaderFastBootAndHomeStaticFallback) {
  auto& settings = SETTINGS;
  settings.sleepScreen = CrossPointSettings::QUICK_RESUME;
  for (const uint8_t legacy : {0, 1}) {
    settings.quickResumeSleepScreen = legacy;
    EXPECT_TRUE(settings.sleepScreenPolicy(true).preserveView);
    EXPECT_TRUE(settings.sleepScreenPolicy(true).resumeReader);
    EXPECT_FALSE(settings.sleepScreenPolicy(false).preserveView);
    EXPECT_FALSE(settings.sleepScreenPolicy(false).resumeReader);
  }
}

TEST(SleepScreenPolicy, TransparentHomeNeedsRollbackButNeverReaderFastBoot) {
  auto& settings = SETTINGS;
  settings.sleepScreen = CrossPointSettings::TRANSPARENT_OVERLAY;
  for (const bool reader : {false, true}) {
    const auto policy = settings.sleepScreenPolicy(reader);
    EXPECT_TRUE(policy.preserveView);
    EXPECT_EQ(policy.resumeReader, reader);
  }
}

TEST(SleepScreenPolicy, EntryCapturesOneDecisionBeforeExitAndPassesItToRenderer) {
  const auto main = source("src/main.cpp");
  const auto entry = main.substr(main.find("void enterDeepSleep("), main.find("// Boot paths") -
                                                                         main.find("void enterDeepSleep("));
  const auto decision = entry.find("SETTINGS.sleepScreenPolicy(APP_STATE.lastSleepFromReader)");
  ASSERT_NE(decision, std::string::npos);
  EXPECT_LT(decision, entry.find("APP_STATE.showBootScreen = !sleepPolicy.resumeReader"));
  EXPECT_LT(entry.find("saveSleepFrameBuffer()"), entry.find("activityManager.goToSleep(sleepPolicy)"));
  EXPECT_LT(entry.find("activityManager.goToSleep(sleepPolicy)"), entry.find("BookOrbitAutoSync::syncOnSleep"));
  EXPECT_NE(entry.find("sleepPolicy.resumeReader && quickResumeCleanBaseView"), std::string::npos);
  const auto sleep = source("src/activities/boot_sleep/SleepActivity.cpp");
  EXPECT_NE(sleep.find("if (policy.preserveView)"), std::string::npos);
  EXPECT_NE(sleep.find("switch (policy.screen)"), std::string::npos);
  EXPECT_EQ(sleep.find("fromTimeout"), std::string::npos);
}

TEST(SleepScreenPolicy, RetiredKeyStillPersistsButNeitherUiMutatesIt) {
  const auto list = source("src/SettingsList.h");
  const auto base = list.substr(0, list.find("inline bool settingHiddenOnThisBoard"));
  EXPECT_NE(base.find("\"quickResumeSleepScreen\""), std::string::npos);
  const auto ui = list.substr(list.find("inline std::vector<SettingInfo> getSettingsList("));
  EXPECT_NE(ui.find("s.nameId == StrId::STR_QUICK_RESUME_TIMEOUT"), std::string::npos);
  for (const char* path : {"src/activities/settings/SettingsActivity.cpp", "src/network/html/SettingsPage.html"}) {
    const auto text = source(path);
    EXPECT_EQ(text.find("syncQuickResumeTimeoutForSleepScreen"), std::string::npos);
    EXPECT_EQ(text.find("quickResumeSleepScreen"), std::string::npos);
  }
}
}  // namespace
