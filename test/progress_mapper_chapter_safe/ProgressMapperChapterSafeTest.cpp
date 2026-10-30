#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include <Epub/ReaderRenderSpec.h>

#include "ProgressMapper.h"

namespace {

const std::vector<std::string> kChapters = {
    "<html><body><p>front matter front matter</p><p>more front matter</p></body></html>",
    "<html><body><div><section><p>alpha alpha alpha</p><p>bravo bravo bravo</p>"
    "<p>charlie charlie charlie</p></section></div></body></html>",
    "<html><body><p>delta delta delta</p><p>echo echo echo</p></body></html>",
};

std::shared_ptr<Epub> makeBook(const std::vector<size_t>& weights = {900, 50, 50}) {
  return std::make_shared<Epub>(kChapters, weights);
}

TEST(ProgressMapperChapterSafe, StructuralChapterWinsOverConflictingCrossEnginePercentage) {
  auto epub = makeBook();
  epub->setCachedPageCount(0, 8);
  epub->setCachedPageCount(1, 9);
  GfxRenderer renderer;

  const auto mapped = ProgressMapper::toCrossPoint(
      epub, {"/body/DocFragment[2]/body/div[1]/section[1]/p[2]/text()[1].3", 0.10f}, renderer);

  EXPECT_EQ(mapped.spineIndex, 1);
  EXPECT_EQ(mapped.totalPages, 9);
}

TEST(ProgressMapperChapterSafe, MissingCacheStaysWithinStructuralChapterAndRemainsApproximate) {
  auto epub = makeBook({100, 100, 100});
  epub->setBoundedLayoutPageCount(1, 11);
  GfxRenderer renderer;
  ReaderRenderSpec renderSpec;
  renderSpec.viewportWidth = 440;
  renderSpec.viewportHeight = 720;

  const auto mapped = ProgressMapper::toCrossPoint(
      epub, {"/body/DocFragment[2]/body/div[1]/section[1]/p[2]/text()[1].3", 0.55f}, renderer,
      /*currentSpineIndex=*/-1, /*totalPagesInCurrentSpine=*/0, /*fallbackTotalPages=*/0, &renderSpec);

  EXPECT_EQ(mapped.spineIndex, 1);
  EXPECT_EQ(mapped.totalPages, 2);
  EXPECT_GE(mapped.pageNumber, 0);
  EXPECT_LT(mapped.pageNumber, mapped.totalPages);
  EXPECT_EQ(epub->layoutBuildAttempts(), 0);
  EXPECT_EQ(epub->finalizedLayoutBuilds(), 0);
  EXPECT_EQ(epub->normalCssLoadCalls(), 0);
  EXPECT_FALSE(epub->cachedPageCount(1).has_value());

  // The reader later completes compatible pagination through its budgeted loop. Mapping
  // consumes that complete cache without doing a second build.
  epub->setCachedPageCount(1, 11);
  const auto reopened = ProgressMapper::toCrossPoint(
      epub, {"/body/DocFragment[2]/body/div[1]/section[1]/p[2]/text()[1].3", 0.55f}, renderer,
      -1, 0, 0, &renderSpec);
  EXPECT_EQ(reopened.spineIndex, mapped.spineIndex);
  EXPECT_EQ(reopened.totalPages, 11);
  EXPECT_EQ(epub->layoutBuildAttempts(), 0);
}

TEST(ProgressMapperChapterSafe, FailedLayoutStillUsesCoarseChapterLocalPositionWithoutJumping) {
  auto epub = makeBook({100, 100, 100});
  GfxRenderer renderer;
  ReaderRenderSpec renderSpec;
  renderSpec.viewportWidth = 440;
  renderSpec.viewportHeight = 720;

  const auto mapped = ProgressMapper::toCrossPoint(
      epub, {"/body/DocFragment[2]/body/div[1]/section[1]/p[3]/text()[1].5", 0.60f}, renderer,
      /*currentSpineIndex=*/-1, /*totalPagesInCurrentSpine=*/0, /*fallbackTotalPages=*/0, &renderSpec);

  EXPECT_EQ(mapped.spineIndex, 1);
  EXPECT_EQ(mapped.totalPages, 2);
  EXPECT_GE(mapped.pageNumber, 0);
  EXPECT_LT(mapped.pageNumber, mapped.totalPages);
  EXPECT_EQ(epub->normalCssLoadCalls(), 0);
  EXPECT_EQ(epub->layoutBuildAttempts(), 0);
  EXPECT_EQ(epub->finalizedLayoutBuilds(), 0);
}

TEST(ProgressMapperChapterSafe, ConfirmedOnePageChapterNeverSynthesizesNextChapter) {
  auto epub = makeBook({100, 100, 100});
  epub->setCachedPageCount(1, 1);
  epub->setCachedPageCount(2, 6);
  GfxRenderer renderer;

  const auto mapped = ProgressMapper::toCrossPoint(
      epub, {"/body/DocFragment[2]/body/div[1]/section[1]/p[3]/text()[1].5", 0.60f}, renderer);

  EXPECT_EQ(mapped.spineIndex, 1);
  EXPECT_EQ(mapped.pageNumber, 0);
  EXPECT_EQ(mapped.totalPages, 1);
}

TEST(ProgressMapperChapterSafe, OutOfRangeDocFragmentFallsBackToWholeBookPercentage) {
  auto epub = makeBook({100, 100, 100});
  epub->setCachedPageCount(1, 7);
  GfxRenderer renderer;

  const auto mapped = ProgressMapper::toCrossPoint(
      epub, {"/body/DocFragment[99]/body/p[2]/text()[1].2", 0.55f}, renderer);

  EXPECT_EQ(mapped.spineIndex, 1);
  EXPECT_EQ(mapped.totalPages, 7);
}

TEST(ProgressMapperChapterSafe, UnresolvableElementFallsBackWithinItsValidStructuralChapter) {
  auto epub = makeBook();
  epub->setCachedPageCount(1, 9);
  GfxRenderer renderer;

  const auto mapped = ProgressMapper::toCrossPoint(
      epub, {"/body/DocFragment[2]/body/div[99]/p[999]/text()[1].20", 0.10f}, renderer);

  EXPECT_EQ(mapped.spineIndex, 1);
  EXPECT_EQ(mapped.totalPages, 9);
  EXPECT_GE(mapped.pageNumber, 0);
  EXPECT_LT(mapped.pageNumber, mapped.totalPages);
}

TEST(ProgressMapperChapterSafe, NestedStructuralPathResolvesInsideItsChapter) {
  auto epub = makeBook({100, 100, 100});
  epub->setCachedPageCount(1, 9);
  GfxRenderer renderer;

  const auto mapped = ProgressMapper::toCrossPoint(
      epub, {"/body/DocFragment[2]/body/div[1]/section[1]/p[2]/text()[1].3", 0.50f}, renderer);

  EXPECT_EQ(mapped.spineIndex, 1);
  EXPECT_TRUE(mapped.hasParagraphIndex);
  EXPECT_EQ(mapped.paragraphIndex, 2);
  EXPECT_GE(mapped.pageNumber, 1);
  EXPECT_LT(mapped.pageNumber, mapped.totalPages);
}

TEST(ProgressMapperChapterSafe, ChapterStartStaysAtRequestedChapterDespitePercentageConflict) {
  auto epub = makeBook();
  epub->setCachedPageCount(0, 8);
  epub->setCachedPageCount(1, 9);
  GfxRenderer renderer;

  const auto mapped =
      ProgressMapper::toCrossPoint(epub, {"/body/DocFragment[2]/body", 0.02f}, renderer);

  EXPECT_EQ(mapped.spineIndex, 1);
  EXPECT_EQ(mapped.pageNumber, 0);
}

TEST(ProgressMapperChapterSafe, InvalidXPathCanStillReachTrueBookEndByPercentage) {
  auto epub = makeBook({100, 100, 100});
  epub->setCachedPageCount(2, 5);
  GfxRenderer renderer;

  const auto mapped = ProgressMapper::toCrossPoint(epub, {"not-an-xpath", 1.0f}, renderer);

  EXPECT_EQ(mapped.spineIndex, 2);
  EXPECT_EQ(mapped.pageNumber, 4);
  EXPECT_EQ(mapped.totalPages, 5);
}

TEST(ProgressMapperChapterSafe, ValidNewChapterStartStillMovesForward) {
  auto epub = makeBook();
  epub->setCachedPageCount(2, 5);
  GfxRenderer renderer;

  const auto mapped =
      ProgressMapper::toCrossPoint(epub, {"/body/DocFragment[3]/body", 0.95f}, renderer);

  EXPECT_EQ(mapped.spineIndex, 2);
  EXPECT_EQ(mapped.pageNumber, 0);
}

TEST(ProgressMapperChapterSafe, OutgoingPositionRoundTripsWithinTheSameChapter) {
  auto epub = makeBook();
  epub->setCachedPageCount(1, 9);
  GfxRenderer renderer;
  const CrossPointPosition local{1, 4, 9};

  const SavedProgressPosition saved = ProgressMapper::toSavedProgress(epub, local);
  const CrossPointPosition mapped = ProgressMapper::toCrossPoint(epub, saved, renderer);

  EXPECT_NE(saved.xpath.find("/body/DocFragment[2]/body/"), std::string::npos);
  EXPECT_NEAR(saved.percentage, 0.925f, 0.0001f);
  EXPECT_EQ(mapped.spineIndex, 1);
  EXPECT_NEAR(mapped.pageNumber, local.pageNumber, 1);
}

TEST(ProgressMapperChapterSafe, UncachedChapterStartNeedsNoLayoutOrXhtmlStream) {
  auto epub = makeBook();
  GfxRenderer renderer;
  ReaderRenderSpec spec;
  spec.viewportWidth = 440;
  spec.viewportHeight = 720;
  const auto result = ProgressMapper::toCrossPoint(epub, {"/body/DocFragment[2]/body", 0.02f}, renderer,
                                                   -1, 0, 0, &spec);
  EXPECT_EQ(result.spineIndex, 1);
  EXPECT_EQ(result.pageNumber, 0);
  EXPECT_EQ(result.totalPages, 0);  // unknown, never a fabricated one-page chapter
  EXPECT_EQ(epub->layoutBuildAttempts(), 0);
  EXPECT_EQ(epub->streamCalls(), 0);
}

}  // namespace
