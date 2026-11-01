#pragma once

#include <string>

class GfxRenderer;

// Main-task automatic sync scope. Providers retain independent outcomes and TLS sessions,
// but share one saved-network attempt and one immutable book identity per event.
namespace ProgressSyncNetwork {
// Association + DHCP budget. Open/close/sleep keep the short one: someone may be waiting on it.
// A timer wake starts the radio cold with nobody waiting, and a timeout there costs a whole
// interval, so it gets the long one.
constexpr unsigned long CONNECT_BUDGET_MS = 3500;
constexpr unsigned long UNATTENDED_CONNECT_BUDGET_MS = 12000;

class Batch {
  std::string path;
  GfxRenderer* renderer;

 public:
  explicit Batch(std::string path, GfxRenderer* renderer = nullptr,
                 unsigned long connectBudgetMs = CONNECT_BUDGET_MS);
  ~Batch();
  Batch(const Batch&) = delete;
  Batch& operator=(const Batch&) = delete;
};

bool active();
bool available();
bool connect();
void disconnect();
std::string binaryDocument(const std::string& path);
}  // namespace ProgressSyncNetwork
