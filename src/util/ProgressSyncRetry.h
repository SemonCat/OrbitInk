#pragma once

#include <cstdint>
#include <string>

// Failed transactions retain lightweight diagnostic state, but scheduling is a recurring
// user-selected cadence and never depends on this state. Wall-clock time is irrelevant: the ESP
// sleep timer counts the selected interval from each return to sleep.
namespace ProgressSyncRetry {
enum class Provider : uint8_t { KOSync = 1, BookOrbit = 2 };
enum class Outcome { Confirmed, Retry, NoRetry };

struct State {
  std::string path;
  uint8_t providers = 0;

  bool pending(Provider provider) const { return (providers & static_cast<uint8_t>(provider)) != 0; }
  void observe(Provider provider, const std::string& book, Outcome outcome) {
    if (path != book) {
      providers = 0;
      path = book;
    }
    const auto bit = static_cast<uint8_t>(provider);
    if (outcome == Outcome::Retry && !book.empty()) {
      providers |= bit;
    } else {
      providers &= ~bit;
    }
    if (providers == 0) path.clear();
  }

  void prune(const std::string& currentBook, uint8_t enabledProviders) {
    providers &= enabledProviders & 3;
    if (path.empty() || path != currentBook) providers = 0;
    if (providers == 0) path.clear();
  }
};

// Both clients expose the same named error categories, but different enum values.
// Their progress APIs collapse many HTTP failures into SERVER_ERROR; inspect the
// status too so bad requests/auth/configuration never become an hourly loop.
// NOT_FOUND is handled by the caller's GET comparison, never an upload success.
template <typename Client>
Outcome classify(typename Client::Error error, int httpCode) {
  if (error == Client::OK) return Outcome::Confirmed;
  // Client JSON errors can be truncated network responses (including HTTP 200).
  // Deterministic local EPUB/progress parsing never enters this classifier.
  if (error == Client::NETWORK_ERROR || error == Client::LOW_MEMORY || error == Client::JSON_ERROR) {
    return Outcome::Retry;
  }
  if (error == Client::SERVER_ERROR &&
      (httpCode == 408 || httpCode == 429 || (httpCode >= 500 && httpCode <= 599 && httpCode != 501))) {
    return Outcome::Retry;
  }
  return Outcome::NoRetry;
}

// Persistence adapter; the State and result policy above are host-testable.
void record(Provider provider, const std::string& path, Outcome outcome);
void confirm(Provider provider, const std::string& path);
uint64_t sleepDelayMicros();
}  // namespace ProgressSyncRetry
