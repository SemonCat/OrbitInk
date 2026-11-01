# BookOrbit cover library

Visual authority: user-approved BookOrbit design v2 dated 2026-11-01, approved with
「去吧，這樣好多了」. The exact originals are `00-design-board.png`,
`01-cover-library.png`, `02-title-library.png`, `03-reading.png` under
`/Users/edisonpve/clawd/media/bookorbit-design-v2/`; `REFERENCES.md` and `draw.py`
identify their synthetic artwork, system Heiti typography and provisional button bindings.
They are design references, never firmware captures.

The library enters directly into the title-sorted server catalog. All means all online
catalog books in the current scope, including downloaded ones. Downloaded is an explicit
tools action and uses this account's local index. Each grid/title row identifies local or
online availability; Reading cards say whether the book can be read offline. Confirm opens
detail, where another Confirm explicitly downloads the displayed EPUB and size, or opens
the indexed local file. Reading never depends on a cover request.

## Composition at 528 x 792

| Surface | Geometry |
| --- | --- |
| Shared header | identity x20/y14, title x20/y51, total x400/y66 |
| Fixed tabs | All / Reading / Unread / Finished, x20 + 124*n, y107, 112 x 38 |
| Sort/scope and view/tools rail | y161; full-width separator y193 |
| Default covers | 2 columns x 3 rows; x20/270, y205/373/541; 238 x 157 selection |
| Cover and metadata | 85 x 135 cover; title outside cover, up to 3 lines; one author line, availability, status |
| Title view | six rows; y201 + 80*n; 44 x 65 actual thumbnail or title fallback; title up to 2 lines |
| Reading with cover view | two cards; y215/435; 488 x 203; 112 x 166 covers, progress and availability |
| Shared footer | availability notice and exact range/total y701; rule y721; button instructions y738/770 |

Selection is an inside four-pixel closed outline (three pixels in title rows), never
Journal crop brackets or an inverted book row. No empty book cells are fabricated.
Other status tabs retain the selected cover/title browse model; Reading uses two cards
when cover mode is selected and six rows when title mode is selected.

The packaged Chinese UI shares one resident 14pt SD fallback. `drawTextSized` shrinks its
existing glyphs without allocating a second font or bitmap. Measurement and wrapping use
the same resolved font/scale as rendering. The grid title uses a 22px scaled line box with
27px advance, author 17px with 19px advance, status 16px; title rows use 23px with 27px
advance. These preserve the reference hierarchy/density despite the different typeface.
BookOrbit's small bitmap Latin face and the real Khai Chinese face differ from mock Heiti.
The grid reserves a separate metadata line for online/downloaded availability. Its author
is one ellipsized line (full author in detail), keeping availability above the closed
selection border and outside the cover art.

## X3 controls

All actions are reachable with physical buttons. "Up/Down" in the footer refers to the
mapped front arrows (logical Left/Right); physical side Up/Down paginate.

| Focus | Front previous / next | Confirm | Back |
| --- | --- | --- | --- |
| Book | previous / next book; previous from first book reaches tools rail | detail / download confirmation | tools |
| Tools rail | previous reaches Finished tab; next reaches first book | tools | tools |
| Fixed tabs | previous/next walks All, Reading, Unread, Finished, tools rail, first book | apply that exact status in the existing scope/sort/search | tools |
| Tools | move through actions with arrows or side keys | apply action | return to the same browse focus |
| Detail | move through local/download/status/rating actions | explicit action | same selected book and query |

Tools offers view switch, scoped search, sort/status options, downloaded, author/series/
locations, parent location, Retry, Exit. The tabs are always visible directly above books;
categories do not hide or replace them. Side buttons move one visible screen, including
the final partial server page, and preserve the current selection on failed loads. The
server still pages at 20 books, facets at 60, so a 20-book batch has screens of 6/6/6/2.
Back opens tools; the explicit parent action restores the previous location/query/page.
Exit returns to the caller/Home. View changes and detail returns preserve the selected
book, sort, status, search, scope and server page.

## Real cover contract and bounds

Server contract was inspected in `koreader-catalog.controller.ts` /
`koreader-catalog.service.ts` and `packages/types/src/koreader.ts` from the previously
pinned BookOrbit source reference. Summary `hasCover` advertises optional media.
`GET /api/v1/koreader/plugin/catalog/books/:bookId/thumbnail` checks access and streams
the stored thumbnail; it supports ETag but no width/height/format query. The client
also verified `metadata/lib/cover.ts` and `common/book-cover-storage.ts` at the same
server commit: generated `thumbnail.jpg` fits inside 400 x 600, JPEG quality 90,
without enlargement. The existing decoder supports that baseline JPEG shape.
The device's 96 KiB transfer ceiling may still decline a noisy large JPEG.
The client
constructs this endpoint from a numeric ID (at most 20 digits), never from the supplied
`thumbnailUrl`. It uses the existing configured BookOrbit base, credentials, TLS heap
gate and transport. A redirect is not followed by this request method. No third-party
cover service is contacted and no new certificate/auth/provider policy is installed.

Only the current screen's six (or two) covers are attempted, one request per idle loop
turn, after 350ms without browse navigation. A failure is attempted once per loaded
server page; Retry/refresh can reload that page. Input is polled through the transport
abort callback. Each request has a 3-second transport timeout and streams at most
96 KiB to a single scratch file. The TLS connection is released before decoding.

Supported stored images: baseline 8-bit JPEG and noninterlaced 8-bit PNG, source at most
512 x 768. Unsupported/progressive/oversized/corrupt images fall back to title/author.
The firmware's existing converters resize to at most 112 x 166 with preserved aspect
ratio, producing a one-bit BMP of at most 3,000 bytes. No source image or whole page
bitmap is allocated. JPEG MCU scratch is bounded by 16 source rows (at most 8 KiB);
PNG row buffers are at most about 2 KiB each plus the existing streaming inflater.
Actual free heap and network timing on physical hardware remain hardware acceptance.

Cache: `/.crosspoint/bookorbit/lib_<owner-key>/covers/<numeric-id>.bmp`, at most 48
live thumbnails per owner. One raw scratch and one converted temp are written at a
time. Startup recovers app-owned temp/backup transactions with a bounded directory
scan; unresolved storage state disables optional media. New conversions must pass
closed-file BMP header, dimensions and complete-row readback before atomic
temp/backup promotion. Offline reads reuse validated BMPs without networking.
Cache pressure evicts only numeric BMPs in this dedicated owner directory and protects the current visible books.
Existing downloaded EPUBs, metadata/progress/bookmarks and other caches are untouched.
Thumbnail freshness is cache-until-eviction; ETag revalidation is not implemented.

Catalog cache version 3 adds the `hasCover` bit and still reads versions 1 and 2;
old caches simply have no newly advertised covers. Existing cached BMPs can render
regardless of that hint. No account/cache/data migration is destructive.

## Acceptance

Run `uv run --with pillow python scripts/sim_bookorbit_library_smoke.py --no-build`
with `CROSSFADE_SIM_ZH_TW_FONTS` pointing at packaged fonts. This exercises the real
firmware via the fixture-only bridge, never a live BookOrbit service. It covers
the three compositions, 0/1/6/7 books, long CJK/Latin titles, tab/tools/side-page focus,
status transitions, numeric series, all 60 facets, drill/detail/page Retry,
download/rating, missing/error/unsupported/bounded covers and offline reuse.
`sim_bookorbit_library_geometry.py` measures actual pixels; the former text-only
capture fails its required y193 separator and book-card geometry.

The prior successful PUT + offline GET regression remains exact: author-sorted Reading
book208 is marked Finished, an old cached page is loaded after transport failure, and
Confirm must open remaining book224. Page commits only succeed after successful loads.
BookOrbit-only product routing and sleep-only sync remain unchanged.

Native CTest, library/Home/setup/periodic simulator suites, guarded X3 build and two
complete independent aggregate P2 review passes bind to the final frozen candidate.
Reports, side-by-side galleries, screenshot provenance and review attempt receipts
live outside the repository under
`/Users/edisonpve/Documents/OpenClaw Reports/orbitink-cover-library-20261101/`.
