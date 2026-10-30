#!/usr/bin/env python3
"""Unit tests for fontconvert_sdcard.py's charset handling: resolve_intervals,
merge_intervals, parse_charset_file, and their combination in main()'s
interval-resolution step. Stdlib unittest only.

Run: python3 lib/EpdFont/scripts/test_fontconvert_sdcard_charset.py
(from the repo root, or anywhere -- sys.path is adjusted below).

One slow, opt-in integration test (`SlowIntegrationTests`) actually invokes
the CLI end-to-end against the repo's own NotoSans-Regular.ttf source fixture
with a tiny --charset-file, to prove the new code path really produces a
loadable .cpfont file, not just that the Python parses. Requires
freetype-py/fontTools (this project's own lib/EpdFont/scripts/requirements.txt);
skips itself cleanly if they're not installed.
"""

import os
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(__file__))
import fontconvert_sdcard as fcs  # noqa: E402
import repack_cpfont_1bit as repack  # noqa: E402

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
NOTOSANS_TTF = os.path.join(
    REPO_ROOT, "lib", "EpdFont", "builtinFonts", "source", "NotoSans", "NotoSans-Regular.ttf"
)
SCRIPT_PATH = os.path.join(os.path.dirname(__file__), "fontconvert_sdcard.py")


class ResolveIntervalsContractTests(unittest.TestCase):
    """resolve_intervals() was refactored to return a RAW (unmerged, no
    replacement-char) list -- callers now merge explicitly. Guards against
    regressing that contract, which main() depends on."""

    def test_resolve_intervals_does_not_auto_merge(self):
        # "ascii" and an adjacent unnamed range should NOT come back merged --
        # that's now merge_intervals()'s job, not resolve_intervals()'s.
        raw = fcs.resolve_intervals("ascii,(0x007F-0x0080)")
        self.assertIn((0x0020, 0x007E), raw)
        self.assertIn((0x007F, 0x0080), raw)
        # Not merged into one (0x0020, 0x0080) span:
        self.assertNotIn((0x0020, 0x0080), raw)

    def test_resolve_intervals_does_not_append_replacement_char(self):
        raw = fcs.resolve_intervals("ascii")
        self.assertNotIn((0xFFFD, 0xFFFD), raw)

    def test_resolve_intervals_unknown_preset_exits(self):
        with self.assertRaises(SystemExit):
            fcs.resolve_intervals("not-a-real-preset")


class MergeIntervalsTests(unittest.TestCase):
    def test_merges_overlapping_and_adjacent(self):
        merged = fcs.merge_intervals([(0x20, 0x30), (0x28, 0x40), (0x41, 0x41), (0x100, 0x110)])
        self.assertEqual(merged, [(0x20, 0x41), (0x100, 0x110)])

    def test_empty_input(self):
        self.assertEqual(fcs.merge_intervals([]), [])

    def test_disjoint_stays_disjoint(self):
        merged = fcs.merge_intervals([(0x10, 0x20), (0x30, 0x40)])
        self.assertEqual(merged, [(0x10, 0x20), (0x30, 0x40)])


class ParseCharsetFileTests(unittest.TestCase):
    def _write(self, content):
        f = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="ascii")
        f.write(content)
        f.close()
        self.addCleanup(os.unlink, f.name)
        return f.name

    def test_basic_hex_and_comments(self):
        path = self._write("# a comment\n0x4E2D 0x4E2D\n\n0x3000 0x303F\n")
        self.assertEqual(fcs.parse_charset_file(path), [(0x4E2D, 0x4E2D), (0x3000, 0x303F)])

    def test_decimal_also_accepted(self):
        path = self._write("19968 19968\n")
        self.assertEqual(fcs.parse_charset_file(path), [(19968, 19968)])

    def test_invalid_line_shape_exits(self):
        path = self._write("0x20\n")
        with self.assertRaises(SystemExit):
            fcs.parse_charset_file(path)

    def test_start_greater_than_end_exits(self):
        path = self._write("0x30 0x20\n")
        with self.assertRaises(SystemExit):
            fcs.parse_charset_file(path)

    def test_out_of_unicode_range_exits(self):
        path = self._write("0x0 0x110000\n")
        with self.assertRaises(SystemExit):
            fcs.parse_charset_file(path)


class CombinedResolutionTests(unittest.TestCase):
    """Mirrors main()'s new interval-resolution step (resolve_intervals +
    parse_charset_file + always-append 0xFFFD + merge_intervals) directly,
    since main() itself isn't a pure function to call from a test."""

    def _resolve(self, intervals_arg, charset_file_path):
        raw = []
        if intervals_arg:
            raw.extend(fcs.resolve_intervals(intervals_arg))
        if charset_file_path:
            raw.extend(fcs.parse_charset_file(charset_file_path))
        raw.append((0xFFFD, 0xFFFD))
        return fcs.merge_intervals(raw)

    def test_intervals_only_still_works(self):
        result = self._resolve("ascii", None)
        self.assertIn((0xFFFD, 0xFFFD), result)
        self.assertTrue(any(a <= 0x41 <= b for a, b in result))

    def test_charset_file_only(self):
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="ascii") as tf:
            path = tf.name
        with open(path, "w", encoding="ascii") as f:
            f.write("0x4E2D 0x4E2D\n")
        self.addCleanup(os.unlink, path)
        result = self._resolve(None, path)
        self.assertIn((0x4E2D, 0x4E2D), result)
        self.assertIn((0xFFFD, 0xFFFD), result)

    def test_both_combine_and_merge(self):
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="ascii") as tf:
            path = tf.name
        with open(path, "w", encoding="ascii") as f:
            f.write("0x007F 0x0080\n")  # adjacent to ascii's 0x0020-0x007E
        self.addCleanup(os.unlink, path)
        result = self._resolve("ascii", path)
        self.assertIn((0x0020, 0x0080), result)  # merged across the two sources


class BitmapPackingTests(unittest.TestCase):
    def test_one_bit_packs_eight_pixels_msb_first(self):
        # >= 4 is visible in the existing 2-bit BW renderer; preserve that
        # threshold when flattening grayscale glyphs to the X3 fast 1-bit form.
        pixels4 = [0, 3, 4, 7, 8, 11, 12, 15]
        self.assertEqual(fcs.pack_4bit_pixels(pixels4, is_2bit=False), bytes([0b00111111]))

    def test_two_bit_default_keeps_existing_quantisation(self):
        pixels4 = [0, 4, 8, 12]
        self.assertEqual(fcs.pack_4bit_pixels(pixels4, is_2bit=True), bytes([0b00011011]))

    def test_repack_two_bit_glyph_to_one_bit(self):
        # Four 2-bit pixels: white, light, dark, black -> one 1-bit byte.
        self.assertEqual(repack.repack_glyph_bitmap(bytes([0b00011011]), 4), bytes([0b01110000]))


class RepackFileTests(unittest.TestCase):
    def test_repack_preserves_two_style_layout_and_rewrites_offsets(self):
        def style_blob(bitmap):
            interval = struct.pack("<III", 0x41, 0x41, 0)
            glyph = struct.pack(repack.GLYPH_FORMAT, 4, 2, 5, -1, 2, 2, 0)
            return interval + glyph + bitmap

        style0 = style_blob(bytes([0b00011011, 0b11100100]))
        style1 = style_blob(bytes([0b11100100, 0b00011011]))
        data_start = repack.HEADER_SIZE + 2 * repack.TOC_SIZE
        toc0 = struct.pack(repack.TOC_FORMAT, 0, 1, 1, 4, 3, -1, 0, 0, 0, 0, 0, data_start)
        toc1 = struct.pack(repack.TOC_FORMAT, 1, 1, 1, 4, 3, -1, 0, 0, 0, 0, 0, data_start + len(style0))
        header = struct.pack(repack.HEADER_FORMAT, repack.MAGIC, repack.CPFONT_VERSION, 1, 2, bytes(19))

        with tempfile.TemporaryDirectory() as d:
            source = os.path.join(d, "source.cpfont")
            target = os.path.join(d, "target.cpfont")
            with open(source, "wb") as f:
                f.write(header + toc0 + toc1 + style0 + style1)
            repack.repack_file(source, target)
            with open(target, "rb") as f:
                converted = f.read()

        _, version, flags, style_count, _ = struct.unpack_from(repack.HEADER_FORMAT, converted)
        self.assertEqual(version, repack.CPFONT_VERSION)
        self.assertEqual(flags & 1, 0)
        self.assertEqual(style_count, 2)
        new_toc0 = struct.unpack_from(repack.TOC_FORMAT, converted, repack.HEADER_SIZE)
        new_toc1 = struct.unpack_from(repack.TOC_FORMAT, converted, repack.HEADER_SIZE + repack.TOC_SIZE)
        self.assertEqual(new_toc0[:11], struct.unpack(repack.TOC_FORMAT, toc0)[:11])
        self.assertEqual(new_toc1[:11], struct.unpack(repack.TOC_FORMAT, toc1)[:11])
        self.assertEqual(new_toc0[11], data_start)
        self.assertEqual(new_toc1[11], data_start + len(style0) - 1)
        glyph0 = struct.unpack_from(repack.GLYPH_FORMAT, converted, data_start + 12)
        glyph1 = struct.unpack_from(repack.GLYPH_FORMAT, converted, new_toc1[11] + 12)
        self.assertEqual(glyph0[:5], (4, 2, 5, -1, 2))
        self.assertEqual(glyph1[:5], (4, 2, 5, -1, 2))
        self.assertEqual((glyph0[5], glyph0[6]), (1, 0))
        self.assertEqual((glyph1[5], glyph1[6]), (1, 0))


class SlowIntegrationTests(unittest.TestCase):
    """End-to-end CLI smoke test: real rasterization of a tiny sparse charset
    against a real font, producing an actual .cpfont file."""

    def test_charset_file_produces_loadable_cpfont(self):
        try:
            import freetype  # noqa: F401
        except ImportError:
            self.skipTest("freetype-py not installed")
        if not os.path.isfile(NOTOSANS_TTF):
            self.skipTest("NotoSans-Regular.ttf source fixture not found")

        with tempfile.TemporaryDirectory() as d:
            charset_path = os.path.join(d, "charset.txt")
            with open(charset_path, "w", encoding="ascii") as f:
                f.write("0x0041 0x0042\n")  # just 'A','B' -- keep this test fast
            out_path = os.path.join(d, "Test_12.cpfont")
            result = subprocess.run(
                [sys.executable, SCRIPT_PATH, NOTOSANS_TTF,
                 "--charset-file", charset_path, "--size", "12", "--style", "regular",
                 "-o", out_path],
                capture_output=True, text=True, timeout=120,
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            self.assertTrue(os.path.isfile(out_path))
            with open(out_path, "rb") as f:
                header = f.read(12)
            magic = header[:8]
            self.assertEqual(magic, b"CPFONT\x00\x00")
            self.assertEqual(int.from_bytes(header[10:12], "little") & 1, 1)

    def test_one_bit_flag_and_size(self):
        try:
            import freetype  # noqa: F401
        except ImportError:
            self.skipTest("freetype-py not installed")
        if not os.path.isfile(NOTOSANS_TTF):
            self.skipTest("NotoSans-Regular.ttf source fixture not found")

        with tempfile.TemporaryDirectory() as d:
            charset_path = os.path.join(d, "charset.txt")
            with open(charset_path, "w", encoding="ascii") as f:
                f.write("0x0041 0x005A\n")
            out_2bit = os.path.join(d, "Test2_28.cpfont")
            out_1bit = os.path.join(d, "Test1_28.cpfont")
            common = [sys.executable, SCRIPT_PATH, NOTOSANS_TTF,
                      "--charset-file", charset_path, "--size", "28", "--style", "regular"]
            result_2bit = subprocess.run(common + ["-o", out_2bit], capture_output=True, text=True, timeout=120)
            result_1bit = subprocess.run(common + ["--1bit", "-o", out_1bit], capture_output=True, text=True,
                                         timeout=120)
            self.assertEqual(result_2bit.returncode, 0, msg=result_2bit.stderr)
            self.assertEqual(result_1bit.returncode, 0, msg=result_1bit.stderr)
            with open(out_1bit, "rb") as f:
                header = f.read(12)
            self.assertEqual(header[:8], b"CPFONT\x00\x00")
            self.assertEqual(int.from_bytes(header[10:12], "little") & 1, 0)
            self.assertLess(os.path.getsize(out_1bit), os.path.getsize(out_2bit))


if __name__ == "__main__":
    unittest.main()
