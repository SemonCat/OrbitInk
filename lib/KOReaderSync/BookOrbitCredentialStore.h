#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>

/**
 * Singleton class for storing BookOrbit sync credentials on the SD card.
 *
 * BookOrbit (https://github.com/bookorbit/bookorbit) speaks a KOSync-compatible protocol but is
 * an EXPLICIT, standalone provider here -- its own server URL, its own username/password, its
 * own file -- never inferred from whatever server generic KOSync (KOReaderCredentialStore) is
 * pointed at. That opportunistic-detection model is what shipped in e51b383
 * (src/util/BookOrbitStatsSync.h); BookOrbitMigration.h carries a one-time, one-directional
 * upgrade from it so an existing user's page-stats queue is not silently orphaned.
 *
 * Passwords are XOR-obfuscated with the device's unique hardware MAC address and base64-encoded
 * before writing to JSON (not cryptographically secure, but prevents casual reading and ties
 * credentials to the specific device) -- identical scheme to KOReaderCredentialStore, via the
 * same PersistableStore::extractPassword()/ObfuscationUtils helpers.
 *
 * Selectively adapted, with attribution, from the MIT-licensed CrossInk-BookOrbit fork
 * (https://github.com/agosez/CrossInk-Bookorbit, commit 8c64d44), which first introduced a
 * standalone BookOrbitCredentialStore (lib/BookOrbitSync/BookOrbitCredentialStore.h) separate
 * from its own KOReaderCredentialStore. The field set, `/api/v1/koreader` URL normalization
 * (BookOrbit's own kosync-compatible endpoints live under that prefix, not at the server root),
 * and "self-hosted only, no public default" framing come from there; the persistence plumbing
 * below uses this codebase's own extractPassword()/PersistableStore helpers instead of that
 * fork's inline obfuscation calls, since those already carry this project's own hardening.
 *
 * BookOrbit always identifies documents by the binary partial-MD5 hash
 * (KOReaderDocumentId::calculateForStats), so unlike KOReaderCredentialStore there is no document
 * matching method to store.
 */
class BookOrbitCredentialStore : public PersistableStore<BookOrbitCredentialStore> {
 private:
  std::string username;
  std::string password;
  std::string serverUrl;

  BookOrbitCredentialStore() = default;
  ~BookOrbitCredentialStore() = default;

  friend class PersistableStore<BookOrbitCredentialStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/bookorbit.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Credential management
  void setCredentials(const std::string& user, const std::string& pass);
  const std::string& getUsername() const { return username; }
  const std::string& getPassword() const { return password; }

  // MD5 of the password, used as BookOrbit's kosync-compatible "userkey" (x-auth-key header).
  std::string getMd5Password() const;

  // True once username, password, AND a server URL are all set -- unlike KOReaderCredentialStore,
  // an empty serverUrl has no default to fall back to (BookOrbit is self-hosted only), so it is
  // part of "configured" here rather than a separate check callers must remember to make.
  bool hasCredentials() const;

  // Clears username and password (and persists). Leaves serverUrl untouched, matching
  // KOReaderCredentialStore::clearCredentials() -- the server address is a deployment detail the
  // user is more likely to want to keep than re-type when re-entering credentials.
  void clearCredentials();

  // Server URL management: the raw, user-entered value (may be bare host, may carry a pasted
  // /api/v1 or /api/v1/koreader suffix). Use getBaseUrl() for the normalized value to build
  // requests against.
  void setServerUrl(const std::string& url);
  const std::string& getServerUrl() const { return serverUrl; }

  // Normalized base URL for API calls: scheme defaulted to https (BookOrbit is self-hosted and
  // typically deployed behind TLS, unlike KOReaderCredentialStore's http:// fallback for the
  // common bare-LAN-IP KOSync-alike setup), trailing slashes stripped, and BookOrbit's own
  // canonical "/api/v1/koreader" prefix appended after stripping a pasted "/api/v1" or
  // "/api/v1/koreader" suffix the user may have copied from BookOrbit's own settings page.
  // Returns "" when no server URL has been configured -- there is no default to fall back to.
  std::string getBaseUrl() const;
};

// Helper macro to access credential store
#define BOOKORBIT_STORE BookOrbitCredentialStore::getInstance()
