// Direct, Epub-free tests of the bounded XPath tokenizer in
// lib/KOReaderSync/ChapterXPathResolverInternal.h -- the replacement for the Expat-based
// outgoing-XPath generation that could OOM on the X3 immediately after a TLS handshake (field
// evidence: "[KOX] XML parse error: out of memory", "Min Free reached 5116 bytes").
//
// These drive the resolver classes directly via their Print write() interface with raw XHTML
// strings, bypassing Epub/ZIP/HalStorage entirely -- the tokenizer's correctness does not depend
// on how the bytes arrive, only on what they contain.

#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "ChapterXPathResolverInternal.h"

namespace {

using koxpath_detail::BoundedVisibleCharCounter;
using koxpath_detail::BoundedXPathParagraphResolver;
using koxpath_detail::BoundedXPathProgressResolver;
using koxpath_detail::MAX_XPATH_DEPTH;

void feed(Print& scanner, const std::string& xhtml) {
  scanner.write(reinterpret_cast<const uint8_t*>(xhtml.data()), xhtml.size());
}

std::string resolveParagraph(const std::string& xhtml, int targetParagraph, int spineIndex = 0) {
  BoundedXPathParagraphResolver resolver(targetParagraph);
  resolver.spineIndex = spineIndex;
  feed(resolver, xhtml);
  resolver.finish();
  return resolver.hasMatch() ? resolver.getXPath() : "";
}

std::string resolveProgress(const std::string& xhtml, size_t targetVisibleChar, int spineIndex = 0) {
  BoundedXPathProgressResolver resolver(targetVisibleChar);
  resolver.spineIndex = spineIndex;
  feed(resolver, xhtml);
  resolver.finish();
  return resolver.hasMatch() ? resolver.getXPath() : "";
}

size_t countVisible(const std::string& xhtml) {
  BoundedVisibleCharCounter counter;
  feed(counter, xhtml);
  counter.finish();
  return counter.totalVisibleChars();
}

}  // namespace

// ---- Basic paragraph resolution -------------------------------------------------------------

TEST(ChapterXPathResolver, ResolvesSimpleParagraphBySiblingIndex) {
  const std::string xhtml = "<html><body><p>one</p><p>two</p><p>three</p></body></html>";
  EXPECT_EQ(resolveParagraph(xhtml, 1), "/body/DocFragment[1]/body/p[1]");
  EXPECT_EQ(resolveParagraph(xhtml, 2), "/body/DocFragment[1]/body/p[2]");
  EXPECT_EQ(resolveParagraph(xhtml, 3), "/body/DocFragment[1]/body/p[3]");
}

TEST(ChapterXPathResolver, ParagraphCountIncludesListItems) {
  // KOReader progress counts <p> and <li> together, matching the prior Expat implementation.
  const std::string xhtml = "<html><body><p>a</p><ul><li>b</li><li>c</li></ul><p>d</p></body></html>";
  EXPECT_EQ(resolveParagraph(xhtml, 1), "/body/DocFragment[1]/body/p[1]");
  EXPECT_EQ(resolveParagraph(xhtml, 2), "/body/DocFragment[1]/body/ul[1]/li[1]");
  EXPECT_EQ(resolveParagraph(xhtml, 3), "/body/DocFragment[1]/body/ul[1]/li[2]");
  EXPECT_EQ(resolveParagraph(xhtml, 4), "/body/DocFragment[1]/body/p[2]");
}

TEST(ChapterXPathResolver, SiblingIndexIsPerTagNameNotPositional) {
  // The 2nd <div> among interleaved <div>/<p> siblings must be div[2], not div[3].
  const std::string xhtml = "<html><body><div><p>x</p></div><p>y</p><div><p>z</p></div></body></html>";
  EXPECT_EQ(resolveParagraph(xhtml, 1), "/body/DocFragment[1]/body/div[1]/p[1]");
  EXPECT_EQ(resolveParagraph(xhtml, 2), "/body/DocFragment[1]/body/p[1]");
  EXPECT_EQ(resolveParagraph(xhtml, 3), "/body/DocFragment[1]/body/div[2]/p[1]");
}

TEST(ChapterXPathResolver, NoMatchBeyondAvailableParagraphsReturnsEmpty) {
  const std::string xhtml = "<html><body><p>only one</p></body></html>";
  EXPECT_EQ(resolveParagraph(xhtml, 2), "");
}

TEST(ChapterXPathResolver, SpineIndexIsReflectedInDocFragmentNumber) {
  const std::string xhtml = "<html><body><p>a</p></body></html>";
  EXPECT_EQ(resolveParagraph(xhtml, 1, /*spineIndex=*/7), "/body/DocFragment[8]/body/p[1]");
}

// ---- Namespace prefix stripping -------------------------------------------------------------

TEST(ChapterXPathResolver, StripsNamespacePrefixFromTagNames) {
  const std::string xhtml = "<html xmlns:epub=\"x\"><body><epub:p>a</epub:p></body></html>";
  EXPECT_EQ(resolveParagraph(xhtml, 1), "/body/DocFragment[1]/body/p[1]");
}

// ---- Self-closing tags --------------------------------------------------------------------

TEST(ChapterXPathResolver, SelfClosingTagsDoNotUnbalanceAncestryOrParagraphCounting) {
  const std::string xhtml =
      "<html><body><p>a<br/>b</p><div><img src=\"x.png\" alt=\"y\" /></div><p>c</p></body></html>";
  EXPECT_EQ(resolveParagraph(xhtml, 1), "/body/DocFragment[1]/body/p[1]");
  EXPECT_EQ(resolveParagraph(xhtml, 2), "/body/DocFragment[1]/body/p[2]");
}

TEST(ChapterXPathResolver, SelfClosingWithAttributeContainingSlashIsHandledCorrectly) {
  // The '/' inside a quoted attribute value must not be mistaken for self-close.
  const std::string xhtml = "<html><body><p title=\"a/b\">text</p><p>next</p></body></html>";
  EXPECT_EQ(resolveParagraph(xhtml, 1), "/body/DocFragment[1]/body/p[1]");
  EXPECT_EQ(resolveParagraph(xhtml, 2), "/body/DocFragment[1]/body/p[2]");
}

// ---- Comments / CDATA / processing instructions / doctype ----------------------------------

TEST(ChapterXPathResolver, CommentWithEmbeddedGreaterThanDoesNotConfuseTagScanning) {
  const std::string xhtml = "<html><body><!-- a > b --><p>a</p><p>b</p></body></html>";
  EXPECT_EQ(resolveParagraph(xhtml, 1), "/body/DocFragment[1]/body/p[1]");
  EXPECT_EQ(resolveParagraph(xhtml, 2), "/body/DocFragment[1]/body/p[2]");
}

TEST(ChapterXPathResolver, CommentTextIsNotCountedAsVisibleCharacters) {
  const std::string xhtml = "<html><body><p><!-- five > chars -->hi</p></body></html>";
  EXPECT_EQ(countVisible(xhtml), 2u);  // only "hi"
}

TEST(ChapterXPathResolver, CdataWithEmbeddedGreaterThanDoesNotConfuseTagScanning) {
  const std::string xhtml = "<html><body><p><![CDATA[a > b]]>x</p><p>y</p></body></html>";
  EXPECT_EQ(resolveParagraph(xhtml, 1), "/body/DocFragment[1]/body/p[1]");
  EXPECT_EQ(resolveParagraph(xhtml, 2), "/body/DocFragment[1]/body/p[2]");
}

TEST(ChapterXPathResolver, ProcessingInstructionAndDoctypeAreSkipped) {
  const std::string xhtml = "<?xml version=\"1.0\"?><!DOCTYPE html><html><body><p>a</p></body></html>";
  EXPECT_EQ(resolveParagraph(xhtml, 1), "/body/DocFragment[1]/body/p[1]");
}

// ---- Entities -------------------------------------------------------------------------------

TEST(ChapterXPathResolver, NamedEntityCountsAsOneCharacter) {
  const std::string xhtml = "<html><body><p>a&amp;b</p></body></html>";
  EXPECT_EQ(countVisible(xhtml), 3u);  // 'a', '&', 'b'
}

TEST(ChapterXPathResolver, NumericDecimalEntityCountsAsOneCharacter) {
  const std::string xhtml = "<html><body><p>a&#233;b</p></body></html>";  // &#233; = 'e' with acute
  EXPECT_EQ(countVisible(xhtml), 3u);
}

TEST(ChapterXPathResolver, NumericHexEntityCountsAsOneCharacter) {
  const std::string xhtml = "<html><body><p>a&#x00E9;b</p></body></html>";
  EXPECT_EQ(countVisible(xhtml), 3u);
}

TEST(ChapterXPathResolver, UnknownEntityFallsBackToLiteralCharacterCount) {
  const std::string xhtml = "<html><body><p>a&notreal;b</p></body></html>";
  // "&notreal;" (9 chars) counted literally, matching prior (Expat-era) fallback behavior.
  EXPECT_EQ(countVisible(xhtml), 2u + 9u);
}

// ---- Progress / text-offset resolution ------------------------------------------------------

TEST(ChapterXPathResolver, ProgressResolvesToCorrectParagraphAndCharOffset) {
  const std::string xhtml = "<html><body><p>hello</p><p>world</p></body></html>";
  EXPECT_EQ(countVisible(xhtml), 10u);
  // 1st char of "world" is the 6th visible char overall.
  EXPECT_EQ(resolveProgress(xhtml, 6), "/body/DocFragment[1]/body/p[2]/text()[1].1");
  // 3rd char of "hello" ('l') is the 3rd visible char overall.
  EXPECT_EQ(resolveProgress(xhtml, 3), "/body/DocFragment[1]/body/p[1]/text()[1].3");
}

TEST(ChapterXPathResolver, ProgressCountsBothPAndLiUnlikeTheDenominatorCounter) {
  // Faithful-parity quirk carried over from the Expat implementation: the denominator
  // (BoundedVisibleCharCounter) only counts <p>, but the progress resolver itself also matches
  // inside <li>. This test locks in that existing (not newly introduced) asymmetry.
  const std::string xhtml = "<html><body><ul><li>ab</li></ul><p>cd</p></body></html>";
  EXPECT_EQ(countVisible(xhtml), 2u);  // only "cd" -- <li> text excluded from the denominator
  EXPECT_EQ(resolveProgress(xhtml, 1), "/body/DocFragment[1]/body/ul[1]/li[1]/text()[1].1");
}

TEST(ChapterXPathResolver, ProgressTextNodeIndexAdvancesAcrossInlineElements) {
  const std::string xhtml = "<html><body><p>a<em>b</em>c</p></body></html>";
  // Text node 1 = "a", text node 2 = "c" (the <em>'s "b" is its own element's text node 1).
  EXPECT_EQ(resolveProgress(xhtml, 1), "/body/DocFragment[1]/body/p[1]/text()[1].1");
  EXPECT_EQ(resolveProgress(xhtml, 3), "/body/DocFragment[1]/body/p[1]/text()[2].1");
}

// ---- Depth-cap and sibling-name-overflow: deterministic fallback ---------------------------

TEST(ChapterXPathResolver, DepthBeyondCapProducesTruncatedButValidXPathNotACrash) {
  std::string xhtml = "<html><body>";
  for (int i = 0; i < MAX_XPATH_DEPTH + 5; i++) xhtml += "<div>";
  xhtml += "<p>deep</p>";
  for (int i = 0; i < MAX_XPATH_DEPTH + 5; i++) xhtml += "</div>";
  xhtml += "</body></html>";

  BoundedXPathParagraphResolver resolver(1);
  resolver.spineIndex = 0;
  feed(resolver, xhtml);
  resolver.finish();

  ASSERT_TRUE(resolver.hasMatch()) << "a paragraph beyond the depth cap must still resolve to SOME xpath";
  EXPECT_TRUE(resolver.wasDepthCapped());
  // Valid, parseable, and truncated at exactly MAX_XPATH_DEPTH ancestry segments.
  std::string expected = "/body/DocFragment[1]/body";
  for (int i = 0; i < MAX_XPATH_DEPTH; i++) expected += "/div[1]";
  EXPECT_EQ(resolver.getXPath(), expected);
}

TEST(ChapterXPathResolver, MoreThanEightDistinctSiblingNamesDoesNotCrash) {
  // Deterministic overflow fallback for LevelSiblingState: real content essentially never has
  // this many distinct child tag names at one level, but it must not crash or corrupt state.
  std::string xhtml = "<html><body>";
  const char* tags[] = {"a1", "a2", "a3", "a4", "a5", "a6", "a7", "a8", "a9", "a10"};
  for (const char* t : tags) {
    xhtml += "<";
    xhtml += t;
    xhtml += ">x</";
    xhtml += t;
    xhtml += ">";
  }
  xhtml += "<p>target</p></body></html>";

  EXPECT_NO_FATAL_FAILURE({
    const std::string xp = resolveParagraph(xhtml, 1);
    EXPECT_EQ(xp, "/body/DocFragment[1]/body/p[1]");
  });
}

// ---- Large / deep XHTML bounded regression --------------------------------------------------
// Proves the tokenizer stays within its fixed bounds (no crash, no unbounded growth, completes
// promptly) on a large, moderately deep synthetic chapter, and that it can resolve a target deep
// in the document. This is the class of input ("large/deep XHTML") the task specifically asked
// to regress-test, since it is what triggered the original OOM in the field.
TEST(ChapterXPathResolver, LargeDeepDocumentStaysBoundedAndResolvesCorrectly) {
  constexpr int kSections = 200;
  constexpr int kNestingPerSection = 6;
  std::string xhtml = "<html><body>";
  for (int s = 0; s < kSections; s++) {
    for (int d = 0; d < kNestingPerSection; d++) xhtml += "<div>";
    xhtml += "<p>Paragraph number " + std::to_string(s) +
             " with some reasonably long filler text to simulate a real book chapter's prose.</p>";
    for (int d = 0; d < kNestingPerSection; d++) xhtml += "</div>";
  }
  xhtml += "</body></html>";

  // Real EPUB chapters are commonly tens to a few hundred KB; this is in that range.
  ASSERT_GT(xhtml.size(), 20000u);

  const auto start = std::chrono::steady_clock::now();
  const std::string xp = resolveParagraph(xhtml, 150);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_EQ(xp, "/body/DocFragment[1]/body/div[150]/div[1]/div[1]/div[1]/div[1]/div[1]/p[1]");
  // Generous bound: a linear single-pass scan of ~a few hundred KB must not be anywhere close
  // to slow; this catches an accidental O(n^2) regression (e.g. from an unbounded sibling list)
  // without being flaky on slower CI hardware.
  EXPECT_LT(elapsed, std::chrono::seconds(2));

  // Also confirm the progress-based path (character counting, not just paragraph index) stays
  // bounded and correct on the same large document.
  const size_t total = countVisible(xhtml);
  EXPECT_GT(total, 10000u);
  const std::string progressXp = resolveProgress(xhtml, total / 2);
  EXPECT_FALSE(progressXp.empty());
  EXPECT_NE(progressXp.find("/p[1]/text()["), std::string::npos);
}

TEST(ChapterXPathResolver, EmptyDocumentDoesNotCrashAndReportsNoMatch) {
  EXPECT_EQ(resolveParagraph("", 1), "");
  EXPECT_EQ(resolveProgress("", 1), "");
  EXPECT_EQ(countVisible(""), 0u);
}

TEST(ChapterXPathResolver, DocumentWithNoBodyReportsNoMatch) {
  const std::string xhtml = "<html><head><title>t</title></head></html>";
  EXPECT_EQ(resolveParagraph(xhtml, 1), "");
}
