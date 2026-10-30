#pragma once

#include <string>

class GfxRenderer;

// Main-task automatic sync scope. Providers retain independent outcomes and TLS sessions,
// but share one saved-network attempt and one immutable book identity per event.
namespace ProgressSyncNetwork {
class Batch {
  std::string path;
  GfxRenderer* renderer;

 public:
  explicit Batch(std::string path, GfxRenderer* renderer = nullptr);
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
