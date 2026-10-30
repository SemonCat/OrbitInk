#!/usr/bin/env python3
"""Reproducible EPUB fixture for the selective 1.6.0 parser/layout ports."""
import pathlib
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

def generate(path):
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    note = 'Text/' + 'calibre_' * 20 + '_split_099.xhtml'
    chapter = '''<html xmlns="http://www.w3.org/1999/xhtml"><body>
<h1>Selective ports</h1><p>Ordinary paragraph before a table.</p>
<!-- invalid-looking markup in a comment: <p> -->
<p>中文<ruby>漢字<rt>かんじ</rt></ruby>測試。it&apos;s readable.</p>
<table><tr><th>Header A</th><th>Header B</th></tr><tr><td>short</td><td><b>bold</b> and <i>italic</i></td></tr>
<tr><td>uneven</td></tr><tr><td colspan="2">span fallback</td><td>retained</td></tr>
<tr><td>outer<table><tr><td>nested</td></tr></table></td><td>end</td></tr></table>
<table><tr><td>''' + ('Oversized cell content. ' * 40) + '</td><td>end</td></tr></table><p>Calibre note <a href="' + note + '#note">[1]</a></p>'
    chapter += '<p>Normal paragraph after tables.</p>' * 60 + '</body></html>trailing garbage'
    files = {
        'mimetype': 'application/epub+zip',
        'META-INF/container.xml': '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>',
        'content.opf': '<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id"><metadata xmlns:meta="http://purl.org/dc/elements/1.1/"><meta:title>Selective ports</meta:title><meta:creator>CrossFade tests</meta:creator><meta:language>zh-TW</meta:language><meta:identifier id="id">ports-160</meta:identifier></metadata><manifest><item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/><item id="main" href="chapter.xhtml" media-type="application/xhtml+xml"/><item id="note" href="'+note+'" media-type="application/xhtml+xml"/></manifest><spine toc="ncx"><itemref idref="main"/><itemref idref="note"/></spine></package>',
        'toc.ncx': '<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><head/><docTitle><text>Selective ports</text></docTitle><navMap><navPoint id="chapter" playOrder="1"><navLabel><text>Selective ports</text></navLabel><content src="chapter.xhtml"/></navPoint></navMap></ncx>',
        'chapter.xhtml': chapter,
        note: '<html xmlns="http://www.w3.org/1999/xhtml"><body><p id="note">Long path footnote target.</p></body></html>',
    }
    with zipfile.ZipFile(path, 'w') as out:
        for name, text in files.items():
            info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED if name == 'mimetype' else zipfile.ZIP_DEFLATED
            out.writestr(info, text.encode())

if __name__ == '__main__':
    generate(ROOT / 'test/epubs/test_ports_1_6.epub')
