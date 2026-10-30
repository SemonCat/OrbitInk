// Host-only, opt-in subprocess transport. No shell, secrets in argv, or dependency edits.
// Without CROSSFADE_SIM_HTTP_BRIDGE this retains the simulator's offline behavior.
#include "SecureHttpClient.h"

#include <ArduinoJson.h>
#include <HalGPIO.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

extern char** environ;

namespace {
bool decodeBase64(const std::string& in, std::string& out) {
  out.clear();
  out.reserve(in.size() / 4 * 3);
  uint32_t acc = 0;
  int bits = 0;
  for (const char c : in) {
    int v;
    if (c >= 'A' && c <= 'Z')
      v = c - 'A';
    else if (c >= 'a' && c <= 'z')
      v = c - 'a' + 26;
    else if (c >= '0' && c <= '9')
      v = c - '0' + 52;
    else if (c == '+')
      v = 62;
    else if (c == '/')
      v = 63;
    else if (c == '=')
      break;
    else
      return false;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((acc >> bits) & 0xFF));
    }
  }
  return true;
}
}  // namespace

namespace freeink {
int SecureHttpClient::sendRequest(const char* method, const uint8_t* payload, size_t size,
                                  const DataCallback& onData, const AbortCallback& shouldAbort) {
  _body.clear();
  _responseHeaders.clear();
  _status = 0;
  _complete = false;
  _aborted = false;
  const char* bridge = std::getenv("CROSSFADE_SIM_HTTP_BRIDGE");
  if (!bridge || bridge[0] != '/' || size > 1024 * 1024) return kTransportFailure;
  const auto abortRequested = [&]() {
    // Device InputManager::update clears edges before polling. The pinned host
    // HAL instead latches them until beginFrame: otherwise the Confirm that
    // launched manual sync would cancel its first request on the host only.
    gpio.beginFrame();
    return shouldAbort && shouldAbort();
  };
  if (abortRequested()) {
    _aborted = true;
    return kTransportFailure;
  }

  using File = std::unique_ptr<FILE, decltype(&std::fclose)>;
  File request(std::tmpfile(), std::fclose), response(std::tmpfile(), std::fclose);
  if (!request || !response) return kTransportFailure;
  JsonDocument doc;
  doc["url"] = _url;
  doc["method"] = method;
  doc["body"] = payload ? std::string(reinterpret_cast<const char*>(payload), size) : std::string();
  for (const auto& header : _headers) doc["headers"][header.first] = header.second;
  std::string serialized;
  serializeJson(doc, serialized);
  if (std::fwrite(serialized.data(), 1, serialized.size(), request.get()) != serialized.size()) return kTransportFailure;
  std::rewind(request.get());

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, fileno(request.get()), STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, fileno(response.get()), STDOUT_FILENO);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  char* args[] = {const_cast<char*>(bridge), nullptr};
  pid_t child = 0;
  const int spawned = posix_spawn(&child, bridge, &actions, nullptr, args, environ);
  posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0) return kTransportFailure;
  int status = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (waitpid(child, &status, WNOHANG) == 0) {
    if (abortRequested() || std::chrono::steady_clock::now() >= deadline) {
      kill(child, SIGKILL);
      waitpid(child, &status, 0);
      _aborted = true;
      return kTransportFailure;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return kTransportFailure;
  std::rewind(response.get());
  serialized.clear();
  char buffer[4096];
  size_t count = 0;
  while ((count = std::fread(buffer, 1, sizeof(buffer), response.get())) != 0) {
    if (serialized.size() + count > 8 * 1024 * 1024) return kTransportFailure;
    serialized.append(buffer, count);
  }
  doc.clear();
  if (deserializeJson(doc, serialized) || !doc["status"].is<int>()) return kTransportFailure;
  _status = doc["status"].as<int>();
  if (doc["body_b64"].is<std::string>()) {
    // Binary bodies (EPUB downloads from the fixture bridge) cannot travel as a JSON string.
    if (!decodeBase64(doc["body_b64"].as<std::string>(), _body)) return kTransportFailure;
  } else if (doc["body"].is<std::string>()) {
    _body = doc["body"].as<std::string>();
  } else {
    return kTransportFailure;
  }
  for (JsonPair pair : doc["headers"].as<JsonObject>()) {
    _responseHeaders.emplace_back(pair.key().c_str(), pair.value().as<std::string>());
  }
  if (onData && !onData(reinterpret_cast<const uint8_t*>(_body.data()), _body.size())) {
    _aborted = true;
    return kTransportFailure;
  }
  _complete = _status > 0;
  return _status;
}

std::string SecureHttpClient::getHeader(const std::string& name) const {
  for (const auto& header : _responseHeaders) {
    if (strcasecmp(header.first.c_str(), name.c_str()) == 0) return header.second;
  }
  return {};
}
}  // namespace freeink
