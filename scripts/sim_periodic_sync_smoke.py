#!/usr/bin/env python3
"""
Accelerated overnight periodic-sync acceptance on [env:simulator_x3], against the FIXTURE bridge.

    python3 scripts/sim_periodic_sync_smoke.py               # build, run, verify
    python3 scripts/sim_periodic_sync_smoke.py --no-build    # reuse the existing binary
    python3 scripts/sim_periodic_sync_smoke.py --only S1,S7  # a subset

Each scenario boots the production firmware once, resumes into the reader (no network), turns
pages, and presses sleep. From then on nothing is scripted: the owned simulator adapter
(sim/CrossFadeSimHal/HalPowerManagerCompat.cpp) waits delay/CROSSFADE_SIM_TIMER_DIVISOR and
relaunches the program as a timer wake -- a fresh process on the same SD directory, which is what a
deep-sleep reset is to firmware state -- so the production setup() -> synchronizeProgressHeadlessly()
-> startDeepSleep() route runs repeatedly, unmodified. CROSSFADE_SIM_TIMER_MAX_WAKES ends the run.

Evidence per scenario: the firmware's own /.crosspoint/sync-receipts.log, the adapter's timer trace
(every armed uint64 delay), and the fixture bridge's request log. The bridge never opens a socket.

What this does NOT model: WiFi association time, TLS, real heap or fragmentation (heap figures are
static numbers chosen per wake via CROSSFADE_SIM_TIMER_HEAP_SCHEDULE), the RTC timer hardware, X3
power rails, or battery. Those remain device-verification items.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sim_smoke  # noqa: E402
from sim_smoke import REPO_ROOT  # noqa: E402

BRIDGE = REPO_ROOT / "scripts" / "sim_bookorbit_fixture_bridge.py"
BOOK = REPO_ROOT / "test" / "epubs" / "test_br_section_break.epub"
BASE_URL = "https://bookorbit.fixture.invalid"
HOUR_US = 3_600_000_000
# Interval enum (src/util/ProgressSyncSchedule.h): 0 Off, 2 one hour, 6 twenty-four hours.
OFF, HOUR1, HOURS24 = 0, 2, 6
# Resume straight into the reader, turn pages for ~4 s, then sleep. The reader must stay offline.
READING = "1500:DOWN;2500:DOWN;3500:DOWN;4500:DOWN;6000:SLEEP"
LAST_READING_INPUT_S = 4.5
SLEEP_S = 6.0


class Scenario:
    def __init__(self, root: Path, name: str, args):
        self.name, self.args = name, args
        self.work = root / name
        self.sd = self.work / "fs_" / ".crosspoint"
        self.fx = self.work / "fixture"
        self.state = self.work / "fixture-state.json"
        self.requests_log = self.work / "requests.jsonl"
        self.trace = self.work / "timer-trace.txt"
        self.receipts_path = self.sd / "sync-receipts.log"
        self.failures: "list[str]" = []
        self.sd.mkdir(parents=True)
        self.fx.mkdir()
        (self.fx / "catalog.json").write_text(json.dumps({"root": [], "books": []}))
        shutil.copy2(BOOK, self.work / "fs_" / "book.epub")
        self.write("state.json", {"openEpubPath": "/book.epub", "lastSleepFromReader": True})
        self.write("crossfade-settings.json", {"periodicSyncInterval": HOUR1, "bookOrbitSetupGuideDismissed": 1})
        # Non-secret placeholders; legacy plaintext form is accepted and re-obfuscated on load.
        self.write("bookorbit.json", {"username": "fixture", "password": "fixture-placeholder", "serverUrl": BASE_URL})
        self.write("wifi.json", {"lastConnectedSsid": "Simulator WiFi (fake)",
                                 "credentials": [{"ssid": "Simulator WiFi (fake)", "password": ""}]})

    def write(self, name, data):
        (self.sd / name).write_text(json.dumps(data))

    def settings(self, **values):
        data = json.loads((self.sd / "crossfade-settings.json").read_text())
        data.update(values)
        self.write("crossfade-settings.json", data)

    def faults(self, faults):
        self.state.write_text(json.dumps({"faults": faults}))

    def env(self, divisor, max_wakes, heap_schedule=None):
        env = {k: v for k, v in os.environ.items() if not k.startswith(("CROSSPOINT_SIM", "CROSSFADE_SIM"))}
        env.update({
            "SDL_VIDEODRIVER": self.args.video_driver,
            "CROSSFADE_SIM_HTTP_BRIDGE": str(BRIDGE),
            "CROSSFADE_SIM_FIXTURE_DIR": str(self.fx),
            "CROSSFADE_SIM_FIXTURE_STATE": str(self.state),
            "CROSSFADE_SIM_FIXTURE_LOG": str(self.requests_log),
            "CROSSPOINT_SIM_INPUT_SCRIPT": READING,
            "CROSSFADE_SIM_TIMER_DIVISOR": str(divisor),
            "CROSSFADE_SIM_TIMER_MAX_WAKES": str(max_wakes),
            "CROSSFADE_SIM_TIMER_TRACE": str(self.trace),
        })
        if heap_schedule is not None:
            env["CROSSFADE_SIM_TIMER_HEAP_SCHEDULE"] = heap_schedule
        return env

    def start(self, divisor=2400, max_wakes=3, heap_schedule=None):
        print(f"[periodic:{self.name}] running {self.args.program}")
        self.t0 = time.time()
        self.log_file = (self.work / "simulator.log").open("w")
        self.proc = subprocess.Popen([str(self.args.program)], cwd=self.work, stdout=self.log_file,
                                     stderr=subprocess.STDOUT, env=self.env(divisor, max_wakes, heap_schedule))

    def wait(self):
        try:
            code = self.proc.wait(timeout=self.args.timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            code = "timeout"
        self.log_file.close()
        self.need(code == 0, f"simulator chain exited {code}")
        return self

    def run(self, **kwargs):
        self.start(**kwargs)
        return self.wait()

    # ---- evidence -------------------------------------------------------------------------------
    def receipts(self):
        out = []
        if self.receipts_path.exists():
            for line in self.receipts_path.read_text().splitlines():
                out.append(dict(field.split("=", 1) for field in line.split()))
        return out

    def traces(self):
        out = []
        if self.trace.exists():
            for line in self.trace.read_text().splitlines():
                out.append(dict(field.split("=", 1) for field in line.split()))
        return out

    def requests(self):
        if not self.requests_log.exists():
            return []
        return [json.loads(line) for line in self.requests_log.read_text().splitlines()]

    def wait_for(self, predicate, what, timeout=30):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if predicate():
                return True
            if self.proc.poll() is not None:
                break
            time.sleep(0.05)
        self.need(False, f"timed out waiting for {what}")
        return False

    def need(self, cond, msg):
        if not cond:
            self.failures.append(f"{self.name}: {msg}")

    def triggers(self):
        return [r.get("t") for r in self.receipts()]

    def common(self):
        """Reading is offline and leaves no receipt: one boot line, then nothing until sleep."""
        receipts = self.receipts()
        self.need(len(receipts) >= 2 and receipts[0].get("t") == "boot" and receipts[1].get("t") == "sleep",
                  f"expected boot then sleep receipts first, got {self.triggers()}")
        early = [r for r in self.requests() if r["t"] < self.t0 + LAST_READING_INPUT_S + 0.5]
        self.need(not early, f"network request(s) while reading: {[(r['method'], r['path']) for r in early]}")
        for line in (self.receipts_path.read_text() if self.receipts_path.exists() else "").splitlines():
            for secret in ("book.epub", "Simulator WiFi", "fixture-placeholder", "bookorbit.fixture", "d4f99a48"):
                self.need(secret not in line, f"receipt leaks {secret!r}: {line}")


def scenario_cadence(s: Scenario):
    """S1: hourly cadence over repeated timer wakes; unchanged progress costs one GET, no PUT."""
    s.run(max_wakes=3).common()
    r, tr = s.receipts(), s.traces()
    s.need(s.triggers() == ["boot", "sleep", "timer", "timer", "timer"], f"triggers {s.triggers()}")
    s.need([t["action"] for t in tr] == ["timer"] * 3 + ["budget-spent"], f"trace {tr}")
    s.need(all(int(t["armed_us"]) == HOUR_US for t in tr), "every sleep must arm exactly one hour")
    for i, rec in enumerate(r[2:], start=1):
        s.need(rec["bo"] == "ok" and rec["next"] == "3600s" and rec["prev"] == "3600s" and rec["cycle"] == str(i),
               f"timer receipt {i}: {rec}")
    reqs = s.requests()
    puts = [q for q in reqs if q["method"] == "PUT"]
    s.need(len(puts) == 1 and puts[0]["t"] >= s.t0 + SLEEP_S - 0.5, "exactly one PUT, from the sleep sync")
    gets = [q for q in reqs if q["method"] == "GET" and "/syncs/progress/" in q["path"]]
    # Sleep: GET, PUT, read-back GET. Each unchanged timer wake: exactly one GET.
    s.need(len(gets) == 2 + 3, f"progress GETs: two for the sleep sync plus one per timer wake; got {len(gets)}")


def scenario_retry(s: Scenario):
    """S2: an outage across the sleep and first timer wake keeps the cadence; wake 2 pushes."""
    s.faults({"offline_requests": 2})
    s.run(max_wakes=3).common()
    bo = [rec["bo"] for rec in s.receipts()[1:]]
    s.need(bo == ["retry", "retry", "ok", "ok"], f"bookorbit outcomes {bo}")
    s.need(all(rec["next"] == "3600s" for rec in s.receipts()[1:]), "a failure must keep the hourly cadence")
    puts = [q for q in s.requests() if q["method"] == "PUT"]
    s.need(len(puts) == 1, f"the pending local position is pushed once after recovery, got {len(puts)} PUTs")


def scenario_low_memory(s: Scenario):
    """S3: the TLS heap gate postpones a wake without any request; the next wake recovers."""
    s.faults({"offline_requests": 1})  # leave an upload pending from the sleep sync
    s.run(max_wakes=2, heap_schedule="30000:19000,-").common()
    r = s.receipts()
    s.need(s.triggers() == ["boot", "sleep", "timer", "timer"], f"triggers {s.triggers()}")
    low, recovered = (r[2], r[3]) if len(r) >= 4 else ({}, {})
    s.need(low.get("bo") == "retry" and low.get("gate", "").endswith("/1") and low.get("gmin") == "30000/19000"
           and low.get("heap", "").startswith("30000/19000/") and low.get("next") == "3600s",
           f"low-memory wake: {low}")
    s.need(recovered.get("bo") == "ok" and recovered.get("gate", "").endswith("/0"), f"recovery wake: {recovered}")
    # The failed sleep request, nothing at all from the gated wake, then version/GET/PUT/read-back.
    reqs = [q["method"] for q in s.requests()]
    s.need(reqs == ["GET", "GET", "GET", "PUT", "GET"], f"requests {reqs}")


def scenario_off(s: Scenario):
    """S4: Off arms nothing; the chain ends after the sleep sync."""
    s.settings(periodicSyncInterval=OFF)
    s.run(max_wakes=3).common()
    s.need(s.triggers() == ["boot", "sleep"], f"triggers {s.triggers()}")
    s.need([t["action"] for t in s.traces()] == ["unarmed"], f"trace {s.traces()}")
    s.need(s.receipts()[1].get("next") == "0s" if len(s.receipts()) > 1 else False, "sleep receipt next=0s")


def scenario_no_wifi(s: Scenario):
    """S5: no saved WiFi (file absent) is configuration, not a failure: nothing is armed."""
    (s.sd / "wifi.json").unlink()
    s.run(max_wakes=3)
    s.need(s.triggers() == ["boot", "sleep"], f"triggers {s.triggers()}")
    s.need([t["action"] for t in s.traces()] == ["unarmed"], f"trace {s.traces()}")
    s.need(not s.requests(), "no WiFi must mean no request at all")


def scenario_24h(s: Scenario):
    """S6: the 24 h interval reaches the sleep timer as a uint64 microsecond count, intact."""
    s.settings(periodicSyncInterval=HOURS24)
    s.run(divisor=57_600_000, max_wakes=1).common()
    tr = s.traces()
    s.need([t["armed_us"] for t in tr] == ["86400000000", "86400000000"], f"trace {tr}")
    s.need([rec.get("next") for rec in s.receipts()[1:]] == ["86400s", "86400s"], f"receipts {s.receipts()}")
    s.need(s.receipts()[-1].get("prev") == "86400s", "timer wake must see the 24 h delay retained")


def corrupt_between_wakes(s: Scenario, name: str):
    """Corrupt one store while the device sleeps, keep it corrupt for one timer wake, restore it."""
    path = s.sd / name
    original = path.read_bytes()
    s.start(max_wakes=3)
    if s.wait_for(lambda: len(s.traces()) >= 1, "the first armed sleep"):
        path.write_bytes(b"{ this is not json")
        if s.wait_for(lambda: len(s.traces()) >= 2, "the corrupted timer wake to sleep again"):
            path.write_bytes(original)
    s.wait()
    tr = s.traces()
    s.need(len(tr) >= 2 and tr[1]["action"] == "timer" and int(tr[1]["armed_us"]) == HOUR_US,
           f"a wake that cannot read {name} must re-arm the retained hour; trace {tr}")
    s.need([t["action"] for t in tr] == ["timer"] * 3 + ["budget-spent"], f"chain must continue; trace {tr}")


def scenario_corrupt_state(s: Scenario):
    """S7 (former defect): unreadable state.json on one wake used to arm 0 and end the night."""
    corrupt_between_wakes(s, "state.json")
    r = s.receipts()
    s.need(s.triggers() == ["boot", "sleep", "timer", "timer", "timer"], f"triggers {s.triggers()}")
    if len(r) == 5:
        s.need(r[2]["store"] == "fail" and r[2]["next"] == "3600s" and r[2]["bo"] == "-", f"failed wake {r[2]}")
        s.need(r[3]["store"] == "ok" and r[3]["bo"] == "ok", f"recovered wake {r[3]}")


def scenario_corrupt_wifi(s: Scenario):
    """S8 (former defect): an unreadable wifi.json used to read as "no WiFi" and end the night."""
    corrupt_between_wakes(s, "wifi.json")
    r = s.receipts()
    s.need(s.triggers() == ["boot", "sleep", "timer", "timer", "timer"], f"triggers {s.triggers()}")
    if len(r) == 5:
        s.need(r[2]["store"] == "fail" and r[2]["next"] == "3600s", f"failed wake {r[2]}")
        s.need(r[3]["store"] == "ok" and r[3]["bo"] == "ok", f"recovered wake {r[3]}")


SCENARIOS = {
    "S1": scenario_cadence, "S2": scenario_retry, "S3": scenario_low_memory, "S4": scenario_off,
    "S5": scenario_no_wifi, "S6": scenario_24h, "S7": scenario_corrupt_state, "S8": scenario_corrupt_wifi,
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--program", type=Path, default=sim_smoke.PROGRAM)
    ap.add_argument("--video-driver", default="dummy")
    ap.add_argument("--timeout", type=int, default=90)
    ap.add_argument("--keep", help="artifact directory (default: a new temp dir, kept)")
    ap.add_argument("--only", help="comma-separated scenario names, e.g. S1,S7")
    args = ap.parse_args()
    if not args.no_build:
        pio = sim_smoke.find_pio()
        cmd = [pio, "run", "-e", sim_smoke.PIOENV] if pio else [sys.executable, "-m", "platformio", "run", "-e",
                                                                 sim_smoke.PIOENV]
        if subprocess.run(cmd, cwd=REPO_ROOT).returncode != 0:
            print("[periodic] FAIL: simulator build failed")
            return 1
    if not args.program.exists():
        print(f"[periodic] FAIL: no simulator binary at {args.program}")
        return 1

    root = Path(args.keep) if args.keep else Path(tempfile.mkdtemp(prefix="crossfade-sim-periodic-"))
    root.mkdir(parents=True, exist_ok=True)
    print(f"[periodic] artifacts: {root}")
    names = args.only.split(",") if args.only else list(SCENARIOS)
    failures = []
    for name in names:
        s = Scenario(root, name, args)
        SCENARIOS[name](s)
        print(f"[periodic:{name}] {'OK' if not s.failures else 'FAIL'}: {SCENARIOS[name].__doc__.strip()}")
        for line in s.receipts_path.read_text().splitlines() if s.receipts_path.exists() else []:
            print(f"    receipt  {line}")
        failures += s.failures
    for f in failures:
        print(f"[periodic] FAIL: {f}")
    print(f"[periodic] {'PASS' if not failures else 'FAIL'}: {len(names)} scenario(s), {len(failures)} failure(s)")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
