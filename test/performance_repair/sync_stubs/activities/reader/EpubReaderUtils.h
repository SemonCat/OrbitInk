#pragma once
#include <Epub.h>
namespace EpubReaderUtils {
inline bool haveProgress = false, saveWorks = true;
inline int spine = 0, page = 0, pages = 0, saves = 0;
inline uint64_t timestamp = 0;
inline bool legacy = false;
inline bool loadProgress(const std::string&, int& s, int& p, int& n, uint64_t& t, bool& l) {
  s = spine; p = page; n = pages; t = timestamp; l = legacy; return haveProgress;
}
inline bool saveProgress(Epub&, int s, int p, int n, uint64_t t) {
  ++saves;
  if (!saveWorks) return false;
  spine = s; page = p; pages = n; timestamp = t; haveProgress = true;
  return true;
}
}
