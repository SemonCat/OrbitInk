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
              "readStatus": "reading", "seriesId": 7, "seriesName": "...", "seriesIndex": 2,
              "libraryId": 1, "collectionIds": [3], "addedAt": "2026-01-02T00:00:00Z",
              "lastReadAt": "...", "description": "...",
              "files": [{"id": 1001, "format": "epub", "file": "a.epub"}]}],
   "libraries": [{"id": 1, "name": "..."}], "collections": [{"id": 3, "name": "..."}]}
  sizeBytes and fileHash (KOReader partial MD5 of the file) are computed from the file.

Modelled server behaviour (koreader-catalog.service.ts / opds-book.service.ts at the commit cited
in lib/KOReaderSync/BookOrbitCatalogClient.h): books accept sort (6 values, natural order),
q (title, author, series name), readStatus buckets, author, seriesId, libraryId, collectionId;
sections/authors and sections/series page at 60 entries and ignore `size`; series entries carry
id "series:<n>" plus a numeric seriesId; list/detail items carry the server's URL/date fields, and
detail adds description, collections and relatedSections AFTER/AROUND files exactly as the server
orders them, so client body caps are exercised with realistic bytes.

State "faults" (set by the smoke script between runs):
  {"offline": true}          every request fails at the transport (status 0)
  {"download_status": 500}   file downloads answer this status
  {"stats_status": 503}      page-stats POSTs answer this status
  {"fail_once": "/plugin/catalog/books"}   the NEXT request to exactly this route fails at the
                             transport (status 0), then the fault clears itself
  {"offline_requests": 3}    the next 3 requests (any route) fail at the transport, then the
                             fault clears itself -- a network outage spanning several sync windows
  {"delay_ms": 1500}         every answer is delayed (loading screens become observable)

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

    if faults.get("delay_ms"):
        time.sleep(int(faults["delay_ms"]) / 1000.0)
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
    if method == "GET" and route.startswith("/plugin/catalog/books/") and faults.get("fail_detail_once"):
        status = int(faults.pop("fail_detail_once"))
        return finish(status, {"error": "fixture detail fault"})
    if route.startswith("/plugin/catalog") and faults.get("catalog_status"):
        return finish(int(faults["catalog_status"]), {"error": "fixture catalog fault"})
    if route == "/plugin/catalog/books" and (query.get("page") or ["1"])[0] == str(faults.get("fail_page_once")):
        faults.pop("fail_page_once")
        return finish(0)

    def file_entry(f):
        fp = fixture / f["file"]
        return {"id": f["id"], "format": f.get("format", "epub"), "role": "primary",
                "downloadVariant": "original", "sizeBytes": fp.stat().st_size,
                "fileHash": partial_md5(fp), "durationSeconds": None,
                "downloadUrl": f"{PREFIX}/plugin/catalog/files/{f['id']}/download", "devicePath": ""}

    catalog_base = PREFIX + "/plugin/catalog"

    def list_item(b):
        return {"id": b["id"], "title": b["title"], "authors": b.get("authors", []),
                "seriesId": b.get("seriesId"), "seriesName": b.get("seriesName"),
                "seriesIndex": b.get("seriesIndex"),
                "progressPercentage": b.get("progressPercentage"), "lastReadAt": b.get("lastReadAt"),
                "readStatus": b.get("readStatus"), "formats": ["epub"], "hasCover": True,
                "thumbnailUrl": f"{catalog_base}/books/{b['id']}/thumbnail",
                "detailUrl": f"{catalog_base}/books/{b['id']}",
                "addedAt": b.get("addedAt", "2026-01-01T00:00:00.000Z"),
                "updatedAt": b.get("addedAt", "2026-01-01T00:00:00.000Z")}

    reading_bucket = ("reading", "rereading", "on_hold")
    finished_bucket = ("read", "skimmed", "abandoned")

    def bad_request(message):
        return finish(400, {"message": message, "error": "Bad Request", "statusCode": 400})

    def positive_int(name):
        raw = (query.get(name) or [None])[0]
        if raw is None:
            return None, False
        return (int(raw), False) if raw.isdigit() and int(raw) > 0 else (None, True)

    if method == "GET" and route == "/users/auth":
        return finish(200, {"authorized": "OK"})
    if method == "GET" and route == "/plugin/version":
        return finish(200, {"pluginVersion": "fixture", "serverVersion": "fixture", "capabilities": []})
    if method == "GET" and route == "/plugin/catalog/root":
        return finish(200, {"sections": catalog.get("root", [])})
    if method == "GET" and route.startswith("/plugin/catalog/sections/"):
        section = route.rsplit("/", 1)[1]
        page = int((query.get("page") or ["1"])[0])
        all_books = catalog.get("books", [])
        if section == "authors":
            counts = {}
            for b in all_books:
                for a in b.get("authors", []):
                    counts[a] = counts.get(a, 0) + 1
            rows = [{"id": a, "title": a, "section": "authors", "count": n,
                     "booksHref": f"{catalog_base}/books?author={a}&sort=title"} for a, n in sorted(counts.items())]
        elif section == "series":
            series = {}
            for b in all_books:
                if b.get("seriesId"):
                    series.setdefault(b["seriesId"], [b["seriesName"], 0])[1] += 1
            rows = [{"id": f"series:{sid}", "title": name, "section": "series", "count": n, "seriesId": sid,
                     "booksHref": f"{catalog_base}/books?seriesId={sid}&sort=series"}
                    for sid, (name, n) in sorted(series.items(), key=lambda kv: kv[1][0])]
        elif section in ("libraries", "collections"):
            key = "libraries" if section == "libraries" else "collections"
            rows = [{"id": str(e["id"]), "title": e["name"], "section": section,
                     "booksHref": f"{catalog_base}/books?{'libraryId' if key == 'libraries' else 'collectionId'}={e['id']}"}
                    for e in catalog.get(key, [])]
            return finish(200, {"section": section, "items": rows})
        elif section == "smart-scopes":
            return finish(200, {"section": section, "items": []})
        else:
            return bad_request("Unknown catalog section")
        chunk = rows[(page - 1) * 60:page * 60]  # AUTHOR_SERIES_PAGE_SIZE; `size` is ignored
        has_next = page * 60 < len(rows)
        return finish(200, {"section": section, "items": chunk, "page": page, "hasNext": has_next,
                            "hasPrevious": page > 1, "nextUrl": None, "previousUrl": None, "query": None})
    if method == "GET" and route == "/plugin/catalog/books":
        sort = (query.get("sort") or ["recently_added"])[0]
        if sort not in ("title", "author", "recently_added", "recently_updated", "recently_read", "series"):
            return bad_request("sort must be one of the following values")
        status = (query.get("readStatus") or [""])[0]
        if status not in ("", "unread", "reading", "finished"):
            return bad_request("readStatus must be one of the following values")
        filters = {}
        for name in ("seriesId", "libraryId", "collectionId"):
            value, invalid = positive_int(name)
            if invalid:
                return bad_request(f"{name} must be an integer number")
            filters[name] = value
        selected = list(catalog.get("books", []))
        q = (query.get("q") or [""])[0].strip().lower()
        if q:
            selected = [b for b in selected if q in b["title"].lower() or q in (b.get("seriesName") or "").lower()
                        or any(q in a.lower() for a in b.get("authors", []))]
        author = (query.get("author") or [""])[0]
        if author:
            selected = [b for b in selected if author in b.get("authors", [])]
        if filters["seriesId"]:
            selected = [b for b in selected if b.get("seriesId") == filters["seriesId"]]
        if filters["libraryId"]:
            selected = [b for b in selected if b.get("libraryId") == filters["libraryId"]]
        if filters["collectionId"]:
            selected = [b for b in selected if filters["collectionId"] in b.get("collectionIds", [])]
        if status == "unread":
            selected = [b for b in selected if b.get("readStatus") not in reading_bucket + finished_bucket]
        elif status == "reading":
            selected = [b for b in selected if b.get("readStatus") in reading_bucket]
        elif status == "finished":
            selected = [b for b in selected if b.get("readStatus") in finished_bucket]
        keys = {
            "title": (lambda b: b["title"], False),
            "author": (lambda b: (b.get("authors") or [""])[0], False),
            "series": (lambda b: (b.get("seriesName") or "￿", b.get("seriesIndex") or 0), False),
            "recently_added": (lambda b: b.get("addedAt", ""), True),
            "recently_updated": (lambda b: b.get("addedAt", ""), True),
            "recently_read": (lambda b: b.get("lastReadAt") or "", True),
        }
        key, descending = keys[sort]
        selected.sort(key=key, reverse=descending)
        items = [list_item(b) for b in selected]
        page = int((query.get("page") or ["1"])[0])
        size = int((query.get("size") or ["20"])[0])
        chunk = items[(page - 1) * size:page * size]
        return finish(200, {"items": chunk, "total": len(items), "page": page, "size": size,
                            "hasNext": page * size < len(items), "hasPrevious": page > 1,
                            "nextUrl": None, "previousUrl": None})
    if method == "GET" and route.startswith("/plugin/catalog/books/") and route.count("/") == 4:
        b = books.get(route.rsplit("/", 1)[1])
        if not b:
            return finish(404, {"error": "no such book"})
        detail = list_item(b)
        detail.update({"subtitle": None, "description": b.get("description"), "publisher": None,
                       "publishedDate": None, "publishedYear": None, "language": "zh-TW", "isbn10": None,
                       "isbn13": None, "libraryId": b.get("libraryId", 1), "libraryName": "Fixture",
                       "rating": state.get("ratings", {}).get(str(b["id"])), "pageCount": None,
                       "collections": [], "genres": [], "tags": [],
                       "progress": None,
                       "files": [file_entry(f) for f in b.get("files", [])],
                       "relatedSections": [{"id": "author", "title": "More by this author", "books": [
                           {"id": o["id"], "title": o["title"], "authors": o.get("authors", []), "seriesIndex": None,
                            "hasCover": True, "thumbnailUrl": f"{catalog_base}/books/{o['id']}/thumbnail",
                            "detailUrl": f"{catalog_base}/books/{o['id']}", "updatedAt": None}
                           for o in catalog.get("books", [])[:8] if o["id"] != b["id"]]}]})
        state_status = state.get("statuses", {}).get(str(b["id"]))
        if state_status:
            detail["readStatus"] = state_status
        return finish(200, detail)
    if method == "PUT" and route.startswith("/plugin/catalog/books/") and route.endswith("/read-status"):
        doc = json.loads(body or "{}")
        if doc.get("status") not in ("unread", "want_to_read", "reading", "on_hold", "read", "abandoned"):
            return bad_request("status must be one of the following values")
        state.setdefault("statuses", {})[route.split("/")[-2]] = doc["status"]
        return finish(200, {"readStatus": doc["status"]})
    if method == "PUT" and route.startswith("/plugin/catalog/books/") and route.endswith("/rating"):
        doc = json.loads(body or "{}")
        rating = doc.get("rating")
        state.setdefault("ratings", {})[route.split("/")[-2]] = rating
        return finish(200, {"rating": rating})
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
