#!/usr/bin/env python3
"""Regenerate the 120x120 1-bit boot/sleep logo from its editable SVG.

    python scripts/generate_boot_logo.py                      # write src/images/Logo120.h
    python scripts/generate_boot_logo.py --preview out.png    # also write an upright preview
    python scripts/generate_boot_logo.py --check              # fail if the header is stale

Reuses convert_icon.py's rasterize/rotate/pack steps (cairosvg + Pillow from
scripts/requirements.txt). GfxRenderer::drawImage() copies bits to the panel unrotated and
Portrait maps logical (x, y) to panel (y, H-1-x), so the stored bitmap is the upright art
rotated 90 degrees counter-clockwise; MSB-first, 1 = white. --preview writes the upright,
thresholded image of exactly what the panel shows (src/images/Logo120.png is a separate
user-guide asset and is not generated here).
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import convert_icon  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SIZE = 120
HEADER_COMMENT = ("// OrbitInk boot logo -- 120x120, 1bpp, MSB-first, 1=white/0=black, stored rotated 90 deg CCW\n"
                  "// (Portrait drawImage). Generated from src/images/logo.svg by scripts/generate_boot_logo.py.\n")


def render(svg_path):
    stored = convert_icon.load_image(str(svg_path), SIZE, SIZE)
    header = convert_icon.image_to_c_array(stored, "Logo120")
    header = header.replace("// size: 120x120\n", HEADER_COMMENT, 1)
    header = "".join(line.rstrip() + "\n" for line in header.splitlines())
    threshold = convert_icon.threshold
    preview = stored.convert("L").point(lambda v: 255 if v >= threshold else 0).rotate(-90, expand=True)
    return header, preview


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--svg", type=Path, default=ROOT / "src/images/logo.svg")
    parser.add_argument("--out-dir", type=Path, default=ROOT / "src/images")
    parser.add_argument("--preview", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    header, preview = render(args.svg)
    target = args.out_dir / "Logo120.h"
    if args.check:
        if not target.exists() or target.read_text() != header:
            print(f"{target} is stale; run scripts/generate_boot_logo.py", file=sys.stderr)
            return 1
        return 0
    args.out_dir.mkdir(parents=True, exist_ok=True)
    target.write_text(header)
    print(f"Wrote {target}")
    if args.preview:
        preview.convert("1").save(args.preview)
        print(f"Wrote {args.preview}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
