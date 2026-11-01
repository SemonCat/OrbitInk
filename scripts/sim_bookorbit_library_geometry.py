"""Pixel composition gates over actual firmware BMPs, independent of source strings."""
from pathlib import Path

from PIL import Image


def assert_composition(path: Path, mode: str, count: int = 6, selected: int = 0) -> dict:
    image = Image.open(path).convert("L")
    assert image.size == (528, 792), f"wrong viewport: {image.size}"

    def ink(x, y):
        return image.getpixel((x, y)) < 128

    def horizontal(x, y, width, minimum=.97):
        ratio = sum(ink(px, y) for px in range(x, x + width)) / width
        assert ratio >= minimum, f"{mode}: missing horizontal rule ({x},{y},{width}), ink={ratio:.3f}"

    def outline(x, y, width, height, thickness=1):
        for inset in range(thickness):
            horizontal(x + inset, y + inset, width - 2 * inset)
            horizontal(x + inset, y + height - 1 - inset, width - 2 * inset)
            for px in (x + inset, x + width - 1 - inset):
                ratio = sum(ink(px, py) for py in range(y + inset, y + height - inset)) / (height - 2 * inset)
                assert ratio >= .97, f"{mode}: missing card edge ({px},{y}) ink={ratio:.3f}"

    # All three surfaces share the approved identity/title/tabs/rail/footer hierarchy.
    horizontal(20, 193, 489)
    horizontal(20, 721, 489)
    for box in ((20, 14, 180, 30), (20, 51, 250, 45), (330, 158, 178, 30), (345, 701, 163, 19)):
        x, y, w, h = box
        assert any(ink(px, py) for py in range(y, y + h) for px in range(x, x + w)), f"missing header/footer text {box}"
    # Fixed tabs occupy the band above the books, never a category list.
    for i in range(4):
        x = 20 + i * 124
        assert any(ink(px, py) for py in range(113, 141) for px in range(x + 12, x + 105)), f"missing fixed tab {i}"
    for slot in range(count):
        if mode == "covers":
            x, y = 20 + slot % 2 * 250, 205 + slot // 2 * 168
            outline(x + 10, y + 9, 85, 135)
            titlebox = (x + 105, y + 8, 127, 81)
            if slot == selected:
                outline(x, y, 238, 157, 4)
        elif mode == "titles":
            y = 201 + slot * 80
            outline(29, y + 6, 44, 65)
            titlebox = (86, y + 3, 410, 54)
            if slot == selected:
                outline(20, y, 488, 78, 3)
        elif mode == "reading":
            y = 215 + slot * 220
            outline(20, y, 488, 203, 4 if slot == selected else 1)
            outline(34, y + 16, 112, 166)
            outline(166, y + 151, 309, 8)
            titlebox = (166, y + 14, 326, 64)
        else:
            raise AssertionError(f"unknown composition {mode}")
        x, y, w, h = titlebox
        assert any(ink(px, py) for py in range(y, y + h) for px in range(x, x + w)), f"title outside cover missing in slot {slot}"
    if mode == "covers":
        for slot in range(count, 6):
            x, y = 30 + slot % 2 * 250, 214 + slot // 2 * 168
            assert sum(ink(x, py) for py in range(y, y + 135)) < 120, "manufactured empty cover slot"
    return {"path": str(path), "viewport": list(image.size), "mode": mode, "books": count,
            "separator": 193, "footer": 721, "result": "pass"}
