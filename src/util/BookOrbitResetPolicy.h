#pragma once

namespace BookOrbitResetPolicy {

struct Decision {
  bool standingReset = false;
  bool acknowledgeStart = false;
  bool pushAdvance = false;
  bool holdUnknown = false;
};

// Once a web reset has been durably applied, BookOrbit keeps returning a synthetic start with a
// fresh timestamp until this device sends start once and later sends a non-start position. Treat
// that repeated GET as protocol state rather than as a newer reading event.
constexpr Decision decide(bool remoteIsWebReset, bool markerMatches, bool acknowledged, bool hasLocal,
                          bool localIsStart, bool localTimestampValid) {
  if (!remoteIsWebReset || !markerMatches) return {};
  Decision result;
  result.standingReset = true;
  result.acknowledgeStart = !acknowledged;
  if (!hasLocal || (!localIsStart && !localTimestampValid)) {
    result.holdUnknown = true;
  } else if (!localIsStart) {
    result.pushAdvance = true;
  }
  return result;
}

}  // namespace BookOrbitResetPolicy
