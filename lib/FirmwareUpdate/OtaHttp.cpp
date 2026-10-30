#include "OtaHttp.h"

#include <strings.h>

#include <cstring>

namespace ota_http {

namespace {
constexpr const char* ALLOWED_HOSTS[] = {"github.com", "objects.githubusercontent.com",
                                         "release-assets.githubusercontent.com"};

bool nameIs(const char* name, const size_t len, const char* want) {
  return strlen(want) == len && strncasecmp(name, want, len) == 0;
}

// Printable ASCII without space: safe to place verbatim in a request line.
bool isPathChar(const char c) { return c > 0x20 && c < 0x7F; }

bool isHostChar(const char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
}

bool copyPath(const char* src, Url& out) {
  const size_t n = strlen(src);
  if (n == 0 || n > PATH_MAX_LEN || src[0] != '/') return false;
  for (size_t i = 0; i < n; i++) {
    if (!isPathChar(src[i])) return false;
  }
  memcpy(out.path, src, n + 1);
  return true;
}
}  // namespace

size_t ResponseHeadParser::feed(const uint8_t* data, const size_t len) {
  size_t i = 0;
  while (i < len && st == Status::NEED_MORE) {
    const char c = static_cast<char>(data[i++]);
    if (++headBytes > MAX_HEAD_BYTES) {
      st = Status::ERROR;
      break;
    }
    if (c == '\n') {
      if (lineLen > 0 && line[lineLen - 1] == '\r' && !lineOverflow) lineLen--;
      line[lineLen] = '\0';
      finishLine();
      lineLen = 0;
      lineOverflow = false;
      continue;
    }
    if (lineLen < LINE_KEEP) {
      line[lineLen++] = c;
    } else {
      lineOverflow = true;  // keep the prefix (header name), drop the rest
    }
  }
  return i;
}

void ResponseHeadParser::finishLine() {
  if (++lines > MAX_HEADER_LINES) {
    st = Status::ERROR;
    return;
  }
  if (!sawStatusLine) {
    // "HTTP/1.x NNN[ reason]"
    if (lineOverflow || lineLen < 12 || strncmp(line, "HTTP/1.", 7) != 0 || line[8] != ' ') {
      st = Status::ERROR;
      return;
    }
    int value = 0;
    for (size_t k = 9; k < 12; k++) {
      if (line[k] < '0' || line[k] > '9') {
        st = Status::ERROR;
        return;
      }
      value = value * 10 + (line[k] - '0');
    }
    if (lineLen > 12 && line[12] != ' ') {
      st = Status::ERROR;
      return;
    }
    code = value;
    sawStatusLine = true;
    return;
  }
  if (lineLen == 0 && !lineOverflow) {
    st = Status::DONE;
    return;
  }
  if (line[0] == ' ' || line[0] == '\t') {
    st = Status::ERROR;  // obsolete line folding: ambiguous, never sent by the release hosts
    return;
  }
  const char* colon = static_cast<const char*>(memchr(line, ':', lineLen));
  if (!colon) {
    if (!lineOverflow) st = Status::ERROR;
    return;  // an overflowed line whose name alone exceeds LINE_KEEP is not one we keep
  }
  const size_t nameLen = static_cast<size_t>(colon - line);
  const bool kept = nameIs(line, nameLen, "location") || nameIs(line, nameLen, "content-length") ||
                    nameIs(line, nameLen, "transfer-encoding");
  if (!kept) return;
  if (lineOverflow) {
    // A truncated value of a header we act on cannot be trusted.
    st = Status::ERROR;
    return;
  }
  const char* value = colon + 1;
  const char* end = line + lineLen;
  while (value < end && (*value == ' ' || *value == '\t')) value++;
  while (end > value && (end[-1] == ' ' || end[-1] == '\t')) end--;
  handleHeader(line, nameLen, value, static_cast<size_t>(end - value));
}

void ResponseHeadParser::handleHeader(const char* name, const size_t nameLen, const char* value,
                                      const size_t valueLen) {
  if (nameIs(name, nameLen, "transfer-encoding")) {
    transferEncoding = true;
    return;
  }
  if (nameIs(name, nameLen, "content-length")) {
    if (valueLen == 0 || valueLen > 12) {
      st = Status::ERROR;
      return;
    }
    uint64_t v = 0;
    for (size_t k = 0; k < valueLen; k++) {
      if (value[k] < '0' || value[k] > '9') {
        st = Status::ERROR;
        return;
      }
      v = v * 10 + static_cast<uint64_t>(value[k] - '0');
    }
    if (haveLength && v != length) {
      st = Status::ERROR;
      return;
    }
    haveLength = true;
    length = v;
    return;
  }
  // location
  if (haveLocation || valueLen == 0 || valueLen > LOCATION_MAX) {
    st = Status::ERROR;
    return;
  }
  memcpy(loc, value, valueLen);
  loc[valueLen] = '\0';
  haveLocation = true;
}

bool parseHttpsUrl(const char* url, Url& out) {
  out = Url{};
  if (!url || strncmp(url, "https://", 8) != 0) return false;
  const char* authority = url + 8;
  const char* slash = strchr(authority, '/');
  const char* authEnd = slash ? slash : authority + strlen(authority);
  const char* colon = static_cast<const char*>(memchr(authority, ':', static_cast<size_t>(authEnd - authority)));
  const char* hostEnd = colon ? colon : authEnd;
  const size_t hostLen = static_cast<size_t>(hostEnd - authority);
  if (hostLen == 0 || hostLen > HOST_MAX) return false;
  for (const char* p = authority; p < hostEnd; p++) {
    if (!isHostChar(*p)) return false;  // also rejects '@' userinfo
  }
  memcpy(out.host, authority, hostLen);
  out.host[hostLen] = '\0';
  if (colon) {
    const char* p = colon + 1;
    if (p == authEnd || authEnd - p > 5) return false;
    uint32_t port = 0;
    for (; p < authEnd; p++) {
      if (*p < '0' || *p > '9') return false;
      port = port * 10 + static_cast<uint32_t>(*p - '0');
    }
    if (port == 0 || port > 65535) return false;
    out.port = static_cast<uint16_t>(port);
  }
  return copyPath(slash ? slash : "/", out);
}

bool resolveRedirect(const Url& current, const char* location, Url& out) {
  if (!location) return false;
  if (strncmp(location, "https://", 8) == 0) return parseHttpsUrl(location, out);
  if (location[0] != '/' || location[1] == '/') return false;
  out = Url{};
  memcpy(out.host, current.host, sizeof(out.host));
  out.port = current.port;
  return copyPath(location, out);
}

bool isAllowedHost(const char* host) {
  if (!host) return false;
  for (const char* allowed : ALLOWED_HOSTS) {
    if (strcasecmp(host, allowed) == 0) return true;
  }
  return false;
}

}  // namespace ota_http
