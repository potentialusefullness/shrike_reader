#pragma once
#include "EpdFontData.h"

class EpdKanjiOverlay;

class EpdFont {
  void getTextBounds(const char* string, int startX, int startY, int* minX, int* minY, int* maxX, int* maxY) const;

 public:
  const EpdFontData* data;
  // Optional SD-backed kanji overlay. Set via setKanjiOverlay(); a nullptr
  // means "no overlay attached" (default). Mutable state at the EpdFont
  // level (not EpdFontData) so the in-flash const EpdFontData structs
  // stay immutable — they live in .rodata / flash.
  EpdKanjiOverlay* kanjiOverlay = nullptr;
  void setKanjiOverlay(EpdKanjiOverlay* overlay) { kanjiOverlay = overlay; }

  explicit EpdFont(const EpdFontData* data) : data(data) {}
  ~EpdFont() = default;
  void getTextDimensions(const char* string, int* w, int* h) const;

  const EpdGlyph* getGlyph(uint32_t cp) const;

  /// Returns the kerning adjustment (4.4 fixed-point in pixels) between two codepoints.
  /// Returns 0 if no kerning data exists for the pair.
  int8_t getKerning(uint32_t leftCp, uint32_t rightCp) const;

  /// Returns the ligature codepoint for a pair, or 0 if no ligature exists.
  uint32_t getLigature(uint32_t leftCp, uint32_t rightCp) const;

  /// Greedily applies ligature substitutions starting from cp, consuming
  /// as many following codepoints from text as possible. Returns the
  /// (possibly substituted) codepoint; advances text past consumed chars.
  uint32_t applyLigatures(uint32_t cp, const char*& text) const;
};
