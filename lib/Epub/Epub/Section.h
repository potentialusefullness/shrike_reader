#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Epub.h"

class Page;
class GfxRenderer;

class Section {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  FsFile file;

  // Shrike: in-RAM copies of the on-disk LUTs. Populated once per section load
  // (either by loadSectionFile on a cache hit or by createSectionFile after a
  // fresh build) so page turns and anchor/paragraph lookups become O(1) RAM
  // accesses instead of O(file-open + seek) round-trips through SdFat.
  std::vector<uint32_t> pageLut_;  // file offset of each serialized Page, indexed by page number
  std::vector<std::pair<std::string, uint16_t>> anchorMap_;  // anchor id -> page index
  std::vector<uint16_t> paragraphLut_;  // synthetic paragraph index of the first line of each page

  // Load the three LUTs from `file` into the in-RAM vectors. Assumes `file` is
  // open and positioned anywhere; restores position undefined afterwards. The
  // header must have been read already so the three LUT offsets are known.
  bool readLutsFromFile(uint32_t pageLutOffset, uint32_t anchorMapOffset, uint32_t paragraphLutOffset);

  void writeSectionFileHeader(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                              uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                              bool embeddedStyle, uint8_t imageRendering);
  uint32_t onPageComplete(std::unique_ptr<Page> page);

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  explicit Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
      : epub(epub),
        spineIndex(spineIndex),
        renderer(renderer),
        filePath(epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".bin") {}
  ~Section() {
    // Release the persistent read handle opened by loadSectionFile /
    // createSectionFile. Safe to call on an already-closed handle.
    if (file) {
      file.close();
    }
  }
  bool loadSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                       uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                       uint8_t imageRendering);
  bool clearCache() const;
  bool createSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                         uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                         uint8_t imageRendering, const std::function<void()>& popupFn = nullptr);
  std::unique_ptr<Page> loadPageFromSectionFile();

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;

  // Look up the page number for a synthetic paragraph index from XPath p[N].
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;

  // Look up the synthetic paragraph index for the given rendered page.
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;
};
