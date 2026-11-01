#pragma once

#include <SecureHttpClient.h>

#include <memory>
#include <string>

#include "KOSyncTransport.h"

// Production KOSyncTransport backed by the real freeink-sdk freeink::SecureHttpClient
// (third-party, never modified). Owns exactly one SecureHttpClient instance for its whole
// lifetime and leaves connection reuse to SecureHttpClient's own setReuse(true) default
// (confirmed already implemented there: begin()/sendRequest() reuse the underlying connection
// whenever scheme/host/port match a still-open one, with one transparent reconnect-and-retry if
// the server closed a kept-alive connection) -- this adapter adds no reuse logic of its own, it
// only forwards to what freeink-sdk already does correctly.
//
// Ownership of the explicit end()/close call belongs one layer up, in
// KOReaderSyncClient::Session::~Session(), which calls transport_->end() exactly once while the
// transport is still fully alive. This class's own destructor is therefore intentionally left
// as the compiler-generated default: it must NOT call http_.end() itself, or the connection would
// be closed twice (once explicitly by Session, once here) every time a Session is torn down.
// freeink::SecureHttpClient's own destructor separately calls its own end() unconditionally as
// belt-and-braces RAII cleanup for any caller that skips the explicit close -- that is vendor
// code we do not control or patch, and closeConnection() is written to be idempotent (it no-ops
// once the connection is already null), so that harmless second internal call is not something
// we need to (or can, without touching freeink-sdk) eliminate.
class SecureHttpClientTransport final : public KOSyncTransport {
 public:
  SecureHttpClientTransport() { http_.setInsecure(); }

  bool begin(const std::string& url) override {
    bufferedBody_.clear();
    return http_.begin(url);
  }
  void addHeader(const std::string& name, const std::string& value) override { http_.addHeader(name, value); }

  int sendRequest(const char* method, const uint8_t* payload, size_t payloadLen, const DataCallback& onData,
                  const AbortCallback& shouldAbort) override {
    bufferedBody_.clear();
    // The SDK returns the parsed status even when the body was truncated or aborted.
    // A status alone must never confirm a sync or clear its retry state.
    // Buffer here only for callers without a sink: the SDK convenience overload drops
    // shouldAbort. This replaces its internal body buffer, without retaining two copies.
    const DataCallback buffer = [this](const uint8_t* data, size_t len) {
      bufferedBody_.append(reinterpret_cast<const char*>(data), len);
      return true;
    };
    const int status = http_.sendRequest(method, payload, payloadLen, onData ? onData : buffer, shouldAbort);
    if (status > 0 && (!http_.responseComplete() || http_.aborted() || http_.callbackAborted())) return -1;
    return status;
  }

  const std::string& getString() const override { return bufferedBody_; }
  std::string getResponseHeader(const std::string& name) const override { return http_.getHeader(name); }
  void end() override { http_.end(); }
  void setTimeout(unsigned long milliseconds) override { http_.setTimeout(milliseconds); }

 private:
  freeink::SecureHttpClient http_;
  std::string bufferedBody_;
};
