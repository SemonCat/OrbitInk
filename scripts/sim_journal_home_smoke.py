#!/usr/bin/env python3
"""
Headless scenarios for the Journal (文藝刊物) Home theme on [env:simulator_x3_zh_tw], the shipped
X3 zh-TW profile, with the packaged GuanKiapTsingKhai SD fonts (CROSSFADE_SIM_ZH_TW_FONTS, see
sim_bookorbit_home_smoke.py).

    CROSSFADE_SIM_ZH_TW_FONTS=<pkg>/sd-assets/fonts python3 scripts/sim_journal_home_smoke.py [--no-build]

Recent books are made by really opening fixture EPUBs in the unchanged reader: each seed run
boots with state.json naming one book (the firmware's own resume path), pages forward, and quits.
That writes recent.json, book.bin and progress.bin exactly as on a device, so Home shows real
titles, authors and whole-book progress. Fixture titles/authors live only here.

Scenarios:
  J3  three books (long zh-TW hero title, long Latin title, a book with no author), BookOrbit
      configured with placeholder credentials and no network (fixture only): Home first
      paint, covers filled in after paint, focus hero -> recent 1 -> recent 2 -> BookOrbit 書庫 ->
      搜尋 -> 設定, Confirm on 設定 opens Settings (Journal list chrome)
  J2  two books: one recent column, no divider
  J0  no book, BookOrbit not configured: empty hero prompt, 「沒有最近書籍」, 設定 BookOrbit
  M   theme migration and persistence: a fresh profile lands on JOURNAL (7); the prior product
      default BOOKORBIT (6) moves to JOURNAL once; a deliberate LYRA (1) is untouched; JOURNAL
      survives a second boot; a later deliberate BOOKORBIT is not migrated again.

Checks beyond 528x792 and no [ERR]: no tofu, no EPUB parsed before Home's first paint, one exact-box
thumbnail per shown book and no more, each focus move changes the frame, the 設定 tile opens
Settings, and the persisted uiTheme/marker values. Layout geometry (no overlap, bounds, order) is
asserted by the host test test/journal_layout. Simulator only: no RAM, waveform or device claims.
"""

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sim_bookorbit_fixture_epub as fixture_epub  # noqa: E402
import sim_bookorbit_home_smoke as bo  # noqa: E402
import sim_smoke  # noqa: E402
from sim_smoke import EXPECTED_HEIGHT, EXPECTED_WIDTH, REPO_ROOT, bmp_size, launch  # noqa: E402

PROGRAM = bo.PROGRAM
sim_smoke.PROGRAM = PROGRAM

# Fixture books only: never production data.
BOOKS = [
    ("wanderer.epub", "The Lighthouse Keeper's Almanac of Distant Harbours and Quiet Tides", "Eleanor Whitcombe"),
    ("noauthor.epub", "無名氏的札記", ""),
    ("starlight.epub", fixture_epub.TITLE, fixture_epub.AUTHOR),  # opened last: the hero
]

JOURNAL, BOOKORBIT, LYRA = 7, 6, 1


def write_epub(path: Path, title: str, author: str) -> None:
    fixture_epub.write(path, title=title, author=author)
    if author:
        return
    # Rewrite the OPF without a creator element: a genuinely missing author, not an empty string.
    tmp = path.with_suffix(".tmp")
    with zipfile.ZipFile(path) as src, zipfile.ZipFile(tmp, "w", compression=zipfile.ZIP_DEFLATED) as dst:
        for info in src.infolist():
            data = src.read(info.filename)
            if info.filename == "OEBPS/content.opf":
                data = data.replace(b"<dc:creator></dc:creator>", b"")
            dst.writestr(info, data)
    tmp.replace(path)


def make_sd(work: Path, settings: dict) -> Path:
    cp = work / "fs_" / ".crosspoint"
    cp.mkdir(parents=True)
    (work / "shots").mkdir()
    (work / "fs_" / "books").mkdir()
    shutil.copytree(bo.zh_tw_fonts(), work / "fs_" / "fonts")
    (cp / "crossfade-settings.json").write_text(json.dumps(settings))
    return cp


def settings_of(work: Path) -> dict:
    return json.loads((work / "fs_" / ".crosspoint" / "crossfade-settings.json").read_text())


def run_sim(name: str, work: Path, actions: str, shots, args, failures, expect_errs=()) -> str:
    env = {"CROSSPOINT_SIM_INPUT_SCRIPT": actions}
    if shots:
        env["CROSSPOINT_SIM_SCREENSHOTS"] = ";".join(f"{ms}:./shots/{f}" for ms, f, _ in shots)
    result = launch(work, args.video_driver, args.timeout, env, label=f"journal:{name}")
    log = (result.stdout or "") + (result.stderr or "")
    (work / f"{name}.log").write_text(log)
    if result.returncode != 0:
        failures.append(f"{name}: simulator exited {result.returncode}")
    for line in [ln for ln in log.splitlines() if "[ERR]" in ln and not any(e in ln for e in expect_errs)][:5]:
        failures.append(f"{name}: firmware error: {line.strip()}")
    if "No glyph for codepoint" in log:
        failures.append(f"{name}: a UI string rendered without glyphs (tofu)")
    if "Entering activity: BookOrbitSetupGuide" in log:
        failures.append(f"{name}: the first-run setup guide covered Home")
    for _, f, _ in shots:
        size = bmp_size(work / "shots" / f) if (work / "shots" / f).exists() else None
        if size != (EXPECTED_WIDTH, EXPECTED_HEIGHT):
            failures.append(f"{name}: screenshot {f} missing or wrong size ({size})")
    return log


def seed_books(work: Path, books, args, failures) -> None:
    """Open each book in the real reader (boot resume), page forward, quit."""
    for i, (fname, title, author) in enumerate(books):
        write_epub(work / "fs_" / "books" / fname, title, author)
        (work / "fs_" / ".crosspoint" / "state.json").write_text(json.dumps(
            {"openEpubPath": f"/books/{fname}", "lastSleepFromReader": True, "readerActivityLoadCount": 0}))
        turns = 2 + 3 * i  # distinct progress per book
        acts = ";".join(f"{2500 + k * 900}:DOWN" for k in range(turns))
        log = run_sim(f"seed{i}", work, f"{acts};{2500 + turns * 900 + 1500}:QUIT", [], args, failures)
        if f"Loading ePub: /books/{fname}" not in log:
            failures.append(f"seed{i}: the reader never opened {fname}")
    # Next boot lands on Home, not the reader.
    (work / "fs_" / ".crosspoint" / "state.json").write_text(json.dumps({"openEpubPath": "", "lastSleepFromReader": False}))


def epubs_loaded_before_first_paint(log: str) -> "list[str]":
    out = []
    for line in log.splitlines():
        if "[BOHOME] First paint done" in line:
            break
        if "Loading ePub: " in line:
            out.append(line.split("Loading ePub: ", 1)[1].strip())
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--video-driver", default="dummy")
    ap.add_argument("--timeout", type=int, default=90)
    ap.add_argument("--keep", help="artifact directory (default: a new temp dir, kept)")
    ap.add_argument("--export", help="also write PNG copies of the screenshots here")
    args = ap.parse_args()
    bo.zh_tw_fonts()
    if not args.no_build:
        pio = sim_smoke.find_pio()
        cmd = [pio, "run", "-e", bo.PIOENV] if pio else [sys.executable, "-m", "platformio", "run", "-e", bo.PIOENV]
        if subprocess.run(cmd, cwd=REPO_ROOT).returncode != 0:
            print("[journal] FAIL: build failed")
            return 1
    root = Path(args.keep) if args.keep else Path(tempfile.mkdtemp(prefix="crossfade-sim-journal-"))
    root.mkdir(parents=True, exist_ok=True)
    print(f"[journal] artifacts: {root}")
    failures: "list[str]" = []
    captured: "list[tuple[Path, str]]" = []
    # Home is under test here, so profiles start past the first-run BookOrbit setup guide (稍後再說);
    # the guide itself has its own suite, scripts/sim_bookorbit_setup_smoke.py.
    base = {"trackReadingStats": 1, "bookOrbitSetupGuideDismissed": 1}

    # ---- J3: three books ------------------------------------------------------------------------
    w3 = root / "J3"
    make_sd(w3, base)
    seed_books(w3, BOOKS, args, failures)
    s3 = settings_of(w3)
    if s3.get("uiTheme") != JOURNAL or s3.get("zhTwJournalProfileApplied") != 1:
        failures.append(f"J3: fresh profile did not land on JOURNAL (uiTheme={s3.get('uiTheme')}, "
                        f"marker={s3.get('zhTwJournalProfileApplied')})")
    # J3 Home runs configured, so the utility row reads BookOrbit 書庫. Non-secret placeholders,
    # as in sim_bookorbit_home_smoke.py; no WiFi and no bridge, so nothing reaches any server.
    (w3 / "fs_" / ".crosspoint" / "bookorbit.json").write_text(json.dumps(
        {"username": "fixture", "password": "fixture-placeholder", "serverUrl": bo.BASE_URL}))
    shots3 = [
        (900, "J3-01-home-early.bmp", "Home shortly after first paint"),
        (3200, "J3-02-home.bmp", "Home with covers, hero focused"),
        (4500, "J3-03-focus-recent1.bmp", "focus: first recent column"),
        (5800, "J3-04-focus-recent2.bmp", "focus: second recent column"),
        (7100, "J3-05-focus-library.bmp", "focus: BookOrbit 書庫"),
        (8400, "J3-06-focus-search.bmp", "focus: 搜尋"),
        (9700, "J3-07-focus-settings.bmp", "focus: 設定"),
        (11600, "J3-08-settings.bmp", "Settings in Journal chrome"),
    ]
    acts3 = "3800:DOWN;5100:DOWN;6400:DOWN;7700:DOWN;9000:DOWN;10300:ENTER;13000:QUIT"
    log3 = run_sim("J3", w3, acts3, shots3, args, failures)
    captured += [(w3 / "shots" / f, d) for _, f, d in shots3]
    early = epubs_loaded_before_first_paint(log3)
    if early:
        failures.append(f"J3: EPUB parsed before Home's first paint: {early}")
    if "Entering activity: Settings" not in log3:
        failures.append("J3: Confirm on the 設定 tile did not open Settings")
    thumbs = sorted(p.name for p in (w3 / "fs_" / ".crosspoint").glob("epub_*/thumb_*x*.bmp"))
    hero_thumbs = [t for t in thumbs if not t.startswith(f"thumb_{68}x")]
    recent_thumbs = [t for t in thumbs if t.startswith(f"thumb_{68}x")]
    if len(hero_thumbs) != 1 or len(recent_thumbs) != 2:
        failures.append(f"J3: expected 1 hero + 2 recent exact-box thumbnails, got {thumbs}")
    frames = [(w3 / "shots" / f).read_bytes() for _, f, _ in shots3[1:7] if (w3 / "shots" / f).exists()]
    if len(frames) == 6 and len(set(frames)) != 6:
        failures.append("J3: a focus move did not change the frame")
    # Covers are generated after the first paint (no EPUB before it, checked above) and finish in
    # well under a second in the simulator, so the 900ms frame may already hold them; the generation
    # order itself is the claim: every cover load follows the first paint.
    if "Generated thumb BMP" not in log3:
        failures.append("J3: no cover thumbnail was generated after the first paint")

    # Persistence: a second boot keeps JOURNAL and does not re-run the migration.
    log3b = run_sim("J3b", w3, "2500:QUIT", [(1500, "J3-09-reboot.bmp", "second boot")], args, failures)
    if "Applied X3 zh-TW Journal profile" in log3b or settings_of(w3).get("uiTheme") != JOURNAL:
        failures.append("J3b: JOURNAL did not persist across a reboot, or the migration ran twice")

    # ---- J2: two books --------------------------------------------------------------------------
    w2 = root / "J2"
    make_sd(w2, base)
    seed_books(w2, BOOKS[1:], args, failures)
    shots2 = [(3000, "J2-01-home.bmp", "two books: one recent column")]
    run_sim("J2", w2, "4500:QUIT", shots2, args, failures)
    captured += [(w2 / "shots" / f, d) for _, f, d in shots2]

    # ---- J0: empty, not configured ----------------------------------------------------------------
    w0 = root / "J0"
    make_sd(w0, base)
    shots0 = [(1500, "J0-01-home-empty.bmp", "no books, BookOrbit not configured")]
    run_sim("J0", w0, "3000:QUIT", shots0, args, failures)
    captured += [(w0 / "shots" / f, d) for _, f, d in shots0]

    # ---- M: migration matrix ----------------------------------------------------------------------
    # Each case starts from the settings file a real fresh boot wrote (so language/font are exactly
    # what the profile migration persists), then changes only the theme fields under test.
    fresh = settings_of(w0)
    cases = [
        ("M-bookorbit", {"uiTheme": BOOKORBIT, "zhTwJournalProfileApplied": 0}, JOURNAL),
        ("M-lyra", {"uiTheme": LYRA, "zhTwJournalProfileApplied": 0}, LYRA),
        ("M-chosen-bookorbit", {"uiTheme": BOOKORBIT, "zhTwJournalProfileApplied": 1}, BOOKORBIT),
    ]
    for name, change, want in cases:
        wm = root / name
        make_sd(wm, {**fresh, **change})
        run_sim(name, wm, "2500:QUIT", [], args, failures)
        got = settings_of(wm)
        if got.get("uiTheme") != want or got.get("zhTwJournalProfileApplied") != 1:
            failures.append(f"{name}: uiTheme {got.get('uiTheme')} (want {want}), "
                            f"marker {got.get('zhTwJournalProfileApplied')} (want 1)")
        if got.get("language") != fresh.get("language") or got.get("fontPointSize") != fresh.get("fontPointSize"):
            failures.append(f"{name}: the Journal migration changed unrelated settings")

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
        print("\n[journal] FAIL:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print(f"\n[journal] PASS: {len(captured)} Home/Settings screenshots, migration matrix {len(cases)} cases")
    return 0


if __name__ == "__main__":
    sys.exit(main())
