#!/usr/bin/env python3
"""
Deterministic headless scenario test for the Dashboard home theme, on [env:simulator_x3].

`sim_smoke.py` exercises the *default* home theme plus the EPUB/page-turn/menu/sleep-wake path.
This script exercises the Dashboard specifically: its empty state, its active reading card, and
the four things about it that dimensions and log markers alone cannot see --

  * Home opens NO book to draw itself. The firmware traces every book open as
    "[EBP] Loading ePub: <path>", so the count of those lines before the reader is entered must be
    exactly zero -- the card is drawn from the recent-books entry already in memory plus, at most,
    a ~19-byte stats.bin. This is the check that keeps Home cheap, and it is asserted on every one
    of the runs below, not just one.
  * Left/Right browses the card's book (the two cards must not be byte-identical), and
  * Confirm opens exactly the book the card was showing -- the first book open of the whole run,
    and it must be the browsed path, not the pinned one, and
  * re-entering Home returns to the pinned/default book rather than staying where browsing left
    off, and without repinning anything.

    python3 scripts/sim_dashboard_smoke.py              # build, run, verify
    python3 scripts/sim_dashboard_smoke.py --no-build   # reuse the existing binary
    python3 scripts/sim_dashboard_smoke.py --window     # watch it happen in an SDL window

How the fixture state is made, and why there is no test-only backdoor for it
---------------------------------------------------------------------------
Everything is either a real scripted UI action or a file written through a format the firmware
already reads and writes in production:

  * Recent books and the per-book cache come from actually opening the fixture EPUBs through the
    Dashboard's own "Browse Books" row -- so the menu routes are exercised too, and the cache
    directory names (a std::hash of the book path) never have to be guessed. Those seed runs do of
    course open books, but only from inside the reader; the zero-opens-on-Home check is scoped to
    what happens before EpubReader is entered, which is exactly the claim being made.
  * crossfade-settings.json and pinned-book.json are written in the exact JSON these stores
    serialize (CrossPointSettings::toJson, PinnedBookStore::toJson). A settings file carrying only
    the keys under test is valid: every other key falls back to its struct default on load.
  * stats.bin is written in the documented 19-byte v1 layout from
    src/activities/reader/BookReadingStats.cpp. Reading stats otherwise need a 60-second
    qualifying session (ReadingStatsSession::MIN_SESSION_SECONDS), which is not something to spend
    a minute of wall clock on per run -- and the target cache directory is discovered from the
    seed run's own output rather than computed, so nothing here depends on the host's std::hash.

No firmware code path exists only for this script.

Network isolation is inherited unchanged from sim_smoke.py's `launch()`: every CROSSPOINT_SIM_*
variable in the developer's environment is dropped, and sim/CrossFadeSimHal/SecureHttpClient.h
fails every request as a transport error, so no run here can reach BookOrbit, a KOReader server,
an OPDS catalog or the release endpoint.

Screenshot *contents* are deliberately not compared against reference images: the card renders a
real battery percentage and (when enabled) a real clock, so a golden would be unstable. The
pixel-level checks here are the ones that stay true under a changing clock -- correct dimensions,
and specific pairs of screenshots that must differ from each other.

Exit code is 0 only if every check passes.
"""

import argparse
import json
import os
import shutil
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sim_smoke import (  # noqa: E402  (path shim above must run first)
    EXPECTED_HEIGHT,
    EXPECTED_WIDTH,
    PROGRAM,
    REPO_ROOT,
    bmp_size,
    build,
    launch,
)

# Two small, tracked, non-personal EPUBs. PINNED_BOOK deliberately has an author and the other
# does not, so the active card is checked both with and without an author line. Sorted order in
# the file browser is what the seed scripts below index into, so the names matter.
PINNED_BOOK = REPO_ROOT / "test" / "epubs" / "test_supsub.epub"  # 2nd alphabetically
OTHER_BOOK = REPO_ROOT / "test" / "epubs" / "test_br_section_break.epub"  # 1st alphabetically
PINNED_PATH = "/books/test_supsub.epub"
OTHER_PATH = "/books/test_br_section_break.epub"
PINNED_TITLE = "Subscript and Superscript Tests"

# uiTheme 5 is CrossPointSettings::UI_THEME::DASHBOARD. pinBookToHome and trackReadingStats are
# both off by default, and both gate behaviour this scenario is here to check.
DASHBOARD_SETTINGS = {"uiTheme": 5, "pinBookToHome": 1, "trackReadingStats": 1}

# A plausible reading history for the pinned book: ~1h27m read, ~2h35m left, 143 pages at ~36s
# each. Chosen so every stat formats to a distinct multi-character string ("1h 27m", "2h 35m",
# "1.6") rather than a value that could coincidentally match a default.
FIXTURE_STATS = {
    "sessionCount": 4,
    "totalReadingSeconds": 5220,
    "totalPagesTurned": 143,
    "avgSecondsPerForwardPage": 36,
    "paceSampleCount": 143,
    "estimatedTimeLeftSeconds": 9300,
}


def write_settings(work_dir: Path, values: "dict[str, int]") -> None:
    path = work_dir / "fs_" / ".crosspoint" / "crossfade-settings.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(values))


def write_pinned_book(work_dir: Path, path_value: str, title: str) -> None:
    """PinnedBookStore::toJson -- a path and a display title, nothing else."""
    target = work_dir / "fs_" / ".crosspoint" / "pinned-book.json"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps({"path": path_value, "title": title}))


def write_stats_bin(cache_dir: Path, values: "dict[str, int]") -> None:
    """BookReadingStats binary layout v1 -- see src/activities/reader/BookReadingStats.cpp."""
    data = bytearray(19)
    data[0] = 1  # STATS_FILE_VERSION
    struct.pack_into("<H", data, 1, values["sessionCount"])
    struct.pack_into("<I", data, 3, values["totalReadingSeconds"])
    struct.pack_into("<I", data, 7, values["totalPagesTurned"])
    struct.pack_into("<H", data, 11, values["avgSecondsPerForwardPage"])
    struct.pack_into("<H", data, 13, values["paceSampleCount"])
    struct.pack_into("<I", data, 15, values["estimatedTimeLeftSeconds"])
    (cache_dir / "stats.bin").write_bytes(bytes(data))


def cache_dirs(work_dir: Path) -> "set[Path]":
    root = work_dir / "fs_" / ".crosspoint"
    return {p for p in root.glob("epub_*") if p.is_dir()} if root.is_dir() else set()


def recent_paths(work_dir: Path) -> "list[str]":
    recent = work_dir / "fs_" / ".crosspoint" / "recent.json"
    if not recent.is_file():
        return []
    return [b.get("path", "") for b in json.loads(recent.read_text()).get("books", [])]


def make_sd_root(work_dir: Path, with_books: bool) -> None:
    books = work_dir / "fs_" / "books"
    books.mkdir(parents=True)
    if with_books:
        shutil.copy2(PINNED_BOOK, books / PINNED_BOOK.name)
        shutil.copy2(OTHER_BOOK, books / OTHER_BOOK.name)
    (work_dir / "shots").mkdir()


def run(work_dir: Path, video_driver: str, timeout_s: int, name: str, script: str,
        shots: "list[tuple[int, str, str]]", failures: "list[str]") -> str:
    """One simulator run; returns its combined log.

    Records a failure on a bad exit, on any firmware [ERR], and -- for every run, unconditionally --
    on any book open that happened before the reader was entered. That last one is the Dashboard's
    load-bearing invariant, so it is checked here rather than at one call site.
    """
    sim_env = {"CROSSPOINT_SIM_INPUT_SCRIPT": script}
    if shots:
        sim_env["CROSSPOINT_SIM_SCREENSHOTS"] = ";".join(f"{ms}:./shots/{f}" for ms, f, _ in shots)
    result = launch(work_dir, video_driver, timeout_s, sim_env, label=f"dashboard:{name}")
    log = (result.stdout or "") + (result.stderr or "")
    (work_dir / f"{name}.log").write_text(log)
    if result.returncode != 0:
        failures.append(f"{name}: simulator exited {result.returncode}, expected 0")
    # A clean run at LOG_LEVEL=2 has no [ERR] at all. ("[SIM] open failed" lines are the storage
    # shim tracing expected cache misses, not firmware errors.)
    for line in [ln for ln in log.splitlines() if "[ERR]" in ln][:5]:
        failures.append(f"{name}: firmware logged an error: {line.strip()}")
    opened_on_home = books_opened_before_reader(log)
    if opened_on_home:
        failures.append(f"{name}: Home opened {len(opened_on_home)} book(s) before the reader was "
                        f"entered ({opened_on_home[:3]}) -- the Dashboard card must draw without "
                        f"opening any book")
    return log


LOAD_MARKER = "Loading ePub: "
READER_MARKER = "Entering activity: Reader"


def opened_books(log: str) -> "list[str]":
    """Book paths the EPUB engine opened, in order, from its own load trace."""
    return [ln.split(LOAD_MARKER, 1)[1].strip() for ln in log.splitlines() if LOAD_MARKER in ln]


def books_opened_before_reader(log: str) -> "list[str]":
    """Books opened before EpubReader was entered -- i.e. on Home's own account. Must be empty.

    The Dashboard card is drawn from the RecentBooksStore entry already in memory plus, at most, a
    ~19-byte stats.bin read off a cache path derived by string arithmetic. Nothing on that path
    calls Epub::load() or BookMetadataCache::load(), so no "[EBP] Loading ePub" line may appear
    before the reader opens. A regression that reintroduced a book-level progress read -- the
    obvious way to put a percentage back on the card -- would show up here immediately.
    """
    opened = []
    for line in log.splitlines():
        if READER_MARKER in line:
            break
        if LOAD_MARKER in line:
            opened.append(line.split(LOAD_MARKER, 1)[1].strip())
    return opened


def check_shot(work_dir: Path, name: str, description: str, allow_any_size: bool,
               failures: "list[str]") -> bool:
    path = work_dir / "shots" / name
    if not path.is_file():
        failures.append(f"screenshot never written: {name} ({description})")
        print(f"  MISSING  {name:26s} {description}")
        return False
    size = bmp_size(path)
    if size is None:
        failures.append(f"not a readable BMP: {name}")
        print(f"  BAD BMP  {name:26s} {description}")
        return False
    width, height = size
    ok = allow_any_size or (width == EXPECTED_WIDTH and height == EXPECTED_HEIGHT)
    if not ok:
        failures.append(f"{name} is {width}x{height}, expected {EXPECTED_WIDTH}x{EXPECTED_HEIGHT}")
    print(f"  {'ok      ' if ok else 'WRONG   '} {name:26s} {width}x{height}  {description}")
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--no-build", action="store_true", help="reuse the existing binary")
    parser.add_argument("--window", action="store_true",
                        help="render into a real SDL window instead of offscreen (screenshots may "
                             "then be HiDPI-scaled, so the size check is skipped)")
    parser.add_argument("--timeout", type=int, default=120, help="seconds before a run is killed")
    parser.add_argument("--work-dir", type=Path, default=None,
                        help="directory to run in (default: a fresh temporary one)")
    args = parser.parse_args()

    for fixture in (PINNED_BOOK, OTHER_BOOK):
        if not fixture.is_file():
            print(f"[dashboard] FAIL: fixture missing: {fixture}")
            return 1
    if not args.no_build:
        build()
    if not PROGRAM.is_file():
        print(f"[dashboard] FAIL: no simulator binary at {PROGRAM} (drop --no-build)")
        return 1

    driver = "dummy" if not args.window else os.environ.get("SDL_VIDEODRIVER", "")
    root = args.work_dir or Path(tempfile.mkdtemp(prefix="crossfade-sim-dashboard-"))
    root.mkdir(parents=True, exist_ok=True)
    if any(root.iterdir()):
        print(f"[dashboard] FAIL: work dir is not empty: {root}")
        return 1
    failures: "list[str]" = []

    # --- Scenario 1: empty state ---------------------------------------------------------------
    # A card with no book to continue. Its own work dir, with no EPUBs at all, so the empty state
    # is reached honestly rather than by hiding books the firmware could have found.
    empty_dir = root / "empty"
    empty_dir.mkdir()
    make_sd_root(empty_dir, with_books=False)
    write_settings(empty_dir, DASHBOARD_SETTINGS)
    empty_shots = [(1400, "01-dashboard-empty.bmp", "Dashboard, empty state")]
    empty_log = run(empty_dir, driver, args.timeout, "empty", "2600:QUIT", empty_shots, failures)
    if "Using Dashboard theme" not in empty_log:
        failures.append("empty: the Dashboard theme was never selected (uiTheme=5 did not stick)")
    if "Entering activity: Home" not in empty_log:
        failures.append("empty: never reached Home")

    # --- Scenario 2: seed two books through the Dashboard's own menu ----------------------------
    # Home -> Browse Books -> books/ -> a fixture -> one page forward -> Back to Home. Run once per
    # book, so the cache directory each one creates is identifiable by difference.
    active_dir = root / "active"
    active_dir.mkdir()
    make_sd_root(active_dir, with_books=True)
    write_settings(active_dir, DASHBOARD_SETTINGS)

    # First seed: the card is empty, so the Home selection starts on "Browse Books" itself.
    seed_one = run(active_dir, driver, args.timeout, "seed-pinned",
                   "1000:ENTER;"   # Home: Browse Books (row 0 -- there is no card to select yet)
                   "1800:ENTER;"   # File browser at SD root: descend into books/
                   "2600:DOWN;"    # books/: move to the second entry (test_supsub.epub)
                   "3200:ENTER;"   # open it
                   "5000:DOWN;"    # Reader: one page forward, so progress.bin gets written
                   "6200:BACK;"    # leave the reader, back to Home
                   "8000:QUIT", [], failures)
    if "Entering activity: EpubReader" not in seed_one:
        failures.append("seed-pinned: the reader never opened the first fixture")
    pinned_caches = cache_dirs(active_dir)
    if len(pinned_caches) != 1:
        failures.append(f"seed-pinned: expected exactly 1 book cache dir, found {len(pinned_caches)}")
    pinned_cache = next(iter(pinned_caches), None)

    # Second seed: the card now holds a book, so the Home selection starts on it and one Down is
    # needed to reach Browse Books -- which is itself a check that the card occupies exactly one
    # selector slot above the menu.
    seed_two = run(active_dir, driver, args.timeout, "seed-other",
                   "1000:DOWN;"    # Home: card -> Browse Books
                   "1600:ENTER;"
                   "2400:ENTER;"   # books/
                   "3200:ENTER;"   # first entry (test_br_section_break.epub)
                   "5000:DOWN;"
                   "6200:BACK;"
                   "8000:QUIT", [], failures)
    if "Entering activity: EpubReader" not in seed_two:
        failures.append("seed-other: the reader never opened the second fixture")
    if sorted(recent_paths(active_dir)) != sorted([PINNED_PATH, OTHER_PATH]):
        failures.append(f"seed: recent.json does not hold both fixtures: {recent_paths(active_dir)}")

    # Reading stats for the pinned book only, so the "no stats -> no stat row" path is covered by
    # the other book in the very same run.
    if pinned_cache is not None:
        write_stats_bin(pinned_cache, FIXTURE_STATS)
    write_pinned_book(active_dir, PINNED_PATH, PINNED_TITLE)

    # --- Scenario 3: default selection, browsing, and Confirm ----------------------------------
    browse_shots = [
        (900, "02-dashboard-pinned.bmp", "Dashboard, pinned default book"),
        (2400, "03-dashboard-browsed.bmp", "Dashboard, after one Right"),
        (5000, "04-dashboard-opened.bmp", "Reader, opened from the card"),
    ]
    browse_log = run(active_dir, driver, args.timeout, "browse",
                     "1200:RIGHT;"  # browse to the next book in the ephemeral list
                     "3000:ENTER;"  # open exactly what the card is showing
                     "6000:QUIT", browse_shots, failures)

    # run() has already asserted that nothing was opened before the reader. So the FIRST book open
    # of the whole run is Confirm's, and it must be the book the card was showing after the Right
    # press -- not the pinned default the card started on. That single assertion is both halves of
    # "Confirm opens exactly the shown book".
    opened = opened_books(browse_log)
    if "Entering activity: EpubReader" not in browse_log:
        failures.append("browse: Confirm did not open a book")
    elif not opened:
        failures.append("browse: the reader opened without any book-open trace")
    elif opened[0] != OTHER_PATH:
        failures.append(f"browse: Confirm opened {opened[0]}, expected the browsed book {OTHER_PATH}"
                        + (" -- it opened the pinned default instead, so Right did not move the card"
                           if opened[0] == PINNED_PATH else ""))

    # --- Scenario 4: re-entering Home returns to the pinned default -----------------------------
    # Same work dir, immediately after the browse run left the card on the OTHER book. A fresh
    # launch must land back on the pinned book, and pinned-book.json must be untouched: browsing
    # is ephemeral and never repins.
    pinned_json_before = (active_dir / "fs_" / ".crosspoint" / "pinned-book.json").read_text()
    reentry_shots = [(1400, "05-dashboard-reentry.bmp", "Dashboard, re-entered Home")]
    reentry_log = run(active_dir, driver, args.timeout, "reentry", "2600:QUIT", reentry_shots, failures)
    # Nothing to assert from the log here beyond run()'s zero-opens check -- Home names the book
    # without opening it, so the proof that it returned to the pinned default is the pixel
    # comparison against 02-dashboard-pinned.bmp at the bottom of this script.
    if "Entering activity: Home" not in reentry_log:
        failures.append("reentry: never reached Home")
    pinned_json_after = (active_dir / "fs_" / ".crosspoint" / "pinned-book.json").read_text()
    if pinned_json_before != pinned_json_after:
        failures.append("reentry: pinned-book.json changed -- browsing must never repin")

    # --- Artifact checks -----------------------------------------------------------------------
    print()
    print(f"[dashboard] artifacts: {root}")
    for work_dir, shots in ((empty_dir, empty_shots), (active_dir, browse_shots),
                            (active_dir, reentry_shots)):
        for _, name, description in shots:
            check_shot(work_dir, name, description, args.window, failures)

    # Pairs that must differ. Dimensions and log markers cannot see any of these: if Left/Right
    # became a no-op the card would still be 528x792 and the log would still show a book load, and
    # if the card ignored the browse position entirely the re-entry shot would still render.
    distinct = [
        ("02-dashboard-pinned.bmp", "03-dashboard-browsed.bmp",
         "Right did not change the book on the card"),
        ("01-dashboard-empty.bmp", "02-dashboard-pinned.bmp",
         "the empty state and the active card render identically"),
    ]
    shot_dirs = {"01-dashboard-empty.bmp": empty_dir}
    for first, second, regression in distinct:
        left = shot_dirs.get(first, active_dir) / "shots" / first
        right = shot_dirs.get(second, active_dir) / "shots" / second
        if not (left.is_file() and right.is_file()):
            continue  # already reported as missing above
        if left.read_bytes() == right.read_bytes():
            failures.append(f"{first} and {second} are byte-identical: {regression}")
            print(f"  IDENTICAL {first} == {second}  ({regression})")
        else:
            print(f"  differ   {first} != {second}")

    # The re-entry card must match the pinned card that the browse run started from: same book,
    # same layout. The battery reads 100% throughout a simulator run and the clock is disabled by
    # default, so this pair is stable -- unlike a golden image, it compares two shots from the
    # same run rather than against a checked-in reference.
    pinned_shot = active_dir / "shots" / "02-dashboard-pinned.bmp"
    reentry_shot = active_dir / "shots" / "05-dashboard-reentry.bmp"
    if pinned_shot.is_file() and reentry_shot.is_file():
        if pinned_shot.read_bytes() == reentry_shot.read_bytes():
            print("  match    02-dashboard-pinned.bmp == 05-dashboard-reentry.bmp")
        else:
            failures.append("05-dashboard-reentry.bmp differs from 02-dashboard-pinned.bmp: "
                            "re-entering Home did not return to the pinned default card")
            print("  DIFFER   02-dashboard-pinned.bmp != 05-dashboard-reentry.bmp")

    print()
    if failures:
        for failure in failures:
            print(f"[dashboard] FAIL: {failure}")
        return 1
    print("[dashboard] PASS: empty state, pinned default, Left/Right browse, Confirm-opens-shown-book, "
          "and ephemeral re-entry all verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
