"""The firmware download (only) uses the bounded Range resume; the manifest fetch stays single-shot.

ota_http::checkContinuation/shouldResume and the session's stream checks are host-tested in
test/firmware_update. The fetch loop needs a TLS device, so this pins the wiring around it.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


class OtaDownloadResume(unittest.TestCase):
    def test_firmware_download_is_resumable_and_manifest_is_not(self):
        updater = (ROOT / "src/network/OtaUpdater.cpp").read_text()
        self.assertEqual(len(re.findall(r"ota_fetch::getResumable\(otaUrl\.c_str\(\)", updater)), 1)
        self.assertRegex(updater, r"ota_fetch::get\(ota_channel::manifestUrl\(")

    def test_resume_validates_continuation_and_reports_the_stop_cause(self):
        fetch = (ROOT / "src/network/OtaFetch.cpp").read_text()
        self.assertIn("ota_http::checkContinuation(*head, offset, expectedLength)", fetch)
        self.assertIn("ota_http::shouldResume(resumes, before, delivered, expectedLength)", fetch)
        self.assertIn('Range: bytes=%llu-', fetch)
        for cause in ("TLS_READ_ERROR", "PEER_CLOSED", "BODY_IDLE"):
            self.assertIn(cause, fetch)
        # A 206 is only ever accepted for a resume request.
        self.assertIn("code != 200 && !(offset > 0 && code == 206)", fetch)


if __name__ == "__main__":
    unittest.main()
