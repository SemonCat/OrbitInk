#pragma once

#include <Epub.h>
#include <Epub/ReaderRenderSpec.h>
#include <GfxRenderer.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

class Section {
 public:
  Section(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer&) : epub_(epub), spineIndex_(spineIndex) {}

  std::optional<uint16_t> getCachedPageCount() const { return epub_->cachedPageCount(spineIndex_); }
  bool loadSectionFile(const ReaderRenderSpec&) {
    const auto count = epub_->cachedPageCount(spineIndex_);
    if (!count) return false;
    pageCount = *count;
    return true;
  }
  bool createSectionFile(const ReaderRenderSpec&) {
    epub_->recordLayoutBuildAttempt();
    const auto count = epub_->boundedLayoutPageCount(spineIndex_);
    if (!count) return false;
    pageCount = *count;
    epub_->finalizeLayout(spineIndex_, pageCount);
    return true;
  }
  uint16_t estimatedTotalPages() const { return pageCount; }
  bool isPartial() const { return false; }
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t paragraph) const {
    return epub_->pageForParagraph(spineIndex_, paragraph);
  }
  std::optional<uint16_t> getPageForListItemIndex(uint16_t listItem) const {
    return epub_->pageForListItem(spineIndex_, listItem);
  }
  std::optional<uint16_t> getPageForAnchor(const std::string&) const { return std::nullopt; }

  uint16_t pageCount = 0;

 private:
  std::shared_ptr<Epub> epub_;
  int spineIndex_;
};
