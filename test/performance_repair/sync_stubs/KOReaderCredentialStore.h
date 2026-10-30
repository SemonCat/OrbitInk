#pragma once
enum class DocumentMatchMethod { BINARY, FILENAME };
struct HostKOStore {
  bool enabled = true;
  bool credentials = true;
  bool getAutoSyncEnabled() { return enabled; }
  bool hasCredentials() { return credentials; }
  DocumentMatchMethod getMatchMethod() { return DocumentMatchMethod::BINARY; }
};
inline HostKOStore KOREADER_STORE;
