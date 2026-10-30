#!/usr/bin/env python3
"""Headless X3 smoke for selective ports; isolated SD roots, no real services."""
import argparse
import json
import re
import shutil
import struct
import tempfile
import zipfile
import zlib
from pathlib import Path

from generate_ports_1_6_fixtures import generate
from sim_smoke import REPO_ROOT, build, launch, bmp_size


def require(condition, message='smoke check failed'):
    # Executable gates must remain active under -O / PYTHONOPTIMIZE.
    if not condition:
        raise AssertionError(message)


def ink_mask(data, top=0):
    """Book/definition pixels, excluding footer counters and the loading icon."""
    offset = struct.unpack_from('<I', data, 10)[0]
    return bytes(data[offset + ((791-y)*528+x)*4:offset + ((791-y)*528+x)*4 + 3] != b'\xff' * 3
                 for y in range(top, 744) for x in range(528))


def png_overlay():
    def chunk(name, data):
        return struct.pack('>I', len(data)) + name + data + struct.pack('>I', zlib.crc32(name + data))
    width, height = 48, 32
    rows = b''.join(b'\0' + b''.join(bytes((255, 255, 255, 255 if x < 12 else 0)) for x in range(width)) for _ in range(height))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')


def bmp_overlay():
    width, height = 48, 32
    pixels = b''.join(bytes((255, 255, 255, 255 if x < 12 else 0)) for _ in range(height) for x in range(width))
    return b'BM' + struct.pack('<IHHI', 54 + len(pixels), 0, 0, 54) + struct.pack('<IiiHHIIiiII', 40, width, height, 1, 32, 0, len(pixels), 0, 0, 0, 0) + pixels


def setup(root, dictionary=False):
    sd = root / 'fs_'
    (sd / 'books').mkdir(parents=True)
    (sd / '.crosspoint').mkdir()
    (root / 'shots').mkdir()
    book = sd / 'books' / 'ports.epub'
    source = REPO_ROOT / '.cache/ports-1.6.0/test_ports_1_6.epub'
    generate(source)
    if dictionary:
        with zipfile.ZipFile(source) as src, zipfile.ZipFile(book, 'w') as dst:
            for name in src.namelist():
                data = src.read(name)
                if name == 'chapter.xhtml':
                    data = ('<html><body>' + '<p>' + 'word ' * 1200 + '</p></body></html>').encode()
                dst.writestr(name, data)
        directory = sd / 'dictionaries' / 'smoke'
        directory.mkdir(parents=True)
        definition = ('<h1>Formatted word</h1><p><b>Bold</b> <i>italic</i> &amp; 中文定義。</p>' + '<p>Another definition paragraph with readable text.</p>' * 70).encode()
        (directory / 'smoke.dict').write_bytes(definition)
        (directory / 'smoke.idx').write_bytes(b'word\0' + struct.pack('>II', 0, len(definition)))
        (directory / 'smoke.ifo').write_text("StarDict's dict ifo file\nversion=2.4.2\nbookname=smoke\nwordcount=1\nidxfilesize=13\nsametypesequence=h\n")
    else:
        shutil.copyfile(source, book)
    (sd / '.crosspoint/crossfade-settings.json').write_text(json.dumps({'sleepScreen': 6, 'longPressMenuFunction': 3, 'dictionaryName': 'smoke', 'lineSpacing': 3}))


def run(root, kind, sd_font=None, corrupt_overlay=False):
    setup(root, kind == 'dictionary')
    if sd_font:
        target = root / 'fs_/fonts' / sd_font.parent.name
        target.mkdir(parents=True)
        shutil.copyfile(sd_font, target / sd_font.name)
        settings = root / 'fs_/.crosspoint/crossfade-settings.json'
        values = json.loads(settings.read_text())
        values.update(sdFontFamilyName=sd_font.parent.name, fontSize=int(re.search(r'_(\d+)\.cpfont$', sd_font.name)[1]))
        settings.write_text(json.dumps(values))
    env = {}
    prefix = '1000:ENTER;1800:ENTER;2600:ENTER;'
    if kind == 'dictionary':
        env['CROSSPOINT_SIM_INPUT_SCRIPT'] = prefix + '4300:ENTER:700;6000:ENTER;7800:DOWN;9400:BACK;10400:BACK;12000:QUIT'
        env['CROSSPOINT_SIM_SCREENSHOTS'] = '3800:./shots/reader.bmp;5500:./shots/selection.bmp;7300:./shots/definition.bmp;8800:./shots/definition-page2.bmp;11400:./shots/back-reader.bmp'
        markers = ['Entering activity: DictionaryWordSelect', 'Entering activity: DictionaryDefinition',
                   'Exiting activity: DictionaryDefinition', 'Exiting activity: DictionaryWordSelect']
        shots = ['reader', 'selection', 'definition', 'definition-page2', 'back-reader']
    else:
        overlay = bytearray(png_overlay() if kind == 'png' else bmp_overlay())
        if corrupt_overlay:
            # Reserved DEFLATE block type: valid PNG dimensions, invalid compressed stream.
            overlay[43] = (overlay[43] & ~6) | 6
            length = struct.unpack_from('>I', overlay, 33)[0]
            struct.pack_into('>I', overlay, 41 + length, zlib.crc32(overlay[37:41 + length]))
        (root / 'fs_' / ('sleep-overlay.' + kind)).write_bytes(overlay)
        env['CROSSPOINT_SIM_INPUT_SCRIPT'] = prefix + '5000:SLEEP;7200:ENTER;12000:QUIT'
        env['CROSSPOINT_SIM_INPUT_SCRIPT_AFTER_WAKE'] = '2500:DOWN;4000:QUIT'
        env['CROSSPOINT_SIM_SCREENSHOTS'] = '4200:./shots/reader.bmp'
        env['CROSSPOINT_SIM_SCREENSHOTS_AFTER_WAKE'] = '2000:./shots/after-wake.bmp;3400:./shots/wake-page-turn.bmp'
        markers = ['Entering deep sleep', 'Rendering transparent PNG overlay:' if kind == 'png' else 'Rendering transparent overlay:']
        shots = ['reader', 'after-wake', 'wake-page-turn']
        if corrupt_overlay:
            markers = ['Entering deep sleep']  # header rejection happens before the rendering log
    result = launch(root, 'dummy', 60, env, label='ports-' + kind)
    log = result.stdout + result.stderr
    (root / 'run.log').write_text(log)
    require(result.returncode == 0, f'{kind}: simulator exit {result.returncode}')
    errors = [line for line in log.splitlines() if '[ERR]' in line]
    if corrupt_overlay:
        rejected = ('[PNG] Decode failed:', '[PNG] Failed to open PNG for dimensions:')
        require(any(any(marker in line for marker in rejected) for line in errors), 'corrupt PNG was not rejected')
        # The SDK preview shim decodes the whole file in open(), before painting.
        # A native decoder may instead fail during scanlines, which requires rollback.
        if any(rejected[0] in line for line in errors):
            require('Restored clean sleep framebuffer after overlay failure' in log, 'smoke check failed')
        errors = [line for line in errors if not any(marker in line for marker in rejected)]
    require(not errors, f'{kind}: firmware errors; see {root}/run.log')
    for marker in markers:
        require(marker in log, f'{kind}: missing {marker}; see {root}/run.log')
    for name in shots:
        require(bmp_size(root / 'shots' / (name + '.bmp')) == (528, 792), f'{kind}: missing/invalid {name}')
    if kind == 'dictionary':
        require(ink_mask((root / 'shots/definition.bmp').read_bytes(), 64) != ink_mask((root / 'shots/definition-page2.bmp').read_bytes(), 64), 'dictionary page turn did not change definition content')
        require(ink_mask((root / 'shots/reader.bmp').read_bytes()) == ink_mask((root / 'shots/back-reader.bmp').read_bytes()), 'dictionary Back did not restore the reader content')
        require(not (root / 'fs_/.crosspoint/dicthtml.tmp').exists(), 'HTML temporary file leaked')
    else:
        before, after = [(root / 'shots' / (name + '.bmp')).read_bytes() for name in ('reader', 'after-wake')]
        offsets = [struct.unpack_from('<I', data, 10)[0] for data in (before, after)]
        # Quick Resume stores the existing 1-bit base, so antialiasing becomes black.
        # Its existing 48px loading icon occupies the lower-left corner until navigation.
        for y in range(792):
            for x in range(528):
                if x <= 48 and y >= 744:
                    continue
                i, j = [offset + ((791-y)*528+x)*4 for offset in offsets]
                require((before[i:i+3] != b'\xff'*3) == (after[j:j+3] != b'\xff'*3), f'{kind}: saved base differs at {x},{y}')
        require(ink_mask(after) != ink_mask((root / 'shots/wake-page-turn.bmp').read_bytes()), f'{kind}: reader content did not advance after wake')
        require(not (root / 'fs_/.crosspoint/sleep_frame.bin').exists(), 'wake did not consume saved base')
    print(f'[ports-{kind}] PASS: {len(shots)} screenshots, activity/decoder markers, navigation and cleanup; {root}')



def verify_old_cache_invalidation(root):
    cache = next((root / 'fs_/.crosspoint').glob('epub_*'))
    book = cache / 'book.bin'
    data = bytearray(book.read_bytes())
    require(data[4] == 11, 'smoke check failed')
    data[4] = 10
    title = data.index(b'Selective ports')
    data[title] = ord('X')
    book.write_bytes(data)
    cases = [('metadata', 'Cache version mismatch: expected 11, got 10', None),
             ('section', 'Deserialization failed: Unknown version 33', 33),
             ('partial', 'Deserialization failed: Unknown version 249', 249)]
    for name, marker, version in cases:
        # QUIT terminates the simulator abruptly before the boot-loop guard's
        # grace period expires. Model a clean restart for this cache-only probe.
        state_path = root / 'fs_/.crosspoint/state.json'
        state = json.loads(state_path.read_text())
        state['readerActivityLoadCount'] = 0
        state_path.write_text(json.dumps(state))
        section = cache / 'sections/0.bin'
        if version is not None:
            data = bytearray(section.read_bytes())
            require(data[0] == 34, 'smoke check failed')
            data[0] = version
            section.write_bytes(data)
        result = launch(root, 'dummy', 30, {
            'CROSSPOINT_SIM_INPUT_SCRIPT': '3000:QUIT',
            'CROSSPOINT_SIM_SCREENSHOTS': f'2200:./shots/rebuilt-{name}.bmp',
        }, label='ports-cache-' + name)
        log = result.stdout + result.stderr
        (root / ('cache-' + name + '.log')).write_text(log)
        require(result.returncode == 0 and marker in log, f'{name}: old cache was not rejected')
        require(not [line for line in log.splitlines() if '[ERR]' in line and marker not in line], 'smoke check failed')
        require(book.read_bytes()[4] == 11 and b'Selective ports' in book.read_bytes(), 'smoke check failed')
        require(b'Xelective ports' not in book.read_bytes(), 'smoke check failed')
        require(section.read_bytes()[0] == 34, 'smoke check failed')
        require(bmp_size(root / 'shots' / ('rebuilt-' + name + '.bmp')) == (528, 792), 'smoke check failed')
    print('[ports-cache] PASS: metadata v10, section v33 and paired partial v249 reject and rebuild')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--no-build', action='store_true')
    parser.add_argument('--sd-font', type=Path, help='copy one existing bundled-style Family_Size.cpfont into the isolated SD card')
    args = parser.parse_args()
    if args.sd_font and (not args.sd_font.is_file() or not re.search(r'_(\d+)\.cpfont$', args.sd_font.name)):
        parser.error('--sd-font must be an existing Family_Size.cpfont')
    if not args.no_build:
        build()
    parent = REPO_ROOT / '.cache/ports-1.6.0'
    parent.mkdir(parents=True, exist_ok=True)
    for kind in ('dictionary', 'bmp', 'png'):
        root = Path(tempfile.mkdtemp(prefix='sim-' + kind + '-', dir=parent))
        run(root, kind, args.sd_font)
    # Cache version checks use the completed built-in-font fixture. SD fonts
    # deliberately retain incremental .part builds until later pages are needed.
    if not args.sd_font:
        verify_old_cache_invalidation(root)
    run(Path(tempfile.mkdtemp(prefix='sim-corrupt-png-', dir=parent)), 'png', args.sd_font, corrupt_overlay=True)


if __name__ == '__main__':
    main()
