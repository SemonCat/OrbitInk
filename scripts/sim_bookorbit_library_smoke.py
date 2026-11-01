#!/usr/bin/env python3
"""
BookOrbit library browsing screens on [env:simulator_x3_zh_tw], against the FIXTURE bridge.

    python3 scripts/sim_bookorbit_library_smoke.py                 # build, run, verify
    python3 scripts/sim_bookorbit_library_smoke.py --no-build
    python3 scripts/sim_bookorbit_library_smoke.py --no-build --program /path/to/program \\
        --flows before --keep /tmp/shots                            # baseline capture only

Drives the production library through scripted X3 button presses (no touch) and captures the
cover/title/Reading books, tools, sort/filter, search, detail, empty, error and offline screens. Every byte comes
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
    from PIL import Image, ImageDraw
    import hashlib
    import zipfile
    from sim_bookorbit_library_geometry import assert_composition
    shots = []
    geometry = []
    active = True

    def need(cond, msg):
        if active:
            home.need(failures, cond, msg)

    def steps(f, key, n):
        for _ in range(n):
            f.press(key, 350)
        return f

    def enter():
        return Flow().press('ENTER').wait(1400)

    def tool(f, index):
        return steps(f.press('BACK'), 'DOWN', index).press('ENTER').wait(500)

    def tab(f, index):
        # First book -> tools rail -> last tab -> chosen tab; Confirm then back to books.
        steps(f, 'LEFT', 5-index).press('ENTER').wait(650)
        return steps(f, 'RIGHT', 5-index)

    def sort_author(f):
        return tool(f, 2).press('DOWN').press('ENTER').wait(650)

    def finish_book(f):
        f.press('ENTER').wait(600).press('DOWN').press('ENTER')
        steps(f, 'UP', 5)
        return steps(f, 'DOWN', 4).press('ENTER').wait(600).press('BACK').wait(650)

    def gets(routes):
        return [(q, status) for method, path, q, status in routes if method == 'GET' and path == '/plugin/catalog/books']

    def details(routes):
        return [r for r in routes if r[0] == 'GET' and r[1].startswith('/plugin/catalog/books/') and r[1].count('/') == 4]

    def run(name, f, faults=None, fixture=None, make=None):
        nonlocal active
        active = not args.only or name in args.only.split(',')
        if not active:
            return '', []
        log, reqs, images = run_flow(args, name, fixture or fx, state, flog, f, failures, faults, make)
        shots.extend(images)
        routes = catalog_requests(reqs)
        for method, path, q, status in routes:
            print(f'  [{name}] {method} {path} {q} -> {status}')
        need(all(r['path'].startswith('/api/v1/koreader/') for r in reqs), name + ': escaped fixture origin')
        need('Outside range' not in log, name + ': renderer wrote beyond panel')
        for path, _ in images:
            if path.exists():
                Image.open(path).save(path.with_suffix('.png'))
        return log, routes

    def check(name, filename, mode, count=6, selected=0):
        if active:
            path = Path(args.keep)/name/'shots'/filename
            try:
                geometry.append(assert_composition(path, mode, count, selected))
            except (AssertionError, OSError) as e:
                failures.append(f'{name}: {e}')

    # Fixtures are synthetic, but travel through the real authenticated endpoint, decoder,
    # atomic cache and renderer. Cover imagery is deliberately independent of product code.
    visual = fx.parent/'visual'
    visual.mkdir(exist_ok=True)
    shutil.copy2(fx/'book.epub', visual/'book.epub')
    names = ['小王子', '深度工作力', '原子習慣', '人類大歷史：從野獸到扮演上帝', '解憂雜貨店', '被討厭的勇氣']
    authors = ['聖修伯里', '卡爾紐波特', '詹姆斯克利爾', '哈拉瑞', '東野圭吾', '岸見一郎、古賀史健']
    visual_books = []
    for i in range(7):
        im = Image.new('L', (240, 360), 255)
        d = ImageDraw.Draw(im)
        d.rectangle((1, 1, 238, 358), outline=0, width=4)
        d.line((16, 1, 16, 358), fill=0, width=2)
        for j in range(i % 3 + 1):
            d.ellipse((35+j*20, 180-j*35, 200-j*15, 320-j*35), outline=0, width=5)
        for j in range(3):
            d.line((35, 40+j*24, 205-j*20, 40+j*24), fill=0, width=7)
        cover = f'cover{i}.jpg' if i % 2 else f'cover{i}.png'
        im.save(visual/cover)
        visual_books.append({'id': i+1, 'title': names[i] if i < 6 else '龜的第七本書', 'authors': [authors[i] if i < 6 else '作者'],
                             'readStatus': 'reading' if i in (0, 4) else 'read' if i == 2 else 'unread',
                             'progressPercentage': 32 if i == 0 else 18 if i == 4 else None,
                             'cover': cover, 'files': [{'id': 8000+i, 'format':'epub', 'file':'book.epub'}]})
    catalog = {'root': ROOT, 'books': visual_books[:6]}
    (visual/'catalog.json').write_text(json.dumps(catalog, ensure_ascii=False))

    f = enter().shot('01-cover-library.bmp', 'A: actual firmware, six real endpoint thumbnails')
    tool(f, 0).shot('02-title-library.bmp', 'B: actual firmware, title first and compact thumbnails')
    tool(f, 0)
    tab(f, 1)
    f.press('ENTER').wait(500).press('ENTER').wait(800).press('BACK').wait(500)
    f.press('RIGHT').press('ENTER').wait(500).press('ENTER').wait(800).press('BACK').wait(500).press('LEFT')
    f.shot('03-reading.bmp', 'C: actual firmware, two downloaded Reading cards with progress and offline availability')
    f.press('LEFT').shot('04-tools-focus.bmp', 'explicit tools rail focus')
    f.press('LEFT').shot('05-tab-focus.bmp', 'explicit fixed tab focus')
    f.press('BACK').shot('06-tools.bmp', 'reachable tools and Exit')
    _, routes = run('visual', f, fixture=visual)
    check('visual', '01-cover-library.bmp', 'covers')
    check('visual', '02-title-library.bmp', 'titles')
    check('visual', '03-reading.bmp', 'reading', 2)
    need(len([r for r in routes if r[1].endswith('/thumbnail')]) == 6, 'visual: cover/title/filter change re-fetched cached images')
    if active:
        cache = list((Path(args.keep)/'visual/fs_/.crosspoint/bookorbit').glob('lib_*/covers/*.bmp'))
        need(len(cache) == 6 and all(p.stat().st_size <= 3000 for p in cache), 'visual: bounded decoded cover cache missing')

    # 0 / 1 / 6 / 7 cardinalities; side key must reach the seventh, never manufacture empty slots.
    for cardinality in (0, 1, 6, 7):
        variant = fx.parent/f'cardinality-{cardinality}'
        shutil.copytree(visual, variant, dirs_exist_ok=True)
        (variant/'catalog.json').write_text(json.dumps({'root':ROOT, 'books':visual_books[:cardinality]}, ensure_ascii=False))
        f = enter().shot('first.bmp', f'{cardinality} books, first screen')
        if cardinality == 7:
            f.press('DOWN').wait(650).shot('second.bmp','seventh book, side-key next page')
            f.press('ENTER').wait(600).shot('detail.bmp','seventh book opens')
            f.press('BACK').press('UP').wait(600).shot('previous.bmp','side-key previous page')
        _, routes = run(f'cardinality-{cardinality}', f, fixture=variant)
        check(f'cardinality-{cardinality}', 'first.bmp', 'covers', min(cardinality,6))
        if cardinality == 7:
            check('cardinality-7', 'second.bmp', 'covers', 1)
            need(any(r[1] == '/plugin/catalog/books/7' for r in details(routes)), 'seventh book inaccessible')

    # Long CJK/Latin titles, query preservation and selected-book preservation across view/detail.
    f = sort_author(enter()).press('RIGHT').press('RIGHT').press('ENTER').wait(600)
    f.press('BACK').wait(400)
    tool(f, 0).shot('long-titles.bmp','long unspaced CJK and Latin titles, two lines')
    f.press('ENTER').wait(600).press('BACK')
    _, routes = run('long-titles', f)
    need(len(details(routes)) == 2 and details(routes)[0][1] == details(routes)[1][1], 'view switch lost selected book')
    check('long-titles','long-titles.bmp','titles',selected=2)

    # Keep the exact prior PUT-success / GET-offline regression: remaining Reading book224.
    f = tab(sort_author(enter()), 1)
    finish_book(f).shot('edited-offline.bmp','confirmed Finished removed from stale cached Reading page')
    f.press('ENTER').wait(650).shot('remaining-detail.bmp','remaining selected detail must be book224')
    _, routes = run('status-refresh-offline', f, {'books_offline_after_status':True})
    need([r[1] for r in details(routes)] == ['/plugin/catalog/books/208','/plugin/catalog/books/224'], 'offline refresh did not retain remaining book224')
    need(len([r for r in routes if r[0]=='PUT' and r[1].endswith('/read-status') and r[3]==200]) == 1, 'status requires one PUT')
    need(bool(gets(routes)) and gets(routes)[-1][1] == 0 and gets(routes)[-1][0].get('sort') == 'author' and gets(routes)[-1][0].get('readStatus') == 'reading', 'refresh lost query or did not hit transport failure')

    # Side paging within and across 20-book server pages; failed next request never commits.
    f = steps(enter(),'DOWN',4).shot('page-failed.bmp','failed server page keeps last loaded screen')
    f.press('DOWN').wait(650).shot('page-retry.bmp','same edge retries page 2').press('UP').wait(650)
    _, routes = run('page-failure', f, {'fail_page_once':2})
    need([(q.get('page'),s) for q,s in gets(routes)] == [('1',200),('2',0),('2',200),('1',200)], 'page failure committed or lost retry')

    # Numeric series, all 60 facets and drill Retry must use page1, not parent page2.
    f = steps(tool(enter(),4),'DOWN',6).press('ENTER').wait(600).shot('series.bmp','series facets')
    f.press('ENTER').wait(600).shot('series-books.bmp','numeric series drill-in')
    _, routes = run('series', f)
    need(any(q.get('seriesId','').isdigit() and s==200 for q,s in gets(routes)), 'series id is not numeric')
    many = fx.parent/'many-authors'
    many.mkdir(exist_ok=True)
    shutil.copy2(fx/'book.epub', many/'book.epub')
    many_books = [{'id':701+i,'title':f'Fixture book {i:03d}','authors':[f'Author {i:03d} '+('關於星光圖書館的作者與漫長旅程'*2)],
                   'files':[{'id':9000+i,'format':'epub','file':'book.epub'}]} for i in range(61)]
    (many/'catalog.json').write_text(json.dumps({'root':ROOT,'books':many_books},ensure_ascii=False))
    f = steps(tool(enter(),4),'DOWN',5).press('ENTER').wait(600)
    steps(f,'DOWN',60).press('ENTER').wait(600).shot('drill-error.bmp','drill from facet page2 rejected')
    f.press('ENTER').wait(600).shot('drill-retry.bmp','Retry uses requested book page1')
    _, routes = run('drill-failure',f,{'drill_offline_once':True},many)
    need(any(r[1]=='/plugin/catalog/sections/authors' and r[2].get('page')=='2' for r in routes),'facet60 page2 unreachable')
    need([(q.get('page'),s) for q,s in gets(routes)] == [('1',200),('1',0),('1',200)], 'drill inherited parent page or did not Retry')

    f = enter().press('ENTER').wait(600).shot('detail-error.bmp','detail error with Retry')
    f.press('ENTER').wait(600).shot('detail-retry.bmp','same detail after Retry')
    _, routes = run('detail-failure',f,{'fail_detail_once':400})
    need([r[3] for r in details(routes)] == [400,200] and len({r[1] for r in details(routes)})==1,'detail Retry changed request')

    # Scoped search via physical keyboard, then explicit parent action restores filters.
    f = tool(tab(sort_author(enter()),1),1).shot('search-keyboard.bmp','physical keyboard')
    f.press('ENTER')
    steps(f,'DOWN',4)
    steps(f,'RIGHT',2).press('ENTER').wait(650).shot('search-results.bmp','scoped search')
    tool(f,5).shot('search-parent.bmp','explicit parent restores query')
    _, routes = run('scoped-search',f)
    need(any(q.get('q')=='1' and q.get('sort')=='author' and q.get('readStatus')=='reading' for q,s in gets(routes)),'scoped search lost filters')
    need(bool(gets(routes)) and 'q' not in gets(routes)[-1][0] and gets(routes)[-1][0].get('readStatus')=='reading','search parent lost scope')

    # Download detail is the confirmation; a second Confirm downloads. Rating is one write.
    f = enter().press('ENTER').wait(600).shot('download-confirm.bmp','detail confirms download and size')
    f.press('ENTER').wait(1000).shot('downloaded.bmp','Open local focused after download')
    steps(f,'DOWN',3).press('ENTER')
    steps(f,'DOWN',5).press('ENTER').wait(600).press('BACK').wait(600).shot('local-book.bmp','per-book local availability')
    _, routes = run('download',f)
    need(any(r[1].endswith('/download') and r[3]==200 for r in routes),'download did not complete')
    need(len([r for r in routes if r[0]=='PUT' and r[1].endswith('/rating')])==1,'rating did not issue exactly one write')

    # Missing/error/unsupported/oversized images must retain the six-book composition and access.
    for name, faults in [('missing',None),('cover-error',{'thumbnail_status':500})]:
        variant = visual
        if name == 'missing':
            variant = fx.parent/'missing';shutil.copytree(visual,variant,dirs_exist_ok=True)
            b = json.loads((variant/'catalog.json').read_text())
            for row in b['books']:row.pop('cover',None)
            (variant/'catalog.json').write_text(json.dumps(b,ensure_ascii=False))
        f = enter().shot('covers.bmp', name+' placeholders').press('ENTER').wait(600).shot('detail.bmp','book access independent of covers')
        _, routes = run(name,f,faults,variant)
        check(name,'covers.bmp','covers')
        need(len(details(routes))==1 and details(routes)[0][3]==200,name+': failed cover blocked detail')

    # Bad media stays optional, including valid but unsupported formats and dimension bombs.
    for name in ('invalid-cover', 'oversize-cover', 'body-limit', 'webp-cover', 'progressive-cover'):
        variant = fx.parent/name
        shutil.copytree(visual,variant,dirs_exist_ok=True)
        payload = variant/'media.bin'
        if name == 'invalid-cover': payload.write_bytes(b'<html>server error</html>'*4)
        elif name == 'oversize-cover': payload.write_bytes(fixture_epub.png_cover(513,769))
        elif name == 'body-limit': payload.write_bytes(b'x'*(96*1024+1))
        elif name == 'webp-cover': Image.new('RGB',(120,180),'gray').save(payload,format='WEBP')
        else: Image.new('RGB',(120,180),'gray').save(payload,format='JPEG',progressive=True)
        b = json.loads((variant/'catalog.json').read_text())
        for row in b['books']: row['cover']='media.bin'
        (variant/'catalog.json').write_text(json.dumps(b,ensure_ascii=False))
        _,routes=run(name,enter().wait(1200).shot('covers.bmp',name+' fallback').press('ENTER').wait(600),fixture=variant)
        check(name,'covers.bmp','covers')
        need(len(details(routes))==1 and details(routes)[0][3]==200,name+': blocked book access')
        if active:
            cache=Path(args.keep)/name/'fs_/.crosspoint/bookorbit'
            need(not list(cache.glob('lib_*/covers/*.bmp')),name+': invalid media promoted')
            need(not list(cache.glob('lib_*/covers/*.tmp')),name+': partial media left behind')

    f=tab(enter(),1)
    tab(f,2).shot('unread.bmp','fixed Unread tab')
    tab(f,3).shot('finished.bmp','fixed Finished tab')
    tab(f,0).shot('all.bmp','fixed All tab')
    _,routes=run('status-tabs',f)
    need([q.get('readStatus','') for q,s in gets(routes)]==['','reading','unread','finished',''],'fixed tabs did not send the exact status filters')

    # Downloaded has no authoritative status filter: entering it from Reading must
    # still render six grid cells, rather than inheriting the two-card page size.
    f = enter()
    for i in range(6):
        f.press('ENTER').wait(600).press('ENTER').wait(1000).press('BACK').wait(500)
        if i < 5:
            f.press('RIGHT')
    steps(f, 'LEFT', 5)
    tool(tab(f, 1), 3).shot('covers.bmp', 'six downloaded books after the Reading tab')
    tool(f, 0).shot('titles.bmp', 'six downloaded title rows after the Reading tab')
    _, routes = run('downloaded-after-reading', f, fixture=visual)
    need(len([r for r in routes if r[1].endswith('/download') and r[3] == 200]) == 6,
         'downloaded-after-reading: fixture did not download all six books')
    check('downloaded-after-reading', 'covers.bmp', 'covers')
    check('downloaded-after-reading', 'titles.bmp', 'titles')

    # Existing repository image assets are extracted byte-for-byte, never normalized.
    original=fx.parent/'original-media';shutil.copytree(visual,original,dirs_exist_ok=True)
    manifest=[]
    original_books=json.loads((original/'catalog.json').read_text())
    for i,(epub,member) in enumerate([('test_jpeg_images.epub','OEBPS/images/grayscale_test.jpg'),
                                     ('test_png_images.epub','OEBPS/images/grayscale_test.png')]):
        source=REPO_ROOT/'test/epubs'/epub
        with zipfile.ZipFile(source) as z: data=z.read(member)
        dest=original/Path(member).name;dest.write_bytes(data)
        original_books['books'][i]['cover']=dest.name
        manifest.append({'source':str(source),'archive_member':member,'bytes':len(data),
                         'sha256':hashlib.sha256(data).hexdigest(),'dimensions':list(Image.open(dest).size)})
    (original/'catalog.json').write_text(json.dumps(original_books,ensure_ascii=False))
    run('original-media',enter().wait(1000).shot('covers.bmp','unmodified repository JPEG and PNG image consumers'),fixture=original)
    check('original-media','covers.bmp','covers')
    if active:
        (Path(args.keep)/'original-media/manifest.json').write_text(json.dumps(manifest,indent=2))
        need(len(list((Path(args.keep)/'original-media/fs_/.crosspoint/bookorbit').glob('lib_*/covers/*.bmp')))==6,
             'original media: one of the six supported images failed to decode')

    # Offline cache reuse: preserve the actual first run's owner-scoped BMPs and catalog.
    def cached_sd(work):
        shutil.copytree(Path(args.keep)/'visual/fs_',work/'fs_');(work/'shots').mkdir()
    _, routes = run('cached-offline',enter().shot('offline.bmp','offline catalog with reused decoded covers'),{'offline':True},visual,cached_sd)
    check('cached-offline','offline.bmp','covers')
    need(not any(r[1].endswith('/thumbnail') for r in routes),'offline cover path attempted network')
    def interrupted_cache(work):
        cached_sd(work)
        directory = next((work/'fs_/.crosspoint/bookorbit').glob('lib_*/covers'))
        for book_id in ('1', '2'):
            live = directory/f'{book_id}.bmp'
            shutil.copy2(live, live.with_suffix('.bmp.bak'))
            live.with_suffix('.bmp.tmp').write_bytes(b'partial image')
        (directory/'1.bmp').unlink()  # power loss after staging old live aside
        (directory/'download.tmp').write_bytes(b'partial network response')
    run('cache-recovery',enter().shot('offline.bmp','offline cache recovered after interrupted promotion'),
        {'offline':True},visual,interrupted_cache)
    check('cache-recovery','offline.bmp','covers')
    if active:
        directory=next((Path(args.keep)/'cache-recovery/fs_/.crosspoint/bookorbit').glob('lib_*/covers'))
        need(len(list(directory.glob('*.bmp')))==6 and not list(directory.glob('*.tmp')) and
             not list(directory.glob('*.bak')),'interrupted cover cache did not recover atomically')
        original=next((Path(args.keep)/'visual/fs_/.crosspoint/bookorbit').glob('lib_*/covers'))
        need(all((directory/f'{i}.bmp').read_bytes()==(original/f'{i}.bmp').read_bytes() for i in (1,2)),
             'interrupted promotion changed a complete cover')
    def full_cache(work):
        cached_sd(work)
        directory=next((work/'fs_/.crosspoint/bookorbit').glob('lib_*/covers'))
        template=(directory/'1.bmp').read_bytes()
        for p in directory.glob('*.bmp'):p.unlink()
        for i in range(100,148):(directory/f'{i}.bmp').write_bytes(template)
    run('cache-limit',enter().wait(1200).shot('covers.bmp','bounded cache replaces only owned numeric thumbnails'),
        fixture=visual,make=full_cache)
    check('cache-limit','covers.bmp','covers')
    if active:
        directory=next((Path(args.keep)/'cache-limit/fs_/.crosspoint/bookorbit').glob('lib_*/covers'))
        need(len(list(directory.glob('*.bmp')))==48,'cover cache exceeded 48 live thumbnails')
        need(all((directory/f'{i}.bmp').exists() for i in range(1,7)),'visible covers not retained under cache pressure')
    run('offline-empty',enter().shot('offline.bmp','offline without catalog: downloaded view remains usable'),{'offline':True})
    run('error',enter().shot('error.bmp','server rejection and Retry'),{'catalog_status':400})

    # Exit is reachable using front/side buttons; final active activity is Home.
    log,_ = run('exit',tool(enter(),7).wait(600).shot('home.bmp','Exit returns to Home'))
    need(log.count('Entering activity: Home') >= 2,'Exit did not return Home')
    (Path(args.keep)/'geometry.json').write_text(json.dumps(geometry,indent=2))
    return shots


if __name__ == "__main__":
    sys.exit(main())
