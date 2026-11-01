#!/usr/bin/env python3
"""Real X3 manual/accelerated-idle sleep, SDL captures and button-wake regression.

Uses isolated SD roots, packaged zh-TW fonts and the offline simulator transport.
One wallpaper and one overlay make image selection deterministic. No device writes.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import shutil

from PIL import Image, ImageChops

import sim_smoke
from sim_bookorbit_home_smoke import PROGRAM, zh_tw_fonts
from sim_bookorbit_fixture_epub import write as write_epub
from sim_ports_1_6_smoke import png_overlay, ink_mask

MODES = {"custom": 2, "cover": 3, "cover-custom": 4, "quick": 6, "transparent": 7}


def need(value, message):
    if not value:
        raise AssertionError(message)


def run(root, mode, reader, idle, legacy, baseline=False):
    root.mkdir(parents=True)
    sd = root / "fs_"
    cp = sd / ".crosspoint"
    cp.mkdir(parents=True)
    shots = root / "shots"
    shots.mkdir()
    shutil.copytree(zh_tw_fonts(), sd / "fonts")
    write_epub(sd / "book.epub")
    # A high-contrast wallpaper unlike the fixture book's actual embedded cover.
    wallpaper = Image.new("RGB", (528, 792), "white")
    for y in range(792):
        for x in range(528):
            if (x // 88 + y // 132) % 2:
                wallpaper.putpixel((x, y), (0, 0, 0))
    wallpaper.save(sd / "sleep.bmp")
    (sd / "sleep-overlay.png").write_bytes(png_overlay())
    settings = {"sleepScreen": MODES[mode], "quickResumeSleepScreen": legacy,
                "bookOrbitSetupGuideDismissed": 1, "periodicSyncInterval": 0,
                "sleepTimeoutMinutes": 10}
    (cp / "crossfade-settings.json").write_text(json.dumps(settings))
    (cp / "state.json").write_text(json.dumps(
        {"openEpubPath": "/book.epub", "lastSleepFromReader": reader, "showBootScreen": True}))
    env = {
        "CROSSPOINT_SIM_INPUT_SCRIPT": ("10000:ENTER;15000:QUIT" if idle else
                                      "6000:SLEEP;10000:ENTER;15000:QUIT"),
        "CROSSPOINT_SIM_SCREENSHOTS": "3500:./shots/before.bmp",
        "CROSSPOINT_SIM_INPUT_SCRIPT_AFTER_WAKE": "3000:DOWN;4500:QUIT",
        "CROSSPOINT_SIM_SCREENSHOTS_AFTER_WAKE":
            "2200:./shots/wake.bmp;3900:./shots/wake-next.bmp",
        "CROSSFADE_SIM_SLEEP_CAPTURE": str(shots / "sleep.bmp"),
    }
    if idle:
        env["CROSSFADE_SIM_IDLE_TIMEOUT_MS"] = "5000"
    sim_smoke.PROGRAM = PROGRAM
    result = sim_smoke.launch(root, "dummy", 35, env, label=root.name)
    log = result.stdout + result.stderr
    (root / "run.log").write_text(log)
    need(result.returncode == 0, f"{root}: simulator exit {result.returncode}")
    need("Entering deep sleep" in log, f"{root}: no actual sleep")
    need(("Auto-sleep triggered" in log) == idle, f"{root}: wrong trigger")
    need("Exiting activity: EpubReader" in log if reader else "Exiting activity: Home" in log,
         f"{root}: sleep context changed")
    for name in ("before", "sleep", "wake", "wake-next"):
        need(sim_smoke.bmp_size(shots / f"{name}.bmp") == (528, 792), f"{root}: missing {name}")
    wake_log = log.split("Saved sleep capture:")[-1]
    need("Entering activity: EpubReader" in wake_log if reader else "Entering activity: Home" in wake_log,
         f"{root}: missing wake activity")
    if not baseline:
        screen = (3 if reader else 2) if mode == "cover-custom" else MODES[mode]
        preserve = mode == "transparent" or (mode == "quick" and reader)
        resume = preserve and reader
        marker = (f"Sleep entry trigger={'idle' if idle else 'manual'} reader={int(reader)} "
                  f"selected={MODES[mode]} screen={screen} preserve={int(preserve)} resume={int(resume)}")
        need(marker in log, f"{root}: incorrect shared renderer/wake decision")
        need(("Entering activity: Boot" not in wake_log) == resume, f"{root}: incorrect wake presentation")
        if preserve:
            need("Rendering transparent PNG overlay:" in log, f"{root}: selected overlay not rendered")
        elif screen == 2:
            need("Loading: /sleep.bmp" in log, f"{root}: selected wallpaper not rendered")
        elif screen == 3:
            need("Rendering sleep cover:" in log, f"{root}: actual book cover not rendered")
    state = json.loads((cp / "state.json").read_text())
    need(state["lastSleepFromReader"] == reader, f"{root}: lost origin")
    need(state["openEpubPath"] == "/book.epub", f"{root}: lost book")
    need(not (cp / "sleep_frame.bin").exists(), f"{root}: frame not consumed/cleaned")
    # Persistence retains the retired preference without letting it control rendering.
    saved = json.loads((cp / "crossfade-settings.json").read_text())
    need(saved["quickResumeSleepScreen"] == legacy, f"{root}: legacy setting lost")
    need(saved["sleepScreen"] == MODES[mode], f"{root}: selected mode changed")
    if reader:
        need(ink_mask((shots / "before.bmp").read_bytes()) ==
             ink_mask((shots / "wake.bmp").read_bytes()), f"{root}: wake restored a different reader page")
        need(ink_mask((shots / "wake.bmp").read_bytes()) !=
             ink_mask((shots / "wake-next.bmp").read_bytes()), f"{root}: reader failed to turn after wake")
    need(not [line for line in log.splitlines() if "[ERR]" in line], f"{root}: firmware errors")
    return root


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--keep", required=True, type=Path)
    parser.add_argument("--baseline", action="store_true", help="reproduce the old CUSTOM trigger mismatch")
    args = parser.parse_args()
    cases = [("custom", True, 1)] if args.baseline else [
        (mode, reader, 1) for mode in MODES for reader in (True, False)] + [
        ("custom", True, 0), ("cover-custom", False, 0)]
    jobs = [(args.keep / f"{mode}-{'reader' if reader else 'home'}-legacy{legacy}-{'idle' if idle else 'manual'}",
             mode, reader, idle, legacy, args.baseline)
            for mode, reader, legacy in cases for idle in (False, True)]
    # Independent isolated firmware processes; no shared SD files or socket transport.
    with ThreadPoolExecutor(max_workers=2) as pool:
        list(pool.map(lambda values: run(*values), jobs))
    comparisons = []
    for mode, reader, legacy in cases:
        prefix = f"{mode}-{'reader' if reader else 'home'}-legacy{legacy}"
        a, b = [Image.open(args.keep / f"{prefix}-{trigger}" / "shots/sleep.bmp").convert("RGB")
                for trigger in ("manual", "idle")]
        equal = ImageChops.difference(a, b).getbbox() is None
        comparisons.append({"case": prefix, "same_sleep_pixels": equal})
        need(equal != args.baseline, f"{prefix}: unexpected manual/idle screen equality={equal}")
    (args.keep / "result.json").write_text(json.dumps(comparisons, indent=2))
    print(f"PASS: {len(jobs)} actual sleep/wake runs; {comparisons}")


if __name__ == "__main__":
    main()
