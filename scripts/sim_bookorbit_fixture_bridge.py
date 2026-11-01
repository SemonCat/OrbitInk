#!/usr/bin/env python3
"""Fixture-only BookOrbit server for the X3 simulator's owned SecureHttpClient bridge.

Same stdin/stdout contract as scripts/sim_bookorbit_http_bridge.py (one JSON request in, one JSON
response out, see sim/CrossFadeSimHal/SecureHttpClient.cpp), but it NEVER opens a socket: every
answer comes from a fixture directory and a small JSON state file, so a smoke run can exercise the
production catalog, download, progress and page-stats code paths without touching any real
BookOrbit account or reading history.

Environment:
  CROSSFADE_SIM_FIXTURE_DIR   directory with catalog.json and the EPUB files it references
  CROSSFADE_SIM_FIXTURE_STATE JSON state file (created if absent): progress, stats ledger, faults
  CROSSFADE_SIM_FIXTURE_LOG   JSONL request log (appended)

catalog.json:
  {"root": [{"section": "all-books", "title": "..."}, ...],
   "books": [{"id": 101, "title": "...", "authors": ["..."], "progressPercentage": 12.5,
              "files": [{"id": 1001, "format": "epub", "file": "a.epub"}]}]}
  sizeBytes and fileHash (KOReader partial MD5 of the file) are computed from the file.

State "faults" (set by the smoke script between runs):
  {"offline": true}          every request fails at the transport (status 0)
  {"download_status": 500}   file downloads answer this status
  {"stats_status": 503}      page-stats POSTs answer this status
  {"fail_once": "/plugin/catalog/books"}   the NEXT request to exactly this route fails at the
                             transport (status 0), then the fault clears itself
  {"offline_requests": 3}    the next 3 requests (any route) fail at the transport, then the
                             fault clears itself -- a network outage spanning several sync windows

Page-stats responses follow BookOrbit's contract (results[{hash, accepted, duplicates}],
unmatched[]); dedup key (deviceId, hash, page, startTime) mirrors the server's documented
(user, book, device, page, startTime). This models, but does not prove, the live server.
"""
import base64
import hashlib
import json
import os
import sys
import time
from pathlib import Path
from urllib.parse import urlsplit, parse_qs, unquote

PREFIX = "/api/v1/koreader"


def partial_md5(path: Path) -> str:
    """KOReader's partial MD5 (lib/KOReaderSync/KOReaderDocumentId.cpp): 1 KiB at 1024 << 2i."""
    md5 = hashlib.md5()
    size = path.stat().st_size
    with path.open("rb") as f:
        for i in range(-1, 11):
            offset = 1024 << (2 * i) if i >= 0 else 0
            if offset >= size:
                break
            f.seek(offset)
            md5.update(f.read(1024))
    return md5.hexdigest()


def load_json(path: Path, default):
    try:
        return json.loads(path.read_text())
    except (FileNotFoundError, json.JSONDecodeError):
        return default


def respond(status, body=None, raw: bytes = None):
    out = {"status": status, "headers": {}}
    if raw is not None:
        out["body_b64"] = base64.b64encode(raw).decode()
    else:
        out["body"] = "" if body is None else (body if isinstance(body, str) else json.dumps(body))
    sys.stdout.write(json.dumps(out))


def main() -> int:
    fixture = Path(os.environ["CROSSFADE_SIM_FIXTURE_DIR"])
    state_path = Path(os.environ["CROSSFADE_SIM_FIXTURE_STATE"])
    log_path = Path(os.environ["CROSSFADE_SIM_FIXTURE_LOG"])
    request = json.loads(sys.stdin.read(8 * 1024 * 1024))
    catalog = load_json(fixture / "catalog.json", {"root": [], "books": []})
    state = load_json(state_path, {})
    state.setdefault("progress", {})
    state.setdefault("stats", [])
    faults = state.get("faults", {})

    method = request.get("method", "")
    url = urlsplit(request.get("url", ""))
    path = unquote(url.path)
    query = parse_qs(url.query)
    entry = {"t": time.time(), "method": method, "path": path, "query": url.query}
    body = request.get("body") or ""

    def finish(status, payload=None, raw=None):
        entry["status"] = status
        with log_path.open("a") as log:
            log.write(json.dumps(entry, ensure_ascii=False) + "\n")
        state_path.write_text(json.dumps(state))
        if status == 0:
            respond(0, "")  # SecureHttpClient maps status <= 0 to a transport failure
        else:
            respond(status, payload, raw)
        return 0

    if faults.get("offline"):
        return finish(0)
    if faults.get("offline_requests", 0) > 0:
        faults["offline_requests"] -= 1
        if faults["offline_requests"] == 0:
            faults.pop("offline_requests")
        entry["injected"] = "offline_requests"
        return finish(0)
    if faults.get("fail_once") and path == PREFIX + faults["fail_once"]:
        faults.pop("fail_once")  # one transport failure, then the route answers normally
        entry["injected"] = "fail_once"
        return finish(0)
    if not path.startswith(PREFIX):
        return finish(404, {"error": "fixture: outside /api/v1/koreader"})
    route = path[len(PREFIX):]
    books = {str(b["id"]): b for b in catalog.get("books", [])}

    def file_entry(f):
        fp = fixture / f["file"]
        return {"id": f["id"], "format": f.get("format", "epub"), "role": "primary",
                "downloadVariant": "original", "sizeBytes": fp.stat().st_size,
                "fileHash": partial_md5(fp), "durationSeconds": None,
                "downloadUrl": f"{PREFIX}/plugin/catalog/files/{f['id']}/download", "devicePath": ""}

    def list_item(b):
        return {"id": b["id"], "title": b["title"], "authors": b.get("authors", []),
                "progressPercentage": b.get("progressPercentage"), "readStatus": b.get("readStatus")}

    if method == "GET" and route == "/users/auth":
        return finish(200, {"authorized": "OK"})
    if method == "GET" and route == "/plugin/version":
        return finish(200, {"pluginVersion": "fixture", "serverVersion": "fixture", "capabilities": []})
    if method == "GET" and route == "/plugin/catalog/root":
        return finish(200, {"sections": catalog.get("root", [])})
    if method == "GET" and route == "/plugin/catalog/books":
        items = [list_item(b) for b in catalog.get("books", [])]
        q = (query.get("q") or [""])[0].lower()
        if q:
            items = [i for i in items if q in i["title"].lower()]
        page = int((query.get("page") or ["1"])[0])
        size = int((query.get("size") or ["20"])[0])
        chunk = items[(page - 1) * size:page * size]
        return finish(200, {"items": chunk, "total": len(items), "page": page, "size": size,
                            "hasNext": page * size < len(items), "hasPrevious": page > 1})
    if method == "GET" and route.startswith("/plugin/catalog/books/"):
        b = books.get(route.rsplit("/", 1)[1])
        if not b:
            return finish(404, {"error": "no such book"})
        detail = list_item(b)
        detail.update({"files": [file_entry(f) for f in b.get("files", [])], "rating": None})
        return finish(200, detail)
    if method == "GET" and route.startswith("/plugin/catalog/files/") and route.endswith("/download"):
        if faults.get("download_status"):
            return finish(int(faults["download_status"]), {"error": "fixture fault"})
        file_id = route.split("/")[-2]
        for b in catalog.get("books", []):
            for f in b.get("files", []):
                if str(f["id"]) == file_id:
                    return finish(200, raw=(fixture / f["file"]).read_bytes())
        return finish(404, {"error": "no such file"})
    if method == "GET" and route.startswith("/syncs/progress/"):
        rec = state["progress"].get(route.rsplit("/", 1)[1])
        return finish(200, rec if rec else {})
    if method == "PUT" and route == "/syncs/progress":
        doc = json.loads(body)
        state["progress"][doc["document"]] = {k: doc[k] for k in
                                              ("document", "progress", "percentage", "device", "device_id",
                                               "timestamp") if k in doc}
        return finish(200, {"document": doc["document"], "timestamp": doc.get("timestamp")})
    if method == "POST" and route == "/plugin/page-stats":
        if faults.get("stats_status"):
            return finish(int(faults["stats_status"]), {"error": "fixture fault"})
        doc = json.loads(body)
        device = doc.get("deviceId", "")
        results = []
        seen = {(s["device"], s["hash"], s["page"], s["startTime"]) for s in state["stats"]}
        for book in doc.get("books", []):
            accepted = duplicates = 0
            for ev in book.get("events", []):
                key = (device, book["hash"], ev.get("page"), ev.get("startTime"))
                if key in seen:
                    duplicates += 1
                    continue
                seen.add(key)
                accepted += 1
                state["stats"].append({"device": device, "hash": book["hash"], "page": ev.get("page"),
                                       "startTime": ev.get("startTime"), "duration": ev.get("durationSeconds"),
                                       "totalPages": ev.get("totalPages")})
            results.append({"hash": book["hash"], "accepted": accepted, "duplicates": duplicates})
        entry["stats"] = results
        return finish(200, {"results": results, "unmatched": []})
    return finish(404, {"error": "fixture: unsupported route"})


if __name__ == "__main__":
    sys.exit(main())
