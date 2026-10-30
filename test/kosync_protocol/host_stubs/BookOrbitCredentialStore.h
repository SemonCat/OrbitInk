#pragma once
// Host-test stand-in for lib/KOReaderSync/BookOrbitCredentialStore.h, mirroring
// KOReaderCredentialStore.h's own stub in this directory: the real class persists to SD via
// PersistableStore<T> and hashes/obfuscates via MD5Builder/ObfuscationUtils, none of which is
// relevant to testing BookOrbitSyncClient's PROTOCOL logic. This stub is a plain, test-settable
// struct instead.
//
// Exposes BOTH the real class's production API (setCredentials/setServerUrl/saveToFile, used by
// BookOrbitMigration.cpp) and direct test-only setters (setTestCredentials/setTestBaseUrl, used
// by BookOrbitClientProtocolTest/BookOrbitSyncPolicyTest to force an exact state in one call).
// hasCredentials() is computed from the three fields exactly like the real class -- NOT an
// independent flag -- so a test using either API sees consistent results; setTestCredentials()
// still accepts an explicit `has` for the rare case a test wants to force a value the three
// fields alone would not produce (e.g. asserting no credentials with a non-empty serverUrl).
//
// Unlike the real class, getBaseUrl() is a plain accessor with NO normalization (no scheme
// defaulting, no "/api/v1/koreader" suffix logic) -- that logic belongs to
// BookOrbitCredentialStore.cpp itself, not to code that merely reads through this store, and
// every test using this stub sets the value it wants getBaseUrl() to return directly.
#include <optional>
#include <string>

class BookOrbitCredentialStore {
 public:
  static BookOrbitCredentialStore& getInstance() {
    static BookOrbitCredentialStore instance;
    return instance;
  }

  bool hasCredentials() const {
    if (hasCredentialsOverride_) return *hasCredentialsOverride_;
    return !username_.empty() && !password_.empty() && !baseUrl_.empty();
  }
  const std::string& getUsername() const { return username_; }
  const std::string& getPassword() const { return password_; }
  std::string getMd5Password() const { return md5Password_; }
  std::string getBaseUrl() const { return baseUrl_; }
  const std::string& getServerUrl() const { return rawServerUrl_; }

  // Production API, matching the real class -- used by BookOrbitMigration.cpp.
  void setCredentials(const std::string& user, const std::string& pass) {
    username_ = user;
    password_ = pass;
    hasCredentialsOverride_.reset();
  }
  // Strips exactly one trailing slash -- a deliberately minimal slice of the real class's fuller
  // normalization (scheme defaulting, canonical "/api/v1/koreader" suffix), just enough to let a
  // migration test manufacture a genuine divergence between this store's and
  // KOReaderCredentialStore's getBaseUrl() for the SAME underlying URL (see
  // test/bookorbit_migration/BookOrbitMigrationTest.cpp's owner-rewrite-fallback test), the way
  // the real BookOrbitCredentialStore::getBaseUrl() legitimately can.
  void setServerUrl(const std::string& url) {
    rawServerUrl_ = url;
    baseUrl_ = url;
    if (!baseUrl_.empty() && baseUrl_.back() == '/') baseUrl_.pop_back();
    hasCredentialsOverride_.reset();
  }
  void clearCredentials() {
    username_.clear();
    password_.clear();
    hasCredentialsOverride_.reset();
  }
  int saveCount = 0;
  bool saveShouldFail = false;
  bool saveToFile() {
    saveCount++;
    return !saveShouldFail;
  }

  // Test-only: force an exact state (and, via `has`, override hasCredentials() even against
  // fields that would otherwise compute a different answer) in one call.
  void setTestCredentials(bool has, std::string user = "", std::string pass = "", std::string md5 = "") {
    hasCredentialsOverride_ = has;
    username_ = std::move(user);
    password_ = std::move(pass);
    md5Password_ = std::move(md5);
  }
  void setTestBaseUrl(std::string url) {
    rawServerUrl_ = url;
    baseUrl_ = std::move(url);
  }
  void resetToDefaults() {
    hasCredentialsOverride_.reset();
    username_ = "bouser";
    password_ = "bopass";
    md5Password_ = "cafebabecafebabecafebabecafebabe";
    baseUrl_ = "https://bookorbit.example.test/api/v1/koreader";
    rawServerUrl_ = baseUrl_;
    saveCount = 0;
    saveShouldFail = false;
  }

 private:
  BookOrbitCredentialStore() { resetToDefaults(); }

  std::string username_ = "bouser";
  std::string password_ = "bopass";
  std::string md5Password_ = "cafebabecafebabecafebabecafebabe";
  std::string baseUrl_ = "https://bookorbit.example.test/api/v1/koreader";
  std::string rawServerUrl_ = baseUrl_;
  std::optional<bool> hasCredentialsOverride_;
};

#define BOOKORBIT_STORE BookOrbitCredentialStore::getInstance()
