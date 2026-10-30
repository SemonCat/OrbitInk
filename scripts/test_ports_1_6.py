"""Host checks for embedded web JavaScript and the production spacing calculation."""
import json
import pathlib
import re
import subprocess
import sys
import struct
import tempfile
import unittest
import zipfile

from generate_ports_1_6_fixtures import generate

ROOT = pathlib.Path(__file__).resolve().parents[1]

class Ports160Test(unittest.TestCase):
    def test_epub_fixture_is_reproducible_and_complete(self):
        with tempfile.TemporaryDirectory(dir=ROOT / '.cache') as tmp:
            first, second = [pathlib.Path(tmp) / name for name in ('a.epub', 'b.epub')]
            generate(first)
            generate(second)
            self.assertEqual(first.read_bytes(), second.read_bytes())
            self.assertEqual(first.read_bytes(), (ROOT / 'test/epubs/test_ports_1_6.epub').read_bytes())
            with zipfile.ZipFile(first) as book:
                self.assertIsNone(book.testzip())
                self.assertIn('toc.ncx', book.namelist())
                self.assertTrue(any(len(name) > 160 for name in book.namelist()))
                chapter = book.read('chapter.xhtml').decode()
                for marker in ('<ruby>', '<table>', 'colspan=', 'Oversized cell', 'trailing garbage'):
                    self.assertIn(marker, chapter)

    def test_smoke_failure_gate_survives_python_optimization(self):
        code = "from sim_ports_1_6_smoke import require; require(False, 'active-optimized-gate')"
        result = subprocess.run([sys.executable, '-O', '-c', code], cwd=ROOT / 'scripts', capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('active-optimized-gate', result.stderr)

    def test_smoke_page_comparison_ignores_only_footer_changes(self):
        from sim_ports_1_6_smoke import ink_mask
        data = bytearray(b'\xff' * (54 + 528 * 792 * 4))
        struct.pack_into('<I', data, 10, 54)
        original = ink_mask(data)
        def black_pixel(x, y):
            offset = 54 + ((791-y)*528+x)*4
            data[offset:offset+3] = b'\0' * 3
        black_pixel(10, 780)
        self.assertEqual(original, ink_mask(data))
        black_pixel(100, 300)
        self.assertNotEqual(original, ink_mask(data))

    def test_optimizer_comments(self):
        html = (ROOT / 'src/network/html/FilesPage.html').read_text()
        function = html[html.index('function stripComments('):html.index('/**\n * Serialize an XML doc')]
        cases = [
            ('a<!-- 中文 -->b', 'ab', 1),
            ('<![CDATA[<!-- literal -->]]><?pi <!-- literal -->?>', '<![CDATA[<!-- literal -->]]><?pi <!-- literal -->?>', 0),
            ('<!DOCTYPE html [<!ENTITY x "<!-- literal -->">]><p>ok</p>', '<!DOCTYPE html [<!ENTITY x "<!-- literal -->">]><p>ok</p>', 0),
            ('<p>valid</p><!-- unclosed', '<p>valid</p>', 1),
            ('<p title="safe">普通文章</p>', '<p title="safe">普通文章</p>', 0),
        ]
        program = function + '\nconst cases=' + json.dumps(cases) + ';\n'
        program += 'for(const [input,want,count] of cases) { const got=stripComments(input); if(got.text!==want || got.count!==count || got.bytes!==new TextEncoder().encode(input).length-new TextEncoder().encode(want).length) throw Error(JSON.stringify(got)); }'
        subprocess.run(['node', '-e', program], check=True, cwd=ROOT)

    def test_persisted_spacing_indices_and_production_metrics(self):
        header = (ROOT / 'src/CrossPointSettings.h').read_text()
        enum = re.search(r'enum LINE_COMPRESSION \{[^}]+\};', header).group()
        source = (ROOT / 'src/CrossPointSettings.cpp').read_text()
        method = source[source.index('float CrossPointSettings::getReaderLineCompression()'):source.index('unsigned long CrossPointSettings::getSleepTimeoutMs()')]
        code = '#include <cassert>\n#include <cmath>\nclass CrossPointSettings { public: ' + enum + '\n enum {NOTOSERIF,NOTOSANS}; int fontFamily=NOTOSERIF; int lineSpacing=NORMAL; char sdFontFamilyName[2]={}; float getReaderLineCompression() const; };\n' + method
        code += '''
int main() {
 static_assert(CrossPointSettings::TIGHT==0 && CrossPointSettings::NORMAL==1 && CrossPointSettings::WIDE==2 && CrossPointSettings::EXTRA_WIDE==3 && CrossPointSettings::LINE_COMPRESSION_COUNT==4);
 CrossPointSettings s;
 const float serif[]={.95f,1.f,1.1f,1.2f}; const float sans[]={.90f,.95f,1.f,1.05f};
 for(int i=0;i<4;++i) { s.lineSpacing=i; s.fontFamily=s.NOTOSERIF; assert(s.getReaderLineCompression()==serif[i]); s.fontFamily=s.NOTOSANS; assert(s.getReaderLineCompression()==sans[i]); s.sdFontFamilyName[0]='X'; assert(s.getReaderLineCompression()==serif[i]); s.sdFontFamilyName[0]=0; }
 s.lineSpacing=255; assert(s.getReaderLineCompression()==.95f);
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT / '.cache') as tmp:
            path = pathlib.Path(tmp)
            (path / 'spacing.cpp').write_text(code)
            subprocess.run(['c++', '-std=c++17', str(path / 'spacing.cpp'), '-o', str(path / 'spacing')], check=True)
            subprocess.run([str(path / 'spacing')], check=True)
        settings = (ROOT / 'src/SettingsList.h').read_text()
        options = re.search(r'&CrossPointSettings::lineSpacing,\s*\{([^}]+)\}', settings).group(1)
        self.assertEqual(re.findall(r'StrId::(\w+)', options), ['STR_TIGHT', 'STR_NORMAL', 'STR_WIDE', 'STR_EXTRA_WIDE'])
        # Persistence and web validation share this list; neither maintains a separate numeric cap.
        self.assertIn('info.enumValues.size()', source)
        self.assertIn('s.enumValues.size()', (ROOT / 'src/network/CrossPointWebServer.cpp').read_text())
        self.assertIn('STR_EXTRA_WIDE: "特寬"', (ROOT / 'lib/I18n/translations/chinese-traditional.yaml').read_text())

if __name__ == '__main__':
    unittest.main()
