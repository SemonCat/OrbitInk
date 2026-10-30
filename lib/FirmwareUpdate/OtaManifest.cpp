#include "OtaManifest.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>

#include "FirmwareUploadSession.h"

namespace ota_manifest {

namespace {
constexpr int SCHEMA_VERSION = 1;
constexpr char FIRMWARE_ASSET[] = "firmware.bin";

bool isFieldChar(const char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
         c == '-' || c == '+';
}

bool isUrlSegmentChar(const char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

// URL characters allowed anywhere (prefix included): unreserved + ":/". No '%', '?', '#', '@'.
bool isUrlChar(const char c) { return isUrlSegmentChar(c) || c == ':' || c == '/'; }

bool copyField(JsonVariantConst v, char* out, const size_t cap, bool (*allowed)(char)) {
  if (!v.is<const char*>()) return false;
  const char* s = v.as<const char*>();
  const size_t n = strlen(s);
  if (n == 0 || n >= cap) return false;
  for (size_t i = 0; i < n; i++) {
    if (!allowed(s[i])) return false;
  }
  memcpy(out, s, n + 1);
  return true;
}

bool lowerHex(const char* s, const size_t digits) {
  if (!s || strlen(s) != digits) return false;
  for (size_t i = 0; i < digits; i++) {
    const char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

void decodeHex(const char* s, uint8_t* out, const size_t bytes) {
  auto nib = [](const char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (size_t i = 0; i < bytes; i++) out[i] = static_cast<uint8_t>((nib(s[i * 2]) << 4) | nib(s[i * 2 + 1]));
}

bool validSegment(const char* begin, const char* end) {
  if (begin == end) return false;
  if (end - begin == 1 && begin[0] == '.') return false;
  if (end - begin == 2 && begin[0] == '.' && begin[1] == '.') return false;
  for (const char* p = begin; p < end; p++) {
    if (!isUrlSegmentChar(*p)) return false;
  }
  return true;
}
}  // namespace

const char* resultName(const Result r) {
  switch (r) {
    case Result::OK:
      return "OK";
    case Result::TOO_LARGE:
      return "TOO_LARGE";
    case Result::BAD_JSON:
      return "BAD_JSON";
    case Result::BAD_SCHEMA:
      return "BAD_SCHEMA";
    case Result::BAD_FIELD:
      return "BAD_FIELD";
    case Result::WRONG_CHANNEL:
      return "WRONG_CHANNEL";
    case Result::WRONG_TARGET:
      return "WRONG_TARGET";
    case Result::NOT_NEWER:
      return "NOT_NEWER";
    case Result::BAD_SIZE:
      return "BAD_SIZE";
    case Result::BAD_URL:
      return "BAD_URL";
    case Result::BAD_SIGNATURE:
      return "BAD_SIGNATURE";
  }
  return "?";
}

Result parse(const char* json, const size_t len, Manifest& out) {
  out = Manifest{};
  if (!json || len > MAX_MANIFEST_BYTES) return Result::TOO_LARGE;

  JsonDocument doc;
  if (deserializeJson(doc, json, len) != DeserializationError::Ok || !doc.is<JsonObjectConst>()) {
    return Result::BAD_JSON;
  }
  const JsonVariantConst schema = doc["schema"];
  if (!schema.is<int>() || schema.as<int>() != SCHEMA_VERSION) return Result::BAD_SCHEMA;

  if (!copyField(doc["channel"], out.channel, sizeof(out.channel), isFieldChar) ||
      !copyField(doc["target"], out.target, sizeof(out.target), isFieldChar) ||
      !copyField(doc["version"], out.version, sizeof(out.version), isFieldChar) ||
      !copyField(doc["url"], out.url, sizeof(out.url), isUrlChar)) {
    return Result::BAD_FIELD;
  }
  const JsonVariantConst build = doc["build"];
  const JsonVariantConst size = doc["size"];
  if (!build.is<uint32_t>() || !size.is<uint32_t>()) return Result::BAD_FIELD;
  out.build = build.as<uint32_t>();
  out.size = size.as<uint32_t>();

  const char* sha = doc["sha256"].is<const char*>() ? doc["sha256"].as<const char*>() : nullptr;
  const char* sig = doc["sig"].is<const char*>() ? doc["sig"].as<const char*>() : nullptr;
  if (!lowerHex(sha, 64) || !lowerHex(sig, 128)) return Result::BAD_FIELD;
  decodeHex(sha, out.sha256, sizeof(out.sha256));
  decodeHex(sig, out.sig, sizeof(out.sig));
  return Result::OK;
}

bool urlUnderPrefix(const char* url, const char* prefix) {
  if (!url || !prefix || strncmp(prefix, "https://", 8) != 0) return false;
  const size_t prefixLen = strlen(prefix);
  if (prefixLen == 0 || prefix[prefixLen - 1] != '/' || strncmp(url, prefix, prefixLen) != 0) return false;
  const char* tag = url + prefixLen;
  const char* slash = strchr(tag, '/');
  if (!slash || !validSegment(tag, slash)) return false;
  const char* asset = slash + 1;
  return strchr(asset, '/') == nullptr && validSegment(asset, asset + strlen(asset)) &&
         strcmp(asset, FIRMWARE_ASSET) == 0;
}

Result checkPolicy(const Manifest& m, const Policy& policy) {
  if (strcmp(m.channel, policy.channel) != 0) return Result::WRONG_CHANNEL;
  if (strcmp(m.target, policy.target) != 0) return Result::WRONG_TARGET;
  const auto knownChannel = [](const char* c) {
    return c && (strcmp(c, "bookorbit-x3") == 0 || strcmp(c, "orbitink-x3-nightly") == 0);
  };
  const bool switching = knownChannel(policy.runningChannel) && knownChannel(policy.channel) &&
                         strcmp(policy.runningChannel, policy.channel) != 0;
  if (m.build == 0 || (!switching && m.build <= policy.runningBuild)) return Result::NOT_NEWER;
  if (m.size < policy.minSize || (policy.maxSize > 0 && m.size > policy.maxSize)) return Result::BAD_SIZE;
  if (!urlUnderPrefix(m.url, policy.urlPrefix)) return Result::BAD_URL;
  return Result::OK;
}

size_t canonicalPayload(const Manifest& m, char* out, const size_t cap) {
  char shaHex[65];
  firmware_upload::shaToHex(m.sha256, shaHex);
  const int n = snprintf(
      out, cap, "crossfade-ota-v1\nchannel=%s\ntarget=%s\nversion=%s\nbuild=%lu\nsize=%lu\nsha256=%s\nurl=%s\n",
      m.channel, m.target, m.version, static_cast<unsigned long>(m.build), static_cast<unsigned long>(m.size), shaHex,
      m.url);
  if (n <= 0 || static_cast<size_t>(n) >= cap) return 0;
  return static_cast<size_t>(n);
}

}  // namespace ota_manifest
