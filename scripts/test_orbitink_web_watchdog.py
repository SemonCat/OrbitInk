"""The web server must not put loopTask on the 5 s task watchdog.

A b1501 X3 core dump (ESP_PANIC_DETAILS: "Task watchdog got triggered ... loopTask") showed loopTask
in lwip_select under NetworkClient::write <- WebServer::sendContent <- handleFileListData's scan
callback. One such write can block ~10 s (10 x 1 s select retries), so feeding the watchdog between
entries cannot prevent the panic. CrossPoint never subscribes this task; CrossMux removed it in
ae22f6b4. Pinned here because the timing itself needs a device and a stalled browser.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


class WebServerWatchdog(unittest.TestCase):
    def test_no_web_code_subscribes_a_task_to_the_watchdog(self):
        for path in sorted((ROOT / "src/network").glob("*.cpp")) + sorted((ROOT / "src/activities/network").glob("*.cpp")):
            code = re.sub(r"//[^\n]*", "", path.read_text())
            self.assertNotIn("esp_task_wdt_add(", code, path.name)

    def test_file_listing_stops_when_the_client_is_gone(self):
        text = (ROOT / "src/network/CrossPointWebServer.cpp").read_text()
        self.assertIn("const std::function<bool(const FileInfo&)>& callback", text)
        self.assertIn("if (!callback(info))", text)
        listing = text[text.index("void CrossPointWebServer::handleFileListData()"):]
        listing = listing[:listing.index("\n}\n")]
        self.assertIn("if (!server->client().connected())", listing)
        self.assertIn("return false;", listing)


if __name__ == "__main__":
    unittest.main()
