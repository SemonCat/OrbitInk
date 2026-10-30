#pragma once
#include "DeferredProgress.h"
struct HostState {
  DeferredProgress deferredProgress;
  bool quickResumeSkipRenderEligible = true;
  std::string bookOrbitResetPath, bookOrbitResetDocument;
  uint64_t bookOrbitResetTimestamp = 0;
  bool bookOrbitResetAcknowledged = false;
  bool saveWorks = true;
  bool saveToFile() { return saveWorks; }
};
inline HostState APP_STATE;
