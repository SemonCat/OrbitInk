#pragma once

#include <string>

#include "DeferredProgress.h"

class GfxRenderer;

namespace ProgressSyncDeferred {

// Persist a validated raw remote position before any layout-dependent mapping. Returns true when
// the candidate is durable (or an equal/newer candidate for the same book is already durable).
bool stage(const DeferredProgress& candidate);

// Return the durable remote candidate for this book, if any. Callers feed its origin timestamp
// and position identity into ProgressPullPolicy; percentage is never used as freshness.
bool pendingForBook(const std::string& path, DeferredProgress& out);

// Map and atomically save the staged remote with initialized fonts/layout. Returns true only when
// progress.bin changed. Invalid provider/book/hash identity is discarded; transient local layout
// failures keep the handoff for a later normal boot.
bool applyForBook(const std::string& path, GfxRenderer& renderer);

}  // namespace ProgressSyncDeferred
