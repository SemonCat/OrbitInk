// Host allocator instrumentation for the parser's explicit nothrow allocations.
// Regular C++ allocations remain available to the harness; only the selected
// std::nothrow allocation is failed, so reported heap size cannot mask the fault.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

size_t g_failNothrowOrdinal = 0;
size_t g_nothrowCalls = 0;
size_t g_nothrowOutstanding = 0;
size_t g_nothrowPeak = 0;

[[noreturn]] void throwBadAllocFromRuntime();

namespace {
struct AllocationHeader {
  size_t size;
  bool tracked;
};

constexpr size_t kHeaderSize =
    ((sizeof(AllocationHeader) + alignof(std::max_align_t) - 1) / alignof(std::max_align_t)) *
    alignof(std::max_align_t);

void* allocate(const size_t size, const bool nothrow) {
  if (nothrow) {
    ++g_nothrowCalls;
    if (g_failNothrowOrdinal != 0 && g_nothrowCalls == g_failNothrowOrdinal) return nullptr;
  }
  void* raw = std::malloc(kHeaderSize + size);
  if (!raw) return nullptr;
  const AllocationHeader header{size, nothrow};
  std::memcpy(raw, &header, sizeof(header));
  if (nothrow) {
    g_nothrowOutstanding += size;
    g_nothrowPeak = std::max(g_nothrowPeak, g_nothrowOutstanding);
  }
  return static_cast<uint8_t*>(raw) + kHeaderSize;
}

void release(void* pointer) noexcept {
  if (!pointer) return;
  auto* raw = static_cast<uint8_t*>(pointer) - kHeaderSize;
  AllocationHeader header{};
  std::memcpy(&header, raw, sizeof(header));
  if (header.tracked) g_nothrowOutstanding -= header.size;
  std::free(raw);
}
}  // namespace

void resetNothrowTracking(const size_t failOrdinal) {
  g_failNothrowOrdinal = failOrdinal;
  g_nothrowCalls = 0;
  g_nothrowPeak = g_nothrowOutstanding;
}

void* operator new(const size_t size) {
  if (void* pointer = allocate(size, false)) return pointer;
  throwBadAllocFromRuntime();
}

void* operator new[](const size_t size) { return operator new(size); }
void* operator new(const size_t size, const std::nothrow_t&) noexcept { return allocate(size, true); }
void* operator new[](const size_t size, const std::nothrow_t&) noexcept { return allocate(size, true); }

void operator delete(void* pointer) noexcept { release(pointer); }
void operator delete(void* pointer, size_t) noexcept { release(pointer); }
void operator delete[](void* pointer) noexcept { release(pointer); }
void operator delete[](void* pointer, size_t) noexcept { release(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { release(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { release(pointer); }
