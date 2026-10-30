#pragma once

#include <cstdint>

/**
 * First-run BookOrbit setup guide decisions. Pure, so host-tested (test/bookorbit_setup).
 *
 * The guide is shown by the BookOrbit-first Home (themes BOOKORBIT / JOURNAL) only while BookOrbit is
 * not usable -- server URL, username and password, the same three BookOrbitCredentialStore::
 * hasCredentials() requires -- and the first run has not been handled. "Handled"
 * (CrossPointSettings::bookOrbitSetupGuideDismissed) is set once, by "Later"/Back or the first time
 * BookOrbit is seen complete (shouldRecordHandled), and never cleared: clearing the credentials later
 * does not bring the guide back. It is separate from the credentials and never changes them.
 */
namespace BookOrbitSetupPolicy {

enum MissingField : uint8_t {
  MISSING_NONE = 0,
  MISSING_SERVER_URL = 1 << 0,
  MISSING_USERNAME = 1 << 1,
  MISSING_PASSWORD = 1 << 2,
};

constexpr uint8_t missingFields(const bool hasServerUrl, const bool hasUsername, const bool hasPassword) {
  return static_cast<uint8_t>((hasServerUrl ? 0 : MISSING_SERVER_URL) | (hasUsername ? 0 : MISSING_USERNAME) |
                              (hasPassword ? 0 : MISSING_PASSWORD));
}

constexpr bool isConfigured(const uint8_t missing) { return missing == MISSING_NONE; }

// Something was saved but not all three fields: the guide says what is still missing.
constexpr bool isPartial(const uint8_t missing) {
  return missing != MISSING_NONE && missing != (MISSING_SERVER_URL | MISSING_USERNAME | MISSING_PASSWORD);
}

// Configuration complete and not yet recorded: persist the handled marker (a one-time write).
constexpr bool shouldRecordHandled(const uint8_t missing, const bool handled) {
  return isConfigured(missing) && !handled;
}

constexpr bool shouldShowGuide(const bool bookOrbitHomeTheme, const uint8_t missing, const bool dismissed) {
  return bookOrbitHomeTheme && !isConfigured(missing) && !dismissed;
}

}  // namespace BookOrbitSetupPolicy
