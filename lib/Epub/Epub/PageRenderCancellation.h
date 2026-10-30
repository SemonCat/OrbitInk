#pragma once

// The render task and input loop run independently. A newer navigation request
// can therefore supersede a page while its CJK glyph scan/render is still in
// progress; callers use this lightweight callback at safe boundaries.
struct PageRenderCancellation {
  bool (*isCancelled)(const void*) = nullptr;
  const void* context = nullptr;

  bool requested() const { return isCancelled != nullptr && isCancelled(context); }
};
