#!/usr/bin/env python3
"""Repack an existing v4 2-bit .cpfont as v4 1-bit without the source TTF.

This is useful for private/custom font assets whose metrics and sparse glyph
selection are already validated. It preserves every interval, metric, kerning
table, ligature and style, changing only glyph bitmap depth and offsets.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

from cpfont_version import CPFONT_VERSION

MAGIC = b"CPFONT\x00\x00"
HEADER_FORMAT = "<8sHHB19s"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
TOC_FORMAT = "<B3xIIBhhHHBBBI4x"
TOC_SIZE = struct.calcsize(TOC_FORMAT)
GLYPH_FORMAT = "<BBHhhH2xI"
GLYPH_SIZE = struct.calcsize(GLYPH_FORMAT)


def repack_glyph_bitmap(bitmap_2bit: bytes, pixel_count: int) -> bytes:
    """Flatten MSB-first 2-bit coverage to MSB-first 1-bit ink.

    CrossFade's BW renderer paints every non-zero 2-bit coverage value, so the
    1-bit asset uses the same rule. The final byte is padded with white bits.
    """
    output = bytearray((pixel_count + 7) // 8)
    for pos in range(pixel_count):
        raw = (bitmap_2bit[pos >> 2] >> ((3 - (pos & 3)) * 2)) & 0x3
        if raw:
            output[pos >> 3] |= 1 << (7 - (pos & 7))
    return bytes(output)


def repack_file(input_path: str, output_path: str) -> tuple[int, int]:
    with open(input_path, "rb") as source:
        original = source.read()
    if len(original) < HEADER_SIZE:
        raise ValueError("input is shorter than the cpfont header")

    magic, version, flags, style_count, reserved = struct.unpack_from(HEADER_FORMAT, original, 0)
    if magic != MAGIC:
        raise ValueError(f"bad cpfont magic: {magic!r}")
    if version != CPFONT_VERSION:
        raise ValueError(f"cpfont version {version} != supported {CPFONT_VERSION}")
    if not (flags & 1):
        raise ValueError("input cpfont is already 1-bit")
    if len(original) < HEADER_SIZE + style_count * TOC_SIZE:
        raise ValueError("truncated cpfont style table")

    toc_entries = []
    for index in range(style_count):
        toc_entries.append(list(struct.unpack_from(TOC_FORMAT, original, HEADER_SIZE + index * TOC_SIZE)))

    rebuilt_styles = []
    old_bitmap_total = 0
    new_bitmap_total = 0
    for index, toc in enumerate(toc_entries):
        (style_id, interval_count, glyph_count, _advance_y, _ascender, _descender,
         kern_left_count, kern_right_count, kern_left_classes, kern_right_classes,
         ligature_count, data_offset) = toc
        style_end = toc_entries[index + 1][11] if index + 1 < style_count else len(original)
        if data_offset < HEADER_SIZE + style_count * TOC_SIZE or style_end > len(original) or data_offset > style_end:
            raise ValueError(f"invalid data offsets for style {style_id}")

        intervals_bytes = interval_count * 12
        glyph_bytes = glyph_count * GLYPH_SIZE
        kern_left_bytes = kern_left_count * 3
        kern_right_bytes = kern_right_count * 3
        kern_matrix_bytes = kern_left_classes * kern_right_classes
        ligature_bytes = ligature_count * 8
        bitmap_offset = (data_offset + intervals_bytes + glyph_bytes + kern_left_bytes +
                         kern_right_bytes + kern_matrix_bytes + ligature_bytes)
        if bitmap_offset > style_end:
            raise ValueError(f"style {style_id} sections exceed its data range")

        prefix = original[data_offset:data_offset + intervals_bytes]
        glyph_table_offset = data_offset + intervals_bytes
        suffix_start = glyph_table_offset + glyph_bytes
        suffix = original[suffix_start:bitmap_offset]
        old_bitmap = original[bitmap_offset:style_end]
        old_bitmap_total += len(old_bitmap)

        new_glyph_table = bytearray()
        new_bitmap = bytearray()
        for glyph_index in range(glyph_count):
            glyph_pos = glyph_table_offset + glyph_index * GLYPH_SIZE
            width, height, advance_x, left, top, data_length, data_offset_rel = struct.unpack_from(
                GLYPH_FORMAT, original, glyph_pos)
            if data_offset_rel + data_length > len(old_bitmap):
                raise ValueError(f"style {style_id} glyph {glyph_index} bitmap is out of range")
            source_bitmap = old_bitmap[data_offset_rel:data_offset_rel + data_length]
            pixel_count = width * height
            expected_2bit = (pixel_count + 3) // 4
            if data_length != expected_2bit:
                raise ValueError(
                    f"style {style_id} glyph {glyph_index} has {data_length} bytes, expected {expected_2bit}")
            packed = repack_glyph_bitmap(source_bitmap, pixel_count)
            new_glyph_table += struct.pack(GLYPH_FORMAT, width, height, advance_x, left, top,
                                           len(packed), len(new_bitmap))
            new_bitmap += packed

        new_bitmap_total += len(new_bitmap)
        rebuilt_styles.append(bytes(prefix + new_glyph_table + suffix + new_bitmap))

    data_start = HEADER_SIZE + style_count * TOC_SIZE
    next_offset = data_start
    rebuilt_toc = bytearray()
    for toc, style_data in zip(toc_entries, rebuilt_styles):
        toc[11] = next_offset
        rebuilt_toc += struct.pack(TOC_FORMAT, *toc)
        next_offset += len(style_data)

    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    with open(output_path, "wb") as target:
        target.write(struct.pack(HEADER_FORMAT, magic, version, flags & ~1, style_count, reserved))
        target.write(rebuilt_toc)
        for style_data in rebuilt_styles:
            target.write(style_data)
    return old_bitmap_total, new_bitmap_total


def main() -> None:
    parser = argparse.ArgumentParser(description="Repack a v4 2-bit cpfont as 1-bit")
    parser.add_argument("input", help="Existing 2-bit .cpfont")
    parser.add_argument("output", help="Destination 1-bit .cpfont")
    args = parser.parse_args()
    try:
        old_bytes, new_bytes = repack_file(args.input, args.output)
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
    saved = old_bytes - new_bytes
    print(f"Repacked {args.input} -> {args.output}: bitmap {old_bytes} -> {new_bytes} bytes "
          f"(saved {saved} bytes)")


if __name__ == "__main__":
    main()
