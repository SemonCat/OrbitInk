#pragma once
// Host-test stand-in for lib/Serialization/ObfuscationUtils.h: declarations only, matching the
// real signatures exactly. PersistableStore.cpp's extractPassword() (the only caller) is never
// invoked by BookmarkFile.cpp/BookmarkFileTest.cpp, so -Wl,--gc-sections / -dead_strip drops that
// unused function -- and with it the otherwise-unresolved references to these symbols -- before
// the linker ever needs to resolve them. Same technique test/kosync_protocol/CMakeLists.txt uses
// for makeDefaultKOSyncTransport().
#include <cstddef>
#include <string>

namespace obfuscation {
std::string deobfuscateFromBase64(const char* encoded, size_t maxDecodedLength, bool* ok, bool* tooLong);
}  // namespace obfuscation
