#!/usr/bin/env python3
"""Generates the cover-bearing Traditional Chinese EPUB the BookOrbit simulator smoke serves.

Stdlib only. A real grayscale PNG cover (declared both as the EPUB 3 cover-image property and the
EPUB 2 <meta name="cover">), a long zh-TW title, an author, and three chapters long enough to
page through -- so Home's hero is exercised with the book's own title/author/progress and a cover
produced by the firmware's ordinary thumbnail path.
"""
import struct
import zipfile
import zlib
from pathlib import Path

TITLE = "長夜將盡的星光圖書館：關於遠方城市與一位守夜人的漫長故事"
AUTHOR = "林曉雲"

PARAGRAPH = ("<p>夜色漸深，圖書館的燈一盞一盞亮起。守夜人推著書車經過長長的走廊，"
             "書架間傳來紙張翻動的細微聲響，像是遠方城市在低聲說話。</p>")

CONTAINER = ('<?xml version="1.0"?><container version="1.0" '
             'xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile '
             'full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>')

OPF = ('<?xml version="1.0" encoding="utf-8"?>\n'
       '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id" xml:lang="zh-TW">\n'
       '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
       '<dc:identifier id="id">urn:uuid:crossfade-fixture-1</dc:identifier>'
       '<dc:title>{title}</dc:title><dc:creator>{author}</dc:creator><dc:language>zh-TW</dc:language>'
       '<meta name="cover" content="cover"/><meta property="dcterms:modified">2026-10-01T00:00:00Z</meta>'
       '</metadata>\n<manifest>'
       '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>'
       '<item id="cover" href="cover.png" media-type="image/png" properties="cover-image"/>{items}'
       '</manifest>\n<spine>{spine}</spine></package>')

XHTML = ('<?xml version="1.0" encoding="utf-8"?><html xmlns="http://www.w3.org/1999/xhtml" '
         'xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="zh-TW"><body>{body}</body></html>')


def png_cover(width: int = 300, height: int = 450) -> bytes:
    rows = []
    for y in range(height):
        row = bytearray()
        for x in range(width):
            v = 235
            if x < 10 or x >= width - 10 or y < 10 or y >= height - 10:
                v = 20
            elif 60 < y < 170 and 30 < x < width - 30:
                v = 40 + (y - 60)
            elif (y // 24) % 2 == 0 and y > 220:
                v = 170
            row.append(max(0, min(255, v)))
        rows.append(b"\x00" + bytes(row))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))


# Chapter headings are Latin on purpose. With CJK chapter titles the UNCHANGED reader (which this
# task must not modify) writes glyph pixels below the 792-row panel ("[GFX] !! Outside range"):
# reproduced on the pre-redesign baseline 20cabd0 with this same fixture (42 writes with
# 第一章/第二章/第三章, 0 with "Chapter n"; title, body and cover held constant). That is a
# pre-existing reader-chrome defect, reported separately -- not something this fixture should hide
# or Home/library code should cause. The title, author and body stay Traditional Chinese.
HEADINGS = ("Chapter 1", "Chapter 2", "Chapter 3")


def write(path: Path, title: str = TITLE, author: str = AUTHOR, headings=HEADINGS) -> None:
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", CONTAINER)
        items = "".join(f'<item id="c{i}" href="c{i}.xhtml" media-type="application/xhtml+xml"/>' for i in range(3))
        spine = "".join(f'<itemref idref="c{i}"/>' for i in range(3))
        z.writestr("OEBPS/content.opf", OPF.format(title=title, author=author, items=items, spine=spine))
        toc = "".join(f'<li><a href="c{i}.xhtml">{h}</a></li>' for i, h in enumerate(headings))
        z.writestr("OEBPS/nav.xhtml", XHTML.format(body=f'<nav epub:type="toc"><ol>{toc}</ol></nav>'))
        z.writestr("OEBPS/cover.png", png_cover())
        for i, h in enumerate(headings):
            z.writestr(f"OEBPS/c{i}.xhtml", XHTML.format(body=f"<h1>{h}</h1>" + PARAGRAPH * 18))


if __name__ == "__main__":
    import sys
    write(Path(sys.argv[1]))
