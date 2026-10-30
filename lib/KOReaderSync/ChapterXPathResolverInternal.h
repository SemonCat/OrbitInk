#pragma once

// Internal implementation detail of ChapterXPathResolver.cpp, split into a header solely so
// host tests can drive these classes directly with raw XHTML byte strings (via their Print
// write() interface) without needing a real Epub/ZIP/HalStorage stack. Not part of
// ChapterXPathResolver's public API -- callers outside ChapterXPathResolver.cpp and its test
// should not use this header; use ChapterXPathResolver::findXPathForParagraph/
// findXPathForProgress instead.
//
// See ChapterXPathResolver.cpp's top-of-file comment for the full rationale (replacing an
// Expat-based implementation that could OOM immediately after a TLS handshake on the X3).

#include <Print.h>
#include <Utf8.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

#include "Epub/htmlEntities.h"

namespace koxpath_detail {

// ---- Bounded-memory constants --------------------------------------------------------------
// MAX_XPATH_DEPTH matches the existing INBOUND cap in ProgressMapper.cpp (parseXPathSteps),
// which already establishes 16 ancestor levels as this codebase's accepted "real XHTML never
// nests deeper than this" assumption for KOReader XPath handling.
constexpr int MAX_XPATH_DEPTH = 16;
// Longest real XHTML tag name this needs to hold exactly (e.g. "blockquote", "figcaption" are
// both 10 chars); truncation beyond this only affects the (nonexistent, in practice) tag names
// longer than this, and truncated names still compare/sort consistently with themselves.
constexpr int MAX_TAG_NAME_LEN = 15;
// Distinct child tag names tracked per ancestor level for sibling-index purposes (e.g. telling
// the 3rd <div> apart from interleaved <p>/<div> siblings). Real EPUB markup under one parent
// rarely uses more than a handful of distinct element names; this is a generous cap with a
// documented, deterministic (non-crashing) fallback below for the pathological case.
constexpr int MAX_SIBLING_NAMES_PER_LEVEL = 8;

inline std::string stripPrefix(const char* name) {
  if (!name) return "";
  const char* local = std::strrchr(name, ':');
  return local ? std::string(local + 1) : std::string(name);
}

inline size_t countUtf8Codepoints(const char* data, const size_t len) {
  if (!data || len == 0) return 0;
  size_t count = 0;
  const auto* ptr = reinterpret_cast<const unsigned char*>(data);
  const unsigned char* end = ptr + len;
  while (ptr < end) {
    utf8NextCodepoint(&ptr);
    count++;
  }
  return count;
}

// Per-ancestor-level sibling-index tracker: PER LEVEL, remembers how many of each distinct
// child tag name have opened so far, so (for example) the 3rd <div> among interleaved <div>/<p>
// siblings is correctly reported as div[3], matching XPath's per-name sibling indexing.
//
// Deterministic overflow fallback: once a level has seen MAX_SIBLING_NAMES_PER_LEVEL distinct
// child tag names, any FURTHER distinct name is treated as its own first occurrence (index 1)
// rather than crashing, growing unbounded, or aliasing onto a wrong existing slot. This can only
// under/over-count the sibling index for a tag name beyond the 8th distinct one seen at a single
// nesting level -- not observed in real EPUB markup, but bounded and safe if it ever occurs.
struct LevelSiblingState {
  struct Slot {
    char name[MAX_TAG_NAME_LEN + 1];
    int count;
  };
  Slot slots[MAX_SIBLING_NAMES_PER_LEVEL];
  int slotCount = 0;

  void reset() { slotCount = 0; }

  int nextIndex(const char* name) {
    for (int i = 0; i < slotCount; i++) {
      if (std::strcmp(slots[i].name, name) == 0) {
        return ++slots[i].count;
      }
    }
    if (slotCount < MAX_SIBLING_NAMES_PER_LEVEL) {
      std::strncpy(slots[slotCount].name, name, MAX_TAG_NAME_LEN);
      slots[slotCount].name[MAX_TAG_NAME_LEN] = '\0';
      slots[slotCount].count = 1;
      slotCount++;
      return 1;
    }
    return 1;  // deterministic overflow fallback -- see comment above
  }
};

struct AncestrySegment {
  char name[MAX_TAG_NAME_LEN + 1];
  int index;
};

inline std::string buildParagraphXPath(const int spineIndex, const AncestrySegment* path, const int pathCount,
                                       const int textNodeIndex, const size_t charOffset) {
  std::string xpath = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  for (int i = 0; i < pathCount; i++) {
    xpath += "/" + std::string(path[i].name) + "[" + std::to_string(path[i].index) + "]";
  }
  if (textNodeIndex > 0 && charOffset > 0) {
    xpath += "/text()[" + std::to_string(textNodeIndex) + "]." + std::to_string(charOffset);
  }
  return xpath;
}

// ---- Shared bounded tag/entity tokenizer ---------------------------------------------------
// Recognizes start/end/self-closing tags, skips comments/CDATA/processing-instructions/doctype,
// decodes entities, and tracks <body> gating -- everything the three resolvers below need in
// common. Subclasses react to already-<body>-gated open/close/codepoint events; none of the
// low-level byte-scanning state (tag name buffer, quote tracking, entity buffer, comment/CDATA
// skip state) is duplicated per-subclass.
class BoundedHtmlScanner : public Print {
 public:
  // Kept for call-site parity with the Expat classes this replaces (findXPathForParagraph/
  // findXPathForProgress call resolver.ok()/resolver.finish()) -- there is no external parser
  // object that can fail to construct, and no buffered end-of-stream flush is needed, so both
  // are trivially true.
  bool ok() const { return true; }
  bool finish() { return true; }

  size_t write(uint8_t c) override {
    if (stopped_) return 0;
    processByte(c);
    return 1;
  }

  size_t write(const uint8_t* buffer, size_t size) override {
    size_t consumed = 0;
    while (consumed < size && !stopped_) processByte(buffer[consumed++]);
    return consumed;
  }

 protected:
  virtual void onOpenTag(const char* name) {}
  virtual void onCloseTag(const char* name) {}
  virtual void onBodyOpen() {}
  virtual void onBodyClose() {}
  virtual void onVisibleCodepoint() {}

  void requestStop() { stopped_ = true; }
  bool insideBody() const { return insideBody_; }

 private:
  enum SkipKind { SKIP_COMMENT, SKIP_CDATA, SKIP_BANG_OR_PI };

  void handleOpenTagName(const char* rawName) {
    const std::string name = stripPrefix(rawName);
    if (!insideBody_) {
      if (name == "body") {
        insideBody_ = true;
        bodyDepth_ = htmlDepth_;
        onBodyOpen();
      }
      htmlDepth_++;
      return;
    }
    onOpenTag(name.c_str());
    htmlDepth_++;
  }

  void handleCloseTagName(const char* rawName) {
    const std::string name = stripPrefix(rawName);
    htmlDepth_--;
    if (!insideBody_) return;
    if (htmlDepth_ == bodyDepth_ && name == "body") {
      insideBody_ = false;
      onBodyClose();
      return;
    }
    onCloseTag(name.c_str());
  }

  void finishEntity() {
    entityBuffer_[entityLen_] = '\0';
    // Numeric entity: &#123; or &#x1F600; -- only the codepoint COUNT matters here (KOReader
    // text offsets count visible codepoints, never bytes/entities), so no decoding to UTF-8 is
    // needed, just validating it names a real character.
    if (entityLen_ >= 4 && entityBuffer_[1] == '#') {
      const bool isHex = entityBuffer_[2] == 'x' || entityBuffer_[2] == 'X';
      const size_t digitsStart = isHex ? 3 : 2;
      bool allDigits = digitsStart < entityLen_ - 1;
      for (size_t i = digitsStart; i < entityLen_ - 1 && allDigits; i++) {
        const char ch = entityBuffer_[i];
        allDigits = isHex ? std::isxdigit(static_cast<unsigned char>(ch)) != 0 : (ch >= '0' && ch <= '9');
      }
      if (allDigits) {
        onVisibleCodepoint();
        globalInEntity_ = false;
        entityLen_ = 0;
        return;
      }
    }
    const char* resolved = lookupHtmlEntity(entityBuffer_, entityLen_);
    if (resolved) {
      const size_t n = countUtf8Codepoints(resolved, std::strlen(resolved));
      for (size_t i = 0; i < n; i++) onVisibleCodepoint();
    } else {
      // Unknown entity: count its literal text as visible characters, matching this file's
      // prior (Expat-based) behavior of passing unresolved entities through as literal text.
      for (size_t i = 0; i < entityLen_; i++) onVisibleCodepoint();
    }
    globalInEntity_ = false;
    entityLen_ = 0;
  }

  void flushEntityAsLiteral() {
    for (size_t i = 0; i < entityLen_; i++) onVisibleCodepoint();
    globalInEntity_ = false;
    entityLen_ = 0;
  }

  // Comment/CDATA use a small rolling suffix match against their closing marker so embedded '>'
  // characters (e.g. "<!-- a > b -->") don't end the skip early. Generic "<!...>"/"<?...?>"
  // constructs (DOCTYPE, XML declaration) are skipped to the next unquoted '>' -- EPUB chapter
  // content never has these inside <body> with an embedded '>', so the simpler scan is safe
  // there, and content before <body> (where these DO appear) doesn't affect ancestry/counting.
  void processSkipByte(uint8_t c) {
    switch (skipKind_) {
      case SKIP_COMMENT:
        skipTail_ = ((skipTail_ << 8) | c) & 0xFFFFFF;
        if (skipTail_ == 0x2D2D3E) {  // "-->"
          inSkip_ = false;
        }
        break;
      case SKIP_CDATA:
        skipTail_ = ((skipTail_ << 8) | c) & 0xFFFFFF;
        if (skipTail_ == 0x5D5D3E) {  // "]]>"
          inSkip_ = false;
        }
        break;
      case SKIP_BANG_OR_PI:
        if (c == '>') inSkip_ = false;
        break;
    }
  }

  void processByteInTag(uint8_t c) {
    switch (tagState_) {
      case TAG_IDLE:
        if (c == '/') {
          tagIsClose_ = true;
          tagState_ = TAG_IN_NAME;
        } else if (c == '!') {
          // Look ahead for "--" (comment) or "[CDATA[" via a tiny local buffer; anything else
          // starting with '!' is a bang declaration (DOCTYPE, etc.), skipped to the next '>'.
          bangLen_ = 0;
          tagState_ = TAG_BANG_LOOKAHEAD;
        } else if (c == '?') {
          inSkip_ = true;
          skipKind_ = SKIP_BANG_OR_PI;
          tagState_ = TAG_SKIPPING;
        } else {
          tagIsClose_ = false;
          tagName_[0] = static_cast<char>(c);
          tagNameLen_ = 1;
          tagState_ = TAG_IN_NAME;
        }
        break;
      case TAG_BANG_LOOKAHEAD:
        if (bangLen_ < 7) bangLookahead_[bangLen_++] = static_cast<char>(c);
        if (bangLen_ == 2 && bangLookahead_[0] == '-' && bangLookahead_[1] == '-') {
          inSkip_ = true;
          skipKind_ = SKIP_COMMENT;
          skipTail_ = 0;
          tagState_ = TAG_SKIPPING;
        } else if (bangLen_ == 7 && std::memcmp(bangLookahead_, "[CDATA[", 7) == 0) {
          inSkip_ = true;
          skipKind_ = SKIP_CDATA;
          skipTail_ = 0;
          tagState_ = TAG_SKIPPING;
        } else if (bangLen_ >= 2 && !(bangLen_ == 2 && bangLookahead_[0] == '-')) {
          // Not a comment opener and can no longer become "[CDATA[" -- generic bang decl.
          if (!(bangLen_ <= 7 && std::memcmp(bangLookahead_, "[CDATA[", bangLen_) == 0)) {
            inSkip_ = true;
            skipKind_ = SKIP_BANG_OR_PI;
            tagState_ = TAG_SKIPPING;
            if (c == '>') inSkip_ = false;
          }
        }
        break;
      case TAG_SKIPPING:
        // Not currently reachable (processByte()'s own inSkip_ check intercepts every byte
        // before dispatch reaches here once inSkip_ is set), but kept correct defensively in
        // case that call order ever changes -- see the matching fix in processByte().
        processSkipByte(c);
        if (!inSkip_) {
          tagState_ = TAG_IDLE;
          globalInTag_ = false;
        }
        break;
      case TAG_IN_NAME:
        if (c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/') {
          tagName_[tagNameLen_] = '\0';
          if (tagNameLen_ > 0) {
            if (tagIsClose_)
              handleCloseTagName(tagName_);
            else
              handleOpenTagName(tagName_);
            if (c == '/' && !tagIsClose_) handleCloseTagName(tagName_);  // self-closing, e.g. <br/>
          }
          tagNameLen_ = 0;
          tagState_ = (c == '>') ? TAG_IDLE : TAG_ATTRS;
        } else if (tagNameLen_ + 1 < static_cast<int>(sizeof(tagName_))) {
          tagName_[tagNameLen_++] = static_cast<char>(c);
        }
        break;
      case TAG_ATTRS:
        if (!inAttrQuote_) {
          if (c == '"' || c == '\'') {
            inAttrQuote_ = true;
            attrQuoteChar_ = c;
          }
        } else if (c == attrQuoteChar_) {
          inAttrQuote_ = false;
        }
        if (c == '/' && !inAttrQuote_) {
          handleCloseTagName(lastOpenedName_);  // self-closing, e.g. <img src="x" />
        }
        break;
    }
  }

  void processByte(uint8_t c) {
    if (globalInEntity_) {
      if (entityLen_ + 1 < sizeof(entityBuffer_)) {
        entityBuffer_[entityLen_++] = static_cast<char>(c);
      } else {
        flushEntityAsLiteral();
        return;
      }
      if (c == ';') {
        finishEntity();
      } else if (c == '<' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        flushEntityAsLiteral();
      }
      return;
    }

    if (inSkip_) {
      processSkipByte(c);
      if (!inSkip_) {
        // Comment/CDATA/bang-decl/PI has fully closed -- must also drop globalInTag_, or every
        // byte after it (real visible text, or the next real tag) gets misrouted into
        // processByteInTag() as if still inside markup, silently swallowing it.
        tagState_ = TAG_IDLE;
        globalInTag_ = false;
      }
      return;
    }

    if (c == '<') {
      globalInTag_ = true;
      tagState_ = TAG_IDLE;
      tagNameLen_ = 0;
      tagIsClose_ = false;
      inAttrQuote_ = false;
    } else if (c == '>') {
      if (tagState_ == TAG_IN_NAME && tagNameLen_ > 0) {
        tagName_[tagNameLen_] = '\0';
        if (tagIsClose_)
          handleCloseTagName(tagName_);
        else
          handleOpenTagName(tagName_);
        tagNameLen_ = 0;
      }
      globalInTag_ = false;
      inAttrQuote_ = false;
      tagState_ = TAG_IDLE;
    } else if (globalInTag_) {
      // Remember the last opened element's name so a later self-close ('/' inside TAG_ATTRS)
      // closes the right element even though tagName_ isn't repopulated in TAG_ATTRS state.
      if (tagState_ == TAG_IN_NAME) {
        std::strncpy(lastOpenedName_, tagName_, sizeof(lastOpenedName_) - 1);
        lastOpenedName_[sizeof(lastOpenedName_) - 1] = '\0';
      }
      processByteInTag(c);
    } else {
      if (c == '&') {
        globalInEntity_ = true;
        entityBuffer_[0] = '&';
        entityLen_ = 1;
      } else {
        const bool startsCodepoint = (c & 0xC0) != 0x80;
        if (startsCodepoint && insideBody_) onVisibleCodepoint();
      }
    }
  }

  bool stopped_ = false;
  bool insideBody_ = false;
  int bodyDepth_ = -1;
  int htmlDepth_ = 0;

  bool globalInTag_ = false;
  bool globalInEntity_ = false;
  static constexpr size_t MAX_ENTITY_SIZE = 16;
  char entityBuffer_[MAX_ENTITY_SIZE] = {};
  size_t entityLen_ = 0;

  enum TagParseState { TAG_IDLE, TAG_IN_NAME, TAG_ATTRS, TAG_BANG_LOOKAHEAD, TAG_SKIPPING } tagState_ = TAG_IDLE;
  bool tagIsClose_ = false;
  char tagName_[MAX_TAG_NAME_LEN + 2] = {};
  int tagNameLen_ = 0;
  char lastOpenedName_[MAX_TAG_NAME_LEN + 2] = {};
  bool inAttrQuote_ = false;
  char attrQuoteChar_ = 0;

  char bangLookahead_[7] = {};
  int bangLen_ = 0;
  bool inSkip_ = false;
  SkipKind skipKind_ = SKIP_BANG_OR_PI;
  uint32_t skipTail_ = 0;
};

// ---- Ancestry-recording resolvers ----------------------------------------------------------
// Shared ancestry bookkeeping for the two resolvers below: records (tagName, siblingIndex) at
// each open <body>-descendant level in a fixed array, bounded by MAX_XPATH_DEPTH. Deterministic
// depth-cap fallback: once real nesting exceeds the cap, paragraph/list-item counting keeps
// working (it doesn't need the array), but no further ancestry segments are recorded -- a match
// found beyond the cap yields an XPath truncated at the deepest recorded (16th-level) ancestor
// rather than the exact target element. This never crashes or grows memory; it only reduces
// precision for documents nested unrealistically deep (real EPUB XHTML rarely exceeds ~10
// levels).
class AncestryRecordingScanner : public BoundedHtmlScanner {
 protected:
  void onBodyOpen() override {
    ancestryCount_ = 0;
    depthBeyondCap_ = 0;
    levels_[0].reset();
  }

  void onOpenTag(const char* name) override {
    bool recorded;
    if (depthBeyondCap_ > 0 || ancestryCount_ >= MAX_XPATH_DEPTH) {
      depthBeyondCap_++;
      recorded = false;
    } else {
      const int siblingIdx = levels_[ancestryCount_].nextIndex(name);
      std::strncpy(ancestry_[ancestryCount_].name, name, MAX_TAG_NAME_LEN);
      ancestry_[ancestryCount_].name[MAX_TAG_NAME_LEN] = '\0';
      ancestry_[ancestryCount_].index = siblingIdx;
      ancestryCount_++;
      levels_[ancestryCount_].reset();
      recorded = true;
    }
    // Always fires, even beyond the depth cap: paragraph/list-item counting (and progress
    // character counting) must keep working regardless of nesting depth -- KOReader progress
    // counts <p>/<li> globally, not by how deep they happen to be nested. Only the ANCESTRY
    // array recording above is capped; a match found beyond the cap still gets a valid xpath,
    // truncated at the deepest recorded (16th) ancestor -- see class comment.
    onChildOpened(name, recorded);
  }

  void onCloseTag(const char* name) override {
    bool wasRecorded;
    if (depthBeyondCap_ > 0) {
      depthBeyondCap_--;
      wasRecorded = false;
    } else if (ancestryCount_ > 0) {
      wasRecorded = true;
    } else {
      return;  // unbalanced close past body-open; defensively ignore rather than underflow
    }
    onChildClosing(name, wasRecorded);
    if (wasRecorded) ancestryCount_--;
  }

  // Hooks for the leaf classes: fire for EVERY element inside <body>, regardless of whether it
  // was beyond the depth cap (see onOpenTag's comment) -- `recorded` tells the leaf class
  // whether ancestryPath()/ancestryDepth() actually grew for this element (false once capped).
  virtual void onChildOpened(const char* name, bool recorded) {}
  virtual void onChildClosing(const char* name, bool recorded) {}

  int ancestryDepth() const { return ancestryCount_; }
  const AncestrySegment* ancestryPath() const { return ancestry_; }
  bool depthCapped() const { return depthBeyondCap_ > 0; }

 private:
  AncestrySegment ancestry_[MAX_XPATH_DEPTH];
  LevelSiblingState levels_[MAX_XPATH_DEPTH + 1];
  int ancestryCount_ = 0;
  int depthBeyondCap_ = 0;
};

// Replaces XPathParagraphResolver (Expat-based). Finds the Nth <p>/<li> (KOReader progress
// counts both, matching the prior implementation) and captures its ancestry path.
class BoundedXPathParagraphResolver final : public AncestryRecordingScanner {
 public:
  explicit BoundedXPathParagraphResolver(const int targetParagraph) : targetParagraph_(targetParagraph) {}

  bool hasMatch() const { return !xpath_.empty(); }
  const std::string& getXPath() const { return xpath_; }
  bool wasDepthCapped() const { return depthCapped(); }

  int spineIndex = 0;

 protected:
  void onChildOpened(const char* name, bool recorded) override {
    (void)recorded;
    if (matched_) return;
    // Counted regardless of `recorded`: KOReader progress counts <p>/<li> globally. If the
    // match happens beyond the ancestry depth cap, buildParagraphXPath() below naturally
    // truncates at whatever ancestryDepth() actually reached (capped at MAX_XPATH_DEPTH) --
    // see AncestryRecordingScanner's class comment for the deterministic fallback rationale.
    if (std::strcmp(name, "p") == 0 || std::strcmp(name, "li") == 0) {
      paragraphCount_++;
      if (paragraphCount_ == targetParagraph_) {
        xpath_ = buildParagraphXPath(spineIndex, ancestryPath(), ancestryDepth(), 0, 0);
        matched_ = true;
        requestStop();
      }
    }
  }

 private:
  const int targetParagraph_;
  int paragraphCount_ = 0;
  bool matched_ = false;
  std::string xpath_;
};

// Replaces XPathProgressResolver (Expat-based). Finds the ancestry path, text-node index, and
// intra-text-node character offset for a target visible-codepoint count within <p>/<li> content.
class BoundedXPathProgressResolver final : public AncestryRecordingScanner {
 public:
  explicit BoundedXPathProgressResolver(const size_t targetVisibleChar) : targetVisibleChar_(targetVisibleChar) {}

  bool hasMatch() const { return !xpath_.empty(); }
  const std::string& getXPath() const { return xpath_; }
  bool wasDepthCapped() const { return depthCapped(); }

  int spineIndex = 0;

 protected:
  void onChildOpened(const char* name, bool recorded) override {
    // Only touch the per-depth text-node slot when this element actually has one (i.e. was
    // recorded); beyond the depth cap there is no new slot, so leave the capped level's slot
    // alone rather than index out of bounds or misattribute it.
    if (recorded && ancestryDepth() - 1 < MAX_XPATH_DEPTH) {
      textNodeIndexAtDepth_[ancestryDepth() - 1] = 0;
    }
    pendingTextNode_ = true;
    // Counted regardless of `recorded` -- see BoundedXPathParagraphResolver's onChildOpened
    // comment; the same "count globally, truncate the ancestry only" rationale applies here.
    if (std::strcmp(name, "p") == 0) paragraphDepth_++;
    if (std::strcmp(name, "li") == 0) liDepth_++;
  }

  void onChildClosing(const char* name, bool recorded) override {
    (void)recorded;
    if (std::strcmp(name, "p") == 0 && paragraphDepth_ > 0) paragraphDepth_--;
    if (std::strcmp(name, "li") == 0 && liDepth_ > 0) liDepth_--;
    if (paragraphDepth_ > 0 || liDepth_ > 0) pendingTextNode_ = true;
  }

  void onVisibleCodepoint() override {
    if (stoppedMatching_ || (paragraphDepth_ <= 0 && liDepth_ <= 0)) {
      return;
    }
    visibleChars_++;
    if (pendingTextNode_) {
      const int depth = ancestryDepth() - 1;
      if (depth >= 0 && depth < MAX_XPATH_DEPTH) textNodeIndexAtDepth_[depth]++;
      textNodeStartChars_ = visibleChars_ - 1;
      pendingTextNode_ = false;
    }
    if (targetVisibleChar_ <= visibleChars_) {
      const int depth = ancestryDepth() - 1;
      const int textNode = (depth >= 0 && depth < MAX_XPATH_DEPTH) ? textNodeIndexAtDepth_[depth] : 1;
      const size_t charOff = visibleChars_ - 1 - textNodeStartChars_ + (targetVisibleChar_ - (visibleChars_ - 1));
      xpath_ = buildParagraphXPath(spineIndex, ancestryPath(), ancestryDepth(), textNode, charOff);
      stoppedMatching_ = true;
      requestStop();
    }
  }

 private:
  const size_t targetVisibleChar_;
  int paragraphDepth_ = 0;
  int liDepth_ = 0;
  bool pendingTextNode_ = true;
  size_t visibleChars_ = 0;
  size_t textNodeStartChars_ = 0;
  int textNodeIndexAtDepth_[MAX_XPATH_DEPTH] = {};
  bool stoppedMatching_ = false;
  std::string xpath_;
};

// Replaces ParagraphTextCounter (Expat-based). Counts visible codepoints within <p> content
// only (not <li> -- matching the prior implementation's exact, narrower counting basis, since
// this total is the denominator BoundedXPathProgressResolver's target is computed against).
class BoundedVisibleCharCounter final : public BoundedHtmlScanner {
 public:
  size_t totalVisibleChars() const { return visibleChars_; }

 protected:
  void onOpenTag(const char* name) override {
    if (std::strcmp(name, "p") == 0) paragraphDepth_++;
  }

  void onCloseTag(const char* name) override {
    if (std::strcmp(name, "p") == 0 && paragraphDepth_ > 0) paragraphDepth_--;
  }

  void onVisibleCodepoint() override {
    if (paragraphDepth_ > 0) visibleChars_++;
  }

 private:
  int paragraphDepth_ = 0;
  size_t visibleChars_ = 0;
};

}  // namespace koxpath_detail
