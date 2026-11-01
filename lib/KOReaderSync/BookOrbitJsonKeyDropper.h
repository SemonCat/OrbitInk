#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

/**
 * Streams a JSON response body into a bounded string while replacing the VALUE of every member
 * whose key is in `dropKeys` (at any depth) with `null`, before any of it is retained.
 *
 * BookOrbit's catalog responses carry fields this device never shows but that can dwarf the ones
 * it needs: a book detail's `description` (often several KB) is emitted BEFORE `files`, so a plain
 * byte cap cut the body ahead of the download list and the detail failed to parse; facet pages
 * repeat a long percent-encoded `booksHref` on each of their 60 entries. Dropping those values as
 * bytes arrive keeps the existing caps meaningful for what is kept, without buffering the rest.
 *
 * A body that still exceeds the cap can be salvaged at lastTopLevelElementEnd() instead of failing
 * as a whole. Works on arbitrary chunk boundaries (strings, escapes and nested values may split anywhere). It
 * is a filter, not a validator: malformed input passes through (or is cut by the cap) and is then
 * rejected by the normal JSON parse. Keys are compared on their raw bytes, so an escaped spelling
 * of a dropped key is simply kept. Nesting deeper than MAX_DEPTH stops key recognition and passes
 * the remaining bytes through unchanged.
 */
class BookOrbitJsonKeyDropper {
 public:
  BookOrbitJsonKeyDropper(const char* const* dropKeys, size_t dropKeyCount, std::string& out, size_t cap)
      : dropKeys_(dropKeys), dropKeyCount_(dropKeyCount), out_(out), cap_(cap) {}

  void feed(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) consume(static_cast<char>(data[i]));
  }

  // Consume top-level items individually, leaving an empty items array in the envelope.
  // A long page therefore retains one bounded item at a time, without skipping server offsets.
  void setItemSink(std::function<void(const char*, size_t)> sink) { itemSink_ = std::move(sink); }

  // True once a retained byte had to be discarded because `cap` was reached.
  bool truncated() const { return truncated_; }

  // Retained length just past the last fully retained element of an array that is a member of the
  // top-level object (a page's `items`), or 0 when none closed. A capped body cut there and closed
  // with "]}" parses and keeps every complete entry received.
  size_t lastTopLevelElementEnd() const { return lastElementEnd_; }

 private:
  static constexpr int MAX_DEPTH = 32;
  static constexpr size_t MAX_KEY = 32;

  enum class Mode : uint8_t { Emit, AfterDroppedKey, SkipValueStart, SkipString, SkipNested, SkipPrimitive };

  void emit(char c) {
    if (out_.size() < cap_) {
      out_.push_back(c);
    } else {
      truncated_ = true;
    }
  }

  bool topIsObject() const { return depth_ > 0 && depth_ <= MAX_DEPTH && (objectBits_ >> (depth_ - 1) & 1u); }

  bool keyIsDropped() const {
    if (keyOverflow_) return false;
    for (size_t k = 0; k < dropKeyCount_; k++) {
      if (strlen(dropKeys_[k]) == keyLen_ && memcmp(dropKeys_[k], key_, keyLen_) == 0) return true;
    }
    return false;
  }

  void push(bool object) {
    if (depth_ < MAX_DEPTH) {
      if (object) {
        objectBits_ |= 1u << depth_;
      } else {
        objectBits_ &= ~(1u << depth_);
      }
    }
    depth_++;
  }

  void consume(char c) {
    switch (mode_) {
      case Mode::Emit:
        emitMode(c);
        return;
      case Mode::AfterDroppedKey:
        emit(c);
        if (c == ':') {
          emit('n');
          emit('u');
          emit('l');
          emit('l');
          mode_ = Mode::SkipValueStart;
        }
        return;
      case Mode::SkipValueStart:
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return;
        skipEscape_ = false;
        if (c == '"') {
          mode_ = Mode::SkipString;
        } else if (c == '{' || c == '[') {
          skipDepth_ = 1;
          skipInString_ = false;
          mode_ = Mode::SkipNested;
        } else {
          mode_ = Mode::SkipPrimitive;
        }
        return;
      case Mode::SkipString:
        if (skipEscape_) {
          skipEscape_ = false;
        } else if (c == '\\') {
          skipEscape_ = true;
        } else if (c == '"') {
          mode_ = Mode::Emit;
        }
        return;
      case Mode::SkipNested:
        if (skipInString_) {
          if (skipEscape_) {
            skipEscape_ = false;
          } else if (c == '\\') {
            skipEscape_ = true;
          } else if (c == '"') {
            skipInString_ = false;
          }
        } else if (c == '"') {
          skipInString_ = true;
        } else if (c == '{' || c == '[') {
          skipDepth_++;
        } else if ((c == '}' || c == ']') && --skipDepth_ == 0) {
          mode_ = Mode::Emit;
        }
        return;
      case Mode::SkipPrimitive:
        if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
          mode_ = Mode::Emit;
          emitMode(c);  // the terminator belongs to the enclosing structure
        }
        return;
    }
  }

  void emitMode(char c) {
    if (itemSink_ && itemsArray_ && depth_ == 2 && !inString_ && c == ',') return;
    if (itemSink_ && itemsArray_ && depth_ == 2 && !inString_ && c == '{') itemStart_ = out_.size();
    emit(c);
    if (inString_) {
      if (escape_) {
        escape_ = false;
      } else if (c == '\\') {
        escape_ = true;
      } else if (c == '"') {
        inString_ = false;
        if (stringIsKey_ && keyIsDropped()) mode_ = Mode::AfterDroppedKey;
      } else if (stringIsKey_) {
        if (keyLen_ < MAX_KEY) {
          key_[keyLen_++] = c;
        } else {
          keyOverflow_ = true;
        }
      }
      return;
    }
    switch (c) {
      case '"':
        inString_ = true;
        escape_ = false;
        stringIsKey_ = expectKey_ && topIsObject();
        expectKey_ = false;
        keyLen_ = 0;
        keyOverflow_ = false;
        break;
      case '{':
        push(true);
        expectKey_ = true;
        break;
      case '[':
        if (depth_ == 1 && keyLen_ == 5 && memcmp(key_, "items", 5) == 0) itemsArray_ = true;
        push(false);
        expectKey_ = false;
        break;
      case '}':
      case ']':
        if (depth_ == 3 && !truncated_) lastElementEnd_ = out_.size();
        if (itemSink_ && itemsArray_ && depth_ == 3 && c == '}' && !truncated_) {
          itemSink_(out_.data() + itemStart_, out_.size() - itemStart_);
          out_.resize(itemStart_);
        }
        if (depth_ == 2 && c == ']') itemsArray_ = false;
        if (depth_ > 0) depth_--;
        expectKey_ = false;
        break;
      case ',':
        expectKey_ = topIsObject();
        break;
      case ':':
        expectKey_ = false;
        break;
      default:
        break;
    }
  }

  const char* const* dropKeys_;
  size_t dropKeyCount_;
  std::string& out_;
  size_t cap_;
  bool truncated_ = false;
  size_t lastElementEnd_ = 0;
  std::function<void(const char*, size_t)> itemSink_;
  bool itemsArray_ = false;
  size_t itemStart_ = 0;

  Mode mode_ = Mode::Emit;
  int depth_ = 0;
  uint32_t objectBits_ = 0;
  bool inString_ = false;
  bool escape_ = false;
  bool stringIsKey_ = false;
  bool expectKey_ = false;
  char key_[MAX_KEY] = {};
  size_t keyLen_ = 0;
  bool keyOverflow_ = false;

  int skipDepth_ = 0;
  bool skipInString_ = false;
  bool skipEscape_ = false;
};
