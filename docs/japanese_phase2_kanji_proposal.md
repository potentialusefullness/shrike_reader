# Japanese Phase 2 — Kanji via SPIFFS Tile Overlay

**Status:** Proposal (Phase 1 kana shipped in v1.4.0)
**Branch target:** `shrike/japanese-kanji`
**Constraint:** 241 KB flash headroom remaining after Phase 1 — kanji cannot go in flash.
**Scope:** 2,136 Jōyō kanji at one display size, extensible to all 5,000 JIS Level-1 kanji.

## TL;DR

Ship the kanji bitmaps **in SPIFFS** as a packed "overlay font" indexed by a tile table. At runtime the renderer detects a CJK Unified Ideographs codepoint, opens the overlay file once, binary-searches the tile index, and `fread`s the compressed bitmap for that single glyph into a small LRU cache in PSRAM/heap. Flash stays unchanged. SPIFFS uses ~1 MB out of the 3.375 MB partition. One display size (16 px) first — 14/18 as follow-ups if users ask.

## 1. Why SPIFFS, not flash

### Flash math rules flash out

Phase 1 kana adds 95 + 96 = 191 CJK glyphs across 16 `notosans_*` variants for ~424 KB. Rule of thumb per variant: ~1.1 KB per CJK glyph compressed + ~16 B per `EpdGlyph` record + interval/group overhead.

Joyo = 2,136 glyphs. One variant at 16 px would consume:
- Bitmaps (compressed 2-bit): ~2,136 × 1.1 KB ≈ **2.3 MB**
- `EpdGlyph` table: 2,136 × 16 B ≈ 34 KB
- Intervals/groups: negligible

Even a **single size × single style** of Jōyō is ~10× the 241 KB headroom. Across all 16 variants it's ~37 MB — half the flash chip. Not viable.

### SPIFFS is exactly what it's for

The ESP32-C3 partition table (`partitions.csv`) already reserves 3.375 MB for SPIFFS at `0xc90000`. CrossPoint uses it for covers and metadata today — it's read at EPUB-open time anyway. A compressed Jōyō overlay at one size fits in ~1 MB, leaving 2+ MB for everything else.

### Ranges to cover

- **U+4E00–U+9FFF** (CJK Unified Ideographs, ~20,992 codepoints) — superset of Jōyō. Phase 2a ships Jōyō (2,136); Phase 2b optionally extends to JIS X 0208 Level-1 (~3,000) or full plane (~5,000).
- **U+3400–U+4DBF** (Extension A, rare) — out of scope.
- **U+3000–U+30FF** already in flash from Phase 1 — overlay lookup **skips** codepoints with an in-flash glyph.

## 2. File format: `kanji_16_regular.epkf`

"EPKF" = EPD Packed Kanji Font. One file per (size, style) pair. Layout:

```
┌─────────────────────────────────────────────┐
│ HEADER (32 B)                               │
│   magic         'EPKF'       4 B            │
│   version       0x0001       2 B            │
│   flags         2-bit,zlib   2 B            │
│   fontSize      16           2 B            │
│   ascent/desc   int16 × 2    4 B            │
│   glyphCount    uint32       4 B            │
│   tileCount     uint16       2 B            │
│   tileSize      uint16 (64)  2 B            │
│   tileIndexOff  uint32       4 B            │
│   bitmapPoolOff uint32       4 B            │
│   reserved                   2 B            │
├─────────────────────────────────────────────┤
│ GLYPH RECORDS  (16 B × glyphCount)          │
│   EpdGlyph-compatible struct,               │
│   dataOffset is absolute file offset        │
│   into BITMAP POOL                          │
├─────────────────────────────────────────────┤
│ TILE INDEX  (4 B × tileCount)               │
│   For each 64-codepoint tile:               │
│     firstGlyphIdx  uint16                   │
│     presenceBitmap uint64 (packed elsewhere)│
│   -- or --                                  │
│     offset into sparse lookup (see §3)      │
├─────────────────────────────────────────────┤
│ BITMAP POOL (compressed, concatenated)      │
│   Each glyph's bitmap DEFLATE-compressed    │
│   independently. Size known from record.    │
└─────────────────────────────────────────────┘
```

**Why per-glyph compression, not per-group?** In-flash fonts DEFLATE a whole script range at once because you load the entire group into RAM once, then random-access glyphs. For SPIFFS we want to read **one** glyph per codepoint without inflating 20 KB of neighbors — per-glyph DEFLATE is ~5% worse on ratio but massively better on RAM and latency. Each kanji is ~256 B raw 2-bit, ~180 B compressed → fits a single 4 KB flash page, `fread` + `inflate` in <5 ms.

**One file per size/style.** Don't interleave sizes — users only render at one size at a time; keep the hot file small. Style fallback: italic→regular, bolditalic→bold (same approach as Phase 1 kana), so only two files per size. Shipping 16 px only = **2 files**.

## 3. Tile index: sparse lookup

A linear `EpdGlyph[0x5200]` array would be 135 KB per file just for "does this kanji exist?" answers. Three options, ordered by memory vs. speed:

| Scheme            | Index RAM                      | Lookup cost     | Notes                               |
|-------------------|--------------------------------|-----------------|-------------------------------------|
| Linear array      | 135 KB / file resident         | O(1)            | DOA — blows the RAM budget          |
| **Tile bitmap**   | **2.6 KB / file resident**     | **O(1) + popcount** | **Recommended**                 |
| Interval list     | ~1 KB / file resident          | O(log n)        | Same as in-flash `EpdUnicodeInterval` — works but slower |

**Tile bitmap (picked):** Divide U+4E00–U+9FFF into 0x5200/64 = **328 tiles** of 64 codepoints each. For each tile store:
- `uint64_t presence` — one bit per codepoint in the tile
- `uint16_t firstGlyphIdx` — index into the glyph record array for this tile's first present codepoint

Lookup `cp`:
1. `tile = (cp - 0x4E00) >> 6; bit = (cp - 0x4E00) & 63`
2. If `(presence[tile] >> bit) & 1 == 0` → glyph absent, fall back to tofu box
3. `glyphIdx = firstGlyphIdx[tile] + popcount(presence[tile] & ((1ULL << bit) - 1))`
4. Read `EpdGlyph record = glyphRecords[glyphIdx]`
5. `fseek(record.dataOffset); fread(record.dataLength); inflate()`

Index size: 328 × (8 + 2) = **3,280 B** resident per open file. Loaded once at file-open and kept in heap.

## 4. Runtime integration

### 4.1 New class: `EpdKanjiOverlay`

```cpp
class EpdKanjiOverlay {
 public:
  bool open(const char* path);          // /kanji/kanji_16_regular.epkf
  const EpdFontData* wrap();            // returns an EpdFontData facade
  void close();
 private:
  FILE* fp_;
  EpdKanjiHeader hdr_;
  std::vector<uint64_t> presence_;      // 2.6 KB
  std::vector<uint16_t> firstGlyphIdx_; // 656 B
  LruCache<uint32_t, CachedGlyph> cache_; // 64 entries ≈ 18 KB
};
```

Cache key = codepoint. Cache value = decompressed bitmap + `EpdGlyph` copy. 64 entries × ~288 B ≈ **18 KB heap** — covers a full screen of kanji twice over (a page holds ~30–50 kanji at 16 px).

### 4.2 Hooking into the renderer

`EpdFontData` is consumed by the renderer via a glyph lookup. The cleanest change is to add a **chain pointer** so the existing in-flash `notosans_16_regular` can delegate missing glyphs to the overlay:

```cpp
struct EpdFontData {
  // ... existing fields ...
  const EpdFontData* fallback;   // NEW — nullable
};
```

Renderer logic: if `findGlyph(cp)` misses and `fallback != nullptr`, call `fallback->findGlyph(cp)`. The overlay exposes itself as an `EpdFontData` via `wrap()` and the callback routes to `EpdKanjiOverlay::fetchGlyph()`.

One overlay instance per active font size. Instances live in the singleton `FontManager`, opened lazily the first time a kanji is encountered.

### 4.3 Line-break parser — already done

`ChapterHtmlSlimParser`'s CJK break-point logic from Phase 1 already covers U+4E00–U+9FFF (treated as break points even though no glyphs were shipped). Phase 2 needs zero parser changes.

### 4.4 Missing-glyph behaviour

If the tile bitmap says "absent" (non-Jōyō kanji in a Jōyō-only build), render the existing tofu box. Do **not** stall the reader — log once per codepoint and continue.

## 5. Build pipeline

New script `lib/EpdFont/scripts/build-kanji-overlay.py`:

```
Inputs:
  - NotoSansCJKjp-Regular.otf   (already fetched for Phase 1)
  - NotoSansCJKjp-Bold.otf
  - jouyou-2136.txt             (one codepoint per line, committed to repo)
  - --size 16

Output:
  build/kanji_16_regular.epkf
  build/kanji_16_bold.epkf
```

Reuses `fontconvert.py`'s glyph rasterizer + 2-bit quantizer + zlib path — factor the bitmap stage into a library function, then `build-kanji-overlay.py` drives it per-glyph and emits the EPKF container.

Artifacts ship via the SPIFFS image built by `pio run -t buildfs`. Copy the `.epkf` files into `data/kanji/` before `buildfs`.

**CI check:** a Python verifier that opens the EPKF, walks every tile bit, and confirms `fseek`/`inflate` succeeds on every declared glyph.

## 6. Flash / SPIFFS / RAM budget

### Flash delta
- `EpdKanjiOverlay` class + EPKF reader: **~4 KB** code.
- New `fallback` field in `EpdFontData`: 4 B × ~20 font structs = 80 B.
- `FontManager` singleton + overlay wiring: ~1 KB.
- **Total flash growth: ~5 KB** (out of 241 KB headroom → still ~236 KB after Phase 2).

### SPIFFS usage (16 px only, both weights)
- Jōyō (2,136): 2 × ~900 KB = **~1.8 MB**
- Tile index per file: 3.3 KB (lives in SPIFFS too, but trivial)
- Leaves ~1.5 MB SPIFFS for covers/metadata.

### SPIFFS usage if extended
- JIS Level-1 (~3,000): ~2.5 MB across both weights → **tight but possible**
- Full plane (~5,000): ~4 MB → **exceeds SPIFFS**; would require partition repartition or external SD (not proposed here)

### RAM delta (heap, per open overlay)
- Header + tile index: 3.3 KB
- LRU cache (64 glyphs × ~288 B): ~18 KB
- Inflate scratch buffer (shared, amortized): ~2 KB
- **~24 KB per active size**, typically 1–2 sizes open → **~25–50 KB PSRAM**

C3 has no PSRAM — this runs in internal SRAM. Today's RAM usage is 29.9% / ~97 KB of 327 KB. Adding 50 KB pushes to ~45%. Still comfortable, but flag as the real constraint. If RAM gets tight, shrink cache to 32 entries (-9 KB).

## 7. Performance targets

- **Glyph fetch (cache miss):** `fseek` (~0.5 ms on SPIFFS) + `fread` ~200 B (~1 ms) + `inflate` (~2 ms) = **<5 ms**.
- **Cache hit:** table lookup, <50 µs.
- **Page of 50 kanji, cold cache:** 50 × 5 ms = 250 ms — a noticeable but tolerable hit on first page of a kanji chapter. Subsequent pages reuse most of the cache.
- **Pre-warm strategy (optional):** when opening a book detected as Japanese (scan first chapter for CJK ratio >20%), prefetch the top-N most common kanji in a background task. Adds ~1 s to book open, eliminates the per-page stutter.

## 8. Milestones

| # | Task                                                                              | Size |
|---|-----------------------------------------------------------------------------------|------|
| 1 | EPKF format spec + reference writer in Python                                     | M    |
| 2 | `build-kanji-overlay.py` script + Jōyō list committed                             | M    |
| 3 | `EpdKanjiOverlay` reader class with unit test against a tiny 10-glyph fixture     | L    |
| 4 | `EpdFontData::fallback` wiring in renderer + `FontManager` lazy-open              | M    |
| 5 | Tofu-box for absent codepoints, cache-miss logging                                | S    |
| 6 | SPIFFS image build integration (`data/kanji/*.epkf`, `pio run -t buildfs`)        | S    |
| 7 | End-to-end test: Aozora Bunko sample EPUB renders                                 | M    |
| 8 | Pre-warm heuristic (optional; defer if Milestone 7 is acceptable)                 | M    |

Ship as **v1.5.0** on `shrike/japanese-kanji`. Proposal status: awaiting approval before starting Milestone 1.

## 9. Open questions

1. **One size or three?** 16 px is the body default. 14/18 double SPIFFS usage. Recommend shipping 16 px only in v1.5.0, add 14/18 in v1.5.1 if users ask.
2. **Jōyō only or JIS Level-1?** Jōyō covers essentially all modern prose; JIS Level-1 adds ~900 rarer kanji (place names, classical). Jōyō is the safer start — room to extend.
3. **SPIFFS vs. LittleFS?** LittleFS is more crash-resistant and faster on small reads, but migrating the partition is orthogonal to this project. Keep SPIFFS.
4. **Italic fallback.** Phase 1 falls back italic→regular for kana; do the same here, or skip italic entirely for kanji? Recommend: skip italic for kanji (italic kanji looks wrong anyway in Japanese typography).

## 10. Phase 3 preview (furigana)

Not part of this proposal, but worth noting the seams we're leaving in place:
- Furigana needs a rendering pass that draws smaller kana above a base run. That lives in the layout engine, not the font subsystem — Phase 2 doesn't block it.
- Small-size kana (10 px?) for furigana needs a new flash variant or SPIFFS overlay. Decision deferred.
- EPUB3 `<ruby>`/`<rt>` tag support in `ChapterHtmlSlimParser` is the prerequisite, and it can use the exact CJK glyph path Phase 2 delivers.
