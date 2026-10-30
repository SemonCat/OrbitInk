#pragma once

#include <cstdint>

#include "ProgressTimestamp.h"

// Shared temporal ordering for both automatic providers and BookOrbit's manual "sync now".
// Percentage is deliberately absent: it describes position, never freshness. Each existing
// candidate must carry a valid origin timestamp before an automatic overwrite is safe.
namespace ProgressPullPolicy {

enum class RemoteState { Found, Missing, Unavailable };
enum class SyncAction {
  PullRemote,
  PushLocal,
  AlreadySynced,
  NoProgress,
  HoldDeferred,
  HoldUnknown,
  Abort
};

struct DecisionInput {
  bool hasLocal = false;
  uint64_t localTimestamp = 0;
  RemoteState remoteState = RemoteState::Unavailable;
  uint64_t remoteTimestamp = 0;
  bool localSameAsRemote = false;
  bool hasDeferredRemote = false;
  uint64_t deferredTimestamp = 0;
  bool localSameAsDeferred = false;
  bool remoteSameAsDeferred = false;
};

// Old 4/6-byte local records have no event time. The first valid server record establishes the
// baseline; without one, retain the local record rather than manufacturing chronology or pushing.
inline SyncAction decideLegacy(RemoteState remoteState, bool hasDeferredRemote = false) {
  if (remoteState == RemoteState::Unavailable) return SyncAction::Abort;
  if (remoteState == RemoteState::Found) return SyncAction::PullRemote;
  return hasDeferredRemote ? SyncAction::HoldDeferred : SyncAction::HoldUnknown;
}

inline SyncAction decide(const DecisionInput& input) {
  if (input.remoteState == RemoteState::Unavailable) return SyncAction::Abort;
  if (input.remoteState == RemoteState::Found && !ProgressTimestamp::valid(input.remoteTimestamp)) {
    return SyncAction::Abort;
  }
  if (input.hasDeferredRemote && !ProgressTimestamp::valid(input.deferredTimestamp)) {
    return SyncAction::HoldUnknown;
  }
  if (input.hasLocal && !ProgressTimestamp::valid(input.localTimestamp)) return SyncAction::HoldUnknown;

  enum class Winner { None, Local, Deferred };
  Winner winner = input.hasLocal ? Winner::Local : Winner::None;
  uint64_t winnerTimestamp = input.localTimestamp;

  if (input.hasDeferredRemote) {
    if (winner == Winner::None || input.deferredTimestamp > winnerTimestamp ||
        (input.deferredTimestamp == winnerTimestamp && !input.localSameAsDeferred)) {
      winner = Winner::Deferred;
      winnerTimestamp = input.deferredTimestamp;
    }
  }

  if (input.remoteState == RemoteState::Missing) {
    if (winner == Winner::Deferred) return SyncAction::HoldDeferred;
    if (winner == Winner::Local) return SyncAction::PushLocal;
    return SyncAction::NoProgress;
  }

  if (winner == Winner::None || input.remoteTimestamp > winnerTimestamp) {
    return SyncAction::PullRemote;
  }
  if (input.remoteTimestamp < winnerTimestamp) {
    return winner == Winner::Deferred ? SyncAction::HoldDeferred : SyncAction::PushLocal;
  }

  const bool samePosition = winner == Winner::Deferred ? input.remoteSameAsDeferred : input.localSameAsRemote;
  if (!samePosition) return SyncAction::PullRemote;  // equal-second server response is canonical
  return winner == Winner::Deferred ? SyncAction::HoldDeferred : SyncAction::AlreadySynced;
}

}  // namespace ProgressPullPolicy
