"""Every BookOrbit catalog session the browser creates must release the SD-font arena before its TLS gate.

The client side (hook runs before the gate, for all seven operations) is host-tested against the real
lib/KOReaderSync/BookOrbitCatalogClient.cpp in test/bookorbit_catalog. This pins the browser wiring
those tests cannot reach: each Session it constructs installs prepareCatalogHeap, and the hook takes
the RenderLock before freeing font caches so a draw in flight never loses its glyph data.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BROWSER = ROOT / "src/activities/browser/BookOrbitCatalogBrowserActivity.cpp"


def function_body(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for i in range(brace, len(text)):
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        if depth == 0:
            return text[brace:i + 1]
    raise AssertionError(f"unterminated body for {signature}")


class CatalogHeapPreparation(unittest.TestCase):
    def setUp(self):
        self.text = BROWSER.read_text()

    def test_every_session_the_browser_constructs_installs_the_hook(self):
        constructions = [m.start() for m in re.finditer(r"BOClient::Session session;|std::make_unique<BOClient::Session>\(\)",
                                                        self.text)]
        self.assertEqual(len(constructions), 2, "catalog keep-alive session and the download session")
        for pos in constructions:
            window = self.text[pos:pos + 300]
            self.assertRegex(window, r"setPrepareHeap\(&BookOrbitCatalogBrowserActivity::prepareCatalogHeap, this\)")

    def test_every_request_goes_through_a_hooked_session(self):
        # The seven operations, all through catalogSession() except the separate download session.
        for call in ("fetchRootSections(", "fetchBooks(", "fetchSectionEntries(", "fetchBookDetail(",
                     "setReadStatus(", "setRating("):
            for line in [l for l in self.text.splitlines() if call in l and "Error error" in l]:
                self.assertIn("catalogSession().", line, line)
        self.assertIn("result = session.downloadFile(", self.text)

    def test_hook_frees_font_caches_under_the_render_lock(self):
        body = function_body(self.text, "void BookOrbitCatalogBrowserActivity::prepareCatalogHeap(void* activity)")
        lock = body.find("RenderLock lock;")
        release = body.find("forceReleaseTransientCaches()")
        self.assertGreaterEqual(lock, 0)
        self.assertGreater(release, lock)
        self.assertNotIn("requestUpdate", body)  # never schedules a render that could race the release


if __name__ == "__main__":
    unittest.main()
