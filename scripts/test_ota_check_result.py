"""Compile the actual Wi-Fi completion handler with doubles for its device/UI collaborators."""
import subprocess
import tempfile
import unittest
from pathlib import Path


class CheckResult(unittest.TestCase):
    def test_no_update_success_failure_and_cancel_use_the_correct_screen(self):
        root = Path(__file__).resolve().parent.parent
        text = (root / "src/activities/settings/OtaUpdateActivity.cpp").read_text()
        handler = text[text.index("void OtaUpdateActivity::onWifiSelectionComplete("):
                       text.index("void OtaUpdateActivity::onEnter()")]
        harness = r'''
#include <cassert>
#define LOG_ERR(...)
#define LOG_DBG(...)
enum { STR_CANCEL, STR_UPDATE, STR_OTA_DOWNGRADE };
const char* tr(int id) { return id == STR_OTA_DOWNGRADE ? "downgrade" : "update"; }
struct OtaUpdater {
  enum Error { OK, NO_UPDATE, HTTP_ERROR, JSON_PARSE_ERROR, UPDATE_OLDER_ERROR,
               INTERNAL_UPDATE_ERROR, OOM_ERROR, VERIFY_ERROR };
  Error result = OK;
  bool newer = false;
  bool downgrade = false;
  bool isChannelDowngrade() { return downgrade; }
  int checks = 0;
  Error checkForUpdate() { ++checks; return result; }
  bool isUpdateNewer() { return newer; }
};
struct OtaUpdateActivity {
  enum State { CHECKING_FOR_UPDATE, FAILED, NO_UPDATE, WAITING_CONFIRMATION };
  State state = CHECKING_FOR_UPDATE;
  OtaUpdater updater;
  bool finished = false;
  struct Popup {
    bool shown = false;
    const char* title = nullptr;
    template<class F> void show(const char* t, const char**, int n, int selected, F) {
      assert(n == 2 && selected == 0); shown = true; title = t;
    }
  } confirmPopup;
  void finish() { finished = true; }
  void requestUpdateAndWait() {}
  void runUpdateInstall() { assert(false); }
  void onWifiSelectionComplete(bool);
};
struct RenderLock { explicit RenderLock(OtaUpdateActivity&) {} };
'''
        checks = r'''
int main() {
  for (int result = OtaUpdater::OK; result <= OtaUpdater::VERIFY_ERROR; ++result) {
    for (bool newer : {false, true}) {
      OtaUpdateActivity activity;
      activity.updater.result = static_cast<OtaUpdater::Error>(result);
      activity.updater.newer = newer;
      activity.onWifiSelectionComplete(true);
      assert(activity.updater.checks == 1);
      if (result == OtaUpdater::NO_UPDATE || (result == OtaUpdater::OK && !newer))
        assert(activity.state == OtaUpdateActivity::NO_UPDATE && !activity.confirmPopup.shown);
      else if (result == OtaUpdater::OK)
        assert(activity.state == OtaUpdateActivity::WAITING_CONFIRMATION && activity.confirmPopup.shown);
      else
        assert(activity.state == OtaUpdateActivity::FAILED && !activity.confirmPopup.shown);
      assert(!activity.finished);
    }
  }
  OtaUpdateActivity disconnected;
  disconnected.onWifiSelectionComplete(false);
  assert(disconnected.finished && disconnected.updater.checks == 0);
  OtaUpdateActivity older;
  older.updater.newer = true;
  older.updater.downgrade = true;
  older.onWifiSelectionComplete(true);
  assert(older.confirmPopup.shown && std::string(older.confirmPopup.title) == "downgrade");
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp = Path(temp) / "check.cpp"
            cpp.write_text("#include <initializer_list>\n#include <string>\n" + harness + handler + checks)
            binary = Path(temp) / "check"
            subprocess.run(["c++", "-std=c++17", str(cpp), "-o", str(binary)],
                           check=True, capture_output=True)
            subprocess.run([str(binary)], check=True, capture_output=True)


if __name__ == "__main__":
    unittest.main()
