"""Execute the real pre-build generators twice, checking bytes and mtimes."""
import contextlib
import io
import os
import runpy
import subprocess
import tempfile
import unittest
from pathlib import Path

import gen_i18n

SCRIPTS = Path(__file__).resolve().parent


class GeneratorTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def run_script(self, name):
        root = self.root
        class Env:
            def subst(self, value):
                return str(root)
            def __getitem__(self, key):
                return str(root)
        with contextlib.chdir(root), contextlib.redirect_stdout(io.StringIO()):
            runpy.run_path(str(SCRIPTS / name), init_globals={"env": Env(), "Import": lambda *args: None})

    def stamp(self, paths):
        for path in paths:
            os.utime(path, ns=(1_000_000_000, 1_000_000_000))
        return {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in paths}

    def assert_unchanged(self, before):
        self.assertEqual(before, {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in before})

    def test_wolfssl_first_patch_and_rerun_are_idempotent(self):
        header = self.root / ".pio/libdeps/x3/Arduino-wolfSSL/src/user_settings.h"
        header.parent.mkdir(parents=True)
        header.write_text("#define FP_MAX_BITS 16384\n\n")
        self.run_script("patch_wolfssl.py")
        self.assertIn("#define FP_MAX_BITS 8192", header.read_text())
        before = self.stamp([header])
        self.run_script("patch_wolfssl.py")
        self.assert_unchanged(before)
        self.assertEqual(header.read_text().count("/* CrossPoint wolfSSL compatibility overrides */"), 1)

    def test_html_and_js_rerun_does_not_write_but_changed_input_does(self):
        source = self.root / "src"
        source.mkdir()
        html = source / "page.html"
        html.write_text("<p>Hello</p>")
        (source / "app.js").write_text("alert(1);")
        self.run_script("build_html.py")
        before = self.stamp(sorted(source.glob("*.generated.h")))
        self.assertEqual(len(before), 2)
        self.run_script("build_html.py")
        self.assert_unchanged(before)
        html.write_text("<p>Changed</p>")
        self.run_script("build_html.py")
        self.assertNotEqual((source / "pageHtml.generated.h").read_bytes(), before[source / "pageHtml.generated.h"][0])
        self.assert_unchanged({source / "appJs.generated.h": before[source / "appJs.generated.h"]})

    def test_all_i18n_outputs_preserve_mtime_and_only_changed_content_is_written(self):
        translations = self.root / "translations"
        translations.mkdir()
        english = translations / "en.yaml"
        english.write_text('_language_name: "English"\n_language_code: "EN"\nSTR_HELLO: "Hello"\n')
        output = self.root / "output"
        output.mkdir()
        def generate():
            with contextlib.redirect_stdout(io.StringIO()):
                gen_i18n.main(str(translations), str(output), src_dirs=[])
        generate()
        before = self.stamp(sorted(output.iterdir()))
        self.assertEqual(len(before), 3)
        generate()
        self.assert_unchanged(before)
        english.write_text(english.read_text().replace('"Hello"', '"Changed"'))
        generate()
        self.assertNotEqual((output / "I18nStrings.cpp").read_bytes(), before[output / "I18nStrings.cpp"][0])
        self.assert_unchanged({p: record for p, record in before.items() if p.suffix == ".h"})

    def test_jpegdec_already_applied_patch_preserves_mtime(self):
        library = self.root / ".pio/libdeps/x3/JPEGDEC"
        library.mkdir(parents=True)
        subprocess.run(["git", "init", "-q", str(library)], check=True)
        source = library / "source.c"
        source.write_text("old\n")
        patches = self.root / "scripts/jpegdec_patches"
        patches.mkdir(parents=True)
        (patches / "fix.patch").write_text(
            "diff --git a/source.c b/source.c\n--- a/source.c\n+++ b/source.c\n@@ -1 +1 @@\n-old\n+new\n")
        self.run_script("patch_jpegdec.py")
        self.assertEqual(source.read_text(), "new\n")
        before = self.stamp([source])
        self.run_script("patch_jpegdec.py")
        self.assert_unchanged(before)


if __name__ == "__main__":
    unittest.main()
