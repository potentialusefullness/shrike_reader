# Japanese support + furigana — scoping proposal

Shrike Reader · v1.3.0 baseline · April 2026

---

## TL;DR

A clean, incremental path exists. The font pipeline is already CJK-aware (commented-out ranges, frequency-grouped compression, O(1) decompressor). What's missing is: (a) per-codepoint line-break logic, (b) a way to ship kanji without blowing the 380 KB flash budget, (c) a `<ruby>` parser path and a layout primitive for above-baseline text.

Recommended ship order — each stands alone and delivers user value:

1. **Phase 1 — Kana** (~1 day of work, ~30–60 KB flash). Hiragana + Katakana glyphs, basic CJK line-breaking. Ships kana-heavy text (manga, children's books, poetry).
2. **Phase 2 — Kanji via SPIFFS** (~3–5 days, ~0 KB app flash, ~1.5–2 MB SPIFFS). Build a runtime font loader, move all-kanji glyph tables to the data partition, load on language detection.
3. **Phase 3 — Furigana** (~2–4 days, 0 KB fixed cost). Handle `<ruby>/<rt>/<rp>/<rb>` in the HTML parser, add a `RubyRun` layout primitive that stacks a small reading above a base glyph run.

TinySegmenter is **not** required for any phase — Unicode line-break class "ID" (ideographic) allows a break between any two CJK codepoints, which is good enough for e-reader prose.

---

## What already exists in the codebase

| Area | File | State |
| --- | --- | --- |
| CJK unicode ranges in font converter | `lib/EpdFont/scripts/fontconvert.py:84-121` | Defined but commented out. Hiragana, Katakana, Unified Ideographs, Extensions A–G, Kangxi, CJK Symbols. Flip a flag to enable. |
| Frequency-grouped glyph compression | `lib/EpdFont/FontDecompressor.cpp:45` | O(1) `glyphToGroup` lookup path already implemented. This is the compression scheme used for CJK. Verified by `verify_compression.py`. |
| CJK comment in epub parser | `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp:811` | "For CJK text (no spaces), this is the primary word-breaking mechanism" — currently force-cuts at `MAX_WORD_SIZE` buffer fill, UTF-8-safe. No per-codepoint break opportunity logic. |
| Language abbreviations | `scripts/gen_i18n.py:217-218` | Already maps `日本語`→JA, `中文`→ZH. |
| Font family enum | `src/CrossPointSettings.h:95` | `FONT_FAMILY { BOOKERLY=0, NOTOSANS=1, OPENDYSLEXIC=2, FONT_FAMILY_COUNT }` — new entries plug in here. |
| Font picker UI | `src/SettingsList.h:41` | `{STR_BOOKERLY, STR_NOTO_SANS, STR_OPEN_DYSLEXIC}` — add STR_NOTO_SANS_JP. |
| Runtime font registration | `src/main.cpp:211` | `renderer.insertFont(...)` — extension point for SPIFFS-loaded fonts. |

**What does not exist** — any reference to `<ruby>/<rt>/<rp>/<rb>` in the parser, any `loadFontFromSpiffs()` function, any Unicode line-break property table.

---

## Flash budget — the hard constraint

| Partition | Size | Used | Free |
| --- | --- | --- | --- |
| `app0` (firmware) | 6.25 MB | 5.88 MB (89.8%) | **~380 KB** |
| `spiffs` (data) | 3.00 MB | ~0 MB | **~3.0 MB** |

Per-font reference sizes (current build):
- Bookerly 16-regular: 440 KB (Latin+extended Latin, 4 weights × 4 sizes = ~7 MB spread across sizes)
- Noto Sans 16-regular: 340 KB (Latin-only)

Rough glyph counts and size estimates for Japanese at 16px, frequency-compressed:

| Glyph set | Count | Est. per size/weight | 4 sizes × 1 weight | 4 sizes × 4 weights |
| --- | --- | --- | --- | --- |
| Hiragana | 94 | ~12 KB | ~48 KB | ~192 KB |
| Katakana | 93 | ~12 KB | ~48 KB | ~192 KB |
| Kana combined | 187 | ~24 KB | ~96 KB | ~384 KB |
| Jōyō kanji | 2,136 | ~280 KB | ~1.1 MB | ~4.5 MB |
| Full BMP CJK | ~20k | ~2.5 MB | ~10 MB | ~40 MB |

**Conclusion**: Kana alone fits in app flash comfortably (even 4 weights × 4 sizes stays under 400 KB, close to our 380 KB headroom — realistically ship regular + bold only). Kanji at any useful size must live in SPIFFS.

---

## Phase 1 — Kana ship (recommended first)

**Scope**: Japanese text that uses only Hiragana and Katakana renders correctly and line-breaks sensibly. Kanji displays as tofu (missing glyph) — honest fallback, not a crash.

**Why ship this first**: It's cheap, self-contained, and exercises every pipeline stage (converter → compressor → decompressor → line-break → layout). Validates the whole path before spending effort on the kanji loader.

### Changes

1. **Font converter** — enable Hiragana (0x3040-0x309F) + Katakana (0x30A0-0x30FF) ranges in `fontconvert.py`. Regenerate `notosans_*` headers. Or, better: introduce a new family `notosansjp_*` with Latin + kana ranges so existing Noto Sans users don't pay a size tax. Consumes ~200 KB app flash for regular+bold at 4 sizes.

2. **Line-break logic** — in `ChapterHtmlSlimParser.cpp` word-buffer flush path, add: "if the next codepoint is in a CJK range (U+3000–U+30FF, U+4E00–U+9FFF, plus extensions), the current word ends here." This is a ~20-line function. Unicode line-break class ID treats every CJK codepoint as a break opportunity both before and after — we don't need a full break-table, just a range test.

   Edge cases to handle:
   - No break before closing punctuation (`、。」』）】`) — "non-starter" class NS
   - No break after opening punctuation (`「『（【`) — "non-ender" class OP
   - Cover these with a small exception table (~30 codepoints).

3. **Font picker** — add `NOTO_SANS_JP` to `FONT_FAMILY` enum and `SettingsList.h`. Add `STR_NOTO_SANS_JP` to i18n.

4. **No changes needed** to FontDecompressor — frequency-grouped path already supports this.

**Flash cost**: ~200 KB app flash (notosansjp family, regular+bold, 4 sizes). Stays under 380 KB headroom with margin. No SPIFFS usage.

**Risk**: Low. The compression/decompression path is already validated for the range; we're just turning it on.

---

## Phase 2 — Kanji via SPIFFS font loader

**Scope**: Full Jōyō kanji (~2,136 glyphs) available when user picks the Japanese font, loaded from SPIFFS at runtime. Keeps app flash footprint unchanged.

### Changes

1. **SPIFFS font format** — introduce `.bin` wrapper around the existing EpdFont structure: header (magic, version, glyph count, size, weight), then the frequency-grouped glyph tables currently emitted to `.h` files. Write a small Python script `pack_font_bin.py` that converts the converter's output to this binary format.

2. **Data partition build step** — update `partitions.csv` if needed (already has 3 MB spiffs), and wire a PlatformIO `buildfs` invocation into the release pipeline. Ship `notosansjp_16_regular.bin`, `notosansjp_16_bold.bin` (and others on demand) in `data/fonts/`.

3. **Runtime loader** — new function `EpdFont::loadFromSpiffs(const char* path)` that mmap-style reads the file into a heap-allocated struct with the same shape as the compiled-in fonts. Wire it into `renderer.insertFont()` in `main.cpp` behind a language check (load JP glyphs when user's font is set to Noto Sans JP, or lazily on first CJK codepoint encountered).

4. **Memory strategy** — ESP32-C3 has 400 KB SRAM. Loading a ~280 KB font into RAM is viable but tight. Two options:
   - **Option A**: Keep font in SPIFFS and read on demand per-glyph (slow but memory-light). SPIFFS read latency ~100 µs per access — noticeable on page render but acceptable.
   - **Option B**: Load compressed tables to PSRAM if available, decompress per-glyph. The C3 doesn't have PSRAM by default so check the board.
   - **Recommended**: Option A with a small LRU glyph cache (64 entries × ~100 bytes = 6 KB). Covers most rendered-text hot path.

**Flash cost**: 0 KB app flash. ~1.5 MB SPIFFS for regular+bold at 4 sizes.

**Risk**: Medium. New file format, new loader, memory-pressure tuning on C3.

---

## Phase 3 — Furigana (`<ruby>` rendering)

**Scope**: `<ruby>漢字<rt>かんじ</rt></ruby>` renders the kana reading in a smaller font above the kanji base text. Ubiquitous in Japanese ebooks, especially children's and educational titles.

### HTML model

Standard ruby syntax (abbreviated):
```html
<ruby>漢<rt>かん</rt>字<rt>じ</rt></ruby>
```
or
```html
<ruby>漢字<rp>(</rp><rt>かんじ</rt><rp>)</rp></ruby>
```

`<rt>` = ruby text (the reading). `<rp>` = ruby parenthesis (fallback for non-ruby renderers — we should skip these). `<rb>` = ruby base (optional container for the base characters — we should treat as transparent).

### Changes

1. **Parser** — `ChapterHtmlSlimParser.cpp` `startElement()`. Non-block tags currently fall through to the inline-style handler at line 658. Add a distinct branch for `ruby`: push a RubyContext onto a small stack; emit base characters as normal; when `<rt>` opens, stop appending to the current TextBlock and start a sidecar run; when `</ruby>` closes, emit a `RubyRun` layout primitive. Inside `<rp>`, drop characters entirely.

2. **Layout primitive — `RubyRun`** — new subclass of TextBlock (or parallel primitive) holding: base glyph advances, ruby glyph advances, the position mapping (which base glyphs each ruby run sits over). Advance width = `max(base_width, ruby_width + base_padding)`.

3. **Line-break interaction** — a `RubyRun` is atomic for line-breaking purposes. Break opportunities exist between `RubyRun`s, never inside one. Integrate with the Phase 1 CJK break logic — treat each RubyRun as a single "word."

4. **Rendering** — extend the existing TextBlock render path with a vertical offset pass: base line at y, ruby line at `y - ruby_height - 1px`. Ruby font = 50% of base font size, same family, same weight. At 16px base, ruby is 8px; at 14px, ruby is 7px. Ship a 7px and 8px kana-only variant in the font pack (~15 KB each).

5. **Edge cases**:
   - Ruby alignment when reading is longer than base: extend base spacing (`justify-between`) or extend ruby spacing (`justify-center`) — pick one convention (most Japanese e-readers use center-over-base).
   - Stacked `<ruby>` (rare): just flatten, treat outer `<rt>` as authoritative.
   - Missing `<rt>` content: fall back to plain base rendering.

**Flash cost**: ~30 KB (ruby-size font variants) + small parser code additions. Fits in app flash.

**Risk**: Medium-high. New render primitive touches hot path. Needs careful visual testing on real ebooks.

---

## What I recommend deferring or skipping

- **TinySegmenter (Japanese tokenizer)** — Not needed. Justification tokenization per CJK codepoint is what professional e-readers like Kobo and Kindle do for Japanese. Segmentation buys word-boundary search and better-hyphenation behavior, neither of which is critical for an e-reader's primary render path. ~23 KB code + ~400 KB model; save for a later "find word" feature.
- **Vertical (tategaki) writing** — Traditional Japanese typography, but 90% of modern digital Japanese ebooks are horizontal. Would require a full layout-direction rewrite. Don't do this.
- **Full CJK (Chinese + Korean) in one shot** — The scaffolding from Japanese makes Chinese/Korean almost free later, but shipping all three at once multiplies the SPIFFS footprint and the testing surface. Japan first, others as follow-ups.
- **IME (input method)** — For search / keyboard entry in Japanese. Out of scope; current use case is reading.

---

## Interaction with upstream PR #1736 (Bookerly → Noto Serif)

If we eventually adopt upstream's Bookerly→Noto Serif switch:
- **Pro**: Noto Serif JP exists as a companion font with identical design language. The font-family line-up becomes consistent cross-script.
- **Pro**: Noto Serif is smaller than Bookerly (trims ~1–2 MB of app flash), which **directly recovers the budget** we'd spend on Phase 1 kana.
- **Con**: Noto Serif doesn't feel as book-like as Bookerly for English prose — that's why we left #1736 unpicked.
- **Recommendation**: Decide Phase 1 font family before committing. If we want Japanese to look like part of the same family as English, the cleanest ordering is: adopt Noto Serif upstream → ship Noto Serif JP for Phase 1. If we keep Bookerly for English, pair it with Noto Sans JP for Japanese (design-mismatched but functional).

---

## Concrete Phase 1 implementation checklist

1. Branch `shrike/japanese-kana`
2. `fontconvert.py` — enable Hiragana, Katakana ranges; add a new family variant `notosansjp`
3. Regenerate Noto Sans JP headers: `scripts/convert-builtin-fonts.sh` for the new family
4. Verify compression with `verify_compression.py`
5. Add `NOTO_SANS_JP` to `FONT_FAMILY` enum + `SettingsList.h` + `STR_NOTO_SANS_JP` i18n string
6. Register font in `main.cpp` `renderer.insertFont(...)`
7. `ChapterHtmlSlimParser.cpp` — add `isCjkBreakable(codepoint)` + integrate into word buffer flush
8. Add tiny exception table for NS/OP punctuation
9. Test with a Japanese epub (need one — any Aozora Bunko public-domain text works)
10. Measure post-build flash usage, confirm we stay under 95% app0
11. Commit as `shrike: add kana support`, merge to master, cut v1.4.0

---

## Bottom line

The engineering path is clean and the codebase is friendlier to this than it looks — prior CrossPoint devs left the scaffolding in place. The hard part is the flash budget, and the SPIFFS escape hatch solves it cleanly for Phase 2. Phase 3 (furigana) is the most novel work but is a pure layout concern, well-contained.

My recommendation: do Phase 1 next-next after the refresh-policy tightening (current side-quest #4 in the 7-plan). It's a one-day job, demos beautifully, and is a meaningful competitive feature versus stock Kindle at this price point.
