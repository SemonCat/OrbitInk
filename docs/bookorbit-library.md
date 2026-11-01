# OrbitInk — BookOrbit library redesign + KOSync product removal: PLAN

Repo `/Users/edisonpve/Documents/code/personal/orbitink-public`, branch `bookorbit-library-20261101`,
base `main @ 9f788995faa35854477ad2d0c21043699d362cc3` (nightly 2001). Target: X3 (528×792 1-bit
e-paper, 4 front + 2 side buttons, no touch), zh-TW profile (Journal theme default, single 14 pt
regular-only CJK SD UI font ≈34 px/line, QWERTY-English keyboard only).

## 1. Evidence (baseline, fixture modelled on the real server contract)

Baseline screens: captured with `scripts/sim_bookorbit_library_smoke.py --flows before` against main @ 9f78899 (Journal theme, real profile migration).

| # | Baseline defect | Evidence |
|---|---|---|
| B1 | Book detail fails: server emits `description` (KB) **before** `files`; client caps the body at 4 KB → `JSON parse error`; user cannot reach 下載/開啟 | before-08; request log `GET /books/210 → 200` then error |
| B2 | Series drill-in sends `seriesId=series:8` (facet id) → server DTO `@IsInt` → 400 "Request rejected by server" | before-07; `GET /books?seriesId=series:8 → 400` |
| B3 | Authors/Series facets: server pages 60 (ignores `size`); client keeps 20 then asks page 2 (offset 60) → entries 21–60 of every page unreachable; 8 KB body cap also truncates a 60-entry CJK page | server `AUTHOR_SERIES_PAGE_SIZE=60`; client loop `break` at 20 |
| B4 | No location context: every screen titled 「書庫」; no breadcrumb, list name, sort, filter, count or position | before-02..05 |
| B5 | Server section titles shown verbatim in English ("Continue reading", "All Books") on zh-TW | before-01 |
| B6 | Paging is a list row (「下一頁 »」/「« 上一頁」) mixed with books; each page change pushes a Back frame (Back walks pages, not levels) | before-04/05 |
| B7 | No sort or read-status filter although the server supports 6 sorts + unread/reading/finished | DTO `KOREADER_CATALOG_SORTS`, `READ_STATUS_FILTERS` |
| B8 | Search only via a root row or a hidden "Left at row 0" | before-02 hints |
| B9 | Read-status / rating are cycle rows: reaching a status takes up to 5 server writes | `cycleReadStatus()` |
| B10 | Empty list offers 搜尋 on a button and shows generic 找不到項目; errors are raw English (`Request rejected by server`) | before-11, before-07 |

Server contract source (clean-room protocol reading, as the client header already cites):
`github.com/bookorbit/bookorbit@9165dbed…`: `koreader-catalog-query.dto.ts`, `koreader-catalog.service.ts`,
`opds-book.service.ts` (q matches title, author, series name, ISBN; readStatus buckets
reading={reading,rereading,on_hold}, finished={read,skimmed,abandoned}), `packages/types/src/koreader.ts`.

## 2. KOReader design reference (official)

- KOReader user guide, File browser: <https://koreader.rocks/user_guide/#L2-filebrowser>
  (screenshot `pictures/file_browser_ui.png`).
- Collections: <https://koreader.rocks/user_guide/#L2-collections> (`collections_features.png`).
- Source @ `koreader/koreader` master `035e00b2`: `plugins/coverbrowser.koplugin/listmenu.lua`
  (detailed list: title/authors left, right column "x % of N pages" / "Finished" / "On hold"),
  `frontend/ui/widget/menu.lua` (title bar + path subtitle truncated from the left; footer
  "Page x of y" with chevrons — paging is never a list row).
- Artwork: <https://github.com/koreader/koreader-artwork/raw/master/koreader-menu.png>.

Adopted: (a) title bar + path breadcrumb; (b) two-line rows, title over author/series, status at
right; (c) position indicator, paging outside the list; (d) status as explicit choices; (e) sort
by + filter as a list-level menu; (f) Back = up one level. Not adopted: cover mosaic/thumbnails
(see §4), touch gestures, long-press menus (undiscoverable on buttons).

## 3. Library design (X3, buttons only)

Header 「書庫」 (or 「BookOrbit 目錄」 from Settings) keeps the existing theme chrome; the
**sub-header** carries the breadcrumb on the left and position on the right, e.g.
`書庫 › 作者 › 林曉雲` … `21–28 / 46`. Breadcrumb truncates from the left (keeps the tail).

Root (one screen, single-line rows, fixed order; server-advertised ids only, unknown ids appended
with the server title):
1. 搜尋書名、作者或系列 (online only) 2. 繼續閱讀 3. 本機書籍 (downloaded, value 「N 本」)
4. 全部書籍 5. 最近新增 6. 作者 7. 系列 8. 書庫分類 (libraries) 9. 收藏 10. 智慧書單.
Known server ids are localized (B5). Offline: 本機書籍 + cached rows, sub-header 「離線 · 儲存於 …」.

Book list (server): row 0 is an explicit control row 「排序：書名 · 狀態：全部」 (only where sort/filter
apply); rows are title (one line, truncated) over `author · 系列名 #n`, second-line right column = status:
`45%` (reading/progress), 「已讀完」, 「暫停」, 「想讀」, 「放棄」, nothing for unread; downloaded books get a
「本機 · 」 prefix on line 2. No pager rows (B6): moving past the last row of a loaded 20-book server
page loads the next page and selects its first row; before the first row of page > 0 loads the
previous page and selects its last row; hold = jump one screen. Position `first–last / total` from
the server `total`. Paging does **not** push Back frames; Back goes up one level and restores the
parent's page + selection + sort/filter.

Options screen (from row 0): one list of explicit choices with the current one marked 「目前」:
依書名 / 依作者 / 依系列 / 最近新增 / 最近閱讀 / 最近更新 ; 全部狀態 / 未讀 / 閱讀中 / 已讀完 ;
plus 「在此清單搜尋」 (q scoped to the current filters, which the server supports). Confirm applies
one choice, returns to the list at page 0. Sort/filter live in the list's own query, so they
survive detail, download, Back and are restored with the frame.

Facet lists (作者/系列/書庫分類/收藏/智慧書單): title + 「N 本」; server-paged (60 for authors/series)
with the same auto-paging; series drill-in uses the numeric `seriesId` (B2).

Detail: title (≤3 wrapped lines), author, `系列名 #n`, a status line (`閱讀中 · 45%` / 已讀完 …),
file line (`EPUB · 1.2 MB · 已下載`), last error if any; actions: 開啟閱讀 (if local) / 下載 (size)
first; 閱讀狀態：… → choice list (one write, B9); 評分：… → choice list (未評分, 1–5). Breadcrumb
in sub-header. Download keeps the existing temp+backup+promote transaction and index upsert; after
success 開啟閱讀 is focused; list rows are re-marked 本機 on return.

Errors/empty: localized message per error class (連線失敗/伺服器拒絕/登入失敗/記憶體不足…), Retry
re-runs the same request; empty lists say what is empty (「這個清單沒有書」, 「沒有符合的書」 for
search/filter with a hint to change 狀態) — no misleading button hints.

All new labels limited to glyphs the shipped GuanKiap charset already covers (sim tofu check).

## 4. Thumbnails decision

The server exposes `GET .../books/:id/thumbnail` (arbitrary image file, ETag). Showing them in
lists means one extra TLS request + image decode/dither per row (≈8 per screen) on an ESP32-C3
whose TLS gate already needs a 20 KB contiguous block and drops SD-font arenas per request; the
existing cover cache is keyed to local EPUBs. Not efficient or memory-bounded at list speed →
**text-only informative list**, no cover grid. Documented as future work.

## 5. Client/protocol fixes (bounded memory)

- Streaming key-dropper on response bodies: drops the *values* of named keys (any depth) while
  bytes stream in, bounding each retained item and the response envelope. Detail drops
  description/relatedSections/collections/genres/tags (B1); facet pages drop booksHref/href/icon;
  book pages drop thumbnailUrl/detailUrl/formats. Host-tested state machine (strings, escapes,
  nesting, chunk boundaries).
- Book/facet pages stream and parse one complete item at a time, then discard its JSON bytes.
  A page can exceed 16 KB in total without losing entries: only one item plus envelope is retained
  (16 KB cap). Facets keep up to 60 entries (server page size), `hasNext` from server (B3). An
  oversized individual item rejects the request; partial fixed-offset pages are never committed.
- Series facet → numeric `seriesId` (B2); list items parse seriesName/seriesIndex; book page keeps
  `total`.
- No new endpoints; no catalog-wide fetch; only one server page (≤20 books / ≤60 facets) in RAM.

## 6. KOSync product removal (BookOrbit-only ROM)

Removed from product: KOSync settings screen/auth (`KOReaderSettingsActivity`,
`KOReaderAuthActivity`), Settings → KOReader 同步 entry, web settings fields (koUsername,
koPassword, koServerUrl, koMatchMethod, koSendMetadata, koSyncBehavior, koAutoSync) and web
SettingsPage KOSync section, reader menu 「同步進度」 (KOSync only) and long-press KOSync option,
`KOReaderSyncActivity`, KOSync auto/headless transactions on sleep/timer/close/open, KOSync as a
periodic-schedule provider and network-availability source, KOSync help/labels.

Kept (shared by BookOrbit; documented internal names): `lib/KOReaderSync/` protocol layer
(`KOSyncTransport`, `KOSyncRequestHelpers`, `KOReaderSyncClient.h` types `KOReaderProgress`/
`KOReaderRichPosition`, `ProgressMapper`, `KOReaderDocumentId`, `ChapterXPathResolver`, secure
transport), `KOReaderAutoSync::recordProviderSuccess/consumeSuccessNotice/formatLastSuccessRelative`
(shared “last synced” status), `ProgressSync*` batch/retry/receipt/deferred, `KOReaderCredentialStore`
**read-only** for the existing one-shot `BookOrbitMigration` (it only ever fills an *empty*
BookOrbit store and gates stats capture via `migrationPending()`; removing it could strand stats).
Stored `koreader.json`, `deferredProgress` provider-1 records and `longPressMenuFunction=0` are
left on SD and harmlessly ignored (0 behaves as 停用). No SD deletion, no BookOrbit setting rewrite.

Sleep-only sync from 2001 preserved: same `ProgressSyncNetwork::Batch` routes, BookOrbit
provider only, periodic interval unchanged, receipts unchanged; timer gate no longer depends on
`koreader.json` readability.

## 7. Validation plan

Host tests (native CMake, `test/`): new catalog-client tests (key-dropper incl. chunk splits;
detail with 6 KB description; 60-entry facet page; series numeric id; total/series fields); browser
policy tests (auto-paging index math and complete status/filter mapping); simulator request
assertions cover options and query survival; schedule/retry tests updated for BookOrbit-only providers (KOSync creds never enable);
existing bookorbit_* / progress_* / stats / bookmark suites unchanged and passing; KOSync-only
product tests removed with their code, protocol tests kept.
Simulator: new `scripts/sim_bookorbit_library_smoke.py` (fixture-only; before/after screens;
request-log assertions for sort/filter/series/paging/state survival); existing
`sim_bookorbit_home_smoke.py`, `sim_bookorbit_setup_smoke.py`, `sim_periodic_sync_smoke.py`
(BookOrbit-only, not weakened). Guarded X3 firmware build (`x3_zh_tw_custom`). Independent design
review now, final P2 review of the whole branch (fable-5 → opus-5-5 → Codex).

## 8. Out of scope / limits
No CJK text entry on device (keyboard is QWERTY-EN) — search is for Latin titles/authors/ISBN;
Chinese titles are found by browse/sort/filter. No covers (§4). No push/tag/flash. Real-server
behaviour is modelled by fixtures, not live-verified.


## Implementation and accepted design findings (2026-11-01)

All five design-r1b P2 findings are implemented. Status mapping includes rereading, skimmed,
unknown values and every settable choice. Successful detail edits refresh the current server page
with the query and clamped selection retained; if a refresh fails, known nonmatching rows are
removed from the active status filter. Paging commits only successful results and shows failure
inline over the loaded page. Facet counts have an explicit presence bit, so an omitted count is
not rendered as zero. The proposed partial-page salvage was superseded by per-item streaming:
all 60 long CJK facet entries survive even when the response exceeds 16 KB, with no skipped offsets.

The Journal theme keeps its crop brackets and hairlines. Book row titles use the full width;
author/series/local marker and status share the measured second line. This accommodates the
14-point SD CJK font without a fixed Latin subtitle offset. Options and detail actions use the
existing theme list. Breadcrumbs keep their tail; position is a separate line above the list.
The offline cache writes version 2 (total and series fields) and still reads version 1.

KOSync-only activities, reader shortcuts, web settings/routes and automatic provider entry points
are removed. Legacy deferred provider-1 records remain on SD but are ignored at every reader
handoff. Shared protocol types, identity/XPath mapping, transport, last-sync notice, and the
read-only credential migration remain available for BookOrbit. No local book, progress, stats or
bookmark data is deleted. Timer smoke S1-S8 is retained; S9 proves legacy credentials alone issue
no requests or timer, and S10 proves a corrupt retired store cannot stop BookOrbit timer wakes.

The implementation remains text-only and button-driven. Search uses the existing QWERTY input;
the scoped search action preserves current server filters and sort. Publication and final visual
acceptance belong to the parent; this branch is a reviewable candidate, never a pushed release.

The final independent-review remediation also records requested pages separately from committed
visible pages (including Retry after a facet drill-in and Wi-Fi reconnection), and transactionally
replaces ignored legacy provider handoffs before comparing BookOrbit timestamps. Detail titles
use bounded UTF-8 wrapping for unspaced CJK text; the shared Latin word wrapper remains unchanged.
