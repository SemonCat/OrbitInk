#!/usr/bin/env python3
"""
Headless acceptance of the web firmware update (/update, /api/firmware*) on [env:simulator_x3_zh_tw]:
the firmware's own CrossPointWebServer handlers over the simulator loopback, plus the real
on-device confirmation activity driven by the input script.

    CROSSFADE_SIM_ZH_TW_FONTS=<pkg>/sd-assets/fonts python3 scripts/sim_firmware_update_smoke.py [--no-build]

Phase A (HTTP only): page served; status; bad magic, wrong chip, truncated image, missing file part,
  over-partition upload (6.6 MB real body), client disconnect mid-body, valid upload (SHA256 must
  equal the local one and the staged SD bytes), repeat upload replacing it, apply with a wrong
  SHA256 (409), discard.
Phase B: upload -> apply(202) -> device re-validates and shows the confirmation -> declined
  (Cancel is the default) -> back on the server, staged file removed, state IDLE.
Phase C: upload -> apply -> confirmed on the device -> the simulator's non-destructive
  flashFromSdPath stub fails ("Firmware write failed") -> Back -> staged file removed, server live.
Phase D: replace the SD file with another valid image during physical confirmation; confirming
  rejects the changed SHA256 before the simulator flasher stub is reached.

Simulator only. Nothing is flashed: the simulator has no flash, its flasher is a stub that returns
WRITE_FAIL, and this test asserts that stub ran. Body buffering differs from the device (the
simulator reads a whole request before dispatching it), so a mid-body disconnect here never creates
a partial file; partial-file cleanup on the device path is covered by test/firmware_update.
"""

import argparse
import hashlib
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sim_bookorbit_home_smoke as bo  # noqa: E402
import sim_bookorbit_setup_smoke as setup  # noqa: E402
import sim_smoke  # noqa: E402
from sim_smoke import REPO_ROOT  # noqa: E402

PARTITION = 0x640000
HOTSPOT_KEYS = "2500:ENTER;4000:DOWN;5000:ENTER"  # fresh profile: guide -> web -> Create Hotspot
# Absolute simulator times (ms) the HTTP thread and the input script agree on.
T_APPLY_B = 16000
T_DECLINE = 20000
T_UPLOAD_C = 23000
T_APPLY_C = 25000
T_CONFIRM_DOWN = 29000
T_CONFIRM_ENTER = 30000
T_FAILED_BACK = 34000
T_FINAL = 37000
T_APPLY_D = 40000
T_TAMPER_D = 43000
T_CONFIRM_D_DOWN = 45000
T_CONFIRM_D_ENTER = 46000
T_BACK_D = 50000
T_FINAL_D = 53000
T_QUIT = 56000


def esp_image(segment_sizes, hash_appended=True, chip=0x0005, seed=0x1234):
    img = bytearray(24)
    img[0] = 0xE9
    img[1] = len(segment_sizes)
    img[12:14] = struct.pack("<H", chip)
    img[23] = 1 if hash_appended else 0
    checksum = 0xEF
    x = seed
    for size in segment_sizes:
        img += struct.pack("<II", 0, size)
        data = bytearray(size)
        for i in range(size):
            x = (x * 1103515245 + 12345) & 0xFFFFFFFF
            data[i] = (x >> 16) & 0xFF
        for b in data:
            checksum ^= b
        img += data
    pad_end = (len(img) + 16) & ~15
    img += bytes(pad_end - len(img))
    img[pad_end - 1] = checksum
    if hash_appended:
        img += hashlib.sha256(img).digest()
    return bytes(img)


def multipart(data, filename="firmware.bin", field="file"):
    boundary = "----crossfadeotatest7c1f"
    head = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"{field}\"; filename=\"{filename}\"\r\n"
            "Content-Type: application/octet-stream\r\n\r\n").encode()
    tail = f"\r\n--{boundary}--\r\n".encode()
    return head + data + tail, f"multipart/form-data; boundary={boundary}"


def raw_request(port, method, path, body=b"", ctype=None, send_bytes=None, timeout=20):
    """HTTP/1.1 over a plain socket; send_bytes < len(body) simulates a client that disconnects."""
    head = f"{method} {path} HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\nContent-Length: {len(body)}\r\n"
    if ctype:
        head += f"Content-Type: {ctype}\r\n"
    payload = head.encode() + b"\r\n" + (body if send_bytes is None else body[:send_bytes])
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.sendall(payload)
        if send_bytes is not None:
            s.shutdown(socket.SHUT_WR)
        chunks = []
        try:
            while True:
                b = s.recv(65536)
                if not b:
                    break
                chunks.append(b)
        except (socket.timeout, ConnectionResetError):
            pass
    resp = b"".join(chunks)
    if not resp:
        return 0, None
    status = int(resp.split(b" ", 2)[1])
    body_text = resp.split(b"\r\n\r\n", 1)[1] if b"\r\n\r\n" in resp else b""
    try:
        return status, json.loads(body_text)
    except ValueError:
        return status, None


def wait_until(t0, ms):
    delay = t0 + ms / 1000.0 - time.time()
    if delay > 0:
        time.sleep(delay)


def http_driver(port, sd, out, t0):
    steps = out.setdefault("steps", [])
    staged = sd / ".crosspoint" / "fw_staged.bin"
    part = sd / ".crosspoint" / "fw_upload.part"

    def step(label, method, path, body=b"", ctype=None, send_bytes=None, timeout=20):
        status, js = raw_request(port, method, path, body, ctype, send_bytes, timeout)
        steps.append({"label": label, "status": status, "json": js, "t": round(time.time() - t0, 1),
                      "staged_file": staged.exists(), "part_file": part.exists()})
        return status, js

    def upload(label, data, **kw):
        body, ctype = multipart(data, **kw)
        return step(label, "POST", "/api/firmware/upload", body, ctype)

    def form(sha):
        return f"sha256={sha}".encode(), "application/x-www-form-urlencoded"

    base = f"http://127.0.0.1:{port}"
    if not setup.wait_for_server(base, 30):
        out["error"] = "web server never answered"
        return
    s, page = setup.http(base, "GET", "/update")
    out["page_ok"] = s == 200 and "/api/firmware/upload" in page and "韌體" in page
    s, home = setup.http(base, "GET", "/")
    out["home_links_update"] = s == 200 and 'href="/update"' in home
    step("status", "GET", "/api/firmware")

    good = esp_image([150000, 40000])
    good2 = esp_image([120000, 9000], seed=0x9999)
    out["good_sha"] = hashlib.sha256(good).hexdigest()
    out["good2_sha"] = hashlib.sha256(good2).hexdigest()

    upload("bad magic (epub-like)", b"PK\x03\x04" + bytes(200000))
    upload("wrong chip (ESP32-S3 image)", esp_image([150000], chip=0x0009))
    upload("truncated image", good[: len(good) // 2])
    upload("trailing garbage", good + b"\x00")
    step("no file part", "POST", "/api/firmware/upload", b"x=1", "application/x-www-form-urlencoded")
    upload("over partition (6.6 MB)", bytes([0xE9]) + bytes(PARTITION + 200000))
    body, ctype = multipart(good)
    step("client disconnect mid-body", "POST", "/api/firmware/upload", body, ctype, send_bytes=len(body) // 3, timeout=10)
    step("status after disconnect", "GET", "/api/firmware")
    upload("valid upload", good)
    out["staged_bytes_match"] = staged.exists() and staged.read_bytes() == good
    upload("repeat upload replaces", good2)
    out["repeat_bytes_match"] = staged.exists() and staged.read_bytes() == good2
    step("apply wrong sha", "POST", "/api/firmware/apply", *form(out["good_sha"]))
    step("apply missing sha", "POST", "/api/firmware/apply", b"", "application/x-www-form-urlencoded")
    step("discard", "POST", "/api/firmware/discard")
    step("apply after discard", "POST", "/api/firmware/apply", *form(out["good2_sha"]))

    # Phase B: apply then decline on the device.
    upload("B upload", good)
    wait_until(t0, T_APPLY_B)
    step("B apply", "POST", "/api/firmware/apply", *form(out["good_sha"]))
    step("B upload while applying", "POST", "/api/firmware/upload", *multipart(good2), timeout=3)
    wait_until(t0, T_DECLINE + 1500)
    step("B status after decline", "GET", "/api/firmware")

    # Phase C: apply, confirm on device, simulator flasher stub fails.
    wait_until(t0, T_UPLOAD_C)
    upload("C upload", good2)
    wait_until(t0, T_APPLY_C)
    step("C apply", "POST", "/api/firmware/apply", *form(out["good2_sha"]))
    wait_until(t0, T_FINAL)
    step("C status after failed flash", "GET", "/api/firmware")
    upload("D upload", good2)
    wait_until(t0, T_APPLY_D)
    step("D apply", "POST", "/api/firmware/apply", *form(out["good2_sha"]))
    wait_until(t0, T_TAMPER_D)
    staged.write_bytes(esp_image([120000, 9000], seed=0x4321))
    out["tampered_after_confirmation"] = True
    wait_until(t0, T_FINAL_D)
    step("D status after changed image", "GET", "/api/firmware")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--video-driver", default="dummy")
    ap.add_argument("--timeout", type=int, default=90)
    ap.add_argument("--keep", help="artifact directory (default: a new temp dir, kept)")
    args = ap.parse_args()
    bo.zh_tw_fonts()
    if not args.no_build:
        pio = sim_smoke.find_pio()
        if subprocess.run([pio, "run", "-e", bo.PIOENV], cwd=REPO_ROOT).returncode != 0:
            print("[fwup] FAIL: build failed")
            return 1
    root = Path(args.keep) if args.keep else Path(tempfile.mkdtemp(prefix="crossfade-sim-fwup-"))
    root.mkdir(parents=True, exist_ok=True)
    print(f"[fwup] artifacts: {root}")
    failures = []
    work = root / "F1"
    setup.make_sd(work, {"trackReadingStats": 1})
    sd = work / "fs_"
    book = sd / "books" / "keep.epub"
    book.write_bytes(b"PK\x03\x04 placeholder book bytes")
    book_sha = hashlib.sha256(book.read_bytes()).hexdigest()

    port = setup.free_port_pair()
    out = {}
    t0 = time.time()
    thread = threading.Thread(target=http_driver, args=(port, sd, out, t0), daemon=True)
    thread.start()
    keys = (f"{HOTSPOT_KEYS};{T_DECLINE}:ENTER;{T_CONFIRM_DOWN}:DOWN;{T_CONFIRM_ENTER}:ENTER;"
            f"{T_FAILED_BACK}:BACK;{T_CONFIRM_D_DOWN}:DOWN;{T_CONFIRM_D_ENTER}:ENTER;"
            f"{T_BACK_D}:BACK;{T_QUIT}:QUIT")
    shots = [(8000, "F-01-server.bmp", "web server running"),
             (T_APPLY_B + 2500, "F-02-confirm.bmp", "on-device confirmation for a web-staged image"),
             (T_DECLINE + 1200, "F-03-declined.bmp", "declined: back on the server screen"),
             (T_APPLY_C + 3000, "F-04-confirm-again.bmp", "confirmation, second image"),
             (T_CONFIRM_DOWN + 600, "F-05-confirm-focused.bmp", "confirm focused"),
             (T_CONFIRM_ENTER + 2500, "F-06-sim-flash-failed.bmp", "simulator flasher stub: write failed"),
             (T_FAILED_BACK + 1500, "F-07-back-on-server.bmp", "back on the server after the failure"),
             (T_APPLY_D + 1500, "F-08-before-sd-change.bmp", "third confirmation before SD replacement"),
             (T_CONFIRM_D_ENTER + 2000, "F-09-changed-image.bmp", "changed SD image rejected before flash"),
             (T_BACK_D + 1500, "F-10-back-after-change.bmp", "back after changed image")]
    log = setup.run_sim("F1", work, keys, shots, args, [], {"CROSSPOINT_SIM_HTTP_PORT": str(port)},
                        timeout=T_QUIT // 1000 + 60)
    thread.join(timeout=10)
    (root / "http-steps.json").write_text(json.dumps(out, indent=2, ensure_ascii=False))

    if "error" in out:
        failures.append(out["error"])
    steps = {s["label"]: s for s in out.get("steps", [])}

    def expect(label, status, state=None, error=None, staged_file=None, sha=None, image=None):
        s = steps.get(label)
        if not s:
            failures.append(f"{label}: step missing")
            return
        js = s["json"] or {}
        if s["status"] != status:
            failures.append(f"{label}: HTTP {s['status']} != {status} ({js})")
        if state and js.get("state") != state:
            failures.append(f"{label}: state {js.get('state')} != {state}")
        if error and js.get("error") != error:
            failures.append(f"{label}: error {js.get('error')} != {error}")
        if image != js.get("image"):
            failures.append(f"{label}: image {js.get('image')} != {image}")
        if staged_file is not None and s["staged_file"] != staged_file:
            failures.append(f"{label}: staged file present={s['staged_file']}, expected {staged_file}")
        if s["part_file"]:
            failures.append(f"{label}: a .part file was left behind")
        if sha and js.get("sha256") != sha:
            failures.append(f"{label}: sha256 {js.get('sha256')} != {sha}")

    if not out.get("page_ok"):
        failures.append("/update page not served")
    if not out.get("home_links_update"):
        failures.append("Home page does not link /update")
    expect("status", 200, "IDLE", staged_file=False)
    if (steps.get("status", {}).get("json") or {}).get("limit") != PARTITION:
        failures.append("status: limit is not the X3 app partition size")
    expect("bad magic (epub-like)", 400, "IDLE", "INVALID", staged_file=False, image="BAD_MAGIC")
    expect("wrong chip (ESP32-S3 image)", 400, "IDLE", "INVALID", staged_file=False, image="BAD_CHIP")
    expect("truncated image", 400, "IDLE", "INVALID", staged_file=False, image="BAD_SEGMENTS")
    expect("trailing garbage", 400, "IDLE", "INVALID", staged_file=False, image="BAD_SIZE")
    expect("no file part", 400, "IDLE", "NO_FILE", staged_file=False)
    expect("over partition (6.6 MB)", 413, "IDLE", "TOO_LARGE", staged_file=False)
    expect("status after disconnect", 200, "IDLE", staged_file=False)
    expect("valid upload", 200, "STAGED", staged_file=True, sha=out.get("good_sha"))
    if not out.get("staged_bytes_match"):
        failures.append("valid upload: staged SD bytes differ from the uploaded file")
    expect("repeat upload replaces", 200, "STAGED", staged_file=True, sha=out.get("good2_sha"))
    if not out.get("repeat_bytes_match"):
        failures.append("repeat upload: staged SD bytes are not the second file")
    expect("apply wrong sha", 409, "STAGED", "SHA_MISMATCH", staged_file=True)
    expect("apply missing sha", 409, "STAGED", "SHA_MISMATCH", staged_file=True)
    expect("discard", 200, "IDLE", staged_file=False)
    expect("apply after discard", 409, "IDLE", "NOT_STAGED", staged_file=False)
    expect("B upload", 200, "STAGED", staged_file=True, sha=out.get("good_sha"))
    expect("B apply", 202, "APPLYING", staged_file=True)
    b_busy = steps.get("B upload while applying")
    if b_busy and b_busy["status"] not in (0, 409):
        failures.append(f"B upload while applying: HTTP {b_busy['status']} (must be refused or unanswered)")
    expect("B status after decline", 200, "IDLE", staged_file=False)
    expect("C upload", 200, "STAGED", staged_file=True, sha=out.get("good2_sha"))
    expect("C apply", 202, "APPLYING", staged_file=True)
    expect("C status after failed flash", 200, "IDLE", staged_file=False)

    for needle, why in [
        ("Web firmware apply requested", "apply never reached the activity"),
        ("SdFirmwareUpdateActivity version=", "on-device update activity never started"),
        ("staged=1", "staged mode not used"),
        ("Firmware flashing is not supported in the native simulator", "simulator flasher stub did not run"),
        ("flash failed: UNSUPPORTED_IN_SIMULATOR", "flash did not fail as the simulator stub must"),
    ]:
        if needle not in log:
            failures.append(f"log: {why} ({needle!r} missing)")
    if log.count("[SIM] Incomplete request body") != 1:
        failures.append("expected exactly one simulator incomplete-body abort (the disconnect step)")
    expect("D upload", 200, "STAGED", staged_file=True, sha=out.get("good2_sha"))
    expect("D apply", 202, "APPLYING", staged_file=True)
    expect("D status after changed image", 200, "IDLE", staged_file=False)
    if not out.get("tampered_after_confirmation") or "staged image no longer matches" not in log:
        failures.append("changed SD image was not rejected by the confirmed SHA256")
    if log.count("SdFirmwareUpdateActivity version=") != 3:
        failures.append("expected exactly three on-device firmware activities")
    if log.count("flash failed: UNSUPPORTED_IN_SIMULATOR") != 1:
        failures.append("only the untampered confirmed image should reach the simulator flasher")
    if "SD firmware update complete" in log:
        failures.append("simulator claims a completed flash")
    if hashlib.sha256(book.read_bytes()).hexdigest() != book_sha:
        failures.append("a book on the SD card changed")
    leftovers = [p.name for p in (sd / ".crosspoint").glob("fw_*")]
    if leftovers:
        failures.append(f"firmware staging files left on SD: {leftovers}")

    for line in [ln for ln in log.splitlines() if "[ERR]" in ln and "FW" not in ln and "FWSTAGE" not in ln
                 and "FWUP" not in ln and "FLASH" not in ln
                 # caused on purpose by the "client disconnect mid-body" step:
                 and "[SIM] Incomplete request body" not in ln][:5]:
        failures.append(f"unexpected firmware error: {line.strip()}")

    print(json.dumps({"steps": len(out.get("steps", [])), "failures": failures}, indent=2, ensure_ascii=False))
    print(f"[fwup] {'FAIL' if failures else 'PASS'}: artifacts {root}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
