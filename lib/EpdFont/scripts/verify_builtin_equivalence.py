#!/usr/bin/env python3
"""Compare renderer-visible data in two generated built-in font directories.

Compression offsets may differ; decoded group bytes, glyph geometry/advances,
coverage, ligatures, class maps and every kerning matrix cell must be identical.
"""
import pathlib
import re
import sys
import zlib


def arrays(text):
    result = {}
    for match in re.finditer(r'static const \w+ (\w+)\[[^]]*\]\s*=\s*\{(.*?)\};', text, re.S):
        body = re.sub(r'//[^\n]*', '', match[2])
        result[match[1]] = [int(x, 0) for x in re.findall(r'-?0x[0-9a-fA-F]+|-?\d+', body)]
    return result


def visible(path):
    text = path.read_text()
    name = re.search(r'static const EpdGlyph (\w+)Glyphs', text)[1]
    data = arrays(text)
    def get(suffix):
        return data.get(name + suffix, [])
    pixels = bytes(get('Bitmaps'))
    groups = get('Groups')
    if groups:
        decoded = []
        for pos in range(0, len(groups), 5):
            offset, length, size, count, first = groups[pos:pos+5]
            raw = zlib.decompress(pixels[offset:offset+length], -15)
            if len(raw) != size:
                raise ValueError(f'{path.name}: invalid group size')
            decoded.append((count, first, raw))
        pixels = decoded
    maps = []
    for side in ('Left', 'Right'):
        packed = get('Kern' + side + 'Classes')
        if packed:
            maps.append(list(zip(packed[::2], packed[1::2])))
        else:
            maps.append(list(zip(get('Kern' + side + 'Codepoints'), get('Kern' + side + 'ClassIds'))))
    dense = get('KernMatrix')
    rows = get('KernRowOffsets')
    if rows:
        cols, values = get('KernSparseCols'), get('KernSparseValues')
        width = max((cls for _, cls in maps[1]), default=0)
        dense = [0] * ((len(rows)-1) * width)
        if rows[-1] != len(cols) or len(cols) != len(values):
            raise ValueError(f'{path.name}: invalid CSR lengths')
        for row in range(len(rows)-1):
            last = -1
            for i in range(rows[row], rows[row+1]):
                if not last < cols[i] < width:
                    raise ValueError(f'{path.name}: invalid CSR column')
                last = cols[i]
                dense[row*width+cols[i]] = values[i]
    # Font-level metrics precede the representation-dependent pointer fields.
    descriptor = re.search(r'static const EpdFontData \w+\s*=\s*\{(.*?)\};', text, re.S)[1]
    fields = [x.strip() for x in re.sub(r'//[^\n]*', '', descriptor).split(',')]
    coverage = {}
    intervals = get('Intervals')
    for i in range(0, len(intervals), 3):
        first, last, offset = intervals[i:i+3]
        for cp in range(first, last+1):
            if cp in coverage:
                raise ValueError(f'{path.name}: overlapping coverage')
            coverage[cp] = offset + cp - first
    return dict(glyphs=get('Glyphs'), intervals=coverage, pixels=pixels,
                groups=get('GlyphToGroup'), ligatures=get('LigaturePairs'), maps=maps,
                kerning=dense, metrics=fields[4:9])


def main(before, after):
    old = sorted(p for p in pathlib.Path(before).glob('*.h') if 'static const EpdGlyph ' in p.read_text())
    new = sorted(p.name for p in pathlib.Path(after).glob('*.h') if 'static const EpdGlyph ' in p.read_text())
    if [p.name for p in old] != new:
        raise ValueError('font inventory differs')
    for path in old:
        a, b = visible(path), visible(pathlib.Path(after) / path.name)
        mismatches = [key for key in a if a[key] != b[key]]
        if mismatches:
            raise ValueError(f'{path.name}: changed renderer-visible {mismatches}')
        print(f'PASS {path.name}')
    print(f'{len(old)} fonts: byte-equivalent glyphs, coverage, metrics, ligatures and all kerning cells')

if __name__ == '__main__':
    main(*sys.argv[1:])
