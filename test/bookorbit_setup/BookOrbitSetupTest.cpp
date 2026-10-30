#include <gtest/gtest.h>

#include "activities/network/NetworkModeList.h"
#include "activities/settings/BookOrbitSetupPolicy.h"

namespace P = BookOrbitSetupPolicy;

TEST(BookOrbitSetupPolicy, MissingFieldsCoverAllThreeCredentials) {
  EXPECT_EQ(P::missingFields(true, true, true), P::MISSING_NONE);
  EXPECT_EQ(P::missingFields(false, true, true), P::MISSING_SERVER_URL);
  EXPECT_EQ(P::missingFields(true, false, true), P::MISSING_USERNAME);
  EXPECT_EQ(P::missingFields(true, true, false), P::MISSING_PASSWORD);
  EXPECT_EQ(P::missingFields(false, false, false), P::MISSING_SERVER_URL | P::MISSING_USERNAME | P::MISSING_PASSWORD);
}

TEST(BookOrbitSetupPolicy, ConfiguredRequiresServerUrlUsernameAndPassword) {
  // Same three fields as BookOrbitCredentialStore::hasCredentials().
  for (int mask = 0; mask < 8; mask++) {
    const bool url = mask & 1, user = mask & 2, pass = mask & 4;
    EXPECT_EQ(P::isConfigured(P::missingFields(url, user, pass)), url && user && pass) << mask;
  }
}

TEST(BookOrbitSetupPolicy, PartialMeansSomethingButNotEverything) {
  EXPECT_FALSE(P::isPartial(P::missingFields(false, false, false)));  // fresh profile
  EXPECT_FALSE(P::isPartial(P::missingFields(true, true, true)));     // configured
  EXPECT_TRUE(P::isPartial(P::missingFields(true, false, false)));    // URL only
  EXPECT_TRUE(P::isPartial(P::missingFields(false, true, true)));     // account without server
  EXPECT_TRUE(P::isPartial(P::missingFields(true, true, false)));     // password not yet entered
}

TEST(BookOrbitSetupPolicy, FreshBookOrbitHomeProfileGetsTheGuide) {
  EXPECT_TRUE(P::shouldShowGuide(true, P::missingFields(false, false, false), false));
}

TEST(BookOrbitSetupPolicy, PartialAccountStillGetsTheGuide) {
  EXPECT_TRUE(P::shouldShowGuide(true, P::missingFields(true, true, false), false));
  EXPECT_TRUE(P::shouldShowGuide(true, P::missingFields(false, true, true), false));
}

TEST(BookOrbitSetupPolicy, ConfiguredProfileBypassesTheGuide) {
  EXPECT_FALSE(P::shouldShowGuide(true, P::MISSING_NONE, false));
  EXPECT_FALSE(P::shouldShowGuide(true, P::MISSING_NONE, true));
}

TEST(BookOrbitSetupPolicy, LaterIsNotAskedAgain) {
  EXPECT_FALSE(P::shouldShowGuide(true, P::missingFields(false, false, false), true));
  EXPECT_FALSE(P::shouldShowGuide(true, P::missingFields(true, false, true), true));
}

TEST(BookOrbitSetupPolicy, CompleteConfigurationIsRecordedOnce) {
  EXPECT_TRUE(P::shouldRecordHandled(P::MISSING_NONE, false));
  EXPECT_FALSE(P::shouldRecordHandled(P::MISSING_NONE, true));  // already recorded: no second write
  EXPECT_FALSE(P::shouldRecordHandled(P::missingFields(true, true, false), false));    // partial
  EXPECT_FALSE(P::shouldRecordHandled(P::missingFields(false, false, false), false));  // fresh
}

TEST(BookOrbitSetupPolicy, ClearingAfterCompletionDoesNotReopenTheGuide) {
  // Complete once -> recorded; then every field cleared.
  const bool handled = P::shouldRecordHandled(P::MISSING_NONE, false);
  EXPECT_FALSE(P::shouldShowGuide(true, P::missingFields(false, false, false), handled));
  EXPECT_FALSE(P::shouldShowGuide(true, P::missingFields(true, false, false), handled));
}

TEST(BookOrbitSetupPolicy, OtherThemesNeverGetTheGuide) {
  EXPECT_FALSE(P::shouldShowGuide(false, P::missingFields(false, false, false), false));
}

TEST(NetworkModeList, FileTransferKeepsAllThreeModesInTheirOrder) {
  ASSERT_EQ(NetworkModeList::count(true), 3);
  EXPECT_EQ(NetworkModeList::at(true, 0), NetworkMode::JOIN_NETWORK);
  EXPECT_EQ(NetworkModeList::at(true, 1), NetworkMode::CONNECT_CALIBRE);
  EXPECT_EQ(NetworkModeList::at(true, 2), NetworkMode::CREATE_HOTSPOT);
}

TEST(NetworkModeList, BookOrbitSetupOmitsCalibre) {
  ASSERT_EQ(NetworkModeList::count(false), 2);
  EXPECT_EQ(NetworkModeList::at(false, 0), NetworkMode::JOIN_NETWORK);
  EXPECT_EQ(NetworkModeList::at(false, 1), NetworkMode::CREATE_HOTSPOT);
  for (int i = 0; i < NetworkModeList::count(false); i++) {
    EXPECT_NE(NetworkModeList::at(false, i), NetworkMode::CONNECT_CALIBRE);
  }
}
