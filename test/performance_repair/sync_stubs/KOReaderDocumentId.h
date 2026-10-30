#pragma once
#include <string>
struct KOReaderDocumentId {
  inline static int reads = 0;
  inline static std::string identity = std::string(32, 'a');
  static std::string calculateForStats(const std::string&) { ++reads; return identity; }
  static std::string calculateForProgress(const std::string&, bool) { ++reads; return identity; }
};
