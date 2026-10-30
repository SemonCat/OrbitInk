#!/usr/bin/env python3
"""
Headless scenarios for the first-run BookOrbit setup guide on [env:simulator_x3_zh_tw], with the
packaged GuanKiapTsingKhai SD fonts (CROSSFADE_SIM_ZH_TW_FONTS, see sim_bookorbit_home_smoke.py).

    CROSSFADE_SIM_ZH_TW_FONTS=<pkg>/sd-assets/fonts python3 scripts/sim_bookorbit_setup_smoke.py [--no-build]
    ... --serve   # leave the BookOrbit-setup hotspot running for manual/browser checks; prints the URL

Scenarios (fresh profile = only reading stats preset; the zh-TW profile migration picks JOURNAL):
  G1  fresh: Home shows the guide; Down/Down/Confirm = 稍後再說 -> Home; marker persisted, no
      bookorbit.json written. G1b: reboot on the same SD -> straight to Home, no guide.
  G2  configured (placeholder URL/user/password, marker unset = an upgraded device): Home, no guide,
      and the first run is recorded as handled. G2b: that account cleared -> reboot -> still no guide.
  G3  partial (URL + username, no password): guide lists 尚未設定：密碼; 在本機設定 opens the
      existing BookOrbit settings, Back returns to the guide (still incomplete).
  G4  other theme (LYRA): no guide.  G5  reader resume on a fresh profile: reader, no guide.
  G6  re-entry after 稍後再說: BookOrbit settings (from the unconfigured 書庫 tile; also at 設定 > 系統 >
      BookOrbit 同步) -> 用網頁設定 (last row) -> the BookOrbit mode list; no WiFi until a mode is chosen.
  W1  用網頁設定 -> mode list without Calibre (2 rows) -> 建立熱點 -> server screen, while the
      actual firmware HTTP server (simulator loopback port) is exercised: GET /settings, GET/POST
      /api/bookorbit (first save with empty password stays unconfigured, password write-only,
      blank keeps it, 400 on bad input leaves state, clear keeps the URL), then a final save.
      W1b: reboot -> no guide, handled recorded. W1c: then cleared -> reboot -> still no guide.

Simulator only: the soft AP is the simulator's stub (127.0.0.1), the HTTP server is the simulator's
loopback WebServer shim running the firmware's own CrossPointWebServer handlers. No real WiFi,
no TLS, no BookOrbit server, no RAM or panel claims. Storage write failures cannot be injected here;
rollback-on-failed-save is covered by test/sync_web_settings.
"""

import argparse
import gzip
import json
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sim_bookorbit_fixture_epub as fixture_epub  # noqa: E402
import sim_bookorbit_home_smoke as bo  # noqa: E402
import sim_smoke  # noqa: E402
from sim_smoke import EXPECTED_HEIGHT, EXPECTED_WIDTH, REPO_ROOT, bmp_size, launch  # noqa: E402

PROGRAM = bo.PROGRAM
sim_smoke.PROGRAM = PROGRAM
LYRA = 1
GUIDE = "Entering activity: BookOrbitSetupGuide"
# Non-secret placeholders only.
PLACEHOLDER_URL = "https://bookorbit.invalid"
PLACEHOLDER_USER = "fixture"
PLACEHOLDER_PASS = "fixture-placeholder"


def make_sd(work: Path, settings: dict, bookorbit=None) -> Path:
    cp = work / "fs_" / ".crosspoint"
    cp.mkdir(parents=True)
    (work / "shots").mkdir()
    (work / "fs_" / "books").mkdir()
    shutil.copytree(bo.zh_tw_fonts(), work / "fs_" / "fonts")
    (cp / "crossfade-settings.json").write_text(json.dumps(settings))
    if bookorbit is not None:
        # Legacy plaintext form; the store accepts and re-obfuscates it on load.
        (cp / "bookorbit.json").write_text(json.dumps(bookorbit))
    return cp


def settings_of(work: Path) -> dict:
    return json.loads((work / "fs_" / ".crosspoint" / "crossfade-settings.json").read_text())


def clear_credentials(work: Path) -> None:
    """The stored state the firmware's clear leaves (SyncSettingsWebPolicy::withoutCredentials): URL kept."""
    (work / "fs_" / ".crosspoint" / "bookorbit.json").write_text(json.dumps(
        {"username": "", "password": "", "serverUrl": PLACEHOLDER_URL}))


def free_port_pair() -> int:
    """An HTTP port whose +1 (the simulator's WebSocket port) is also free."""
    for _ in range(50):
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        if port >= 65534:
            continue
        try:
            with socket.socket() as s2:
                s2.bind(("127.0.0.1", port + 1))
            return port
        except OSError:
            continue
    raise RuntimeError("no free port pair")


def run_sim(name, work, actions, shots, args, failures, extra_env=None, timeout=None):
    env = {"CROSSPOINT_SIM_INPUT_SCRIPT": actions, **(extra_env or {})}
    if shots:
        env["CROSSPOINT_SIM_SCREENSHOTS"] = ";".join(f"{ms}:./shots/{f}" for ms, f, _ in shots)
    result = launch(work, args.video_driver, timeout or args.timeout, env, label=f"setup:{name}")
    log = (result.stdout or "") + (result.stderr or "")
    (work / f"{name}.log").write_text(log)
    if result.returncode != 0:
        failures.append(f"{name}: simulator exited {result.returncode}")
    for line in [ln for ln in log.splitlines() if "[ERR]" in ln][:5]:
        failures.append(f"{name}: firmware error: {line.strip()}")
    if "No glyph for codepoint" in log:
        failures.append(f"{name}: a UI string rendered without glyphs (tofu)")
    for _, f, _ in shots:
        size = bmp_size(work / "shots" / f) if (work / "shots" / f).exists() else None
        if size != (EXPECTED_WIDTH, EXPECTED_HEIGHT):
            failures.append(f"{name}: screenshot {f} missing or wrong size ({size})")
    return log


def http(base, method, path, body=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(base + path, data=data, method=method,
                                 headers={"Content-Type": "application/json"} if data else {})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            raw = r.read()
            if r.headers.get("Content-Encoding") == "gzip" or raw[:2] == b"\x1f\x8b":
                raw = gzip.decompress(raw)  # pages are served pre-gzipped
            return r.status, raw.decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")


def wait_for_server(base, deadline_s):
    end = time.time() + deadline_s
    while time.time() < end:
        try:
            if http(base, "GET", "/api/status")[0] == 200:
                return True
        except OSError:
            pass
        time.sleep(0.3)
    return False


def api_checks(base, out: dict):
    """Firmware HTTP handlers through the simulator loopback; results recorded, asserted later."""
    steps = []

    def step(label, method, path, body=None):
        status, text = http(base, method, path, body)
        try:
            parsed = json.loads(text)
        except ValueError:
            parsed = None
        steps.append({"label": label, "method": method, "path": path, "status": status,
                      "json": parsed, "text": None if parsed is not None else text[:200]})
        return status, parsed, text

    if not wait_for_server(base, 30):
        out["error"] = "firmware web server never answered /api/status"
        return
    s, _, page = step("settings page", "GET", "/settings")
    out["settings_page_has_bookorbit"] = s == 200 and "bookorbit-container" in page and "Save BookOrbit" in page
    step("initial", "GET", "/api/bookorbit")
    step("first save, no password", "POST", "/api/bookorbit",
         {"serverUrl": PLACEHOLDER_URL, "username": PLACEHOLDER_USER, "password": ""})
    step("bad input (unknown key)", "POST", "/api/bookorbit", {"serverUrl": "x", "bogus": 1})
    step("after bad input", "GET", "/api/bookorbit")
    step("save password", "POST", "/api/bookorbit",
         {"serverUrl": PLACEHOLDER_URL, "username": PLACEHOLDER_USER, "password": PLACEHOLDER_PASS})
    step("blank keeps password", "POST", "/api/bookorbit",
         {"serverUrl": PLACEHOLDER_URL, "username": PLACEHOLDER_USER, "password": ""})
    step("reload", "GET", "/api/bookorbit")
    step("clear", "POST", "/api/bookorbit/clear")
    step("after clear", "GET", "/api/bookorbit")
    step("final save", "POST", "/api/bookorbit",
         {"serverUrl": PLACEHOLDER_URL, "username": PLACEHOLDER_USER, "password": PLACEHOLDER_PASS})
    out["steps"] = steps


def check_api(out: dict, failures):
    if "error" in out:
        failures.append(f"W1: {out['error']}")
        return
    if not out.get("settings_page_has_bookorbit"):
        failures.append("W1: GET /settings did not serve the BookOrbit card")
    by = {s["label"]: s for s in out.get("steps", [])}

    def j(label):
        return by.get(label, {}).get("json") or {}

    for s in out.get("steps", []):
        if s["json"] and "password" in s["json"]:
            failures.append(f"W1: {s['label']}: the password was returned")
    if j("initial").get("configured") is not False:
        failures.append(f"W1: fresh profile reported configured: {j('initial')}")
    first = j("first save, no password")
    if by.get("first save, no password", {}).get("status") != 200 or first.get("configured") is not False \
            or first.get("hasPassword") is not False:
        failures.append(f"W1: a first save without password looked configured: {by.get('first save, no password')}")
    if by.get("bad input (unknown key)", {}).get("status") != 400:
        failures.append(f"W1: bad input was not rejected with 400: {by.get('bad input (unknown key)')}")
    if j("after bad input") != first:
        failures.append(f"W1: rejected input changed state: {j('after bad input')} vs {first}")
    if j("save password").get("configured") is not True:
        failures.append(f"W1: saving all three fields was not configured: {j('save password')}")
    if j("blank keeps password").get("hasPassword") is not True or j("reload").get("configured") is not True:
        failures.append("W1: a blank password did not keep the saved one")
    if j("reload").get("serverUrl") != PLACEHOLDER_URL or j("reload").get("username") != PLACEHOLDER_USER:
        failures.append(f"W1: reload did not return the saved URL/username: {j('reload')}")
    ac = j("after clear")
    if ac.get("configured") is not False or ac.get("hasPassword") is not False or ac.get("username") != "":
        failures.append(f"W1: clear did not remove the credentials: {ac}")
    if j("final save").get("configured") is not True:
        failures.append("W1: final save failed")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--video-driver", default="dummy")
    ap.add_argument("--timeout", type=int, default=90)
    ap.add_argument("--keep", help="artifact directory (default: a new temp dir, kept)")
    ap.add_argument("--export", help="also write PNG copies of the screenshots here")
    ap.add_argument("--serve", action="store_true",
                    help="only start the BookOrbit-setup hotspot on a fresh profile and keep it up (--serve-seconds)")
    ap.add_argument("--serve-seconds", type=int, default=1800)
    ap.add_argument("--port", type=int, help="simulator HTTP port (default: a free pair)")
    args = ap.parse_args()
    bo.zh_tw_fonts()
    if not args.no_build:
        pio = sim_smoke.find_pio()
        cmd = [pio, "run", "-e", bo.PIOENV] if pio else [sys.executable, "-m", "platformio", "run", "-e", bo.PIOENV]
        if subprocess.run(cmd, cwd=REPO_ROOT).returncode != 0:
            print("[setup] FAIL: build failed")
            return 1
    root = Path(args.keep) if args.keep else Path(tempfile.mkdtemp(prefix="crossfade-sim-bosetup-"))
    root.mkdir(parents=True, exist_ok=True)
    print(f"[setup] artifacts: {root}")
    failures: "list[str]" = []
    captured: "list[tuple[Path, str]]" = []
    base = {"trackReadingStats": 1}
    # Guide -> 用網頁設定 -> mode list (加入網路, 建立熱點) -> 建立熱點.
    hotspot_keys = "2500:ENTER;4000:DOWN;5000:ENTER"

    if args.serve:
        port = args.port or free_port_pair()
        ws = root / "serve"
        make_sd(ws, base)
        info = {"url": f"http://127.0.0.1:{port}/settings", "port": port, "work": str(ws),
                "started": time.strftime("%Y-%m-%dT%H:%M:%S%z")}
        (root / "serve.json").write_text(json.dumps(info, indent=2))
        print(f"[setup] serving {info['url']} for {args.serve_seconds}s (info: {root / 'serve.json'})", flush=True)
        run_sim("serve", ws, f"{hotspot_keys};{args.serve_seconds * 1000}:QUIT",
                [(7000, "S-hotspot.bmp", "hotspot running")], args, failures,
                {"CROSSPOINT_SIM_HTTP_PORT": str(port)}, timeout=args.serve_seconds + 60)
        print("\n".join(failures) or "[setup] serve ended")
        return 1 if failures else 0

    # ---- G1: fresh -> guide -> Later ---------------------------------------------------------------
    w1 = root / "G1"
    make_sd(w1, base)
    shots1 = [(1800, "G1-01-guide.bmp", "fresh profile: setup guide"),
              (3300, "G1-02-guide-later-focused.bmp", "focus: 稍後再說"),
              (4800, "G1-03-home-after-later.bmp", "Home after 稍後再說")]
    log1 = run_sim("G1", w1, "2500:DOWN;3000:DOWN;3800:ENTER;6000:QUIT", shots1, args, failures)
    captured += [(w1 / "shots" / f, d) for _, f, d in shots1]
    s1 = settings_of(w1)
    if GUIDE not in log1:
        failures.append("G1: the fresh BookOrbit-first profile did not show the guide")
    if s1.get("bookOrbitSetupGuideDismissed") != 1:
        failures.append(f"G1: 稍後再說 did not persist the marker ({s1.get('bookOrbitSetupGuideDismissed')})")
    if (w1 / "fs_" / ".crosspoint" / "bookorbit.json").exists():
        failures.append("G1: 稍後再說 wrote BookOrbit credentials")
    if "Turning on WiFi" in log1 or "Starting Access Point" in log1:
        failures.append("G1: WiFi was started without a network choice")
    # ---- G1b: reboot -> no guide ----
    shots1b = [(1800, "G1-04-reboot-home.bmp", "reboot after 稍後再說: Home, no guide")]
    log1b = run_sim("G1b", w1, "3000:QUIT", shots1b, args, failures)
    captured += [(w1 / "shots" / f, d) for _, f, d in shots1b]
    if GUIDE in log1b:
        failures.append("G1b: the guide came back after 稍後再說 and a reboot")

    # ---- G6: re-entry after 稍後再說 (G1's profile): the unconfigured 書庫 tile opens BookOrbit settings,
    # whose last row (Up wraps to it) is 用網頁設定 -> the BookOrbit mode list. Back from it goes Home.
    shots6 = [(3300, "G6-01-bookorbit-settings.bmp", "BookOrbit 同步 from Home after 稍後再說"),
              (4600, "G6-02-web-entry-focused.bmp", "focus: 用網頁設定 (last row)"),
              (6200, "G6-03-mode-list.bmp", "BookOrbit mode list from settings")]
    log6 = run_sim("G6", w1, "2500:ENTER;3800:UP;5200:ENTER;7500:QUIT", shots6, args, failures)
    captured += [(w1 / "shots" / f, d) for _, f, d in shots6]
    if "Entering activity: BookOrbitSettings" not in log6 or "Entering activity: CrossPointWebServer" not in log6:
        failures.append("G6: BookOrbit settings -> 用網頁設定 did not open the web setup")
    if GUIDE in log6:
        failures.append("G6: the guide came back after 稍後再說")
    if "Turning on WiFi" in log6 or "Starting Access Point" in log6:
        failures.append("G6: WiFi started before a network mode was chosen")

    # ---- G2: configured -> no guide ---------------------------------------------------------------
    w2 = root / "G2"
    make_sd(w2, base, {"username": PLACEHOLDER_USER, "password": PLACEHOLDER_PASS, "serverUrl": PLACEHOLDER_URL})
    shots2 = [(1800, "G2-01-home-configured.bmp", "configured profile: Home, no guide")]
    log2 = run_sim("G2", w2, "3000:QUIT", shots2, args, failures)
    captured += [(w2 / "shots" / f, d) for _, f, d in shots2]
    if GUIDE in log2:
        failures.append("G2: a configured profile was shown the guide")
    if settings_of(w2).get("bookOrbitSetupGuideDismissed") != 1:
        failures.append("G2: an already-configured profile did not record the first run as handled")
    # ---- G2b: that account later cleared (the state POST /api/bookorbit/clear persists: URL kept,
    # username/password empty) -> reboot -> Home, no guide ----
    clear_credentials(w2)
    shots2b = [(1800, "G2-02-home-after-clear.bmp", "configured once, then cleared: Home, no guide")]
    log2b = run_sim("G2b", w2, "3000:QUIT", shots2b, args, failures)
    captured += [(w2 / "shots" / f, d) for _, f, d in shots2b]
    if GUIDE in log2b:
        failures.append("G2b: clearing a once-configured account reopened the first-run guide")

    # ---- G3: partial -> guide lists the password; device option -> settings -> Back -> guide ----
    w3 = root / "G3"
    make_sd(w3, base, {"username": PLACEHOLDER_USER, "password": "", "serverUrl": PLACEHOLDER_URL})
    shots3 = [(1800, "G3-01-guide-partial.bmp", "partial account: 尚未設定：密碼"),
              (3300, "G3-02-guide-device-focused.bmp", "focus: 在本機設定"),
              (4800, "G3-03-bookorbit-settings.bmp", "existing BookOrbit settings"),
              (6600, "G3-04-guide-again.bmp", "Back: guide again (still incomplete)")]
    log3 = run_sim("G3", w3, "2500:DOWN;3800:ENTER;5500:BACK;7500:QUIT", shots3, args, failures)
    captured += [(w3 / "shots" / f, d) for _, f, d in shots3]
    if log3.count(GUIDE) < 2 or "Entering activity: BookOrbitSettings" not in log3:
        failures.append("G3: partial setup did not go guide -> BookOrbit settings -> guide")
    if settings_of(w3).get("bookOrbitSetupGuideDismissed") == 1:
        failures.append("G3: visiting on-device setup marked the guide dismissed")

    # ---- G4: other theme -> no guide ----
    w4 = root / "G4"
    make_sd(w4, {**base, "uiTheme": LYRA, "zhTwHomeProfileApplied": 1, "zhTwJournalProfileApplied": 1})
    shots4 = [(1800, "G4-01-lyra-home.bmp", "LYRA theme, unconfigured: no guide")]
    log4 = run_sim("G4", w4, "3000:QUIT", shots4, args, failures)
    captured += [(w4 / "shots" / f, d) for _, f, d in shots4]
    if GUIDE in log4:
        failures.append("G4: a non-BookOrbit theme was shown the guide")

    # ---- G5: reader resume on a fresh profile -> reader, no guide ----
    w5 = root / "G5"
    cp5 = make_sd(w5, base)
    fixture_epub.write(w5 / "fs_" / "books" / "resume.epub", title=fixture_epub.TITLE, author=fixture_epub.AUTHOR)
    (cp5 / "state.json").write_text(json.dumps(
        {"openEpubPath": "/books/resume.epub", "lastSleepFromReader": True, "readerActivityLoadCount": 0}))
    log5 = run_sim("G5", w5, "4000:QUIT", [], args, failures)
    if "Loading ePub: /books/resume.epub" not in log5 or GUIDE in log5:
        failures.append("G5: reader resume did not open the book directly (or showed the guide)")

    # ---- W1: web setup over the hotspot + firmware HTTP --------------------------------------------
    ww = root / "W1"
    make_sd(ww, base)
    port = args.port or free_port_pair()
    api_out: dict = {}
    t = threading.Thread(target=api_checks, args=(f"http://127.0.0.1:{port}", api_out), daemon=True)
    t.start()
    shotsw = [(3300, "W1-01-guide-web-focused.bmp", "guide, 用網頁設定 focused (default)"),
              (4600, "W1-02-mode-list.bmp", "mode list without Calibre"),
              (5600, "W1-03-mode-hotspot-focused.bmp", "focus: 建立熱點"),
              (9000, "W1-04-hotspot-running.bmp", "BookOrbit web setup over the hotspot")]
    logw = run_sim("W1", ww, "3800:ENTER;5000:DOWN;6200:ENTER;30000:QUIT", shotsw, args, failures,
                   {"CROSSPOINT_SIM_HTTP_PORT": str(port)})
    t.join(timeout=5)
    captured += [(ww / "shots" / f, d) for _, f, d in shotsw]
    if "Connect to Calibre" in logw:
        failures.append("W1: the BookOrbit mode list selected Calibre")
    if "Network mode selected: Create Hotspot" not in logw:
        failures.append("W1: 建立熱點 was not the second row of the BookOrbit mode list")
    check_api(api_out, failures)
    layouts = re.findall(r"BookOrbit setup layout: qr (-?\d+), bottom (\d+), hints top (\d+)", logw)
    if not layouts:
        failures.append("W1: the BookOrbit setup server screen was never laid out")
    for qr, bottom, hints in layouts:
        if int(bottom) > int(hints) or int(qr) < 100:
            failures.append(f"W1: setup screen does not fit: qr {qr}, bottom {bottom}, hints top {hints}")
    if "Outside range" in logw:
        failures.append("W1: something was drawn outside the screen")
    (root / "W1-api.json").write_text(json.dumps(api_out, indent=2, ensure_ascii=False))
    # ---- W1b: reboot after web setup -> configured -> no guide ----
    shotswb = [(1800, "W1-05-reboot-home.bmp", "reboot after web setup: Home, no guide")]
    logwb = run_sim("W1b", ww, "3000:QUIT", shotswb, args, failures)
    captured += [(ww / "shots" / f, d) for _, f, d in shotswb]
    if GUIDE in logwb:
        failures.append("W1b: the guide was shown after a successful web setup")
    if settings_of(ww).get("bookOrbitSetupGuideDismissed") != 1:
        failures.append("W1b: a successful web setup was not recorded as handled")
    # ---- W1c: cleared after web setup -> reboot -> no guide ----
    clear_credentials(ww)
    logwc = run_sim("W1c", ww, "3000:QUIT", [], args, failures)
    if GUIDE in logwc:
        failures.append("W1c: clearing after web setup reopened the first-run guide")

    for path, desc in captured:
        print(f"  {'ok' if path.exists() else 'MISSING':8} {path.relative_to(root)}  {desc}")
    if args.export:
        out = Path(args.export)
        out.mkdir(parents=True, exist_ok=True)
        for path, _ in captured:
            if path.exists():
                subprocess.run(["sips", "-s", "format", "png", str(path), "--out", str(out / (path.stem + ".png"))],
                               capture_output=True)
    if failures:
        print("\n[setup] FAIL:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print(f"\n[setup] PASS: {len(captured)} screenshots, firmware HTTP {len(api_out.get('steps', []))} steps")
    return 0


if __name__ == "__main__":
    sys.exit(main())
