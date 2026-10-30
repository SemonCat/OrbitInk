#pragma once

#include <memory>

#include "KOSyncTransport.h"

// Factory for the production KOSyncTransport (real freeink::SecureHttpClient, see
// SecureHttpClientTransport.h). Split out from KOReaderSyncClient.cpp specifically so that file
// never needs to #include the real transport (and, transitively, freeink-sdk's SecureHttpClient.h
// -> Arduino.h/WiFiClient.h/wolfSSL) -- letting KOReaderSyncClient.cpp's PROTOCOL logic be host-
// tested against a fake KOSyncTransport (see test/kosync_protocol/) without needing any of that.
// KOReaderSyncClient::Session's default constructor calls this; only production firmware builds
// (which link DefaultKOSyncTransport.cpp) ever actually invoke it.
std::unique_ptr<KOSyncTransport> makeDefaultKOSyncTransport();
