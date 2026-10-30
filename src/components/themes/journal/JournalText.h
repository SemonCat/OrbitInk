#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Title wrapping for the Journal Home. GfxRenderer::wrappedText() breaks only at spaces, so an
// unspaced CJK title is one "word" that is truncated to a single line; the reader and every other
// screen rely on that behaviour, so it is left alone and this narrow helper is used instead.
//
// Breaks at the last space that fits when the line has one (Latin), otherwise at a UTF-8
// codepoint boundary (CJK, or a single over-long word). Never splits a multi-byte sequence, never
// starts a line with a space, and truncates only the final permitted line, with an ellipsis.
// `measure(const std::string&) -> int` returns the rendered width; templated (not std::function) so
// the host test can pass a fake measurer and the firmware a renderer lambda with no heap closure.
namespace journal {

inline size_t utf8SequenceLength(const unsigned char lead) {
  if (lead < 0x80) return 1;
  if ((lead >> 5) == 0x6) return 2;
  if ((lead >> 4) == 0xE) return 3;
  if ((lead >> 3) == 0x1E) return 4;
  return 1;  // stray continuation / invalid byte: advance one so the walk always terminates
}

// Codepoint start offsets of `s`, plus s.size() as the end sentinel.
inline std::vector<size_t> utf8Boundaries(const std::string& s) {
  std::vector<size_t> b;
  b.reserve(s.size() + 1);
  size_t i = 0;
  while (i < s.size()) {
    b.push_back(i);
    i += utf8SequenceLength(static_cast<unsigned char>(s[i]));
  }
  b.push_back(s.size());
  return b;
}

constexpr const char* kEllipsis = "\xe2\x80\xa6";  // U+2026

template <typename Measure>
std::string truncateToWidth(const std::string& text, const int maxWidth, const Measure& measure) {
  if (measure(text) <= maxWidth) return text;
  const auto b = utf8Boundaries(text);
  for (size_t n = b.size() - 1; n > 0; n--) {
    std::string candidate = text.substr(0, b[n - 1]);
    while (!candidate.empty() && candidate.back() == ' ') candidate.pop_back();
    candidate += kEllipsis;
    if (measure(candidate) <= maxWidth) return candidate;
  }
  return kEllipsis;
}

template <typename Measure>
std::vector<std::string> wrapTitle(const std::string& text, const int maxWidth, const int maxLines,
                                   const Measure& measure) {
  std::vector<std::string> lines;
  if (text.empty() || maxWidth <= 0 || maxLines <= 0) return lines;
  lines.reserve(static_cast<size_t>(maxLines));

  size_t pos = 0;
  while (pos < text.size() && static_cast<int>(lines.size()) < maxLines) {
    while (pos < text.size() && text[pos] == ' ') pos++;  // no line starts with a space
    if (pos >= text.size()) break;
    const std::string rest = text.substr(pos);
    if (static_cast<int>(lines.size()) == maxLines - 1 || measure(rest) <= maxWidth) {
      lines.push_back(truncateToWidth(rest, maxWidth, measure));
      return lines;
    }
    // Longest codepoint prefix that fits; prefer the last space inside it.
    const auto b = utf8Boundaries(rest);
    size_t fit = 0;  // index into b of the end of the fitting prefix
    for (size_t n = 1; n < b.size(); n++) {
      if (measure(rest.substr(0, b[n])) > maxWidth) break;
      fit = n;
    }
    if (fit == 0) fit = 1;  // a single glyph wider than the line still advances
    size_t cut = b[fit];
    const size_t space = rest.rfind(' ', cut);
    if (space != std::string::npos && space > 0 && cut < rest.size() && rest[cut] != ' ') {
      cut = space;  // Latin: break between words
    }
    std::string line = rest.substr(0, cut);
    while (!line.empty() && line.back() == ' ') line.pop_back();
    lines.push_back(line);
    pos += cut;
  }
  return lines;
}

}  // namespace journal
