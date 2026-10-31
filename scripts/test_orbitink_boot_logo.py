import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "src/images/Logo120.h"

try:  # cairosvg/Pillow (scripts/requirements.txt) and the native cairo library
    import cairosvg  # noqa: F401
    import PIL  # noqa: F401
    import generate_boot_logo
except (ImportError, OSError):
    generate_boot_logo = None


def header_bytes():
    return bytes(int(h, 16) for h in re.findall(r"0x([0-9A-F]{2})", HEADER.read_text().split("{", 1)[1]))


class BootLogoTest(unittest.TestCase):
    def test_header_is_120x120_one_bit(self):
        self.assertEqual(len(header_bytes()), 120 * 120 // 8)
        self.assertIn("static const uint8_t Logo120[]", HEADER.read_text())

    def test_boot_and_sleep_draw_this_logo_at_its_size(self):
        for name in ("BootActivity.cpp", "SleepActivity.cpp"):
            source = (ROOT / "src/activities/boot_sleep" / name).read_text()
            self.assertIn('#include "images/Logo120.h"', source)
            self.assertIn("drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120)", source)

    @unittest.skipIf(generate_boot_logo is None, "icon tooling unavailable")
    def test_header_matches_editable_svg(self):
        header, preview = generate_boot_logo.render(ROOT / "src/images/logo.svg")
        self.assertEqual(header, HEADER.read_text(), "run scripts/generate_boot_logo.py")
        # Stored bits are the upright preview rotated 90 degrees CCW, MSB-first, 1 = white.
        stored = header_bytes()
        for y in range(120):
            for x in range(120):
                upright_white = preview.getpixel((x, y)) >= 128
                sx, sy = y, 119 - x
                bit = (stored[sy * 15 + sx // 8] >> (7 - sx % 8)) & 1
                self.assertEqual(bool(bit), upright_white, (x, y))


if __name__ == "__main__":
    unittest.main()
