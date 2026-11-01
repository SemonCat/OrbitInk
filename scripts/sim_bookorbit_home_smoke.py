#!/usr/bin/env python3
"""
Headless scenarios for the BookOrbit-first Home and the single BookOrbit library, on
[env:simulator_x3], against the FIXTURE bridge (scripts/sim_bookorbit_fixture_bridge.py).

    python3 scripts/sim_bookorbit_home_smoke.py              # build, run, verify
    python3 scripts/sim_bookorbit_home_smoke.py --no-build   # reuse the existing binary

Every scenario drives the production firmware through its ordinary UI with scripted button
presses. The fixture bridge answers BookOrbit's catalog/download/progress/page-stats routes from
test/bookorbit_fixture/ and a throwaway state file; it never opens a socket, so no run here touches
a real BookOrbit server or anyone's reading history. Results are FIXTURE CONTRACT checks of the
production client paths, not live-server acceptance.

Fixture state written directly (formats the firmware itself reads): crossfade-settings.json
(uiTheme 6 = BOOKORBIT, reading stats on), bookorbit.json and wifi.json with non-secret
placeholder credentials in their legacy plaintext form (the stores upgrade them on load).

Scenarios:
  A  online: Home (empty) -> Library root -> All books -> detail -> Download -> Open -> reader
  B  same SD, next boot: Home hero is the downloaded book; Confirm opens the reader with no EPUB
     parse on Home; reading, then sleep: page-stats POST reaches the fixture; after wake, a second
     sync window sends no duplicate (fixture ledger unchanged, queue file drained)
  C  offline (transport failure): Library opens offline, 已下載 lists the book, Open reads it
  D  download fault (HTTP 500): detail shows the failure and keeps 下載 as a retry, no file left
  E  BookOrbit not configured: the Library tile becomes 設定 BookOrbit and opens its settings

Not covered here, and why: the WiFi heap-defrag reboot (ESP.restart is a no-op in the simulator)
has host tests (test/bookorbit_sync) and remains a device-verification item; the periodic timer wake
has its own accelerated suite, scripts/sim_periodic_sync_smoke.py. Nothing here measures RAM, e-ink
waveforms, WiFi, TLS or power.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sim_bookorbit_fixture_epub as fixture_epub  # noqa: E402
import sim_smoke  # noqa: E402
from sim_smoke import EXPECTED_HEIGHT, EXPECTED_WIDTH, REPO_ROOT, bmp_size, launch  # noqa: E402

# The zh-TW profile build: same simulator, plus CROSSFADE_CUSTOM_ZH_TW (the shipped X3 profile's
# one-time font/language/Home migration and shared 14pt CJK UI fallback).
PIOENV = "simulator_x3_zh_tw"
PROGRAM = REPO_ROOT / ".pio" / "build" / PIOENV / "program"
sim_smoke.PROGRAM = PROGRAM  # launch() runs sim_smoke.PROGRAM

BRIDGE = REPO_ROOT / "scripts" / "sim_bookorbit_fixture_bridge.py"
SECOND_SRC = REPO_ROOT / "test" / "epubs" / "test_br_section_break.epub"
BASE_URL = "https://bookorbit.fixture.invalid"
# The X3 zh-TW product renders its UI with the packaged GuanKiapTsingKhai SD font (the builtin UI
# font is Latin-only). Copying the seven packaged sizes lets the profile's own one-time migration
# switch language/font/theme exactly as on a device. The font is a private release asset that is
# not tracked in git, so its location is REQUIRED explicitly: CROSSFADE_SIM_ZH_TW_FONTS must name
# the directory holding GuanKiapTsingKhai/GuanKiapTsingKhai_{14,18,20,22,24,26,28}.cpfont -- the
# `sd-assets/fonts` directory of a package produced by scripts/release_x3_zh_tw.py.
FONTS_ENV = "CROSSFADE_SIM_ZH_TW_FONTS"
REQUIRED_FONT_SIZES = (14, 18, 20, 22, 24, 26, 28)


def zh_tw_fonts() -> Path:
    value = os.environ.get(FONTS_ENV, "")
    root = Path(value) if value else None
    missing = list(REQUIRED_FONT_SIZES) if root is None else [
        s for s in REQUIRED_FONT_SIZES if not (root / "GuanKiapTsingKhai" / f"GuanKiapTsingKhai_{s}.cpfont").is_file()]
    if missing:
        sys.exit(f"[bookorbit] FAIL: {FONTS_ENV} must point at a packaged sd-assets/fonts directory "
                 f"(GuanKiapTsingKhai_<size>.cpfont, sizes {REQUIRED_FONT_SIZES}); got {value or 'unset'}, "
                 f"missing sizes {missing}. Build a package with `python3 scripts/release_x3_zh_tw.py` "
                 f"and set {FONTS_ENV}=dist/<package>/sd-assets/fonts.")
    return root

# Keys: ENTER=Confirm, BACK=Back, DOWN/UP side buttons (next/previous), SLEEP, QUIT.
STEP = 1300


def script(*actions: str, start: int = 1500, quit_after: int = 3000) -> "tuple[str, int]":
    t = start
    items = []
    for a in actions:
        if a.startswith("wait"):
            t += int(a[4:])
            continue
        items.append(f"{t}:{a}")
        t += STEP
    items.append(f"{t + quit_after}:QUIT")
    return ";".join(items), t


def write_fixture(root: Path) -> Path:
    fx = root / "fixture"
    fx.mkdir()
    # Book 101: cover-bearing zh-TW EPUB; the catalog serves its real embedded title/author.
    fixture_epub.write(fx / "dune.epub")
    shutil.copy2(SECOND_SRC, fx / "other.epub")
    catalog = {
        "root": [{"section": "all-books", "title": "全部書籍"}, {"section": "recent", "title": "最近新增"}],
        "books": [
            {"id": 101, "title": fixture_epub.TITLE, "authors": [fixture_epub.AUTHOR], "progressPercentage": 12.5,
             "files": [{"id": 1001, "format": "epub", "file": "dune.epub"}]},
            {"id": 102, "title": "Second Book With A Fairly Long English Title For Wrapping",
             "authors": ["Someone"], "files": [{"id": 1002, "format": "epub", "file": "other.epub"}]},
        ],
    }
    (fx / "catalog.json").write_text(json.dumps(catalog, ensure_ascii=False))
    return fx


def make_sd(work: Path, configured: bool = True) -> None:
    cp = work / "fs_" / ".crosspoint"
    cp.mkdir(parents=True)
    (work / "shots").mkdir()
    shutil.copytree(zh_tw_fonts(), work / "fs_" / "fonts")
    # Only reading stats is preset; language, font and the BookOrbit Home theme come from the
    # profile migration itself (asserted below via its log lines). The Journal marker is preset so
    # this suite keeps covering the BOOKORBIT theme as a regression; the Journal theme and its
    # migration have their own suite, scripts/sim_journal_home_smoke.py.
    # The first-run setup guide is dismissed too (Home is under test; the guide has its own suite,
    # scripts/sim_bookorbit_setup_smoke.py).
    (cp / "crossfade-settings.json").write_text(json.dumps(
        {"trackReadingStats": 1, "zhTwJournalProfileApplied": 1, "bookOrbitSetupGuideDismissed": 1}))
    if configured:
        # Non-secret placeholders; legacy plaintext form is accepted and re-obfuscated on load.
        (cp / "bookorbit.json").write_text(json.dumps(
            {"username": "fixture", "password": "fixture-placeholder", "serverUrl": BASE_URL}))
        (cp / "wifi.json").write_text(json.dumps(
            {"lastConnectedSsid": "Simulator WiFi (fake)",
             "credentials": [{"ssid": "Simulator WiFi (fake)", "password": ""}]}))


def fixture_env(fx: Path, state: Path, log: Path) -> "dict[str, str]":
    return {
        "CROSSFADE_SIM_HTTP_BRIDGE": str(BRIDGE),
        "CROSSFADE_SIM_FIXTURE_DIR": str(fx),
        "CROSSFADE_SIM_FIXTURE_STATE": str(state),
        "CROSSFADE_SIM_FIXTURE_LOG": str(log),
    }


def set_faults(state: Path, faults: dict) -> None:
    data = json.loads(state.read_text()) if state.exists() else {}
    data["faults"] = faults
    state.write_text(json.dumps(data))


def requests(log: Path) -> "list[dict]":
    return [json.loads(l) for l in log.read_text().splitlines()] if log.exists() else []


class Run:
    def __init__(self, args, failures):
        self.args = args
        self.failures = failures
        self.shots: "list[tuple[Path, str]]" = []

    def go(self, name, work, env, actions, shots, after_wake=None, after_wake_shots=()):
        sim_env = dict(env)
        sim_env["CROSSPOINT_SIM_INPUT_SCRIPT"] = actions
        if shots:
            sim_env["CROSSPOINT_SIM_SCREENSHOTS"] = ";".join(f"{ms}:./shots/{f}" for ms, f, _ in shots)
        if after_wake:
            sim_env["CROSSPOINT_SIM_INPUT_SCRIPT_AFTER_WAKE"] = after_wake
        if after_wake_shots:
            sim_env["CROSSPOINT_SIM_SCREENSHOTS_AFTER_WAKE"] = ";".join(
                f"{ms}:./shots/{f}" for ms, f, _ in after_wake_shots)
        result = launch(work, self.args.video_driver, self.args.timeout, sim_env, label=f"bookorbit:{name}")
        log = (result.stdout or "") + (result.stderr or "")
        (work / f"{name}.log").write_text(log)
        if result.returncode != 0:
            self.failures.append(f"{name}: simulator exited {result.returncode}")
        for line in [ln for ln in log.splitlines() if "[ERR]" in ln and self.unexpected_err(ln)][:5]:
            self.failures.append(f"{name}: firmware error: {line.strip()}")
        for ms, f, desc in list(shots) + list(after_wake_shots):
            path = work / "shots" / f
            size = bmp_size(path) if path.exists() else None
            if size != (EXPECTED_WIDTH, EXPECTED_HEIGHT):
                self.failures.append(f"{name}: screenshot {f} missing or wrong size ({size})")
            self.shots.append((path, f"{name}: {desc}"))
        return log

    expected_errs = ()

    def unexpected_err(self, line: str) -> bool:
        return not any(e in line for e in self.expected_errs)


def need(failures, cond, msg):
    if not cond:
        failures.append(msg)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--video-driver", default="dummy")
    ap.add_argument("--timeout", type=int, default=90)
    ap.add_argument("--keep", help="artifact directory (default: a new temp dir, kept)")
    ap.add_argument("--only", choices=["AB1"], help="run only scenario A then B1 (targeted re-check)")
    args = ap.parse_args()
    zh_tw_fonts()  # fail fast, before building, with setup instructions
    if not args.no_build:
        print(f"[bookorbit] building: pio run -e {PIOENV}")
        pio = sim_smoke.find_pio()
        cmd = [pio, "run", "-e", PIOENV] if pio else [sys.executable, "-m", "platformio", "run", "-e", PIOENV]
        if subprocess.run(cmd, cwd=REPO_ROOT).returncode != 0:
            return fail(f"build of {PIOENV} failed")
    if not PROGRAM.exists():
        return fail(f"no simulator binary at {PROGRAM}")

    root = Path(args.keep) if args.keep else Path(tempfile.mkdtemp(prefix="crossfade-sim-bookorbit-"))
    root.mkdir(parents=True, exist_ok=True)
    print(f"[bookorbit] artifacts: {root}")
    failures: "list[str]" = []
    run = Run(args, failures)
    fx = write_fixture(root)
    def entered(log, name):
        return log.count(f"Entering activity: {name}")

    def no_tofu(name, log):
        need(failures, "No glyph for codepoint" not in log, f"{name}: a UI string rendered without glyphs (tofu)")
        need(failures, "Entering activity: BookOrbitSetupGuide" not in log, f"{name}: the setup guide covered Home")

    def opened_before_reader(log):
        """EPUB loads while Home/library was the active screen. Stops at the first handoff to the
        reader: the reader-open sync ("[BOAuto] Sync: doc=") legitimately loads the book after Home
        has already been left, so it is not a Home paint."""
        out = []
        for line in log.splitlines():
            if "Entering activity: Reader" in line or "[BOAuto] Sync: doc=" in line:
                break
            if "Loading ePub: " in line:
                out.append(line.split("Loading ePub: ", 1)[1].strip())
        return out

    def queue_files(work):
        d = work / "fs_" / ".crosspoint" / "bo_stats"
        return sorted(p.name for p in d.glob("*.bin")) if d.is_dir() else []

    def copy_sd(src, dst):
        shutil.copytree(src / "fs_", dst / "fs_")
        (dst / "shots").mkdir()

    # ---- A: online browse -> download -> open --------------------------------------------------
    wa = root / "A"
    make_sd(wa)
    state, flog = root / "state.json", root / "requests.jsonl"
    env = fixture_env(fx, state, flog)
    acts, _ = script("ENTER", "RIGHT", "ENTER", "ENTER", "wait2500", "ENTER", "wait4000")
    log_a = run.go("A", wa, env, acts, [
        (1300, "A1-home-empty.bmp", "Home, no book yet: hero invites the library"),
        (2600, "A2-library-root.bmp", "Library enters directly into cover books"),
        (3900, "A3-books.bmp", "All books: compact covers and title/author/status"),
        (5200, "A4-detail.bmp", "Book detail: 下載 is the primary action"),
        (7600, "A5-downloaded.bmp", "After download: 開啟閱讀 focused"),
        (12000, "A6-reader.bmp", "Reader on the downloaded copy"),
    ])
    no_tofu("A", log_a)
    need(failures, "Applied X3 zh-TW BookOrbit Home profile" in log_a, "A: zh-TW profile did not apply the BookOrbit Home")
    paths = [r["path"].rsplit("/koreader", 1)[1] for r in requests(flog)]
    for route in ("/plugin/catalog/books", "/plugin/catalog/books/101",
                  "/plugin/catalog/files/1001/download"):
        need(failures, route in paths, f"A: fixture never saw {route}")
    downloads = list((wa / "fs_" / "BookOrbit").glob("*.epub"))
    need(failures, len(downloads) == 1 and downloads[0].read_bytes() == (fx / "dune.epub").read_bytes(),
         "A: downloaded file missing or not byte-identical to the served file")
    need(failures, not list((wa / "fs_" / "BookOrbit").glob("*.tmp")), "A: a download temp file was left behind")
    need(failures, list((wa / "fs_" / ".crosspoint" / "bookorbit").glob("lib_*/index.bin")), "A: download not indexed")
    need(failures, "Entering activity: EpubReader" in log_a, "A: 開啟閱讀 did not open the reader")
    need(failures, not opened_before_reader(log_a), "A: Home/library parsed an EPUB before the reader opened")

    # ---- B1: next boot, hero is the downloaded book; read; Back (close sync probes capabilities) --
    wb = root / "B"
    copy_sd(wa, wb)
    acts, _ = script("ENTER", "wait1500", "DOWN", "wait3500", "DOWN", "wait3500", "BACK", "wait5000")
    log_b1 = run.go("B1", wb, env, acts, [
        (1300, "B1-home-hero.bmp", "Home: Continue Reading hero with cover, long zh-TW title, author"),
    ])
    no_tofu("B1", log_b1)
    # Home's paint never loads an EPUB; the one-time cover pass runs only AFTER the first paint.
    first_paint = log_b1.find("[BOHOME] First paint done")
    need(failures, first_paint >= 0 and "Loading ePub" not in log_b1[:first_paint],
         "B1: an EPUB was loaded before Home's first paint")
    thumbs = list((wb / "fs_" / ".crosspoint").glob("epub_*/thumb_226.bmp"))
    need(failures, thumbs, "B1: the hero cover thumbnail was never generated")
    need(failures, entered(log_b1, "EpubReader") >= 1, "B1: hero Confirm did not open the reader")
    need(failures, "/plugin/version" in [r["path"].rsplit("/koreader", 1)[1] for r in requests(flog)],
         "B1: the close sync never confirmed BookOrbit capabilities")

    if args.only == "AB1":
        for path, desc in run.shots:
            print(f"  {'ok' if path.exists() else 'MISSING':8} {path.relative_to(root)}  {desc}")
        if failures:
            print("\n[bookorbit] FAIL:")
            for f in failures:
                print(f"  - {f}")
            return 1
        print("\n[bookorbit] PASS (A+B1 only)")
        return 0

    # ---- B2: capture on; read with real dwell; Back drains stats; then a second window -------------
    before = len(json.loads(state.read_text()).get("stats", []))
    acts, _ = script("ENTER", "wait1500", "DOWN", "wait3500", "DOWN", "wait3500", "DOWN", "wait3500", "BACK",
                     "wait6000")
    log_b2 = run.go("B2", wb, env, acts, [
        (1300, "B2-home-hero-progress.bmp", "Home hero after reading: progress shown"),
    ])
    no_tofu("B2", log_b2)
    ledger = json.loads(state.read_text()).get("stats", [])
    need(failures, len(ledger) > before, "B2: no page-stats event reached the fixture after a reading session")
    need(failures, all(e.get("duration", 0) >= 2 for e in ledger), "B2: an uploaded dwell is under the 2 s minimum")
    need(failures, not queue_files(wb), f"B2: queue not drained after ACK ({queue_files(wb)})")

    def ledger_keys(st):
        return [(e["device"], e["hash"], e["page"], e["startTime"]) for e in json.loads(st.read_text()).get("stats", [])]

    # B3: the server rejects page-stats (503): read with real dwell; the events stay queued.
    keys_before = ledger_keys(state)
    set_faults(state, {"stats_status": 503})
    acts, _ = script("ENTER", "wait1500", "DOWN", "wait3500", "DOWN", "wait3500", "BACK", "wait5000")
    log_b3 = run.go("B3", wb, env, acts, [])
    no_tofu("B3", log_b3)
    need(failures, ledger_keys(state) == keys_before, "B3: a rejected upload changed the server ledger")
    need(failures, queue_files(wb), "B3: events were lost when the server rejected the upload")

    # B4: the server recovers: a later window delivers exactly those events, once.
    set_faults(state, {})
    acts, _ = script("ENTER", "BACK", "wait5000")  # ~1 s on the page: below the 2 s minimum dwell
    log_b4 = run.go("B4", wb, env, acts, [])
    keys_after = ledger_keys(state)
    need(failures, len(keys_after) > len(keys_before), "B4: retained events were not delivered after recovery")
    need(failures, len(set(keys_after)) == len(keys_after), "B4: the server ledger holds a duplicate event")
    need(failures, not queue_files(wb), f"B4: queue not drained after the retry ACK ({queue_files(wb)})")
    stat_posts = [r for r in requests(flog) if r["path"].endswith("/plugin/page-stats") and r.get("stats")]
    need(failures, all(sum(x.get("duplicates", 0) for x in r["stats"]) == 0 for r in stat_posts),
         "B: a retry re-sent an already acknowledged event")

    # ---- F: transient TRANSPORT failure while WiFi stays connected ------------------------------
    # F1 uncached: the books request fails once; the ERROR screen's Retry must reach the server
    # again on the SAME library visit and show the list.
    wf = root / "F"
    make_sd(wf)
    state_f, log_f = root / "state-transient.json", root / "requests-transient.jsonl"
    set_faults(state_f, {"fail_once": "/plugin/catalog/books"})
    env_f = fixture_env(fx, state_f, log_f)
    acts, _ = script("ENTER", "BACK", "DOWN", "DOWN", "DOWN", "DOWN", "DOWN", "DOWN", "ENTER", "wait1500")
    log_f1 = run.go("F1", wf, env_f, acts, [
        (2600, "F1-transient-error.bmp", "Uncached offline visit: local books and reachable tools"),
        (13500, "F1-retried-list.bmp", "Same visit, after Retry: the list loaded from the server"),
    ])
    no_tofu("F1", log_f1)
    books_calls = [r for r in requests(log_f) if r["path"].endswith("/plugin/catalog/books")]
    need(failures, len(books_calls) >= 2 and books_calls[0].get("injected") == "fail_once" and
         books_calls[-1]["status"] == 200, f"F1: Retry did not re-request and succeed ({[r['status'] for r in books_calls]})")
    need(failures, entered(log_f1, "BookOrbitCatalogBrowser") == 1, "F1: recovery left the library (needs one visit)")
    # F2 cached-offline: the ROOT fails once, so the library opens on its cached root in offline
    # mode; opening a section must reach the server again (without leaving the library) and recover.
    set_faults(state_f, {"fail_once": "/plugin/catalog/books"})
    n_before = len(requests(log_f))
    acts, _ = script("ENTER", "BACK", "DOWN", "DOWN", "DOWN", "DOWN", "DOWN", "DOWN", "ENTER", "BACK", "DOWN", "DOWN", "DOWN", "DOWN", "ENTER", "wait1500")
    log_f2 = run.go("F2", wf, env_f, acts, [
        (2600, "F2-cached-offline-root.bmp", "Cached books after transport failure: offline banner"),
        (13500, "F2-recovered-list.bmp", "Same visit: opening a section went back online"),
        (21300, "F2-root-online-again.bmp", "Back to the root: online again, no offline banner"),
    ])
    no_tofu("F2", log_f2)
    seq = [(r["path"].rsplit("/koreader", 1)[1], r["status"]) for r in requests(log_f)[n_before:]]
    need(failures, bool(seq) and seq[0] == ("/plugin/catalog/books", 0), f"F2: root failure not injected ({seq[:3]})")
    need(failures, ("/plugin/catalog/books", 200) in seq, f"F2: no successful request after recovery ({seq})")
    need(failures, ("/plugin/catalog/root", 200) in seq, f"F2: returning to the root did not go back online ({seq})")
    need(failures, entered(log_f2, "BookOrbitCatalogBrowser") == 1, "F2: recovery left the library (needs one visit)")

    # ---- C: offline: library opens offline, 已下載 lists the book, Open reads it ----------------
    wc = root / "C"
    copy_sd(wb, wc)
    state_c = root / "state-offline.json"
    set_faults(state_c, {"offline": True})
    env_c = fixture_env(fx, state_c, root / "requests-offline.jsonl")
    # Home focus: hero, (no also-reading row), 書庫 tile -> DOWN once.
    acts, _ = script("DOWN", "ENTER", "wait1500", "BACK", "DOWN", "DOWN", "DOWN", "ENTER", "ENTER", "ENTER", "wait3500")
    log_c = run.go("C", wc, env_c, acts, [
        (4000, "C1-library-offline.bmp", "Library offline: banner, 已下載 and cached sections"),
        (13000, "C2-downloaded-list.bmp", "已下載: the book, works offline"),
        (14300, "C3-detail-offline.bmp", "Detail offline: 開啟閱讀 available"),
    ])
    no_tofu("C", log_c)
    need(failures, entered(log_c, "EpubReader") >= 1, "C: the downloaded book did not open offline")

    # ---- D: download fault: failure shown, retry offered, nothing left on SD -------------------
    wd = root / "D"
    make_sd(wd)
    state_d = root / "state-fault.json"
    set_faults(state_d, {"download_status": 500})
    env_d = fixture_env(fx, state_d, root / "requests-fault.jsonl")
    acts, _ = script("ENTER", "RIGHT", "ENTER", "ENTER", "wait2500", "wait1500")
    run.expected_errs = ("unexpected status: 500", "Download failed: Server error")  # the injected fault
    log_d = run.go("D", wd, env_d, acts, [
        (7600, "D1-download-failed.bmp", "Detail after a failed download: 下載失敗 · 重試"),
    ])
    run.expected_errs = ()
    no_tofu("D", log_d)
    bo = wd / "fs_" / "BookOrbit"
    need(failures, not (bo.is_dir() and any(bo.iterdir())), "D: a failed download left a file on SD")
    need(failures, "Download failed" in log_d, "D: the failure was not reported")

    # ---- E: BookOrbit not configured: the library tile sets it up -----------------------------
    we = root / "E"
    make_sd(we, configured=False)
    acts, _ = script("DOWN", "wait300", "ENTER", "wait1500")
    log_e = run.go("E", we, {}, acts, [
        (1300, "E1-home-unconfigured.bmp", "Home without BookOrbit: 設定 BookOrbit tile"),
        (3500, "E2-bookorbit-settings.bmp", "BookOrbit settings opened from the tile"),
    ])
    no_tofu("E", log_e)
    need(failures, "Entering activity: BookOrbitSettings" in log_e, "E: the setup tile did not open BookOrbit settings")

    print()
    for path, desc in run.shots:
        print(f"  {'ok' if path.exists() else 'MISSING':8} {path.relative_to(root)}  {desc}")
    if failures:
        print("\n[bookorbit] FAIL:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print(f"\n[bookorbit] PASS: {len(run.shots)} screenshots at {EXPECTED_WIDTH}x{EXPECTED_HEIGHT}; fixture contract checks only")
    return 0


def fail(message: str) -> int:
    print(f"[bookorbit] FAIL: {message}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
