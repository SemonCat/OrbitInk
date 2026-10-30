#pragma once

#include <cstddef>

namespace pngMemory {
// Preserve the converter's existing total-heap floor. The actual PNG object
// size also depends on PNG_MAX_BUFFERED_PIXELS; it needs one contiguous block.
constexpr size_t MIN_FREE_HEAP = 60 * 1024;
constexpr bool hasDecoderHeadroom(size_t freeHeap, size_t maxAlloc, size_t decoderBytes) {
  return freeHeap >= MIN_FREE_HEAP && maxAlloc >= decoderBytes;
}
}  // namespace pngMemory
