#include "DefaultKOSyncTransport.h"

#include "SecureHttpClientTransport.h"

std::unique_ptr<KOSyncTransport> makeDefaultKOSyncTransport() { return std::make_unique<SecureHttpClientTransport>(); }
