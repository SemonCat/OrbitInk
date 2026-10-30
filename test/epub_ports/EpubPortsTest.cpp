#include <GfxRenderer.h>
#include <XmlParserUtils.h>
#include <gtest/gtest.h>

#include "Epub/Page.h"
#include "Epub/htmlEntities.h"
#include "Epub/parsers/ChapterHtmlSlimParser.h"
TEST(EpubPorts, NamespaceLocalNames) {
  EXPECT_TRUE(xmlLocalNameEquals("book:package", "package"));
  EXPECT_TRUE(xmlLocalNameEquals("title", "title"));
  EXPECT_FALSE(xmlLocalNameEquals(nullptr, "title"));
}
TEST(EpubPorts, ImportantMargins) {
  const auto plain = CssParser::parseInlineStyle("margin-top: 2em; text-align: center");
  const auto important = CssParser::parseInlineStyle("margin-top: 2em !important; text-align: center !IMPORTANT");
  EXPECT_EQ(plain.marginTop.value, important.marginTop.value);
  EXPECT_EQ(plain.textAlign, important.textAlign);
}

#include "Epub/parsers/ContentOpfParser.h"
namespace {
struct Parsed {
  bool ok;
  std::vector<std::unique_ptr<Page>> pages;
};
Parsed parse(const std::string& source, int width = 100, int height = 100, bool extra = false,
             float compression = 1.0f) {
  HalStorage::resetTestFiles();
  const std::string path = "/chapter.xhtml";
  HalStorage::registerTestFile(path, {source.begin(), source.end()});
  GfxRenderer renderer;
  Parsed out{};
  ChapterHtmlSlimParser parser(
      nullptr, path, renderer, 1, compression, extra, 1, width, height, false, false,
      [&](std::unique_ptr<Page> page, uint16_t, uint16_t) { out.pages.push_back(std::move(page)); }, false, "", "");
  out.ok = parser.parseAndBuildPages();
  return out;
}
std::vector<std::string> lines(const Parsed& parsed) {
  std::vector<std::string> out;
  for (const auto& p : parsed.pages)
    for (const auto& e : p->elements)
      if (e->getTag() == TAG_PageLine) {
        const auto& b = static_cast<const PageLine&>(*e).getBlock();
        std::string line;
        for (uint16_t i = 0; i < b->wordCount(); ++i) line += b->wordText(i);
        out.push_back(line);
      }
  return out;
}
}  // namespace
TEST(EpubPorts, CommentsAndTrailingGarbagePreserveValidContent) {
  const auto ordinary = parse("<html><body><p>Hello world</p></body></html>");
  const auto comments = parse("<html><body><!-- <p>hidden</p> --><p>Hello world</p></body></html>garbage\x01");
  ASSERT_TRUE(ordinary.ok);
  ASSERT_TRUE(comments.ok);
  EXPECT_EQ(lines(ordinary), lines(comments));
  EXPECT_FALSE(parse("<html><body><p>broken</body></html>garbage").ok);
}
TEST(EpubPorts, NestedHtmlCloseDoesNotHideMalformedDocument) {
  EXPECT_FALSE(parse("<html><body><html><p>valid inner content</p></html><b>broken</body></html>").ok);
}
TEST(EpubPorts, TrailingGarbageInLaterReadChunk) {
  const auto result = parse("<html><body><p>Valid</p></body></html>" + std::string(2048, ' ') + "garbage");
  ASSERT_TRUE(result.ok);
  EXPECT_EQ(lines(result), std::vector<std::string>{"Valid"});
}
TEST(EpubPorts, ApostropheEntity) {
  ASSERT_NE(lookupHtmlEntity("&apos;", 6), nullptr);
  EXPECT_STREQ(lookupHtmlEntity("&apos;", 6), "'");
  EXPECT_EQ(lines(parse("<html><body><p>it&apos;s</p></body></html>")), std::vector<std::string>{"it's"});
}
TEST(EpubPorts, LongCalibreFootnoteHrefIsPreserved) {
  const std::string href = "../Text/" + std::string(160, 'a') + "_split_099.xhtml#calibre_link-123";
  auto out = parse("<html><body><p>word<a href=\"" + href + "\">[1]</a></p></body></html>");
  ASSERT_TRUE(out.ok);
  bool found = false;
  for (const auto& p : out.pages)
    for (const auto& f : p->footnotes) {
      EXPECT_EQ(std::string(f.href), href);
      found = true;
    }
  EXPECT_TRUE(found);
}
TEST(EpubPorts, RubyBaseAndAnnotationRemainOnOneLine) {
  auto out = parse("<html><body><p>前文前文<ruby>漢字<rt>かんじ</rt></ruby>後文</p></body></html>", 60);
  ASSERT_TRUE(out.ok);
  bool found = false;
  for (const auto& line : lines(out))
    if (line.find("漢") != std::string::npos) {
      EXPECT_NE(line.find("字かんじ"), std::string::npos);
      found = true;
    }
  EXPECT_TRUE(found);
}
TEST(EpubPorts, RubyCrossesSoftFlushThresholdIntact) {
  std::string source = "<html><body><p>";
  for (int i = 0; i < 749; ++i) source += "字";
  source += "<ruby>漢字<rt>かんじ</rt></ruby></p></body></html>";
  auto out = parse(source);
  ASSERT_TRUE(out.ok);
  bool found = false;
  for (const auto& line : lines(out))
    if (line.find("漢") != std::string::npos) {
      EXPECT_NE(line.find("字かんじ"), std::string::npos);
      found = true;
    }
  EXPECT_TRUE(found);
}
TEST(EpubPorts, ParentVerticalSpacingDoesNotLeakIntoFollowingText) {
  BlockStyle parent;
  parent.marginTop = 10;
  parent.marginBottom = 20;
  parent.paddingTop = 3;
  parent.paddingBottom = 4;
  parent.marginLeft = 7;
  auto following = parent.withoutTop().withoutBottom();
  EXPECT_EQ(following.topInset(), 0);
  EXPECT_EQ(following.bottomInset(), 0);
  EXPECT_EQ(following.leftInset(), 7);
  const auto out = parse("<html><body><div><p>First</p><p>Second</p></div></body></html>");
  ASSERT_TRUE(out.ok);
  ASSERT_EQ(out.pages.size(), 1u);
  ASSERT_EQ(out.pages[0]->elements.size(), 2u);
  EXPECT_EQ(out.pages[0]->elements[1]->yPos - out.pages[0]->elements[0]->yPos, 20);
}
TEST(EpubPorts, ExtraWideChangesLineHeightAndPagination) {
  const std::string source = "<html><body><p>one two three four five six seven eight nine ten</p></body></html>";
  auto normal = parse(source, 60, 100, false, 1.0f);
  auto wide = parse(source, 60, 100, false, 1.2f);
  ASSERT_TRUE(normal.ok);
  ASSERT_TRUE(wide.ok);
  EXPECT_EQ(lines(normal), lines(wide));
  EXPECT_GT(wide.pages.size(), normal.pages.size());
}
TEST(EpubPorts, MetadataAcceptsArbitraryPrefixesAndKeepsFirstTitle) {
  for (const std::string prefix : {"", "other:"}) {
    const std::string dc = prefix.empty() ? "" : "d:";
    const std::string source =
        "<" + prefix +
        "package xmlns:other=\"http://www.idpf.org/2007/opf\" xmlns:d=\"http://purl.org/dc/elements/1.1/\"><" + prefix +
        "metadata><" + dc + "title>正確書名</" + dc + "title><" + dc + "title>subtitle</" + dc + "title><" + dc +
        "creator>作者</" + dc + "creator><" + dc + "language>zh-TW</" + dc + "language></" + prefix + "metadata></" +
        prefix + "package>";
    const std::string cache = "/cache", base = "";
    ContentOpfParser parser(cache, base, source.size(), nullptr);
    ASSERT_TRUE(parser.setup());
    for (size_t i = 0; i < source.size(); ++i)
      ASSERT_EQ(parser.write(reinterpret_cast<const uint8_t*>(source.data() + i), 1), 1u);
    EXPECT_EQ(parser.title, "正確書名");
    EXPECT_EQ(parser.author, "作者");
    EXPECT_EQ(parser.language, "zh-TW");
  }
}

#include "DictHtmlPages.h"
#include "HtmlToPlainText.h"
TEST(EpubPorts, HtmlDictionaryFormattingEntitiesAndMultiplePages) {
  GfxRenderer renderer;
  std::vector<std::unique_ptr<Page>> pages;
  ASSERT_TRUE(buildDictionaryHtmlPages(
      renderer, "<B>bold</B><br>normal &amp; 中文 <i>italic</i><p>second paragraph</p>", 90, 40, pages));
  EXPECT_GT(pages.size(), 1u);
  bool bold = false, italic = false;
  for (const auto& page : pages)
    for (const auto& e : page->elements)
      if (e->getTag() == TAG_PageLine) {
        const auto& b = static_cast<const PageLine&>(*e).getBlock();
        for (uint16_t i = 0; i < b->wordCount(); ++i) {
          bold |= (b->wordStyle(i) & EpdFontFamily::BOLD) != 0;
          italic |= (b->wordStyle(i) & EpdFontFamily::ITALIC) != 0;
        }
      }
  EXPECT_TRUE(bold);
  EXPECT_TRUE(italic);
  EXPECT_FALSE(Storage.exists("/.crosspoint/dicthtml.tmp"));
}
TEST(EpubPorts, HtmlDictionaryMalformedAndLowHeapFallBackCleanly) {
  GfxRenderer renderer;
  std::vector<std::unique_ptr<Page>> pages;
  EXPECT_FALSE(buildDictionaryHtmlPages(renderer, "<b>broken</i>", 100, 100, pages));
  EXPECT_TRUE(pages.empty());
  EXPECT_EQ(htmlToPlainText("<b>broken</i>"), "broken");
  ESP.freeHeap = 100;
  EXPECT_FALSE(buildDictionaryHtmlPages(renderer, "<b>valid</b>", 100, 100, pages));
  EXPECT_TRUE(pages.empty());
  ESP.freeHeap = UINT32_MAX;
  EXPECT_TRUE(buildDictionaryHtmlPages(renderer, "<b>valid</b>", 100, 100, pages));
  EXPECT_FALSE(Storage.exists("/.crosspoint/dicthtml.tmp"));
}
TEST(EpubPorts, DictionaryPlainTextAndHeadingFallback) {
  EXPECT_EQ(htmlToPlainText("Plain definition."), "Plain definition.");
  EXPECT_EQ(htmlToPlainText("<h1>Title</h1><p>body &apos; &amp;</p>"), "Title\n\nbody ' &");
}
TEST(EpubPorts, HtmlDictionaryRetainedPageCapAndInputCap) {
  GfxRenderer renderer;
  std::vector<std::unique_ptr<Page>> pages;
  EXPECT_FALSE(buildDictionaryHtmlPages(renderer, std::string(65537, 'x'), 100, 100, pages));
  std::string fragment;
  for (int i = 0; i < 100; ++i) fragment += "<p>entry</p>";
  EXPECT_FALSE(buildDictionaryHtmlPages(renderer, fragment, 100, 20, pages));
  EXPECT_TRUE(pages.empty());
  EXPECT_FALSE(Storage.exists("/.crosspoint/dicthtml.tmp"));
}
TEST(EpubPorts, TableHeadersAndUnevenRowsUseColumns) {
  auto out = parse(
      "<html><body><table><tr><th>Head A</th><th>Head B</th></tr><tr><td>短文</td><td><b>Bold</b> and "
      "<i>italic</i></td></tr><tr><td>one</td></tr></table><p>After</p></body></html>",
      300, 200);
  ASSERT_TRUE(out.ok);
  ASSERT_FALSE(out.pages.empty());
  std::vector<const PageLine*> row;
  for (const auto& e : out.pages[0]->elements)
    if (e->getTag() == TAG_PageLine) row.push_back(static_cast<const PageLine*>(e.get()));
  ASSERT_GE(row.size(), 5u);
  EXPECT_EQ(row[0]->yPos, row[1]->yPos);
  EXPECT_LT(row[0]->xPos, row[1]->xPos);
  EXPECT_NE(row[0]->getBlock()->wordStyle(0) & EpdFontFamily::BOLD, 0);
  EXPECT_EQ(lines(out).back(), "After");
}
TEST(EpubPorts, TableUnsupportedSpansAndOversizedCellsPreserveText) {
  for (const std::string attr : {"colspan=\"2\"", "rowspan=\"2\"", "colspan=\"0\""}) {
    auto out = parse("<html><body><table><tr><td " + attr +
                         ">span</td><td>cell</td></tr><tr><td>next</td><td>end</td></tr></table></body></html>",
                     300, 200);
    ASSERT_TRUE(out.ok);
    std::string all;
    for (const auto& l : lines(out)) all += l;
    EXPECT_NE(all.find("span"), std::string::npos);
    EXPECT_NE(all.find("cell"), std::string::npos);
    EXPECT_NE(all.find("end"), std::string::npos);
  }
  std::string oversized;
  for (int i = 0; i < 60; ++i) oversized += "字";
  auto out =
      parse("<html><body><table><tr><td>" + oversized + "</td><td>tail</td></tr></table></body></html>", 300, 200);
  ASSERT_TRUE(out.ok);
  std::string all;
  for (const auto& l : lines(out)) all += l;
  EXPECT_EQ(all, oversized + "tail");
}
TEST(EpubPorts, NestedTablesImagesAndMalformedStructureKeepReadableContent) {
  auto out = parse(
      "<html><body><table><tr><td>outer<table><tr><td>nested</td></tr></table><img src=\"x.png\" "
      "alt=\"picture\"/></td><td>end</td></tr></table></body></html>",
      300, 200);
  ASSERT_TRUE(out.ok);
  std::string all;
  for (const auto& l : lines(out)) all += l;
  EXPECT_NE(all.find("nested"), std::string::npos);
  EXPECT_NE(all.find("picture"), std::string::npos);
  EXPECT_NE(all.find("end"), std::string::npos);
  auto malformed =
      parse("<html><body><table><td>orphan</td><tr><td>cell</td></tr>bare</table></body></html>", 300, 200);
  ASSERT_TRUE(malformed.ok);
  all.clear();
  for (const auto& l : lines(malformed)) all += l;
  EXPECT_NE(all.find("orphan"), std::string::npos);
  EXPECT_NE(all.find("bare"), std::string::npos);
}
TEST(EpubPorts, FiveColumnTableFallsBackWithoutDroppingCells) {
  auto out = parse(
      "<html><body><table><tr><td>A</td><td>B</td><td>C</td><td>D</td><td>E</td></tr></table></body></html>", 500, 200);
  ASSERT_TRUE(out.ok);
  std::string all;
  for (const auto& l : lines(out)) all += l;
  EXPECT_EQ(all, "ABCDE");
}

TEST(EpubPorts, HtmlDictionaryCommonNamedEntitiesRemainStyled) {
  GfxRenderer renderer;
  std::vector<std::unique_ptr<Page>> pages;
  ASSERT_TRUE(buildDictionaryHtmlPages(renderer, "<b>word&nbsp;&copy;&eacute;&unknown;</b>", 300, 100, pages));
  bool bold = false;
  std::string text;
  for (const auto& page : pages)
    for (const auto& element : page->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto& block = static_cast<const PageLine&>(*element).getBlock();
      for (uint16_t i = 0; i < block->wordCount(); ++i) {
        text += block->wordText(i);
        bold |= (block->wordStyle(i) & EpdFontFamily::BOLD) != 0;
      }
    }
  EXPECT_TRUE(bold);
  EXPECT_NE(text.find("©é&unknown;"), std::string::npos);
}
TEST(EpubPorts, OversizedRubyStreamsBeforeClosingAndPreservesText) {
  HalStorage::resetTestFiles();
  std::string body;
  for (int i = 0; i < 10000; ++i) body += "字";
  const std::string source = "<html><body><p><ruby>" + body + "<rt>annotation</rt></ruby></p></body></html>";
  const std::string path = "/large-ruby.xhtml";
  HalStorage::registerTestFile(path, {source.begin(), source.end()});
  GfxRenderer renderer;
  Parsed out{};
  ChapterHtmlSlimParser parser(
      nullptr, path, renderer, 1, 1.f, false, 1, 100, 100, false, false,
      [&](std::unique_ptr<Page> page, uint16_t, uint16_t) { out.pages.push_back(std::move(page)); }, false, "", "");
  ASSERT_TRUE(parser.beginParse());
  for (int i = 0; i < 4; ++i) ASSERT_EQ(parser.parseStep(), ChapterHtmlSlimParser::ParseStatus::More);
  EXPECT_FALSE(out.pages.empty()) << "oversized ruby must not retain the whole document before emitting pages";
  while (true) {
    const auto status = parser.parseStep();
    ASSERT_NE(status, ChapterHtmlSlimParser::ParseStatus::Error);
    if (status == ChapterHtmlSlimParser::ParseStatus::Done) break;
  }
  ASSERT_TRUE(parser.finishParse());
  std::string text;
  for (const auto& line : lines(out)) text += line;
  EXPECT_EQ(text, body + "annotation");
}
TEST(EpubPorts, ForeignMetadataNamespaceCannotOverrideDublinCore) {
  const std::string source =
      "<package xmlns=\"http://www.idpf.org/2007/opf\" xmlns:vendor=\"urn:vendor\" "
      "xmlns:d=\"http://purl.org/dc/elements/1.1/\"><metadata><vendor:title>Wrong</vendor:title><d:title>Book</"
      "d:title><vendor:creator>Wrong author</vendor:creator><d:creator>Author</d:creator></metadata></package>";
  const std::string cache = "/cache", base = "";
  ContentOpfParser parser(cache, base, source.size(), nullptr);
  ASSERT_TRUE(parser.setup());
  ASSERT_EQ(parser.write(reinterpret_cast<const uint8_t*>(source.data()), source.size()), source.size());
  EXPECT_EQ(parser.title, "Book");
  EXPECT_EQ(parser.author, "Author");
}
TEST(EpubPorts, MetadataAuthorChunksAndSeparatorsStayBounded) {
  const std::string cache = "/cache", base = "";
  const auto document = [](const std::string& authors) {
    return "<package xmlns=\"http://www.idpf.org/2007/opf\" xmlns:d=\"http://purl.org/dc/elements/1.1/\"><metadata>" +
           authors + "</metadata></package>";
  };
  for (const bool large : {false, true}) {
    const std::string source = document(large ? "<d:creator>" + std::string(700, 'A') + "</d:creator><d:creator>" +
                                                    std::string(700, 'B') + "</d:creator>"
                                              : "<d:creator>甲乙</d:creator><d:creator>丙丁</d:creator>");
    ContentOpfParser parser(cache, base, source.size(), nullptr);
    ASSERT_TRUE(parser.setup());
    for (size_t i = 0; i < source.size(); ++i)
      ASSERT_EQ(parser.write(reinterpret_cast<const uint8_t*>(source.data() + i), 1), 1u);
    if (large) {
      EXPECT_EQ(parser.author, std::string(ContentOpfParser::MAX_ACCUMULATED_FIELD_LEN, 'A'));
      EXPECT_TRUE(parser.authorTruncated);
    } else {
      EXPECT_EQ(parser.author, "甲乙, 丙丁");
      EXPECT_FALSE(parser.authorTruncated);
    }
  }
}
