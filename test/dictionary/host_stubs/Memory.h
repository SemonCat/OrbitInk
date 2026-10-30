#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

// Dictionary's host tests need deterministic allocation failures for the
// streaming index builders.  Production Memory.h has the same API, while this
// seam lets a test fail the next (or a later) makeUniqueNoThrow allocation.
namespace HostMemory {

inline int failAfter = -1;

inline void reset() { failAfter = -1; }

inline void failNext() { failAfter = 0; }

inline void failAfterAllocations(int successfulAllocations) { failAfter = successfulAllocations; }

inline bool shouldFail() {
  if (failAfter < 0) return false;
  if (failAfter == 0) return true;
  --failAfter;
  return false;
}

}  // namespace HostMemory

template <typename T, typename... Args>
  requires(!std::is_array_v<T>)
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  if (HostMemory::shouldFail()) return nullptr;
  return std::unique_ptr<T>(new (std::nothrow) T(std::forward<Args>(args)...));
}

template <typename T>
  requires std::is_unbounded_array_v<T>
std::unique_ptr<T> makeUniqueNoThrow(size_t count) {
  if (HostMemory::shouldFail()) return nullptr;
  using Elem = std::remove_extent_t<T>;
  return std::unique_ptr<T>(new (std::nothrow) Elem[count]());
}
