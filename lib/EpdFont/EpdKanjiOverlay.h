#pragma once

// EpdKanjiOverlay — SD-card-backed font overlay for CJK Unified Ideographs
// (U+4E00..U+9FFF). Reads an .epkf container produced by
// scripts/build-kanji-overlay.py and serves glyphs through a small LRU
// cache so that EpdFont::getGlyph() can transparently fall back to SD
// when a codepoint is outside the in-flash coverage.
//
// Design summary (see docs/japanese_phase2_kanji_proposal.md for the
// full write-up):
//   • Tile bitmap index (328 × 64 codepoints) resident in RAM (~3.3 KB).
//   • Per-glyph raw DEFLATE in the pool; each glyph is <300 B typical.
//   • LRU cache of recent glyphs — each slot owns an EpdGlyph record
//     plus its decompressed 2-bit bitmap. Pointers into the cache are
//     stable for the lifetime of the slot.
//
// NOTE on identity: this is a FORK of crosspoint. Symbol names stay
// CrossPoint-flavoured where they already exist; new types here use
// Epd* prefix to match the rest of the font subsystem.

#include <array>
#include <cstdint>
#include <memory>

#include "EpdFontData.h"

class HalFile;

class EpdKanjiOverlay {
 public:
  // Codepoint range covered by the tile index.
  static constexpr uint32_t RANGE_START = 0x4E00;
  static constexpr uint32_t RANGE_END = 0x9FFF;  // inclusive
  static constexpr uint16_t TILE_BITS = 64;
  static constexpr uint16_t TILE_COUNT = (RANGE_END - RANGE_START + 1 + TILE_BITS - 1) / TILE_BITS;  // 328

  // Cache depth — 64 glyph slots ≈ 64 × (14 + ~256) B = ~17 KB.
  // A 480×800 portrait page holds roughly 30–50 kanji at 16 px, so this
  // covers the current page twice over.
  static constexpr uint16_t CACHE_SLOTS = 64;

  EpdKanjiOverlay();
  ~EpdKanjiOverlay();

  EpdKanjiOverlay(const EpdKanjiOverlay&) = delete;
  EpdKanjiOverlay& operator=(const EpdKanjiOverlay&) = delete;

  // Open an .epkf file from HAL storage (SD card). Returns true on success.
  // After open(), hasGlyph()/getGlyph() can be called. Reads the header +
  // tile index into RAM; the bitmap pool stays on disk.
  bool open(const char* path);

  // Close the file and release RAM (except the resident tile index cache,
  // which is cleared too).
  void close();

  bool isOpen() const { return file_ != nullptr; }

  // Fast presence test — O(1). Safe to call before getGlyph() to avoid
  // an unnecessary LRU churn on confirmed-absent codepoints.
  bool hasGlyph(uint32_t cp) const;

  // Fetch (and cache) the glyph for `cp`. Returns nullptr if the
  // codepoint is not covered by this overlay. The returned pointer is
  // stable until the slot is evicted (CACHE_SLOTS-1 subsequent misses).
  const EpdGlyph* getGlyph(uint32_t cp);

  // Return the decompressed 2-bit bitmap matching a previously returned
  // `glyph` pointer from getGlyph(). Returns nullptr if the glyph is not
  // from this overlay or has been evicted.
  const uint8_t* getBitmapFor(const EpdGlyph* glyph) const;

  // --- Global registry ---
  // Every open EpdKanjiOverlay auto-registers itself so that
  // GfxRenderer::getGlyphBitmap() can resolve "which overlay owns this
  // EpdGlyph*" without extra threading of parameters. Registry size is
  // capped small (max one per active font size and style).
  static constexpr size_t MAX_REGISTERED = 4;
  // Find an overlay whose cache currently owns `glyph`, returning the
  // decompressed bitmap. Returns nullptr if no registered overlay owns it.
  static const uint8_t* registryFindBitmap(const EpdGlyph* glyph);

  // Header metrics (valid after open()).
  int ascender() const { return ascender_; }
  int descender() const { return descender_; }
  int advanceY() const { return advanceY_; }
  uint16_t fontSize() const { return fontSize_; }

 private:
  struct Header {
    uint16_t version;
    uint16_t flags;
    uint16_t fontSize;
    int16_t ascent;
    int16_t descent;
    uint16_t advanceY;
    uint32_t glyphCount;
    uint16_t tileCount;
    uint16_t tileFirstCp;
    uint32_t tileIndexOffset;
    uint32_t glyphTableOffset;
    uint32_t bitmapPoolOffset;
    uint32_t bitmapPoolSize;
  };

  struct Tile {
    uint64_t presence;      // bit j set => codepoint (RANGE_START + t*64 + j) present
    uint16_t firstGlyphId;  // index into glyph table of first glyph in this tile
  };

  // LRU cache slot. The slot's `glyph` and `bitmap` are what we hand out.
  struct Slot {
    uint32_t codepoint = 0;   // 0 => empty
    uint32_t lastUsed = 0;    // monotonic tick for LRU eviction
    EpdGlyph glyph = {};
    // Max decompressed bitmap for a 2-bit kanji at 16 px ≈ (33+3)/4 * 33
    // ≈ 297 B. Budget generously to allow headroom for 2x/bold.
    std::array<uint8_t, 512> bitmap = {};
    uint16_t bitmapSize = 0;
  };

  // File-backed state.
  std::unique_ptr<HalFile> file_;
  // Tile index (resident).
  std::array<Tile, TILE_COUNT> tiles_ = {};
  uint32_t glyphTableOffset_ = 0;
  uint32_t glyphCount_ = 0;

  // Header metrics.
  uint16_t fontSize_ = 0;
  int ascender_ = 0;
  int descender_ = 0;
  int advanceY_ = 0;

  // LRU machinery.
  std::array<Slot, CACHE_SLOTS> slots_;
  uint32_t lruTick_ = 0;

  // Helpers.
  bool readHeader(Header& hdr);
  bool readTileIndex(uint32_t off);
  // Resolve (tileIdx, bitIdx) -> glyphIdx within the glyph table.
  bool resolveGlyphIndex(uint32_t cp, uint32_t& glyphIdxOut) const;
  // Load glyph `glyphIdx` into `slot` (reads glyph record + bitmap, inflates).
  bool loadIntoSlot(uint32_t cp, uint32_t glyphIdx, Slot& slot);
  // Evict least-recently-used slot and return its index.
  size_t pickEvictionVictim();
};
