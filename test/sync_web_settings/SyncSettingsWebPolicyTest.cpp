#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <string>

#include "SyncSettingsWebPolicy.h"

namespace {

using SyncSettingsWebPolicy::BookOrbitState;

JsonDocument json(const std::string& value) {
  JsonDocument doc;
  EXPECT_FALSE(deserializeJson(doc, value));
  return doc;
}

void expectEqual(const BookOrbitState& actual, const BookOrbitState& expected) {
  EXPECT_EQ(actual.serverUrl, expected.serverUrl);
  EXPECT_EQ(actual.username, expected.username);
  EXPECT_EQ(actual.password, expected.password);
}

BookOrbitState bookOrbitFixture() {
  return {.serverUrl = "books.example.test/api/v1", .username = "bob", .password = "bookorbit-secret"};
}

TEST(SyncSettingsWebPolicy, GetResponsesNeverDisclosePasswordsOrHashes) {
  JsonDocument boDoc;
  SyncSettingsWebPolicy::writeBookOrbitResponse(boDoc, bookOrbitFixture());
  EXPECT_EQ(boDoc["serverUrl"].as<std::string>(), "books.example.test/api/v1");
  EXPECT_EQ(boDoc["username"].as<std::string>(), "bob");
  EXPECT_TRUE(boDoc["hasPassword"].as<bool>());
  EXPECT_TRUE(boDoc["configured"].as<bool>());
  EXPECT_TRUE(boDoc["password"].isNull());
  EXPECT_TRUE(boDoc["userkey"].isNull());

  std::string wire;
  serializeJson(boDoc, wire);
  EXPECT_EQ(wire.find("bookorbit-secret"), std::string::npos);
}

TEST(SyncSettingsWebPolicy, BlankPasswordsPreserveTheStoredValue) {
  const BookOrbitState boBefore = bookOrbitFixture();
  BookOrbitState boAfter;
  JsonDocument boRequest = json(R"({"username":"new-bob","password":""})");
  const auto boResult = SyncSettingsWebPolicy::parseBookOrbitUpdate(boRequest, boBefore, boAfter);
  ASSERT_TRUE(boResult.valid) << boResult.error;
  EXPECT_TRUE(boResult.hasChanges);
  EXPECT_EQ(boAfter.username, "new-bob");
  EXPECT_EQ(boAfter.password, boBefore.password);

  BookOrbitState missingAfter;
  JsonDocument missingRequest = json(R"({"serverUrl":"books-two.example.test"})");
  ASSERT_TRUE(SyncSettingsWebPolicy::parseBookOrbitUpdate(missingRequest, boBefore, missingAfter).valid);
  EXPECT_EQ(missingAfter.password, boBefore.password);
}

TEST(SyncSettingsWebPolicy, ExplicitClearOnlyClearsCredentials) {
  const BookOrbitState boBefore = bookOrbitFixture();
  const BookOrbitState boAfter = SyncSettingsWebPolicy::withoutCredentials(boBefore);
  EXPECT_TRUE(boAfter.username.empty());
  EXPECT_TRUE(boAfter.password.empty());
  EXPECT_EQ(boAfter.serverUrl, boBefore.serverUrl);
}

TEST(SyncSettingsWebPolicy, RejectsMalformedTypesAndLengthsWithoutChangingState) {
  const BookOrbitState boBefore = bookOrbitFixture();
  BookOrbitState boNext;
  JsonDocument boLong;
  boLong["serverUrl"] = std::string(SyncSettingsWebPolicy::MAX_SERVER_URL_LENGTH + 1, 's');
  EXPECT_FALSE(SyncSettingsWebPolicy::parseBookOrbitUpdate(boLong, boBefore, boNext).valid);
  expectEqual(boNext, boBefore);

  JsonDocument userLong;
  userLong["username"] = std::string(SyncSettingsWebPolicy::MAX_USERNAME_LENGTH + 1, 'u');
  EXPECT_FALSE(SyncSettingsWebPolicy::parseBookOrbitUpdate(userLong, boBefore, boNext).valid);
  expectEqual(boNext, boBefore);

  JsonDocument passwordLong;
  passwordLong["password"] = std::string(SyncSettingsWebPolicy::MAX_PASSWORD_LENGTH + 1, 'p');
  EXPECT_FALSE(SyncSettingsWebPolicy::parseBookOrbitUpdate(passwordLong, boBefore, boNext).valid);
  expectEqual(boNext, boBefore);

  for (const std::string& request :
       {R"([])", R"({})", R"({"unknown":1})", R"({"username":3})", R"({"password":null})", R"({"serverUrl":[]})",
        R"({"serverUrl":"books.test","extra":true})"}) {
    JsonDocument doc = json(request);
    EXPECT_FALSE(SyncSettingsWebPolicy::parseBookOrbitUpdate(doc, boBefore, boNext).valid) << request;
    expectEqual(boNext, boBefore);
  }

  EXPECT_TRUE(SyncSettingsWebPolicy::isRequestBodySizeAllowed(SyncSettingsWebPolicy::MAX_REQUEST_BYTES));
  EXPECT_FALSE(SyncSettingsWebPolicy::isRequestBodySizeAllowed(0));
  EXPECT_FALSE(SyncSettingsWebPolicy::isRequestBodySizeAllowed(SyncSettingsWebPolicy::MAX_REQUEST_BYTES + 1));
}

TEST(SyncSettingsWebPolicy, RetiredKOSyncFieldsAreNotAcceptedAsBookOrbitSettings) {
  const BookOrbitState before = bookOrbitFixture();
  for (const std::string& request :
       {R"({"koUsername":"alice"})", R"({"koPassword":"x","username":"bob"})", R"({"matchMethod":1})",
        R"({"autoSync":true})"}) {
    BookOrbitState next;
    JsonDocument doc = json(request);
    EXPECT_FALSE(SyncSettingsWebPolicy::parseBookOrbitUpdate(doc, before, next).valid) << request;
    expectEqual(next, before);
  }
}

TEST(SyncSettingsWebPolicy, RawUrlsAreLeftForCredentialStoresToNormalize) {
  const BookOrbitState before = bookOrbitFixture();
  BookOrbitState rawAfter;
  JsonDocument rawRequest = json(R"({"serverUrl":"bookorbit.local/api/v1/koreader/"})");
  ASSERT_TRUE(SyncSettingsWebPolicy::parseBookOrbitUpdate(rawRequest, before, rawAfter).valid);
  EXPECT_EQ(rawAfter.serverUrl, "bookorbit.local/api/v1/koreader/");

  BookOrbitState emptyAfter;
  JsonDocument emptyRequest = json(R"({"serverUrl":"https://"})");
  ASSERT_TRUE(SyncSettingsWebPolicy::parseBookOrbitUpdate(emptyRequest, before, emptyAfter).valid);
  EXPECT_TRUE(emptyAfter.serverUrl.empty()) << "match the on-device empty-scheme sentinel";
}

TEST(SyncSettingsWebPolicy, PersistenceFailureRollsBackTheWholeInMemorySnapshot) {
  const BookOrbitState boBefore = bookOrbitFixture();
  BookOrbitState boAfter = boBefore;
  boAfter.username = "new-bookorbit-user";
  boAfter.password = "new-password";
  BookOrbitState boLive = boBefore;
  int saveCalls = 0;
  EXPECT_FALSE(SyncSettingsWebPolicy::persistWithRollback(
      boBefore, boAfter, [&](const BookOrbitState& state) { boLive = state; },
      [&] {
        saveCalls++;
        return false;
      }));
  EXPECT_EQ(saveCalls, 1);
  expectEqual(boLive, boBefore);
}

}  // namespace
