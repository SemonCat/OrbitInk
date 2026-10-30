#!/usr/bin/env python3
"""
Deterministic headless smoke test for the [env:simulator_x3] PlatformIO environment.

Builds the X3 firmware for the host, runs it against a throwaway SD card containing one tracked
test fixture, drives it through a fixed input script, and checks the screenshots it produced.

    python3 scripts/sim_smoke.py              # build, run, verify
    python3 scripts/sim_smoke.py --no-build   # reuse the existing binary
    python3 scripts/sim_smoke.py --window     # watch it happen in an SDL window

Everything the run touches lives in a fresh temporary directory, never in the working tree: the
simulator reads the SD card from ./fs_/ relative to its working directory, so running it from a
temp dir with a fixture book in fs_/books/ gives every run the same empty settings, empty
credential stores, and empty .crosspoint cache. That isolation is what makes the run repeatable,
and it is also the first of two reasons the smoke cannot reach a real service: with no stored
WiFi or account credentials, the firmware's sync paths gate themselves out before any request.
The second is structural: launch() clears all simulator overrides, including the explicit
CrossFade HTTP bridge opt-in. SecureHttpClient therefore fails every request as a transport error.

Exit code is 0 only if every check passes.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
PIOENV = "simulator_x3"
PROGRAM = REPO_ROOT / ".pio" / "build" / PIOENV / "program"

# One small, tracked, non-personal EPUB. Any fixture under test/epubs/ works; this one is chosen
# because it is a few kilobytes, has more than one spine item (so a page turn has somewhere to go),
# and renders text rather than images.
FIXTURE_EPUB = REPO_ROOT / "test" / "epubs" / "test_br_section_break.epub"

# X3 panel is 792x528 in its native landscape orientation; the firmware's default portrait
# orientation makes the simulator window -- and therefore the screenshot -- 528x792.
EXPECTED_WIDTH = 528
EXPECTED_HEIGHT = 792

# Times are milliseconds from process start, which is how the simulator schedules both scripts.
# The gaps are deliberately loose: they only have to outlast a slow host, and boot to Home takes
# roughly 300 ms here. Keys are the simulator's own vocabulary (see its README): ENTER confirms,
# BACK backs out, DOWN is a side-button page turn, SLEEP requests sleep, QUIT ends the run.
#
# Home -> Browse Books -> books/ -> the fixture -> page turn -> reader menu -> back -> sleep.
INPUT_SCRIPT = (
    "1000:ENTER;"    # Home: open "Browse Books"
    "1800:ENTER;"    # File browser at SD root: descend into books/
    "2600:ENTER;"    # books/: open the fixture EPUB
    "4200:DOWN;"     # Reader: turn one page forward
    "5400:ENTER;"    # Reader: open the reader menu
    "6600:BACK;"     # Reader menu: close it
    "7400:SLEEP;"    # Request sleep
    "9200:ENTER;"    # Wake (the simulator models this as a relaunch + synthetic power wake)
    "13000:QUIT"     # Fallback: only reached if the wake above never happened, so a broken
                     # sleep/wake path fails fast with missing artifacts instead of hanging.
)

# Waking relaunches the process, and the pre-sleep schedules are cleared, so the resumed run needs
# its own script and its own screenshot list.
INPUT_SCRIPT_AFTER_WAKE = "2500:QUIT"

# (schedule-time-ms, filename, human description). The sleep screen itself is deliberately not
# captured: the simulator stops servicing the screenshot scheduler once the firmware asks for deep
# sleep, so a shot scheduled in that window silently never lands. The post-wake shot is the honest
# evidence that sleep and resume worked.
SCREENSHOTS = [
    (800, "01-home.bmp", "Home"),
    (2200, "02-file-browser.bmp", "File browser (books/)"),
    (3800, "03-reader.bmp", "Reader, first page"),
    (5000, "04-page-turn.bmp", "Reader, after one page forward"),
    (6200, "05-reader-menu.bmp", "Reader menu"),
]
SCREENSHOTS_AFTER_WAKE = [
    (1800, "06-after-wake.bmp", "Resumed reader after sleep/wake"),
]

# Screenshot pairs that must not be byte-identical, with the regression each pair catches.
#
# Dimensions and log markers alone cannot see this: if the reader's page-forward button became a
# no-op, both reader screenshots would still be 528x792, the reader menu would still open (page
# position does not gate it), and no [ERR] would appear -- so the run would pass while the exact
# behaviour this schedule slot exists to exercise was broken. Comparing the bytes is what makes the
# page-turn slot mean something.
DISTINCT_SCREENSHOTS = [
    ("03-reader.bmp", "04-page-turn.bmp", "page forward did not change the screen"),
]

# Log lines that must appear. These are the activity-transition traces the firmware emits at
# LOG_LEVEL=2, which [env:simulator_x3] pins.
REQUIRED_LOG_MARKERS = [
    "Entering activity: Home",
    "Entering activity: FileBrowser",
    "Entering activity: EpubReader",
    "Entering activity: EpubReaderMenu",
    "Entering deep sleep",
]


def find_pio() -> str:
    """Locate the PlatformIO CLI without assuming it is on PATH."""
    found = shutil.which("pio") or shutil.which("platformio")
    if found:
        return found
    penv = Path.home() / ".platformio" / "penv" / "bin" / "pio"
    if penv.is_file():
        return str(penv)
    # Last resort: the Python module, which works wherever `pip install platformio` put it.
    return ""


def build() -> None:
    pio = find_pio()
    cmd = [pio, "run", "-e", PIOENV] if pio else [sys.executable, "-m", "platformio", "run", "-e", PIOENV]
    print(f"[smoke] building: {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=REPO_ROOT)
    if result.returncode != 0:
        sys.exit(f"[smoke] FAIL: build of {PIOENV} failed (exit {result.returncode})")


def make_sd_root(work_dir: Path) -> None:
    """Lay out the throwaway SD card the simulator will mount as ./fs_/."""
    books = work_dir / "fs_" / "books"
    books.mkdir(parents=True)
    shutil.copy2(FIXTURE_EPUB, books / FIXTURE_EPUB.name)
    (work_dir / "shots").mkdir()


def screenshot_schedule(entries) -> str:
    """Turn (ms, filename, description) triples into the simulator's own schedule syntax."""
    return ";".join(f"{ms}:./shots/{name}" for ms, name, _ in entries)


def launch(work_dir: Path, video_driver: str, timeout_s: int, sim_env: "dict[str, str]",
           label: str = "smoke") -> "subprocess.CompletedProcess[str]":
    """Run the simulator binary once in work_dir with exactly the CROSSPOINT_SIM_* vars given.

    Shared with scripts/sim_dashboard_smoke.py, which drives its own scenarios through the same
    isolation guarantees: a clean CROSSPOINT_SIM_* environment and an offscreen video driver.
    """
    env = os.environ.copy()
    # Drop any simulator settings the developer happens to have exported -- an inherited
    # CROSSPOINT_SIM_HTTP_MOCK_ROOT, input script, or heap override would silently change what
    # this run tests.
    for key in [k for k in env if k.startswith(("CROSSPOINT_SIM", "CROSSFADE_SIM"))]:
        del env[key]
    # "dummy" renders offscreen through SDL's software path, which is what makes this runnable over
    # SSH or in CI -- and it also fixes the screenshot size, because there is no Retina backing
    # scale to double the renderer's output.
    env["SDL_VIDEODRIVER"] = video_driver
    env.update(sim_env)
    print(f"[{label}] running: {PROGRAM} (cwd={work_dir}, SDL_VIDEODRIVER={video_driver})")
    try:
        return subprocess.run(
            [str(PROGRAM)], cwd=work_dir, env=env, timeout=timeout_s,
            capture_output=True, text=True,
        )
    except subprocess.TimeoutExpired:
        sys.exit(f"[{label}] FAIL: simulator did not exit within {timeout_s}s")


def run_simulator(work_dir: Path, video_driver: str, timeout_s: int) -> "subprocess.CompletedProcess[str]":
    return launch(
        work_dir, video_driver, timeout_s,
        {
            "CROSSPOINT_SIM_INPUT_SCRIPT": INPUT_SCRIPT,
            "CROSSPOINT_SIM_INPUT_SCRIPT_AFTER_WAKE": INPUT_SCRIPT_AFTER_WAKE,
            "CROSSPOINT_SIM_SCREENSHOTS": screenshot_schedule(SCREENSHOTS),
            "CROSSPOINT_SIM_SCREENSHOTS_AFTER_WAKE": screenshot_schedule(SCREENSHOTS_AFTER_WAKE),
        },
    )


def bmp_size(path: Path):
    """Read width/height straight out of the BMP header -- no image library needed."""
    header = path.read_bytes()[:30]
    if len(header) < 30 or header[:2] != b"BM":
        return None
    width, height = struct.unpack_from("<ii", header, 18)
    return width, abs(height)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--no-build", action="store_true", help="reuse the existing binary")
    parser.add_argument("--window", action="store_true",
                        help="render into a real SDL window instead of offscreen (screenshots may "
                             "then be HiDPI-scaled, so the size check is skipped)")
    parser.add_argument("--timeout", type=int, default=120, help="seconds before the run is killed")
    parser.add_argument("--work-dir", type=Path, default=None,
                        help="directory to run in (default: a fresh temporary one)")
    args = parser.parse_args()

    if not FIXTURE_EPUB.is_file():
        return fail(f"fixture missing: {FIXTURE_EPUB}")
    if not args.no_build:
        build()
    if not PROGRAM.is_file():
        return fail(f"no simulator binary at {PROGRAM} (drop --no-build)")

    work_dir = args.work_dir or Path(tempfile.mkdtemp(prefix="crossfade-sim-smoke-"))
    work_dir.mkdir(parents=True, exist_ok=True)
    if any(work_dir.iterdir()):
        return fail(f"work dir is not empty: {work_dir}")
    make_sd_root(work_dir)

    result = run_simulator(work_dir, "dummy" if not args.window else os.environ.get("SDL_VIDEODRIVER", ""),
                           args.timeout)
    log = (result.stdout or "") + (result.stderr or "")
    (work_dir / "run.log").write_text(log)

    failures = []
    if result.returncode != 0:
        failures.append(f"simulator exited {result.returncode}, expected 0")

    for marker in REQUIRED_LOG_MARKERS:
        if marker not in log:
            failures.append(f'log never showed "{marker}"')

    # The firmware logs at LOG_LEVEL=2 here, so a clean run has no [ERR] at all. ("[SIM] open
    # failed" lines are not firmware errors -- they are the storage shim tracing expected cache and
    # font-directory misses on a blank SD card.)
    err_lines = [line for line in log.splitlines() if "[ERR]" in line]
    failures.extend(f"firmware logged an error: {line.strip()}" for line in err_lines[:5])

    print()
    print(f"[smoke] artifacts: {work_dir}")
    for _, name, description in SCREENSHOTS + SCREENSHOTS_AFTER_WAKE:
        path = work_dir / "shots" / name
        if not path.is_file():
            failures.append(f"screenshot never written: {name} ({description})")
            print(f"  MISSING  {name:24s} {description}")
            continue
        size = bmp_size(path)
        if size is None:
            failures.append(f"not a readable BMP: {name}")
            print(f"  BAD BMP  {name:24s} {description}")
            continue
        width, height = size
        ok = args.window or (width == EXPECTED_WIDTH and height == EXPECTED_HEIGHT)
        if not ok:
            failures.append(f"{name} is {width}x{height}, expected {EXPECTED_WIDTH}x{EXPECTED_HEIGHT}")
        print(f"  {'ok      ' if ok else 'WRONG   '} {name:24s} {width}x{height}  {description}")

    for first, second, regression in DISTINCT_SCREENSHOTS:
        left, right = work_dir / "shots" / first, work_dir / "shots" / second
        if not (left.is_file() and right.is_file()):
            continue  # already reported as missing above
        if left.read_bytes() == right.read_bytes():
            failures.append(f"{first} and {second} are byte-identical: {regression}")
            print(f"  IDENTICAL {first} == {second}  ({regression})")
        else:
            print(f"  differ   {first} != {second}")

    print()
    if failures:
        for failure in failures:
            print(f"[smoke] FAIL: {failure}")
        return 1
    print(f"[smoke] PASS: {len(SCREENSHOTS) + len(SCREENSHOTS_AFTER_WAKE)} screenshots at "
          f"{EXPECTED_WIDTH}x{EXPECTED_HEIGHT}, clean exit, no firmware errors")
    return 0


def fail(message: str) -> int:
    print(f"[smoke] FAIL: {message}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
