#!/usr/bin/env python3
"""Unit tests for analyze_epub_charset.py. Stdlib unittest only -- no pytest,
no network. Run: python3 -m unittest lib.EpdFont.scripts.test_analyze_epub_charset -v
(from the repo root) or `python3 lib/EpdFont/scripts/test_analyze_epub_charset.py`.

Uses the repository's own tiny EPUB fixtures under test/epubs/ and the
NotoSans-Regular.ttf builtin-font source (both already in-repo) so this suite
needs no network access and no private/non-repo assets.
"""

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(__file__))
import analyze_epub_charset as aec  # noqa: E402

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
TEST_EPUBS_DIR = os.path.join(REPO_ROOT, "test", "epubs")
NOTOSANS_TTF = os.path.join(
    REPO_ROOT, "lib", "EpdFont", "builtinFonts", "source", "NotoSans", "NotoSans-Regular.ttf"
)


class StripHtmlTests(unittest.TestCase):
    def test_basic_tags_and_entities(self):
        html_doc = (
            '<?xml version="1.0"?><html><body><p>Hello&nbsp;&amp; welcome</p></body></html>'
        )
        text = aec.strip_html(html_doc)
        self.assertIn("Hello", text)
        self.assertIn("welcome", text)
        self.assertIn("&", text)
        self.assertNotIn("<p>", text)

    def test_skips_script_and_style(self):
        html_doc = (
            "<html><head><style>body{color:red}</style></head>"
            "<body><script>var x=1;</script><p>Real text</p></body></html>"
        )
        text = aec.strip_html(html_doc)
        self.assertIn("Real text", text)
        self.assertNotIn("color:red", text)
        self.assertNotIn("var x=1", text)

    def test_malformed_document_falls_back_to_regex(self):
        # Missing closing tags -- not well-formed XML, must not raise.
        html_doc = "<html><body><p>Broken <b>markup here"
        text = aec.strip_html(html_doc)
        self.assertIn("Broken", text)
        self.assertIn("markup here", text)

    def test_numeric_entity_decoded(self):
        html_doc = "<p>&#x4E2D;&#25991;</p>"
        text = aec.strip_html_regex(html_doc)
        self.assertIn("中", text)
        self.assertIn("文", text)


class MergeIntervalTests(unittest.TestCase):
    def test_merge_adjacent_basic(self):
        result = aec.merge_adjacent([1, 2, 3, 5, 6, 9])
        self.assertEqual(result, [(1, 3), (5, 6), (9, 9)])

    def test_merge_adjacent_empty(self):
        self.assertEqual(aec.merge_adjacent([]), [])

    def test_merge_adjacent_single(self):
        self.assertEqual(aec.merge_adjacent([42]), [(42, 42)])

    def test_merge_with_budget_no_merge_needed(self):
        cps = [1, 2, 5, 6, 100]
        intervals, extra = aec.merge_with_budget(cps, max_intervals=10)
        self.assertEqual(intervals, [(1, 2), (5, 6), (100, 100)])
        self.assertEqual(extra, 0)

    def test_merge_with_budget_merges_cheapest_gap_first(self):
        # Three natural intervals: [1,2] gap=2 to [5,6] gap=10 to [17,18].
        # Budget of 2 must merge the cheaper (gap=2) pair first, leaving
        # [1,6] and [17,18] -- not the gap=10 pair.
        cps = [1, 2, 5, 6, 17, 18]
        intervals, extra = aec.merge_with_budget(cps, max_intervals=2)
        self.assertEqual(intervals, [(1, 6), (17, 18)])
        self.assertEqual(extra, 2)  # codepoints 3,4 filled in

    def test_merge_with_budget_extra_glyph_accounting(self):
        cps = [1, 2, 10, 11, 20, 21]
        intervals, extra = aec.merge_with_budget(cps, max_intervals=1)
        self.assertEqual(len(intervals), 1)
        self.assertEqual(intervals[0], (1, 21))
        # Original selected codepoints: 6. Final span covers 21 codepoints.
        self.assertEqual(extra, 21 - 6)

    def test_font_covers_range(self):
        covered = [10, 11, 12, 13, 20]
        self.assertTrue(aec._font_covers_range(covered, 10, 13))
        self.assertFalse(aec._font_covers_range(covered, 10, 14))  # 14 missing
        self.assertTrue(aec._font_covers_range(covered, 20, 20))

    def test_merge_with_budget_font_aware_skips_unpavable_gap(self):
        # Gap between (1,2) and (5,6) is codepoints {3,4}; only 3 is font-
        # covered, so that merge is ineligible and must NOT happen even
        # though it's the cheapest gap. The (10,11)-(13,14) gap (just {12})
        # is fully covered and must be the one that merges instead.
        cps = [1, 2, 5, 6, 10, 11, 13, 14]
        font_covered = {1, 2, 3, 5, 6, 10, 11, 12, 13, 14}  # missing: 4
        intervals, extra = aec.merge_with_budget(cps, max_intervals=3, font_covered=font_covered)
        self.assertEqual(intervals, [(1, 2), (5, 6), (10, 14)])
        self.assertEqual(extra, 1)  # only codepoint 12 filled in

    def test_merge_with_budget_font_aware_floor_when_unreachable(self):
        # No gap is fully font-covered at all -- budget cannot be met;
        # function must return the achievable floor (no merges), not lie.
        cps = [1, 2, 10, 11, 20, 21]
        font_covered = {1, 2, 10, 11, 20, 21}  # none of the gap codepoints
        intervals, extra = aec.merge_with_budget(cps, max_intervals=1, font_covered=font_covered)
        self.assertEqual(intervals, [(1, 2), (10, 11), (20, 21)])
        self.assertEqual(extra, 0)

    def test_merge_with_budget_font_aware_none_means_assume_coverable(self):
        # font_covered=None (the default) preserves the original behavior:
        # every gap is assumed mergeable.
        cps = [1, 2, 5, 6]
        intervals, extra = aec.merge_with_budget(cps, max_intervals=1, font_covered=None)
        self.assertEqual(intervals, [(1, 6)])
        self.assertEqual(extra, 2)

    def test_merge_with_budget_already_at_budget(self):
        cps = [1, 2, 3]
        intervals, extra = aec.merge_with_budget(cps, max_intervals=1)
        self.assertEqual(intervals, [(1, 3)])
        self.assertEqual(extra, 0)


class HanClassificationTests(unittest.TestCase):
    def test_common_han_codepoint(self):
        self.assertTrue(aec.is_han(0x4E2D))  # 中

    def test_extension_a_is_han(self):
        self.assertTrue(aec.is_han(0x3400))

    def test_ascii_is_not_han(self):
        self.assertFalse(aec.is_han(ord("A")))

    def test_hiragana_is_not_han(self):
        self.assertFalse(aec.is_han(0x3042))


class SelectByRuleTests(unittest.TestCase):
    def setUp(self):
        from collections import Counter

        # Two Han chars seen 5x and 1x, one Latin char seen once (non-Han
        # codepoints are never subject to the threshold).
        self.counts = Counter({0x4E2D: 5, 0x6587: 1, ord("Q"): 1})

    def test_count1_includes_everything(self):
        self.assertEqual(aec.select_by_rule(self.counts, "count1"), {0x4E2D, 0x6587, ord("Q")})

    def test_count2_drops_rare_han_but_keeps_rare_latin(self):
        selected = aec.select_by_rule(self.counts, "count2")
        self.assertIn(0x4E2D, selected)
        self.assertNotIn(0x6587, selected)
        self.assertIn(ord("Q"), selected)  # non-Han is never thresholded

    def test_han99_99_keeps_dominant_mass(self):
        selected = aec.select_by_rule(self.counts, "han99_99")
        self.assertIn(0x4E2D, selected)  # 5/6 of Han mass, always kept
        self.assertIn(ord("Q"), selected)


class YamlValueLoaderTests(unittest.TestCase):
    def test_loads_values_and_skips_metadata_keys(self):
        with tempfile.NamedTemporaryFile("w", suffix=".yaml", delete=False, encoding="utf-8") as f:
            f.write('_language_name: "Test"\n_order: "99"\n\n')
            f.write('STR_HELLO: "你好"\n')
            f.write('STR_ESCAPED: "line one\\nline two, \\"quoted\\", back\\\\slash"\n')
            path = f.name
        try:
            values = aec.load_yaml_string_values(path)
        finally:
            os.unlink(path)
        self.assertEqual(values, ["你好", 'line one\nline two, "quoted", back\\slash'])


class EpubScanTests(unittest.TestCase):
    def test_scan_one_epub_fixture(self):
        path = os.path.join(TEST_EPUBS_DIR, "test_supsub.epub")
        if not os.path.isfile(path):
            self.skipTest("repo fixture test_supsub.epub not found")
        body_counts, meta_text = aec.scan_one_epub(path)
        self.assertGreater(sum(body_counts.values()), 0)
        self.assertIn("Subscript", meta_text)  # from dc:title

    def test_scan_corpus_over_fixtures_dir(self):
        if not os.path.isdir(TEST_EPUBS_DIR):
            self.skipTest("test/epubs fixture directory not found")
        expected = len([f for f in os.listdir(TEST_EPUBS_DIR) if f.lower().endswith(".epub")])
        if expected == 0:
            self.skipTest("no .epub fixtures present")
        result = aec.scan_corpus(TEST_EPUBS_DIR)
        self.assertEqual(result["book_count"], expected)
        self.assertEqual(result["errors"], [])
        self.assertGreater(len(result["body_counts"]), 0)


class ManifestRoundTripTests(unittest.TestCase):
    def test_write_manifest_produces_parseable_file(self):
        intervals = [(0x20, 0x7E), (0x4E2D, 0x4E2D)]
        with tempfile.TemporaryDirectory() as d:
            out_path = os.path.join(d, "manifest.txt")
            aec.write_manifest(intervals, out_path, "test header")
            with open(out_path, encoding="ascii") as f:
                content = f.read()
        self.assertIn("# test header", content)
        self.assertIn("0x0020 0x007E", content)
        self.assertIn("0x4E2D 0x4E2D", content)


class MissingGlyphCheckTests(unittest.TestCase):
    def test_missing_glyphs_against_latin_only_font(self):
        if not os.path.isfile(NOTOSANS_TTF):
            self.skipTest("NotoSans-Regular.ttf source fixture not found")
        try:
            import fontTools  # noqa: F401
        except ImportError:
            self.skipTest("fontTools not installed")
        missing = aec.check_missing_glyphs({ord("A"), 0x4E2D}, NOTOSANS_TTF)
        self.assertNotIn(ord("A"), missing)
        self.assertIn(0x4E2D, missing)


class CliSmokeTests(unittest.TestCase):
    def test_scan_cli_emits_valid_json(self):
        if not os.path.isdir(TEST_EPUBS_DIR):
            self.skipTest("test/epubs fixture directory not found")
        import io
        from contextlib import redirect_stdout

        buf = io.StringIO()
        with redirect_stdout(buf):
            aec.main(["scan", "--corpus-dir", TEST_EPUBS_DIR])
        data = json.loads(buf.getvalue())
        self.assertIn("book_count", data)
        self.assertIn("body_counts", data)


if __name__ == "__main__":
    unittest.main()
