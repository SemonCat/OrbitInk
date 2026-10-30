#pragma once
#include <HalStorage.h>

#include <string>
class Epub {
 public:
  template <class T>
  bool readItemContentsToStream(const std::string&, T&, size_t, bool = false) {
    return false;
  }
};
