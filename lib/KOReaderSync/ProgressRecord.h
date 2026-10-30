#pragma once

#include <cstddef>
#include <cstdint>

#include "ProgressTimestamp.h"

namespace ProgressRecord {

constexpr size_t LEGACY_SIZE = 4;
constexpr size_t PAGINATION_SIZE = 6;
constexpr size_t TIMESTAMPED_SIZE = 14;

struct Value {
  uint16_t spineIndex = 0;
  uint16_t pageNumber = 0;
  uint16_t pageCount = 0;
  uint64_t timestamp = 0;
  bool legacy = false;
};

inline uint16_t readU16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

inline uint64_t readU64(const uint8_t* data) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) value |= static_cast<uint64_t>(data[i]) << (i * 8);
  return value;
}

inline void writeU16(uint8_t* data, uint16_t value) {
  data[0] = static_cast<uint8_t>(value & 0xff);
  data[1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

inline void writeU64(uint8_t* data, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) data[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xff);
}

// Legacy 4/6-byte records remain readable. Their event chronology is unknowable, so timestamp
// stays zero. A corrupt timestamp likewise degrades to unknown while preserving the position.
inline bool decode(const uint8_t* data, size_t size, Value& out) {
  if (!data || (size != LEGACY_SIZE && size != PAGINATION_SIZE && size != TIMESTAMPED_SIZE)) return false;
  out = {};
  out.legacy = size != TIMESTAMPED_SIZE;
  out.spineIndex = readU16(data);
  out.pageNumber = readU16(data + 2);
  if (out.pageNumber == UINT16_MAX) out.pageNumber = 0;
  if (size >= PAGINATION_SIZE) out.pageCount = readU16(data + 4);
  if (size == TIMESTAMPED_SIZE) {
    const uint64_t timestamp = readU64(data + 6);
    out.timestamp = ProgressTimestamp::valid(timestamp) ? timestamp : 0;
  }
  return true;
}

inline size_t encode(const Value& value, uint8_t (&data)[TIMESTAMPED_SIZE], bool preserveLegacy) {
  writeU16(data, value.spineIndex);
  writeU16(data + 2, value.pageNumber);
  writeU16(data + 4, value.pageCount);
  if (preserveLegacy) return PAGINATION_SIZE;
  writeU64(data + 6, ProgressTimestamp::valid(value.timestamp) ? value.timestamp : 0);
  return TIMESTAMPED_SIZE;
}

inline void encode(const Value& value, uint8_t (&data)[TIMESTAMPED_SIZE]) {
  (void)encode(value, data, false);
}

inline bool sameLogicalPosition(const Value& a, const Value& b) {
  return a.spineIndex == b.spineIndex && a.pageNumber == b.pageNumber;
}

}  // namespace ProgressRecord
