#include "HttpRedirectPolicy.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace HttpRedirectPolicy {

namespace {

// Parses "scheme://host[:port]/path" into a lowercase scheme, lowercase host, and port (defaulted
// per scheme -- 443 for https, 80 for http -- when absent from the authority). Deliberately
// independent of UrlUtils::extractHost(): that helper preserves case and never resolves a default
// port, both of which matter for an origin comparison but not for its own callers (building/
// joining URLs), and duplicating this dozen lines here is cheaper than teaching UrlUtils a second,
// stricter contract only this file needs.
bool parseOrigin(const std::string& url, std::string& scheme, std::string& host, uint16_t& port) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return false;
  scheme = url.substr(0, schemeEnd);
  std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                 [](unsigned char c) { return static_cast<char>(tolower(c)); });
  if (scheme != "http" && scheme != "https") return false;

  const size_t hostStart = schemeEnd + 3;
  const size_t pathStart = url.find('/', hostStart);
  const std::string hostPort =
      pathStart == std::string::npos ? url.substr(hostStart) : url.substr(hostStart, pathStart - hostStart);

  const size_t portSep = hostPort.rfind(':');
  if (portSep != std::string::npos) {
    host = hostPort.substr(0, portSep);
    port = static_cast<uint16_t>(atoi(hostPort.substr(portSep + 1).c_str()));
  } else {
    host = hostPort;
    port = scheme == "https" ? 443 : 80;
  }
  std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
  return !host.empty();
}

}  // namespace

bool shouldForwardAuthOnRedirect(const std::string& fromUrl, const std::string& toUrl) {
  std::string fromScheme;
  std::string fromHost;
  uint16_t fromPort = 0;
  std::string toScheme;
  std::string toHost;
  uint16_t toPort = 0;
  if (!parseOrigin(fromUrl, fromScheme, fromHost, fromPort) || !parseOrigin(toUrl, toScheme, toHost, toPort)) {
    return false;  // can't prove same-origin; fail closed
  }
  return fromScheme == toScheme && fromHost == toHost && fromPort == toPort;
}

bool isRedirectDowngrade(const std::string& fromUrl, const std::string& toUrl) {
  std::string fromScheme;
  std::string fromHost;
  uint16_t fromPort = 0;
  std::string toScheme;
  std::string toHost;
  uint16_t toPort = 0;
  if (!parseOrigin(fromUrl, fromScheme, fromHost, fromPort) || !parseOrigin(toUrl, toScheme, toHost, toPort)) {
    return false;  // unparseable is HttpDownloader's own "bad redirect" error, not a downgrade
  }
  return fromScheme == "https" && toScheme == "http";
}

}  // namespace HttpRedirectPolicy
