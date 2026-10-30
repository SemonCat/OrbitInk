#pragma once

enum class NetworkMode { JOIN_NETWORK, CONNECT_CALIBRE, CREATE_HOTSPOT };

/**
 * Which network modes NetworkModeSelectionActivity lists, in order. The file-transfer entry shows all
 * three; BookOrbit web setup omits Calibre, which cannot configure BookOrbit. Pure, so the order and
 * the index -> mode mapping are host-tested (test/bookorbit_setup).
 */
namespace NetworkModeList {

constexpr int count(const bool includeCalibre) { return includeCalibre ? 3 : 2; }

constexpr NetworkMode at(const bool includeCalibre, const int index) {
  if (includeCalibre) {
    return index == 1 ? NetworkMode::CONNECT_CALIBRE
                      : (index == 2 ? NetworkMode::CREATE_HOTSPOT : NetworkMode::JOIN_NETWORK);
  }
  return index == 1 ? NetworkMode::CREATE_HOTSPOT : NetworkMode::JOIN_NETWORK;
}

}  // namespace NetworkModeList
