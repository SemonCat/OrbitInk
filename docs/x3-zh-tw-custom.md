# X3 zh-TW Custom Profile

A private, personal build profile (`pio run -e x3_zh_tw_custom`) for a single
Xteink X3 device, with an asset-gated one-time migration tailored for Traditional
Chinese reading: full Traditional Chinese UI, the GuanKiapTsingKhai font as
both reading font and CJK UI fallback, and a larger default reading size.
This is **not** a general-purpose feature — it changes compiled-in defaults
for one build environment only. Every other environment (`default`,
`gh_release`, `gh_release_rc`, `slim`, `sticky`) is unaffected.

## What this profile changes

Gated entirely behind the `CROSSFADE_CUSTOM_ZH_TW` compile flag (set only by
`[env:x3_zh_tw_custom]` in `platformio.ini`):

| Setting | Upstream/default builds | This profile |
|---|---|---|
| UI language (`CrossPointSettings::language`) | English | Traditional Chinese (`Language::ZH_TW`) |
| Reader font family (`sdFontFamilyName`) | none (built-in Noto Serif) | `GuanKiapTsingKhai` |
| Reader point size (`fontPointSize`) | 14pt | 28pt |
| Text anti-aliasing | enabled | **disabled by default** (still user-selectable) |
| CJK UI fallback sizes | 8/10/12pt, one per builtin UI role | **one shared 14pt font** covering all three builtin UI roles (`SMALL_FONT_ID`, `UI_10_FONT_ID`, `UI_12_FONT_ID`) |

The profile is applied exactly once after the registry finds all packaged font
sizes (14/18/20/22/24/26/28pt). This covers upgrades whose persisted settings
would otherwise override compile-time defaults. If the folder is absent or
only partly copied, the firmware keeps the existing English/builtin-font
settings and retries on a later boot. Once applied, later user choices are
preserved.

A separate one-time performance marker applies the latency default to devices
that already completed the original font migration. It only turns text
anti-aliasing off once; later user changes are preserved.

## Why one shared UI-fallback size, not three

The original design (mirroring upstream's 8/10/12→10/12/14 idea) would keep
**four** distinct SD-card font styles resident simultaneously: the reader
font at its selected point size, plus three separately-loaded UI-fallback
sizes. Measured against the real corpus (148 EPUBs from Edison's TrueNAS
library) and the real GuanKiapTsingKhai font, the corpus-driven interval
manifest cannot be merged below **~2,779 intervals per resident style**
without either dropping a required character or letting
`fontconvert_sdcard.py`'s own rasterizer silently re-split the interval
anyway (`SdCardFont.cpp`'s `MAX_INTERVALS = 4096` is a hard load-reject, not
a soft target — exceeding it fails the whole font load, not just the
overflow glyphs). The legacy compact index costs 6 bytes/interval, or 16,098
bytes **per** resident GuanKiap style. The runtime now converts highly
fragmented BMP-only fonts to a page-packed coverage/rank index while loading
them. Against every generated GuanKiap size this is 3,568 bytes, saving 12,530
bytes per resident style without changing glyph order. The custom assets are
also packed as 1-bit v4 `.cpfont`: this halves per-page bitmap allocation and
keeps the 28pt prewarm below the X3's measured contiguous-allocation ceiling.

Instead, all three builtin UI font IDs are mapped (via
`GfxRenderer::setFallbackFont`) to the **same** loaded 14pt font.
`SdCardFontManager::loadFamilyExtraSize()` already dedupes by point size, so
this costs exactly one extra resident style, not three — full glyph coverage
for all three UI roles. With 14pt UI plus one reader size resident, the
coverage indices use 7,136 bytes instead of 32,196 bytes. See the
`kUiFontSizes` comment and
`static_assert` in `src/SdCardFontSystem.cpp` for the exact accounting.
The one real trade-off: small/author-line and list-row CJK text now renders
at the same (larger) 14pt as titles, rather than a size visually matched to
their normally-smaller Latin counterparts — see that same comment for why
this was accepted rather than audited call-site-by-call-site.

## Ported CJK runtime optimizations

Two bounded optimizations were adapted from `aBER0724/crosspoint-reader-cjk`:

1. **Page-packed BMP coverage/rank lookup.** Sparse BMP-only fonts whose
   compact interval table exceeds 4KB are converted to a populated-page
   bitmap plus per-page glyph prefix offsets. Supplementary-plane or small
   fonts retain the original interval lookup. A host parity test checks every
   BMP codepoint against the interval algorithm.
2. **Stale render cancellation.** Each render task snapshots a generation.
   New navigation invalidates that generation; SD glyph prewarm, text lines,
   and repeated grayscale passes stop at safe boundaries before submitting a
   stale frame. This improves rapid multi-page navigation responsiveness but
   does not alter the X3 waveform or claim a faster single page refresh.

## X3 latency policy

The private build also avoids two X3-specific stalls without changing the
panel waveform:

1. **Component-batched UI glyph prewarm.** Headers, tab bars, visible list
   rows, menus, popups, book cards, and button hints collect their visible
   strings and batch-load the resolved SD fallback glyphs before measuring or
   drawing. This replaces the previous 8-slot on-demand ring's repeated
   open/seek/read churn on Traditional Chinese screens.
2. **No main-loop idle page prewarm.** The old idle prewarm runs inside the
   same task that polls buttons, so an X3 cannot observe a new button edge
   until that SD scan returns. The private profile disables that misleading
   "background" work. The render task's normal page prewarm remains enabled
   and cancellable.
3. **Text anti-aliasing defaults off.** On X3, anti-aliased text re-renders two
   grayscale planes and submits an additional grayscale waveform. Users may
   still enable it when visual smoothness matters more than page-turn latency;
   image pages retain their own grayscale path either way.

The INFO serial log exposes `UIPERF` batches (SD time, seeks, glyph count) and
`Page render` stage timings (`prewarm`, `bw_render`, `display`, grayscale, and
total) so real-device bottlenecks can be measured instead of inferred.

The Gitee Flash-partition/mmap font cache and experimental Papyrix waveforms
are intentionally not included: the first conflicts with the 30MB multi-size
font set and OTA rollback space, while the latter needs device-specific
ghosting and controller validation.

## Font/charset generation

The `GuanKiapTsingKhai` `.cpfont` assets are generated from:

1. A corpus-driven charset manifest (`lib/EpdFont/scripts/analyze_epub_charset.py`),
   built from Edison's actual 148-book library plus every Traditional Chinese
   UI string plus an essential ASCII/CJK-punctuation baseline. See
   `implementation-report.md` (private, outside git) for the exact coverage
   rule, corpus statistics, and RAM/glyph-bloat tradeoff numbers.
2. `fontconvert_sdcard.py --charset-file <manifest> --1bit`, generating regular-style
   `.cpfont` files at 14/18/20/22/24/26/28pt. The 14pt file is reserved for
   the shared CJK UI fallback; reader choices are 18–28pt in 2pt steps.

Existing validated v4 2-bit assets can be converted without the source TTF by
`repack_cpfont_1bit.py`; it preserves coverage, metrics, kerning and ligatures
while rebuilding only bitmap lengths and offsets.

The reader detects the active font's bit depth. Text grayscale passes are
skipped for 1-bit fonts because they have no grayscale coverage to render;
image-page grayscale remains independent and unchanged.

Regenerating either requires the private font file (`GuanKiapTsingKhai.ttf`,
SIL OFL 1.1, not checked into this repository) and, for the corpus scan, SSH
access to the TrueNAS host holding the book library.

## Installing on a device

Compiling this profile alone does **not** provide Traditional Chinese
reading — the firmware only carries the UI translation and the *default
preference* for `GuanKiapTsingKhai`. The actual font glyphs live on the SD
card (`fonts/GuanKiapTsingKhai/*.cpfont`, see
[sd-card-fonts.md](sd-card-fonts.md)) and must be copied there **before
boot**. Missing or incomplete assets leave the UI in English and are retried
at the next boot; no settings-file deletion is required. See
`dist/x3-zh-tw/README-zh-TW.md` for the exact install steps.
The new OTA release package supplies firmware without optional SD font assets.
Existing users must keep all seven installed font sizes; the update preserves
them. Their unresolved redistribution provenance is not evidence of permission
to copy them into a public package.

The profile disables the stock online firmware and font update feeds: neither
contains this custom image or its corpus-derived assets, and following their
GitHub redirects was the source of a real X3 heap-abort. It now supports a
separate signed BookOrbit-X3 firmware channel and manual web uploads; see
[custom X3 wireless firmware updates](contributing/x3-ota.md). SD-card firmware
installation remains available. Online font updates stay disabled.
