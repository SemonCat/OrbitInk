"""Exercise the generator's actual backend without rasterizing a whole font."""
import ast
import pathlib
import types
import unittest
import zlib

class BuiltinCompressionTest(unittest.TestCase):
    def test_zopfli_is_equivalent_raw_deflate_and_reproducible(self):
        source = pathlib.Path(__file__).with_name('fontconvert.py').read_text()
        tree = ast.parse(source)
        function = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'deflate_raw')
        module = ast.Module(body=[function], type_ignores=[])
        args = types.SimpleNamespace(zopfli=False)
        scope = {'args': args, 'zlib': zlib}
        exec(compile(module, 'fontconvert.py', 'exec'), scope)
        for data in [b'', bytes(range(256))*32, ('繁體中文漢字字型'.encode()*1024), b'\0'*8192]:
            args.zopfli = False
            ordinary = scope['deflate_raw'](data)
            args.zopfli = True
            optimized = scope['deflate_raw'](data)
            self.assertEqual(zlib.decompress(ordinary, -15), data)
            self.assertEqual(zlib.decompress(optimized, -15), data)
            self.assertEqual(scope['deflate_raw'](data), optimized)
            self.assertLessEqual(len(optimized), len(ordinary))

if __name__ == '__main__':
    unittest.main()
