#pragma once
struct HostBOStore {
  bool credentials = true;
  bool hasCredentials() { return credentials; }
};
inline HostBOStore BOOKORBIT_STORE;
