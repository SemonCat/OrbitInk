#pragma once

#include <ProgressMapper.h>

class GfxRenderer;

namespace ProgressSyncPosition {
// The cache is shared by both providers and survives headless resets. It is disposable:
// an identity/layout/position mismatch or any I/O failure falls back to normal mapping.
SavedProgressPosition resolve(const std::shared_ptr<Epub>& epub, CrossPointPosition position,
                               const std::string& binaryDocument, GfxRenderer* renderer);
}
