#pragma once

#include <Print.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class Epub {
 public:
  inline static std::optional<uint16_t> defaultCachedPages;
  inline static bool compatibleLayout = true;
  inline static bool partialLayout = false;
  Epub(const std::string& path, const std::string&) : Epub(std::vector<std::string>(2, "<html><body><p>one</p><p>two</p></body></html>"),
                                                         std::vector<size_t>(2, 100)) { path_ = path; }
  void setupCacheDir() {}
  std::string getPath() const { return path_; }
  std::string getCachePath() const { return "/cache"; }
  struct SpineItem {
    std::string href;
  };

  Epub(std::vector<std::string> chapters, std::vector<size_t> byteWeights)
      : chapters_(std::move(chapters)),
        byteWeights_(std::move(byteWeights)),
        cachePages_(chapters_.size()),
        boundedPages_(chapters_.size()) {
    cumulative_.reserve(chapters_.size());
    size_t total = 0;
    for (size_t i = 0; i < chapters_.size(); ++i) {
      total += i < byteWeights_.size() ? byteWeights_[i] : chapters_[i].size();
      cumulative_.push_back(total);
    }
  }

  int getSpineItemsCount() const { return static_cast<int>(chapters_.size()); }
  bool load(bool, bool skipLoadingCss) {
    if (!skipLoadingCss) normalCssLoadCalls_++;
    return true;
  }
  size_t getBookSize() const { return cumulative_.empty() ? 0 : cumulative_.back(); }
  size_t getCumulativeSpineItemSize(int spineIndex) const { return cumulative_.at(spineIndex); }
  SpineItem getSpineItem(int spineIndex) const { return {"chapter" + std::to_string(spineIndex) + ".xhtml"}; }

  bool getItemSize(const std::string& href, size_t* size) const {
    const int index = indexForHref(href);
    if (index < 0 || !size) return false;
    *size = chapters_[index].size();
    return true;
  }

  bool readItemContentsToStream(const std::string& href, Print& out, size_t, bool allowEarlyStop = false) const {
    ++streamCalls_;
    const int index = indexForHref(href);
    if (index < 0) return false;
    const std::string& chapter = chapters_[index];
    return out.write(reinterpret_cast<const uint8_t*>(chapter.data()), chapter.size()) == chapter.size() || allowEarlyStop;
  }
  bool readSpineContentsToStream(int spineIndex, Print& out, size_t chunkSize, bool allowEarlyStop = false) const {
    return readItemContentsToStream(getSpineItem(spineIndex).href, out, chunkSize, allowEarlyStop);
  }

  float calculateProgress(int spineIndex, float intra) const {
    if (getBookSize() == 0 || spineIndex < 0 || spineIndex >= getSpineItemsCount()) return 0.0f;
    intra = std::max(0.0f, std::min(1.0f, intra));
    const size_t previous = spineIndex > 0 ? cumulative_[spineIndex - 1] : 0;
    const size_t chapterSize = cumulative_[spineIndex] - previous;
    return (static_cast<float>(previous) + intra * static_cast<float>(chapterSize)) /
           static_cast<float>(getBookSize());
  }

  void setCachedPageCount(int spineIndex, std::optional<uint16_t> count) { cachePages_.at(spineIndex) = count; }
  std::optional<uint16_t> cachedPageCount(int spineIndex) const {
    return cachePages_.at(spineIndex).has_value() ? cachePages_.at(spineIndex) : defaultCachedPages;
  }

  void setBoundedLayoutPageCount(int spineIndex, std::optional<uint16_t> count) {
    boundedPages_.at(spineIndex) = count;
  }
  std::optional<uint16_t> boundedLayoutPageCount(int spineIndex) const { return boundedPages_.at(spineIndex); }
  void finalizeLayout(int spineIndex, uint16_t pages) {
    cachePages_.at(spineIndex) = pages;
    finalizedLayoutBuilds_++;
  }
  void recordLayoutBuildAttempt() { layoutBuildAttempts_++; }
  int layoutBuildAttempts() const { return layoutBuildAttempts_; }
  int finalizedLayoutBuilds() const { return finalizedLayoutBuilds_; }
  int normalCssLoadCalls() const { return normalCssLoadCalls_; }
  int streamCalls() const { return streamCalls_; }

  std::optional<uint16_t> pageForParagraph(int spineIndex, uint16_t paragraph) const {
    const auto count = cachedPageCount(spineIndex);
    if (!count || *count == 0 || paragraph == 0) return std::nullopt;
    return std::min<uint16_t>(paragraph - 1, *count - 1);
  }

  std::optional<uint16_t> pageForListItem(int spineIndex, uint16_t listItem) const {
    return pageForParagraph(spineIndex, listItem);
  }

 private:
  std::string path_ = "/book.epub";
  int indexForHref(const std::string& href) const {
    static constexpr char prefix[] = "chapter";
    if (href.rfind(prefix, 0) != 0) return -1;
    const size_t suffix = href.find(".xhtml", sizeof(prefix) - 1);
    if (suffix == std::string::npos) return -1;
    int value = 0;
    for (size_t i = sizeof(prefix) - 1; i < suffix; ++i) {
      if (href[i] < '0' || href[i] > '9') return -1;
      value = value * 10 + (href[i] - '0');
    }
    return value >= 0 && value < getSpineItemsCount() ? value : -1;
  }

  std::vector<std::string> chapters_;
  std::vector<size_t> byteWeights_;
  std::vector<size_t> cumulative_;
  std::vector<std::optional<uint16_t>> cachePages_;
  std::vector<std::optional<uint16_t>> boundedPages_;
  int finalizedLayoutBuilds_ = 0;
  int layoutBuildAttempts_ = 0;
  int normalCssLoadCalls_ = 0;
  mutable int streamCalls_ = 0;
};
