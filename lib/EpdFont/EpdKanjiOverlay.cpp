#include "EpdKanjiOverlay.h"

#include <HalStorage.h>
#include <InflateReader.h>
#include <Logging.h>

#include <cstring>

static constexpr char TAG[] = "KJI";

namespace {
constexpr uint8_t EPKF_MAGIC[4] = {'E', 'P', 'K', 'F'};
constexpr uint8_t FILE_HEADER_SIZE = 64;
constexpr uint8_t TILE_ENTRY_SIZE = 10;
constexpr uint8_t GLYPH_ENTRY_SIZE = 14;

// Tiny global registry of open overlays. Kept small and scanned linearly
// in GfxRenderer::getGlyphBitmap — with at most a handful of registered
// overlays, the linear scan is cheaper than any map / hashing machinery.
EpdKanjiOverlay* g_registered[EpdKanjiOverlay::MAX_REGISTERED] = {nullptr};

template <typename T>
bool readLE(HalFile& f, T& out) {
  return f.read(&out, sizeof(T)) == static_cast<int>(sizeof(T));
}

// Number of set bits below position `bit` within `mask` (i.e. popcount of
// mask & ((1 << bit) - 1)). Used for the tile-index → glyphIdx math.
inline uint32_t popcountBelow(uint64_t mask, unsigned bit) {
  if (bit == 0) return 0;
  const uint64_t lo = mask & ((uint64_t{1} << bit) - 1);
  return static_cast<uint32_t>(__builtin_popcountll(lo));
}
}  // namespace

EpdKanjiOverlay::EpdKanjiOverlay() {
  for (auto& slot : g_registered) {
    if (slot == nullptr) {
      slot = this;
      return;
    }
  }
  LOG_ERR(TAG, "registry full - overlay bitmaps won't be resolvable");
}

EpdKanjiOverlay::~EpdKanjiOverlay() {
  for (auto& slot : g_registered) {
    if (slot == this) {
      slot = nullptr;
      break;
    }
  }
  close();
}

const uint8_t* EpdKanjiOverlay::registryFindBitmap(const EpdGlyph* glyph) {
  if (!glyph) return nullptr;
  for (auto* o : g_registered) {
    if (o == nullptr) continue;
    const uint8_t* bm = o->getBitmapFor(glyph);
    if (bm != nullptr) return bm;
  }
  return nullptr;
}

bool EpdKanjiOverlay::open(const char* path) {
  close();
  auto file = std::make_unique<HalFile>();
  if (!Storage.openFileForRead(TAG, path, *file)) {
    LOG_INF(TAG, "open: %s not found", path);
    return false;
  }
  file_ = std::move(file);

  Header hdr;
  if (!readHeader(hdr)) {
    LOG_ERR(TAG, "bad header: %s", path);
    close();
    return false;
  }
  if (!readTileIndex(hdr.tileIndexOffset)) {
    LOG_ERR(TAG, "bad tile index: %s", path);
    close();
    return false;
  }
  glyphTableOffset_ = hdr.glyphTableOffset;
  glyphCount_ = hdr.glyphCount;
  fontSize_ = hdr.fontSize;
  ascender_ = hdr.ascent;
  descender_ = hdr.descent;
  advanceY_ = hdr.advanceY;
  LOG_INF(TAG, "opened %s  size=%u  glyphs=%lu", path, fontSize_, static_cast<unsigned long>(glyphCount_));
  return true;
}

void EpdKanjiOverlay::close() {
  if (file_) {
    file_->close();
    file_.reset();
  }
  // Clear cache so stale pointers are never returned after a close/reopen.
  for (auto& slot : slots_) {
    slot.codepoint = 0;
    slot.lastUsed = 0;
    slot.bitmapSize = 0;
  }
  lruTick_ = 0;
  tiles_.fill(Tile{});
  glyphTableOffset_ = 0;
  glyphCount_ = 0;
  fontSize_ = 0;
  ascender_ = descender_ = advanceY_ = 0;
}

bool EpdKanjiOverlay::readHeader(Header& hdr) {
  if (!file_) return false;
  file_->seekSet(0);
  uint8_t magic[4];
  if (file_->read(magic, 4) != 4) return false;
  if (std::memcmp(magic, EPKF_MAGIC, 4) != 0) return false;

  if (!readLE(*file_, hdr.version)) return false;
  if (hdr.version != 1) {
    LOG_ERR(TAG, "unsupported EPKF version %u", hdr.version);
    return false;
  }
  if (!readLE(*file_, hdr.flags)) return false;
  if (!readLE(*file_, hdr.fontSize)) return false;
  if (!readLE(*file_, hdr.ascent)) return false;
  if (!readLE(*file_, hdr.descent)) return false;
  if (!readLE(*file_, hdr.advanceY)) return false;
  if (!readLE(*file_, hdr.glyphCount)) return false;
  if (!readLE(*file_, hdr.tileCount)) return false;
  if (hdr.tileCount != TILE_COUNT) {
    LOG_ERR(TAG, "tileCount mismatch: file=%u expected=%u", hdr.tileCount, TILE_COUNT);
    return false;
  }
  if (!readLE(*file_, hdr.tileFirstCp)) return false;
  if (!readLE(*file_, hdr.tileIndexOffset)) return false;
  if (!readLE(*file_, hdr.glyphTableOffset)) return false;
  if (!readLE(*file_, hdr.bitmapPoolOffset)) return false;
  if (!readLE(*file_, hdr.bitmapPoolSize)) return false;
  return true;
}

bool EpdKanjiOverlay::readTileIndex(uint32_t off) {
  if (!file_) return false;
  if (!file_->seekSet(off)) return false;
  for (uint16_t t = 0; t < TILE_COUNT; t++) {
    uint64_t presence;
    uint16_t firstGlyphId;
    if (!readLE(*file_, presence)) return false;
    if (!readLE(*file_, firstGlyphId)) return false;
    tiles_[t] = {presence, firstGlyphId};
  }
  return true;
}

bool EpdKanjiOverlay::hasGlyph(uint32_t cp) const {
  if (cp < RANGE_START || cp > RANGE_END) return false;
  if (!file_) return false;
  const uint32_t off = cp - RANGE_START;
  const uint32_t t = off >> 6;
  const unsigned b = off & 63;
  return (tiles_[t].presence >> b) & 1u;
}

bool EpdKanjiOverlay::resolveGlyphIndex(uint32_t cp, uint32_t& glyphIdxOut) const {
  if (!hasGlyph(cp)) return false;
  const uint32_t off = cp - RANGE_START;
  const uint32_t t = off >> 6;
  const unsigned b = off & 63;
  const Tile& tile = tiles_[t];
  glyphIdxOut = tile.firstGlyphId + popcountBelow(tile.presence, b);
  return true;
}

size_t EpdKanjiOverlay::pickEvictionVictim() {
  size_t victim = 0;
  uint32_t oldest = UINT32_MAX;
  for (size_t i = 0; i < slots_.size(); i++) {
    if (slots_[i].codepoint == 0) return i;
    if (slots_[i].lastUsed < oldest) {
      oldest = slots_[i].lastUsed;
      victim = i;
    }
  }
  return victim;
}

bool EpdKanjiOverlay::loadIntoSlot(uint32_t cp, uint32_t glyphIdx, Slot& slot) {
  if (!file_ || glyphIdx >= glyphCount_) return false;

  // Read the glyph record.
  const uint32_t recOff = glyphTableOffset_ + glyphIdx * GLYPH_ENTRY_SIZE;
  if (!file_->seekSet(recOff)) return false;
  uint8_t width, height;
  uint16_t advanceX;
  int16_t left, top;
  uint16_t dataLength;
  uint32_t dataOffset;
  if (!readLE(*file_, width) || !readLE(*file_, height) || !readLE(*file_, advanceX) || !readLE(*file_, left) ||
      !readLE(*file_, top) || !readLE(*file_, dataLength) || !readLE(*file_, dataOffset)) {
    return false;
  }

  // Sanity: a corrupt file mustn't overflow our bitmap slot.
  const size_t rowBytes = (static_cast<size_t>(width) + 3) / 4;
  const size_t decompressed = rowBytes * height;
  if (decompressed > slot.bitmap.size()) {
    LOG_ERR(TAG, "glyph U+%04lX too large: %u bytes > %u cache slot",
            static_cast<unsigned long>(cp), static_cast<unsigned>(decompressed),
            static_cast<unsigned>(slot.bitmap.size()));
    return false;
  }

  // Load the compressed blob into a small stack buffer (max observed ~300 B,
  // we cap at 1 KB; anything bigger indicates corruption).
  if (dataLength == 0 || dataLength > 1024) {
    LOG_ERR(TAG, "implausible dataLength %u for U+%04lX", dataLength, static_cast<unsigned long>(cp));
    return false;
  }
  uint8_t compBuf[1024];
  if (!file_->seekSet(dataOffset)) return false;
  if (file_->read(compBuf, dataLength) != static_cast<int>(dataLength)) return false;

  // Inflate into the slot's bitmap buffer.
  InflateReader inflater;
  if (!inflater.init(false)) return false;
  inflater.setSource(compBuf, dataLength);
  if (!inflater.read(slot.bitmap.data(), decompressed)) {
    LOG_ERR(TAG, "inflate failed for U+%04lX", static_cast<unsigned long>(cp));
    return false;
  }

  slot.codepoint = cp;
  slot.lastUsed = ++lruTick_;
  // Note: the EpdGlyph dataOffset field is unused for overlay glyphs — the
  // bitmap comes from the slot itself via getBitmapFor(). We zero it so
  // any accidental dereference through fontData->bitmap faults cleanly.
  slot.glyph.width = width;
  slot.glyph.height = height;
  slot.glyph.advanceX = advanceX;
  slot.glyph.left = left;
  slot.glyph.top = top;
  slot.glyph.dataLength = dataLength;
  slot.glyph.dataOffset = 0;
  slot.bitmapSize = static_cast<uint16_t>(decompressed);
  return true;
}

const EpdGlyph* EpdKanjiOverlay::getGlyph(uint32_t cp) {
  if (!hasGlyph(cp)) return nullptr;

  // Cache probe.
  for (auto& slot : slots_) {
    if (slot.codepoint == cp) {
      slot.lastUsed = ++lruTick_;
      return &slot.glyph;
    }
  }

  // Miss: resolve and load into the LRU victim.
  uint32_t glyphIdx = 0;
  if (!resolveGlyphIndex(cp, glyphIdx)) return nullptr;
  const size_t idx = pickEvictionVictim();
  Slot& slot = slots_[idx];
  if (!loadIntoSlot(cp, glyphIdx, slot)) {
    slot.codepoint = 0;
    return nullptr;
  }
  return &slot.glyph;
}

const uint8_t* EpdKanjiOverlay::getBitmapFor(const EpdGlyph* glyph) const {
  if (!glyph) return nullptr;
  for (const auto& slot : slots_) {
    if (&slot.glyph == glyph) {
      if (slot.bitmapSize == 0) return nullptr;
      return slot.bitmap.data();
    }
  }
  return nullptr;
}
