#!/usr/bin/env python3
"""
BookOrbit library browsing screens on [env:simulator_x3_zh_tw], against the FIXTURE bridge.

    python3 scripts/sim_bookorbit_library_smoke.py                 # build, run, verify
    python3 scripts/sim_bookorbit_library_smoke.py --no-build
    python3 scripts/sim_bookorbit_library_smoke.py --no-build --program /path/to/program \\
        --flows before --keep /tmp/shots                            # baseline capture only

Drives the production library through scripted X3 button presses (no touch) and captures the
root, book list, sort/filter, search, detail, empty, error and loading screens. Every byte comes
from a synthetic fixture (invented Traditional Chinese / mixed-script titles, no real library or
account) served by scripts/sim_bookorbit_fixture_bridge.py, which never opens a socket.

`--flows after` (the default) also asserts navigation/request behaviour from the fixture request
log: which catalog routes and query parameters each step sent, that sort/filter survive detail and
Back, and that no request goes anywhere but the fixture's BookOrbit routes.
"""

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from urllib.parse import parse_qs

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sim_bookorbit_fixture_epub as fixture_epub  # noqa: E402
import sim_bookorbit_home_smoke as home  # noqa: E402
import sim_smoke  # noqa: E402
from sim_smoke import EXPECTED_HEIGHT, EXPECTED_WIDTH, REPO_ROOT, bmp_size, launch  # noqa: E402

PIOENV = home.PIOENV
STEP = 1100
# The real server's root, titles included (English): the device must localize known sections.
ROOT = [
    {"id": "continue-reading", "section": "continue-reading", "title": "Continue reading"},
    {"id": "recent", "section": "recent", "title": "Recently added"},
    {"id": "libraries", "section": "libraries", "title": "Libraries"},
    {"id": "collections", "section": "collections", "title": "Collections"},
    {"id": "smart-scopes", "section": "smart-scopes", "title": "SmartScopes"},
    {"id": "authors", "section": "authors", "title": "Authors"},
    {"id": "series", "section": "series", "title": "Series"},
    {"id": "all-books", "section": "all-books", "title": "All Books"},
]

# Invented catalog: long/mixed CJK titles, multi-author, series, every read-status bucket.
AUTHORS = ["林曉雲", "陳雨橋", "Haruki Tanabe", "王小波羅", "Elena Ferrante-Wu", "周夢蝶影", "李安靜", "Mara Lindqvist"]
SERIES = {7: "星光圖書館三部曲", 8: "The Northern Lights Archive", 9: "臺北霧雨偵探事件簿"}
STATUSES = [None, "reading", "read", "on_hold", "want_to_read", None, "abandoned", "reading", None, "skimmed"]
TITLES = [
    "長夜將盡的星光圖書館：關於遠方城市與一位守夜人的漫長故事",
    "霧雨中的臺北：一本關於巷弄、咖啡與遺失信件的城市散步誌（增訂新版）",
    "The Quiet Archive of Small Things 微小事物的安靜檔案",
    "山海之間",
    "給未來的你：二十封寫在旅途中的長信與一張沒寄出的明信片",
    "Kafka on the Night Train 夜車上的卡夫卡",
    "雨夜書店",
    "海風、燈塔與那年夏天我們沒有說出口的話",
    "紙上的潮汐",
    "A Field Guide to Lost Libraries 失落圖書館野外指南",
    "第七號月台的貓",
    "冬日練習曲：鋼琴、舊公寓與一個人的十二個月",
    "島嶼書信集",
    "Northern Lights, Southern Rain 北方極光與南方的雨",
    "記憶的博物館",
    "風起之前",
]


def write_fixture(root: Path) -> Path:
    fx = root / "fixture"
    fx.mkdir()
    fixture_epub.write(fx / "book.epub")
    books = []
    for i in range(46):
        book_id = 201 + i
        title = TITLES[i] if i < len(TITLES) else f"{TITLES[i % len(TITLES)]}（第{i // len(TITLES) + 1}部）"
        series_id = 7 + (i % 3) if i % 2 == 0 else None
        status = STATUSES[i % len(STATUSES)]
        progress = None
        if status in ("reading", "on_hold"):
            progress = float((i * 17) % 97 + 2)
        elif status in ("read", "skimmed"):
            progress = 100.0
        authors = [AUTHORS[i % len(AUTHORS)]] + ([AUTHORS[(i + 3) % len(AUTHORS)]] if i % 5 == 0 else [])
        books.append({
            "id": book_id, "title": title, "authors": authors,
            "seriesId": series_id, "seriesName": SERIES.get(series_id), "seriesIndex": (i // 2) + 1 if series_id else None,
            "progressPercentage": progress, "readStatus": status,
            "lastReadAt": f"2026-10-{(i % 28) + 1:02d}T08:00:00.000Z" if progress else None,
            "addedAt": f"2026-{(i % 9) + 1:02d}-{(i % 27) + 1:02d}T00:00:00.000Z",
            "libraryId": 1 if i % 4 else 2, "collectionIds": [3] if i % 6 == 0 else [],
            # Realistic server descriptions run to kilobytes and precede `files` in the detail body.
            "description": ("夜色漸深，圖書館的燈一盞一盞亮起。守夜人推著書車經過長長的走廊。" * 40),
            "files": [{"id": 3000 + book_id, "format": "epub", "file": "book.epub"}],
        })
    catalog = {"root": ROOT, "books": books,
               "libraries": [{"id": 1, "name": "家中書房"}, {"id": 2, "name": "Work Shelf"}],
               "collections": [{"id": 3, "name": "今年想讀"}, {"id": 4, "name": "空的收藏"}]}
    (fx / "catalog.json").write_text(json.dumps(catalog, ensure_ascii=False))
    return fx


class Flow:
    """Button presses with a screenshot taken just before each next press."""

    def __init__(self, start: int = 1500):
        self.t = start
        self.items: "list[str]" = []
        self.shots: "list[tuple[int, str, str]]" = []

    def press(self, key: str, wait: int = STEP) -> "Flow":
        self.items.append(f"{self.t}:{key}")
        self.t += wait
        return self

    def hold(self, key: str, ms: int, wait: int = STEP) -> "Flow":
        self.items.append(f"{self.t}:{key}:{ms}")
        self.t += ms + wait
        return self

    def wait(self, ms: int) -> "Flow":
        self.t += ms
        return self

    def shot(self, name: str, desc: str) -> "Flow":
        self.shots.append((self.t - 150, name, desc))
        return self

    def script(self) -> str:
        return ";".join(self.items + [f"{self.t + 1500}:QUIT"])


def make_sd(work: Path, journal: bool = True) -> None:
    """home.make_sd, but by default lets the zh-TW profile's own one-time migration select the
    Journal theme -- the X3 zh-TW product default -- instead of pinning the older BookOrbit theme."""
    home.make_sd(work)
    settings = work / "fs_" / ".crosspoint" / "crossfade-settings.json"
    data = json.loads(settings.read_text())
    if journal:
        data.pop("zhTwJournalProfileApplied", None)
    settings.write_text(json.dumps(data))


def run_flow(args, name, fx, state, flog, flow, failures, faults=None, make=None):
    work = Path(args.keep) / name
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    (make or make_sd)(work)
    if state.exists():
        state.unlink()
    if faults:
        home.set_faults(state, faults)
    env = home.fixture_env(fx, state, flog)
    env["CROSSPOINT_SIM_INPUT_SCRIPT"] = flow.script()
    env["CROSSPOINT_SIM_SCREENSHOTS"] = ";".join(f"{ms}:./shots/{f}" for ms, f, _ in flow.shots)
    before = len(home.requests(flog))
    result = launch(work, args.video_driver, args.timeout, env, label=f"library:{name}")
    log = (result.stdout or "") + (result.stderr or "")
    (work / "sim.log").write_text(log)
    if result.returncode != 0:
        failures.append(f"{name}: simulator exited {result.returncode}")
    if "No glyph for codepoint" in log:
        failures.append(f"{name}: a UI string rendered without glyphs (tofu)")
    shots = []
    for _, f, desc in flow.shots:
        path = work / "shots" / f
        size = bmp_size(path) if path.exists() else None
        if size != (EXPECTED_WIDTH, EXPECTED_HEIGHT):
            failures.append(f"{name}: screenshot {f} missing or wrong size ({size})")
        shots.append((path, desc))
    return log, home.requests(flog)[before:], shots


def catalog_requests(reqs):
    out = []
    for r in reqs:
        route = r["path"].rsplit("/koreader", 1)[-1]
        if route.startswith("/plugin/catalog"):
            out.append((r["method"], route, {k: v[0] for k, v in parse_qs(r.get("query", "")).items()}, r["status"]))
    return out


def before_flows():
    """Baseline UI (main @ 9f78899): root rows are 已下載, the 8 server rows in server order, 搜尋."""
    flows = {}
    f = Flow().press("ENTER").wait(1500).shot("before-01-root.bmp", "root: server English titles, 已下載 first")
    for _ in range(8):
        f.press("DOWN", 450)
    f.press("ENTER").wait(1000).shot("before-02-all-books.bmp", "All Books page 1 (title sort)")
    for _ in range(8):
        f.press("DOWN", 450)
    f.wait(600).shot("before-03-all-books-screen2.bmp", "second screen of the same 20-item server page")
    for _ in range(12):
        f.press("DOWN", 450)
    f.wait(600).shot("before-04-next-page-row.bmp", "end of server page: 下一頁 is a row inside the list")
    f.press("ENTER").wait(1200).shot("before-05-page2.bmp", "page 2 via the list row")
    flows["browse"] = f

    f = Flow().press("ENTER").wait(1500)
    for _ in range(7):
        f.press("DOWN", 450)
    f.press("ENTER").wait(1500).shot("before-06-series-facets.bmp", "Series facets")
    f.press("ENTER").wait(1500).shot("before-07-series-books.bmp", "Series drill-in against the server contract")
    flows["series"] = f

    f = Flow().press("ENTER").wait(1500)
    for _ in range(8):
        f.press("DOWN", 450)
    f.press("ENTER").wait(1200).press("ENTER").wait(1500).shot("before-08-detail.bmp", "book detail (realistic body)")
    f.press("BACK").wait(1000).press("BACK").wait(1200).press("UP", 450).press("ENTER").wait(1500).shot(
        "before-09-search.bmp", "search keyboard (root 搜尋 row)")
    flows["detail"] = f

    f = Flow().press("ENTER").wait(1500)
    for _ in range(6):
        f.press("DOWN", 450)
    f.press("ENTER").wait(1500).shot("before-10-authors.bmp", "Authors facets (server pages 60)")
    f.press("BACK").wait(1200)
    for _ in range(2):
        f.press("UP", 450)
    f.press("ENTER").wait(1200).press("DOWN", 450).press("ENTER").wait(1500).shot(
        "before-11-empty.bmp", "an empty collection")
    flows["empty"] = f

    flows["offline"] = Flow().press("ENTER").wait(2500).shot("before-12-offline.bmp", "offline root (transport failure)")
    flows["loading"] = (Flow().press("ENTER").wait(900).shot("before-13-loading.bmp", "loading (1.5 s server latency)")
                        .wait(2500))
    return flows


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--program", help="simulator binary (default: this checkout's build)")
    ap.add_argument("--flows", choices=["before", "after"], default="after")
    ap.add_argument("--video-driver", default="dummy")
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--only", help="comma-separated after-flow names, e.g. search,download")
    ap.add_argument("--keep", help="artifact directory (default: a new temp dir, kept)")
    args = ap.parse_args()
    home.zh_tw_fonts()
    if args.program:
        sim_smoke.PROGRAM = Path(args.program)
    elif not args.no_build:
        pio = sim_smoke.find_pio()
        cmd = [pio, "run", "-e", PIOENV] if pio else [sys.executable, "-m", "platformio", "run", "-e", PIOENV]
        if subprocess.run(cmd, cwd=REPO_ROOT).returncode != 0:
            print(f"[library] FAIL: build of {PIOENV} failed")
            return 1
    if not Path(sim_smoke.PROGRAM).exists():
        print(f"[library] FAIL: no simulator binary at {sim_smoke.PROGRAM}")
        return 1
    args.keep = args.keep or tempfile.mkdtemp(prefix="crossfade-sim-library-")
    root = Path(args.keep)
    root.mkdir(parents=True, exist_ok=True)
    print(f"[library] artifacts: {root}")
    fx_root = root / "_fixture"
    if fx_root.exists():
        shutil.rmtree(fx_root)
    fx_root.mkdir()
    fx = write_fixture(fx_root)
    state, flog = fx_root / "state.json", fx_root / "requests.jsonl"
    failures: "list[str]" = []
    all_shots = []

    if args.flows == "before":
        faults = {"offline": {"offline": True}, "loading": {"delay_ms": 1500}}
        for name, flow in before_flows().items():
            _, reqs, shots = run_flow(args, name, fx, state, flog, flow, failures, faults.get(name))
            all_shots += shots
            for method, route, q, status in catalog_requests(reqs):
                print(f"  [{name}] {method} {route} {q} -> {status}")
    else:
        all_shots += after_suite(args, fx, state, flog, failures)

    for path, desc in all_shots:
        print(f"  {'ok' if path.exists() else 'MISSING':8} {path}  {desc}")
    if failures:
        print("\n[library] FAIL:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("\n[library] PASS")
    return 0


def after_suite(args, fx, state, flog, failures):
    shots = []
    active = True

    def need(_failures, cond, msg):
        if active:
            home.need(failures, cond, msg)

    def down(flow, n):
        for _ in range(n):
            flow.press("DOWN", 350)
        return flow

    def all_books():
        return down(Flow().press("ENTER").wait(1200), 3).press("ENTER").wait(800)

    def run(name, flow, faults=None, fixture=None):
        nonlocal active
        active = not args.only or name in args.only.split(",")
        if not active:
            return "", []
        log, reqs, images = run_flow(args, name, fixture or fx, state, flog, flow, failures, faults)
        shots.extend(images)
        routes = catalog_requests(reqs)
        for method, route, query, status in routes:
            print(f"  [{name}] {method} {route} {query} -> {status}")
        need(failures, all(r["path"].startswith("/api/v1/koreader/") for r in reqs),
                  f"{name}: request outside BookOrbit")
        return log, routes

    def book_gets(routes):
        return [(query, status) for method, route, query, status in routes
                if method == "GET" and route == "/plugin/catalog/books"]

    # Root, long mixed-script rows, automatic paging, and Back = parent (not previous page).
    f = Flow().press("ENTER").wait(1200).shot("after-01-root.bmp", "localized root with visible search and local books")
    down(f, 3).press("ENTER").wait(800).shot("after-02-books.bmp", "title / author / status, list context and position")
    down(f, 21).wait(800).shot("after-03-page2.bmp", "page 2 reached by crossing the list edge")
    f.press("UP").wait(800).shot("after-04-page1-tail.bmp", "previous page restores its last row")
    f.press("BACK").wait(800).shot("after-05-back-root.bmp", "Back returns to the root with All books selected")
    f.press("ENTER").wait(800)
    _, routes = run("browse", f)
    need(failures, [q.get("page") for q, status in book_gets(routes)] == ["1", "2", "1", "1"],
              "browse: page traversal or parent Back changed")

    # Select sort and filter explicitly, make one status write, then return to the same query.
    f = all_books().press("ENTER").shot("after-06-options.bmp", "explicit sort / status choices with current markers")
    down(f, 1).press("ENTER").wait(700).shot("after-07-author-sort.bmp", "author sort with long CJK and mixed titles")
    f.press("ENTER")
    down(f, 8).press("ENTER").wait(700).shot("after-08-filter.bmp", "reading filter retained on the list")
    f.press("DOWN").press("ENTER").wait(700).shot("after-09-detail.bmp", "detail with title, series, status, EPUB size and actions")
    f.press("DOWN").press("ENTER").shot("after-10-status-choices.bmp", "one explicit status selection, no cycling writes")
    # Current book is reading / on_hold; either way the first UP presses clamp at Unread.
    for _ in range(5): f.press("UP", 300)
    down(f, 4).press("ENTER").wait(700).press("BACK").wait(700).shot("after-11-edited-filter.bmp", "edited book removed from reading filter")
    _, routes = run("choices", f)
    gets = book_gets(routes)
    need(failures, bool(gets) and gets[-1][0].get("sort") == "author" and gets[-1][0].get("readStatus") == "reading",
              "choices: detail Back lost sort or filter")
    writes = [r for r in routes if r[0] == "PUT"]
    need(failures, len(writes) == 1 and writes[0][1].endswith("/read-status") and writes[0][3] == 200,
              "choices: status choice must issue exactly one successful write")
    need(failures, list((json.loads(state.read_text()) if state.exists() else {}).get("statuses", {}).values()) == ["read"],
              "choices: selected Finished status did not persist")

    # The status PUT succeeds, but the refresh GET fails and reloads the pre-edit cached page.
    # Reopen the selected row to prove that the edited book is no longer in the Reading filter.
    f = all_books().press("ENTER")
    down(f, 1).press("ENTER").press("ENTER")
    down(f, 8).press("ENTER").press("DOWN").press("ENTER").wait(700)
    f.press("DOWN").press("ENTER")
    for _ in range(5): f.press("UP", 300)
    down(f, 4).press("ENTER").wait(700).press("BACK").wait(700)
    f.shot("after-31-edited-offline-filter.bmp", "confirmed Finished edit removed from cached Reading filter")
    f.press("ENTER").wait(700).shot("after-32-offline-remaining-detail.bmp", "remaining Reading book is selectable after cache reconciliation")
    _, routes = run("status-refresh-offline", f, {"books_offline_after_status": True})
    gets = book_gets(routes)
    need(failures, bool(gets) and gets[-1][0].get("sort") == "author" and
         gets[-1][0].get("readStatus") == "reading" and gets[-1][1] == 0,
         "status-refresh-offline: refresh must fail with the original sort and Reading filter")
    writes = [r for r in routes if r[0] == "PUT" and r[1].endswith("/read-status")]
    details = [r for r in routes if r[0] == "GET" and r[1].startswith("/plugin/catalog/books/")]
    need(failures, len(writes) == 1 and writes[0][3] == 200 and
         len(details) == 2 and all(r[3] == 200 for r in details) and details[0][1] != details[1][1],
         "status-refresh-offline: cached list retained the edited book under Reading")
    need(failures, list((json.loads(state.read_text()) if state.exists() else {}).get("statuses", {}).values()) == ["read"],
         "status-refresh-offline: confirmed Finished status was lost")

    # A request failure at the page edge keeps page 1, its selection and Back stack intact.
    f = all_books()
    down(f, 21).wait(700).shot("after-12-page-error.bmp", "page request failed: loaded rows and selection remain")
    f.press("UP", 350).press("DOWN", 350).press("DOWN").wait(700).shot("after-13-page-retry.bmp", "moving across the same edge retries page 2")
    _, routes = run("page-failure", f, {"fail_page_once": 2})
    need(failures, [(q.get("page"), status) for q, status in book_gets(routes)] ==
              [("1", 200), ("2", 0), ("2", 200)], "page-failure: failed paging lost position or could not retry")

    f = down(Flow().press("ENTER").wait(1200), 6).press("ENTER").wait(700).shot("after-14-series.bmp", "series facets with counts")
    f.press("ENTER").wait(700).shot("after-15-series-books.bmp", "series breadcrumb and filtered books")
    _, routes = run("series", f)
    need(failures, any(q.get("seriesId", "").isdigit() and status == 200 for q, status in book_gets(routes)),
              "series: drill-in must use the server numeric seriesId")

    f = down(Flow().press("ENTER").wait(1200), 8).press("ENTER").wait(700).shot("after-16-collections.bmp", "countless facets do not claim 0 books")
    f.press("DOWN").press("ENTER").wait(700).shot("after-17-empty.bmp", "contextual empty list keeps options available")
    run("empty", f)

    f = all_books().press("DOWN").press("ENTER").wait(700).shot("after-27-detail-error.bmp", "detail request rejection with Retry")
    f.press("ENTER").wait(700).shot("after-28-detail-retry.bmp", "Retry requests the same book detail")
    f.press("BACK").wait(700)
    _, routes = run("detail-failure", f, {"fail_detail_once": 400})
    details = [r for r in routes if r[0] == "GET" and r[1].startswith("/plugin/catalog/books/")]
    need(failures, len(details) == 2 and [r[3] for r in details] == [400, 200] and details[0][1] == details[1][1],
         "detail-failure: Retry must repeat the same detail request")

    many = fx.parent / "many-authors"
    many.mkdir(exist_ok=True)
    shutil.copy2(fx / "book.epub", many / "book.epub")
    catalog = json.loads((fx / "catalog.json").read_text())
    catalog["books"] = [{"id": 701 + i, "title": f"Fixture book {i:03d}", "authors": [f"Author {i:03d}"],
                         "files": [{"id": 9000 + i, "format": "epub", "file": "book.epub"}]} for i in range(61)]
    (many / "catalog.json").write_text(json.dumps(catalog))
    f = down(Flow().press("ENTER").wait(1200), 5).press("ENTER").wait(700)
    down(f, 60).wait(700).press("ENTER").wait(700).shot("after-29-drill-error.bmp", "failed drill-in from facet page 2")
    f.press("ENTER").wait(700).shot("after-30-drill-retry.bmp", "Retry loads the requested first book page")
    _, routes = run("drill-failure", f, {"fail_once": "/plugin/catalog/books"}, many)
    need(failures, [(q.get("page"), status) for q, status in book_gets(routes)] == [("1", 0), ("1", 200)],
         "drill-failure: Retry inherited the parent facet page")

    # QWERTY, physical buttons only: type 1, then bottom-row OK. No Chinese input is implied.
    f = Flow().press("ENTER").wait(1200).press("ENTER").wait(700).shot("after-18-search-keyboard.bmp", "search via the visible root row")
    f.press("ENTER")
    down(f, 4)
    for _ in range(2): f.press("RIGHT", 350)
    f.press("ENTER").wait(700).shot("after-19-search-results.bmp", "search results retain the query and an empty-state explanation")
    _, routes = run("search", f)
    need(failures, any(q.get("q") == "1" and status == 200 for q, status in book_gets(routes)),
              "search: keyboard submit did not issue the expected search")

    f = all_books().press("ENTER")
    down(f, 1).press("ENTER").press("ENTER")
    down(f, 8).press("ENTER").press("ENTER")
    down(f, 10).press("ENTER").wait(500).press("ENTER")
    down(f, 4)
    for _ in range(2): f.press("RIGHT", 350)
    f.press("ENTER").wait(700).shot("after-25-scoped-search.bmp", "search preserves author sort and reading filter")
    f.press("BACK").wait(700).shot("after-26-search-back.bmp", "Back restores the parent query and selection")
    _, routes = run("scoped-search", f)
    gets = book_gets(routes)
    need(failures, any(q.get("q") == "1" and q.get("sort") == "author" and
                       q.get("readStatus") == "reading" for q, status in gets), "scoped-search: lost list filters")
    need(failures, bool(gets) and gets[-1][0].get("sort") == "author" and
         gets[-1][0].get("readStatus") == "reading" and "q" not in gets[-1][0], "scoped-search: Back lost parent query")

    # Download/index/local-row state, then explicit rating (one write) and Back restoration.
    f = all_books().press("DOWN").press("ENTER").wait(700).press("ENTER").wait(1800).shot("after-20-downloaded.bmp", "download complete, Open book focused")
    down(f, 3).press("ENTER")
    down(f, 5).press("ENTER").wait(700).press("BACK").wait(700).shot("after-21-local-row.bmp", "downloaded marker after returning to the list")
    _, routes = run("download", f)
    need(failures, any(r[1].endswith("/download") and r[3] == 200 for r in routes), "download: no successful file transfer")
    need(failures, len([r for r in routes if r[0] == "PUT" and r[1].endswith("/rating")]) == 1,
              "download: rating selection did not issue exactly one write")

    run("offline", Flow().press("ENTER").wait(1400).shot("after-22-offline.bmp", "offline library retains local entry"), {"offline": True})
    run("error", Flow().press("ENTER").wait(1400).shot("after-23-error.bmp", "localized server rejection with Retry"), {"catalog_status": 400})
    f = down(Flow().press("ENTER").wait(4000), 3).press("ENTER", 350).shot("after-24-loading.bmp", "catalog loading retains library context").wait(2200)
    run("loading", f, {"delay_ms": 1500})
    return shots


if __name__ == "__main__":
    sys.exit(main())
