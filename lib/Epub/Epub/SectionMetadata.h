#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <optional>

// Shared by the section writer and metadata-only sync; no renderer or layout is needed.
namespace sectionmetadata {
constexpr uint8_t FILE_VERSION = 34;
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) +
                                 sizeof(uint8_t) + sizeof(bool) + sizeof(uint32_t) + sizeof(uint32_t) +
                                 sizeof(uint32_t) + sizeof(uint32_t);

inline std::optional<uint16_t> pageCount(HalFile& file) {
  uint8_t version = 0;
  uint16_t count = 0;
  if (file.size() < HEADER_SIZE || !file.seekSet(0) || file.read(&version, sizeof(version)) != sizeof(version) ||
      version != FILE_VERSION || !file.seekSet(HEADER_SIZE - sizeof(uint32_t) * 4 - sizeof(uint16_t)) ||
      file.read(&count, sizeof(count)) != sizeof(count)) {
    return std::nullopt;
  }
  return count;
}

inline std::optional<uint16_t> paragraphIndexForPage(HalFile& file, uint16_t page) {
  uint32_t offset = 0;
  uint16_t count = 0;
  uint16_t paragraph = 0;
  if (file.size() < HEADER_SIZE || !file.seekSet(HEADER_SIZE - sizeof(uint32_t) * 2) ||
      file.read(&offset, sizeof(offset)) != sizeof(offset) || offset < HEADER_SIZE || offset >= file.size() ||
      !file.seekSet(offset) || file.read(&count, sizeof(count)) != sizeof(count) || count == 0 || page >= count ||
      static_cast<uint64_t>(offset) + sizeof(count) + static_cast<uint64_t>(count) * sizeof(paragraph) > file.size() ||
      !file.seekSet(offset + sizeof(count) + page * sizeof(paragraph)) ||
      file.read(&paragraph, sizeof(paragraph)) != sizeof(paragraph)) {
    return std::nullopt;
  }
  return paragraph;
}
}  // namespace sectionmetadata
