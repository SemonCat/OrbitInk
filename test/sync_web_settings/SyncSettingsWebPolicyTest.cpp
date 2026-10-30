#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <string>

#include "SyncSettingsWebPolicy.h"

namespace {

using SyncSettingsWebPolicy::BookOrbitState;
using SyncSettingsWebPolicy::KOSyncState;

JsonDocument json(const std::string& value) {
  JsonDocument doc;
  EXPECT_FALSE(deserializeJson(doc, value));
  return doc;
}

void expectEqual(const KOSyncState& actual, const KOSyncState& expected) {
  EXPECT_EQ(actual.serverUrl, expected.serverUrl);
  EXPECT_EQ(actual.username, expected.username);
  EXPECT_EQ(actual.password, expected.password);
  EXPECT_EQ(actual.matchMethod, expected.matchMethod);
  EXPECT_EQ(actual.sendMetadata, expected.sendMetadata);
  EXPECT_EQ(actual.syncBehavior, expected.syncBehavior);
  EXPECT_EQ(actual.autoSync, expected.autoSync);
}

void expectEqual(const BookOrbitState& actual, const BookOrbitState& expected) {
  EXPECT_EQ(actual.serverUrl, expected.serverUrl);
  EXPECT_EQ(actual.username, expected.username);
  EXPECT_EQ(actual.password, expected.password);
}

KOSyncState kosyncFixture() {
  return {.serverUrl = "https://sync.example.test",
          .username = "alice",
          .password = "kosync-secret",
          .matchMethod = 1,
          .sendMetadata = true,
          .syncBehavior = 1,
          .autoSync = true};
}

BookOrbitState bookOrbitFixture() {
  return {.serverUrl = "books.example.test/api/v1", .username = "bob", .password = "bookorbit-secret"};
}

TEST(SyncSettingsWebPolicy, GetResponsesNeverDisclosePasswordsOrHashes) {
  JsonDocument koDoc;
  SyncSettingsWebPolicy::writeKOSyncResponse(koDoc, kosyncFixture());
  EXPECT_EQ(koDoc["serverUrl"].as<std::string>(), "https://sync.example.test");
  EXPECT_EQ(koDoc["username"].as<std::string>(), "alice");
  EXPECT_TRUE(koDoc["hasPassword"].as<bool>());
  EXPECT_TRUE(koDoc["configured"].as<bool>());
  EXPECT_EQ(koDoc["matchMethod"].as<int>(), 1);
  EXPECT_TRUE(koDoc["sendMetadata"].as<bool>());
  EXPECT_EQ(koDoc["syncBehavior"].as<int>(), 1);
  EXPECT_TRUE(koDoc["autoSync"].as<bool>());
  EXPECT_TRUE(koDoc["password"].isNull());
  EXPECT_TRUE(koDoc["passwordHash"].isNull());
  EXPECT_TRUE(koDoc["md5Password"].isNull());

  JsonDocument boDoc;
  SyncSettingsWebPolicy::writeBookOrbitResponse(boDoc, bookOrbitFixture());
  EXPECT_EQ(boDoc["serverUrl"].as<std::string>(), "books.example.test/api/v1");
  EXPECT_EQ(boDoc["username"].as<std::string>(), "bob");
  EXPECT_TRUE(boDoc["hasPassword"].as<bool>());
  EXPECT_TRUE(boDoc["configured"].as<bool>());
  EXPECT_TRUE(boDoc["password"].isNull());
  EXPECT_TRUE(boDoc["userkey"].isNull());

  std::string wire;
  serializeJson(koDoc, wire);
  serializeJson(boDoc, wire);
  EXPECT_EQ(wire.find("kosync-secret"), std::string::npos);
  EXPECT_EQ(wire.find("bookorbit-secret"), std::string::npos);
}

TEST(SyncSettingsWebPolicy, BlankPasswordsPreserveTheStoredValue) {
  const KOSyncState koBefore = kosyncFixture();
  KOSyncState koAfter;
  JsonDocument koRequest = json(R"({"username":"new-alice","password":""})");
  const auto koResult = SyncSettingsWebPolicy::parseKOSyncUpdate(koRequest, koBefore, koAfter);
  ASSERT_TRUE(koResult.valid) << koResult.error;
  EXPECT_TRUE(koResult.hasChanges);
  EXPECT_EQ(koAfter.username, "new-alice");
  EXPECT_EQ(koAfter.password, koBefore.password);

  KOSyncState koMissingAfter;
  JsonDocument koMissingRequest = json(R"({"serverUrl":"sync-two.example.test"})");
  ASSERT_TRUE(SyncSettingsWebPolicy::parseKOSyncUpdate(koMissingRequest, koBefore, koMissingAfter).valid);
  EXPECT_EQ(koMissingAfter.password, koBefore.password);

  const BookOrbitState boBefore = bookOrbitFixture();
  BookOrbitState boAfter;
  JsonDocument boRequest = json(R"({"username":"new-bob","password":""})");
  const auto boResult = SyncSettingsWebPolicy::parseBookOrbitUpdate(boRequest, boBefore, boAfter);
  ASSERT_TRUE(boResult.valid) << boResult.error;
  EXPECT_TRUE(boResult.hasChanges);
  EXPECT_EQ(boAfter.username, "new-bob");
  EXPECT_EQ(boAfter.password, boBefore.password);
}

TEST(SyncSettingsWebPolicy, ExplicitClearOnlyClearsCredentials) {
  const KOSyncState koBefore = kosyncFixture();
  const KOSyncState koAfter = SyncSettingsWebPolicy::withoutCredentials(koBefore);
  EXPECT_TRUE(koAfter.username.empty());
  EXPECT_TRUE(koAfter.password.empty());
  EXPECT_EQ(koAfter.serverUrl, koBefore.serverUrl);
  EXPECT_EQ(koAfter.matchMethod, koBefore.matchMethod);
  EXPECT_EQ(koAfter.sendMetadata, koBefore.sendMetadata);
  EXPECT_EQ(koAfter.syncBehavior, koBefore.syncBehavior);
  EXPECT_EQ(koAfter.autoSync, koBefore.autoSync);

  const BookOrbitState boBefore = bookOrbitFixture();
  const BookOrbitState boAfter = SyncSettingsWebPolicy::withoutCredentials(boBefore);
  EXPECT_TRUE(boAfter.username.empty());
  EXPECT_TRUE(boAfter.password.empty());
  EXPECT_EQ(boAfter.serverUrl, boBefore.serverUrl);
}

TEST(SyncSettingsWebPolicy, RejectsMalformedTypesAndLengthsWithoutChangingState) {
  const KOSyncState koBefore = kosyncFixture();
  for (const std::string& request :
       {R"([])", R"({})", R"({"unknown":1})", R"({"username":"alice","unknown":1})", R"({"username":7})",
        R"({"username":null,"autoSync":true})", R"({"password":false})", R"({"serverUrl":[]})", R"({"sendMetadata":1})",
        R"({"autoSync":"yes"})"}) {
    KOSyncState next = {};
    JsonDocument doc = json(request);
    const auto result = SyncSettingsWebPolicy::parseKOSyncUpdate(doc, koBefore, next);
    EXPECT_FALSE(result.valid) << request;
    expectEqual(next, koBefore);
  }

  JsonDocument koLong;
  koLong["username"] = std::string(SyncSettingsWebPolicy::MAX_USERNAME_LENGTH + 1, 'u');
  KOSyncState koNext;
  EXPECT_FALSE(SyncSettingsWebPolicy::parseKOSyncUpdate(koLong, koBefore, koNext).valid);
  expectEqual(koNext, koBefore);

  JsonDocument passwordLong;
  passwordLong["password"] = std::string(SyncSettingsWebPolicy::MAX_PASSWORD_LENGTH + 1, 'p');
  EXPECT_FALSE(SyncSettingsWebPolicy::parseKOSyncUpdate(passwordLong, koBefore, koNext).valid);
  expectEqual(koNext, koBefore);

  const BookOrbitState boBefore = bookOrbitFixture();
  JsonDocument boLong;
  boLong["serverUrl"] = std::string(SyncSettingsWebPolicy::MAX_SERVER_URL_LENGTH + 1, 's');
  BookOrbitState boNext;
  EXPECT_FALSE(SyncSettingsWebPolicy::parseBookOrbitUpdate(boLong, boBefore, boNext).valid);
  expectEqual(boNext, boBefore);

  for (const std::string& request :
       {R"({"username":3})", R"({"password":null})", R"({"serverUrl":"books.test","extra":true})"}) {
    JsonDocument doc = json(request);
    EXPECT_FALSE(SyncSettingsWebPolicy::parseBookOrbitUpdate(doc, boBefore, boNext).valid) << request;
    expectEqual(boNext, boBefore);
  }

  EXPECT_TRUE(SyncSettingsWebPolicy::isRequestBodySizeAllowed(SyncSettingsWebPolicy::MAX_REQUEST_BYTES));
  EXPECT_FALSE(SyncSettingsWebPolicy::isRequestBodySizeAllowed(0));
  EXPECT_FALSE(SyncSettingsWebPolicy::isRequestBodySizeAllowed(SyncSettingsWebPolicy::MAX_REQUEST_BYTES + 1));
}

TEST(SyncSettingsWebPolicy, BookOrbitAndKOSyncUpdatesRemainIndependent) {
  const KOSyncState koBefore = kosyncFixture();
  const BookOrbitState boBefore = bookOrbitFixture();

  KOSyncState koAfter;
  JsonDocument koRequest = json(R"({"username":"ko-only","password":"new-ko"})");
  ASSERT_TRUE(SyncSettingsWebPolicy::parseKOSyncUpdate(koRequest, koBefore, koAfter).valid);
  EXPECT_EQ(koAfter.username, "ko-only");
  expectEqual(boBefore, bookOrbitFixture());

  BookOrbitState boAfter;
  JsonDocument boRequest = json(R"({"serverUrl":"bo-only.test","password":"new-bo"})");
  ASSERT_TRUE(SyncSettingsWebPolicy::parseBookOrbitUpdate(boRequest, boBefore, boAfter).valid);
  EXPECT_EQ(boAfter.serverUrl, "bo-only.test");
  expectEqual(koBefore, kosyncFixture());
}

TEST(SyncSettingsWebPolicy, RawUrlsAreLeftForCredentialStoresToNormalize) {
  const BookOrbitState before = bookOrbitFixture();
  BookOrbitState rawAfter;
  JsonDocument rawRequest = json(R"({"serverUrl":"bookorbit.local/api/v1/koreader/"})");
  ASSERT_TRUE(SyncSettingsWebPolicy::parseBookOrbitUpdate(rawRequest, before, rawAfter).valid);
  EXPECT_EQ(rawAfter.serverUrl, "bookorbit.local/api/v1/koreader/");

  const KOSyncState koBefore = kosyncFixture();
  KOSyncState koAfter;
  JsonDocument koRequest = json(R"({"serverUrl":"kosync.local/"})");
  ASSERT_TRUE(SyncSettingsWebPolicy::parseKOSyncUpdate(koRequest, koBefore, koAfter).valid);
  EXPECT_EQ(koAfter.serverUrl, "kosync.local/");

  BookOrbitState emptyAfter;
  JsonDocument emptyRequest = json(R"({"serverUrl":"https://"})");
  ASSERT_TRUE(SyncSettingsWebPolicy::parseBookOrbitUpdate(emptyRequest, before, emptyAfter).valid);
  EXPECT_TRUE(emptyAfter.serverUrl.empty()) << "match the on-device empty-scheme sentinel";
}

TEST(SyncSettingsWebPolicy, EnumValuesRequireInRangeIntegers) {
  const KOSyncState before = kosyncFixture();
  for (const std::string& request : {R"({"matchMethod":-1})", R"({"matchMethod":2})", R"({"matchMethod":"1"})",
                                     R"({"syncBehavior":-1})", R"({"syncBehavior":2})", R"({"syncBehavior":true})"}) {
    KOSyncState next;
    JsonDocument doc = json(request);
    EXPECT_FALSE(SyncSettingsWebPolicy::parseKOSyncUpdate(doc, before, next).valid) << request;
    expectEqual(next, before);
  }
}

TEST(SyncSettingsWebPolicy, PersistenceFailureRollsBackTheWholeInMemorySnapshot) {
  const KOSyncState before = kosyncFixture();
  KOSyncState after = before;
  after.serverUrl = "http://new.test";
  after.username = "new-user";
  after.password = "new-password";
  after.matchMethod = 0;
  after.sendMetadata = false;
  after.syncBehavior = 0;
  after.autoSync = false;

  KOSyncState live = before;
  int saveCalls = 0;
  const bool saved = SyncSettingsWebPolicy::persistWithRollback(
      before, after, [&](const KOSyncState& state) { live = state; },
      [&] {
        saveCalls++;
        return false;
      });

  EXPECT_FALSE(saved);
  EXPECT_EQ(saveCalls, 1);
  expectEqual(live, before);

  const BookOrbitState boBefore = bookOrbitFixture();
  BookOrbitState boAfter = boBefore;
  boAfter.username = "new-bookorbit-user";
  BookOrbitState boLive = boBefore;
  EXPECT_FALSE(SyncSettingsWebPolicy::persistWithRollback(
      boBefore, boAfter, [&](const BookOrbitState& state) { boLive = state; }, [] { return false; }));
  expectEqual(boLive, boBefore);
}

TEST(SyncSettingsWebPolicy, LegacySettingsRemainCompatibleWithAWriteOnlyPassword) {
  EXPECT_TRUE(SyncSettingsWebPolicy::isLegacyKOSyncSetting("koUsername"));
  EXPECT_TRUE(SyncSettingsWebPolicy::isLegacyKOSyncSetting("koPassword"));
  EXPECT_TRUE(SyncSettingsWebPolicy::isLegacyKOSyncSetting("koAutoSync"));
  EXPECT_FALSE(SyncSettingsWebPolicy::isLegacyKOSyncSetting("sleepTimeoutMinutes"));
  EXPECT_TRUE(SyncSettingsWebPolicy::isLegacySecretSetting("koPassword"));
  EXPECT_FALSE(SyncSettingsWebPolicy::isLegacySecretSetting("koUsername"));

  const KOSyncState before = kosyncFixture();
  KOSyncState after;
  JsonDocument request = json(
      R"({"koServerUrl":"legacy.test","koUsername":"legacy-user","koPassword":"","koMatchMethod":0,"koSendMetadata":0,"koSyncBehavior":0,"koAutoSync":0})");
  const auto result = SyncSettingsWebPolicy::parseLegacyKOSyncUpdate(request, before, after);
  ASSERT_TRUE(result.valid) << result.error;
  EXPECT_TRUE(result.hasChanges);
  EXPECT_EQ(after.serverUrl, "legacy.test");
  EXPECT_EQ(after.username, "legacy-user");
  EXPECT_EQ(after.password, before.password);
  EXPECT_EQ(after.matchMethod, 0);
  EXPECT_FALSE(after.sendMetadata);
  EXPECT_EQ(after.syncBehavior, 0);
  EXPECT_FALSE(after.autoSync);

  KOSyncState untouched;
  JsonDocument generalRequest = json(R"({"sleepTimeoutMinutes":10})");
  const auto generalResult = SyncSettingsWebPolicy::parseLegacyKOSyncUpdate(generalRequest, before, untouched);
  EXPECT_TRUE(generalResult.valid);
  EXPECT_FALSE(generalResult.hasChanges);
  expectEqual(untouched, before);
}

}  // namespace
