#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace BookOrbitCatalogBrowserPolicy {

constexpr size_t NAVIGATION_HISTORY_LIMIT = 8;

enum class ReadState { Unread, WantToRead, Reading, OnHold, Finished, Abandoned, Other };
ReadState readState(const std::string& status);
bool matchesReadFilter(const std::string& status, const std::string& filter);

struct ListMove {
  int page;
  int index;
  bool fetch;
};
ListMove moveInList(int page, int index, int count, bool hasNext, int delta);
std::vector<std::string> wrapTitle(std::string text, int width, int maxLines,
                                  const std::function<int(const std::string&)>& measure);

std::string downloadPath(const std::string& authors, const std::string& title, const std::string& fileId,
                         size_t fileChoiceCount);

// Fixed storage keeps page traversal from growing the X3 heap without bound. When full, a new
// transition evicts the oldest frame; Back still visits every retained frame in exact LIFO order.
template <typename T, size_t Capacity = NAVIGATION_HISTORY_LIMIT>
class NavigationHistory {
 public:
  static_assert(Capacity > 0);

  bool empty() const { return size_ == 0; }
  size_t size() const { return size_; }
  void clear() {
    for (size_t i = 0; i < size_; ++i) entries_[(first_ + i) % Capacity] = T{};
    first_ = 0;
    size_ = 0;
  }
  void push(T frame) {
    const size_t slot = size_ < Capacity ? (first_ + size_) % Capacity : first_;
    entries_[slot] = std::move(frame);
    if (size_ < Capacity) {
      ++size_;
    } else {
      first_ = (first_ + 1) % Capacity;
    }
  }
  bool popBack(T& frame) {
    if (size_ == 0) return false;
    const size_t slot = (first_ + size_ - 1) % Capacity;
    frame = std::move(entries_[slot]);
    entries_[slot] = T{};
    --size_;
    if (size_ == 0) first_ = 0;
    return true;
  }

 private:
  std::array<T, Capacity> entries_{};
  size_t first_ = 0;
  size_t size_ = 0;
};

// Keeps exactly one connection-owning catalog session for an exact configured server scope. A
// different scope destroys the old session before constructing its replacement, so keep-alive/TLS
// state can accelerate root -> list -> detail requests without ever crossing server boundaries.
// Using the full configured base URL as the key is intentionally stricter than origin-only reuse.
template <typename Session>
class SingleServerSessionCache {
 public:
  template <typename Factory>
  Session* get(const std::string& serverScope, Factory&& factory) {
    if (!session_ || serverScope_ != serverScope) {
      clear();
      session_ = std::forward<Factory>(factory)();
      if (session_) serverScope_ = serverScope;
    }
    return session_.get();
  }

  void clear() {
    session_.reset();
    serverScope_.clear();
  }

 private:
  std::unique_ptr<Session> session_;
  std::string serverScope_;
};

}  // namespace BookOrbitCatalogBrowserPolicy
