#pragma once
// Host-test stand-in for lib/KOReaderSync/KOReaderCredentialStore.h. The real class persists
// to SD via PersistableStore<T> (HalStorage-backed) and hashes/obfuscates via MD5Builder/
// ObfuscationUtils -- none of that is relevant to testing KOReaderSyncClient's PROTOCOL logic
// (JSON building/parsing, HTTP-status-to-Error categorization, primary/alternate handling),
// which only ever calls the read accessors below. This stub is a plain, test-settable struct
// instead: tests configure it directly before exercising KOReaderSyncClient::Session.
#include <string>

class KOReaderCredentialStore {
 public:
  static KOReaderCredentialStore& getInstance() {
    static KOReaderCredentialStore instance;
    return instance;
  }

  bool hasCredentials() const { return hasCredentials_; }
  const std::string& getUsername() const { return username_; }
  const std::string& getPassword() const { return password_; }
  std::string getMd5Password() const { return md5Password_; }
  std::string getBaseUrl() const { return baseUrl_; }
  bool usesCrossPointSyncServer() const { return usesCrossPointSyncServer_; }

  // Test-only setters.
  void setTestCredentials(bool has, std::string user = "", std::string pass = "", std::string md5 = "") {
    hasCredentials_ = has;
    username_ = std::move(user);
    password_ = std::move(pass);
    md5Password_ = std::move(md5);
  }
  void setTestBaseUrl(std::string url) { baseUrl_ = std::move(url); }
  void setTestUsesCrossPointSyncServer(bool v) { usesCrossPointSyncServer_ = v; }
  void resetToDefaults() {
    hasCredentials_ = true;
    username_ = "testuser";
    password_ = "testpass";
    md5Password_ = "deadbeefdeadbeefdeadbeefdeadbeef";
    baseUrl_ = "https://sync.example.test";
    usesCrossPointSyncServer_ = false;
  }

 private:
  KOReaderCredentialStore() { resetToDefaults(); }

  bool hasCredentials_ = true;
  std::string username_ = "testuser";
  std::string password_ = "testpass";
  std::string md5Password_ = "deadbeefdeadbeefdeadbeefdeadbeef";
  std::string baseUrl_ = "https://sync.example.test";
  bool usesCrossPointSyncServer_ = false;
};

#define KOREADER_STORE KOReaderCredentialStore::getInstance()
