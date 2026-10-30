#pragma once

#include <ProgressMapper.h>

namespace ProgressSyncPosition {
// A persisted chapter start may have no pagination yet. Other positions need a
// positive denominator before they can take part in provider timestamp ordering.
std::optional<float> chapterProgress(const CrossPointPosition& position, int spineCount);

// The cache is shared by both providers and survives headless resets. It is disposable:
// an identity/layout/position mismatch or any I/O failure falls back to normal mapping.
SavedProgressPosition resolve(const std::shared_ptr<Epub>& epub, CrossPointPosition position,
                               const std::string& binaryDocument);
}
