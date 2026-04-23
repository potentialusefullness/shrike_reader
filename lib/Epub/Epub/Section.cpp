#include "Section.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include "Epub/css/CssParser.h"
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint8_t SECTION_FILE_VERSION = 21;
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) +
                                 sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);

struct PageLutEntry {
  uint32_t fileOffset;
  uint16_t paragraphIndex;
};
}  // namespace

Section::Section(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer& renderer)
    : epub(epub),
      spineIndex(spineIndex),
      renderer(renderer),
      filePath(epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".bin") {
  fileMutex_ = xSemaphoreCreateMutex();
}

Section::~Section() {
  // Cancel and join any preload task before tearing down state it references.
  joinPreloadTask();
  // Release the persistent read handle opened by loadSectionFile /
  // createSectionFile. Safe to call on an already-closed handle.
  if (file) {
    file.close();
  }
  if (fileMutex_) {
    vSemaphoreDelete(fileMutex_);
    fileMutex_ = nullptr;
  }
}

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", pageCount);
    return 0;
  }

  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", pageCount);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed", pageCount);

  pageCount++;
  return position;
}

void Section::writeSectionFileHeader(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                     const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                     const uint16_t viewportHeight, const bool hyphenationEnabled,
                                     const bool embeddedStyle, const uint8_t imageRendering) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_FILE_VERSION) + sizeof(fontId) + sizeof(lineCompression) +
                                   sizeof(extraParagraphSpacing) + sizeof(paragraphAlignment) + sizeof(viewportWidth) +
                                   sizeof(viewportHeight) + sizeof(pageCount) + sizeof(hyphenationEnabled) +
                                   sizeof(embeddedStyle) + sizeof(imageRendering) + sizeof(uint32_t) +
                                   sizeof(uint32_t) + sizeof(uint32_t),
                "Header size mismatch");
  serialization::writePod(file, SECTION_FILE_VERSION);
  serialization::writePod(file, fontId);
  serialization::writePod(file, lineCompression);
  serialization::writePod(file, extraParagraphSpacing);
  serialization::writePod(file, paragraphAlignment);
  serialization::writePod(file, viewportWidth);
  serialization::writePod(file, viewportHeight);
  serialization::writePod(file, hyphenationEnabled);
  serialization::writePod(file, embeddedStyle);
  serialization::writePod(file, imageRendering);
  serialization::writePod(file, pageCount);  // Placeholder for page count (will be initially 0, patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for anchor map offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for paragraph LUT offset (patched later)
}

bool Section::loadSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                              const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                              const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                              const uint8_t imageRendering) {
  // Reset any state from a prior load attempt.
  pageLut_.clear();
  anchorMap_.clear();
  paragraphLut_.clear();
  if (file) {
    file.close();
  }

  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  // Match parameters
  {
    uint8_t version;
    serialization::readPod(file, version);
    if (version != SECTION_FILE_VERSION) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u", version);
      clearCache();
      return false;
    }

    int fileFontId;
    uint16_t fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    bool fileExtraParagraphSpacing;
    uint8_t fileParagraphAlignment;
    bool fileHyphenationEnabled;
    bool fileEmbeddedStyle;
    uint8_t fileImageRendering;
    serialization::readPod(file, fileFontId);
    serialization::readPod(file, fileLineCompression);
    serialization::readPod(file, fileExtraParagraphSpacing);
    serialization::readPod(file, fileParagraphAlignment);
    serialization::readPod(file, fileViewportWidth);
    serialization::readPod(file, fileViewportHeight);
    serialization::readPod(file, fileHyphenationEnabled);
    serialization::readPod(file, fileEmbeddedStyle);
    serialization::readPod(file, fileImageRendering);

    if (fontId != fileFontId || lineCompression != fileLineCompression ||
        extraParagraphSpacing != fileExtraParagraphSpacing || paragraphAlignment != fileParagraphAlignment ||
        viewportWidth != fileViewportWidth || viewportHeight != fileViewportHeight ||
        hyphenationEnabled != fileHyphenationEnabled || embeddedStyle != fileEmbeddedStyle ||
        imageRendering != fileImageRendering) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
      clearCache();
      return false;
    }
  }

  serialization::readPod(file, pageCount);
  uint32_t pageLutOffset;
  uint32_t anchorMapOffset;
  uint32_t paragraphLutOffset;
  serialization::readPod(file, pageLutOffset);
  serialization::readPod(file, anchorMapOffset);
  serialization::readPod(file, paragraphLutOffset);

  // Shrike: pull all three LUTs into RAM up-front so page turns, anchor jumps,
  // and paragraph lookups no longer need any SD seek. Keep `file` open so the
  // next loadPageFromSectionFile call can skip the open+header walk entirely.
  if (!readLutsFromFile(pageLutOffset, anchorMapOffset, paragraphLutOffset)) {
    file.close();
    pageLut_.clear();
    anchorMap_.clear();
    paragraphLut_.clear();
    LOG_ERR("SCT", "Deserialization failed: LUT read error");
    clearCache();
    return false;
  }

  LOG_DBG("SCT", "Deserialization succeeded: %d pages (LUTs cached in RAM)", pageCount);
  return true;
}

bool Section::readLutsFromFile(const uint32_t pageLutOffset, const uint32_t anchorMapOffset,
                               const uint32_t paragraphLutOffset) {
  if (!file) {
    return false;
  }

  // Page LUT: one uint32_t per page, in page order.
  pageLut_.resize(pageCount);
  if (pageCount > 0) {
    if (!file.seek(pageLutOffset)) {
      return false;
    }
    for (uint16_t i = 0; i < pageCount; i++) {
      uint32_t off;
      serialization::readPod(file, off);
      if (off == 0) {
        LOG_ERR("SCT", "Page LUT has invalid entry at %u", i);
        return false;
      }
      pageLut_[i] = off;
    }
  }

  // Anchor map: uint16 count, then <string, uint16> pairs.
  if (anchorMapOffset != 0) {
    if (!file.seek(anchorMapOffset)) {
      return false;
    }
    uint16_t count;
    serialization::readPod(file, count);
    anchorMap_.reserve(count);
    for (uint16_t i = 0; i < count; i++) {
      std::string key;
      uint16_t page;
      serialization::readString(file, key);
      serialization::readPod(file, page);
      anchorMap_.emplace_back(std::move(key), page);
    }
  }

  // Paragraph LUT: uint16 count, then one uint16 per page.
  if (paragraphLutOffset != 0) {
    if (!file.seek(paragraphLutOffset)) {
      return false;
    }
    uint16_t count;
    serialization::readPod(file, count);
    paragraphLut_.resize(count);
    for (uint16_t i = 0; i < count; i++) {
      serialization::readPod(file, paragraphLut_[i]);
    }
  }

  return true;
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
  if (!Storage.exists(filePath.c_str())) {
    LOG_DBG("SCT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("SCT", "Cache cleared successfully");
  return true;
}

bool Section::createSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                                const uint8_t imageRendering, const std::function<void()>& popupFn) {
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // Retry logic for SD card timing issues
  bool success = false;
  uint32_t fileSize = 0;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
    if (attempt > 0) {
      LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
      delay(50);  // Brief delay before retry
    }

    // Remove any incomplete file from previous attempt before retrying
    if (Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }

    FsFile tmpHtml;
    if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
      continue;
    }
    success = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
    fileSize = tmpHtml.size();
    // Explicitly close() file before calling Storage.remove()
    tmpHtml.close();

    // If streaming failed, remove the incomplete file immediately
    if (!success && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
      LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
    }
  }

  if (!success) {
    LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
    return false;
  }

  LOG_DBG("SCT", "Streamed temp HTML to %s (%d bytes)", tmpHtmlPath.c_str(), fileSize);

  if (!Storage.openFileForWrite("SCT", filePath, file)) {
    return false;
  }
  writeSectionFileHeader(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                         viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering);
  std::vector<PageLutEntry> lut = {};

  // Derive the content base directory and image cache path prefix for the parser
  size_t lastSlash = localPath.find_last_of('/');
  std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  std::string imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  CssParser* cssParser = nullptr;
  if (embeddedStyle) {
    cssParser = epub->getCssParser();
    if (cssParser) {
      if (!cssParser->loadFromCache()) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
    }
  }

  ChapterHtmlSlimParser visitor(
      epub, tmpHtmlPath, renderer, fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
      viewportHeight, hyphenationEnabled,
      [this, &lut](std::unique_ptr<Page> page, const uint16_t paragraphIndex) {
        lut.push_back({this->onPageComplete(std::move(page)), paragraphIndex});
      },
      embeddedStyle, contentBase, imageBasePath, imageRendering, popupFn, cssParser);
  Hyphenator::setPreferredLanguage(epub->getLanguage());
  success = visitor.parseAndBuildPages();

  Storage.remove(tmpHtmlPath.c_str());
  if (!success) {
    LOG_ERR("SCT", "Failed to parse XML and build pages");
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(filePath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }

  const uint32_t lutOffset = file.position();
  bool hasFailedLutRecords = false;
  // Write LUT
  for (const auto& entry : lut) {
    if (entry.fileOffset == 0) {
      hasFailedLutRecords = true;
      break;
    }
    serialization::writePod(file, entry.fileOffset);
  }

  if (hasFailedLutRecords) {
    LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(filePath.c_str());
    return false;
  }

  // Write anchor-to-page map for fragment navigation (e.g. footnote targets)
  const uint32_t anchorMapOffset = file.position();
  const auto& anchors = visitor.getAnchors();
  serialization::writePod(file, static_cast<uint16_t>(anchors.size()));
  for (const auto& [anchor, page] : anchors) {
    serialization::writeString(file, anchor);
    serialization::writePod(file, page);
  }

  const uint32_t paragraphLutOffset = file.position();
  serialization::writePod(file, static_cast<uint16_t>(lut.size()));
  for (const auto& entry : lut) {
    serialization::writePod(file, entry.paragraphIndex);
  }

  // Patch header with final pageCount, lutOffset, anchorMapOffset, and paragraphLutOffset
  file.seek(HEADER_SIZE - sizeof(uint32_t) * 3 - sizeof(pageCount));
  serialization::writePod(file, pageCount);
  serialization::writePod(file, lutOffset);
  serialization::writePod(file, anchorMapOffset);
  serialization::writePod(file, paragraphLutOffset);

  // Shrike: publish the in-RAM LUT copies directly from the build state so the
  // very first page turn after a fresh build skips the usual re-open + seek
  // path. pageLut_ mirrors the on-disk page offsets in page order;
  // paragraphLut_ and anchorMap_ mirror their on-disk counterparts.
  pageLut_.clear();
  pageLut_.reserve(lut.size());
  paragraphLut_.clear();
  paragraphLut_.reserve(lut.size());
  for (const auto& entry : lut) {
    pageLut_.push_back(entry.fileOffset);
    paragraphLut_.push_back(entry.paragraphIndex);
  }
  anchorMap_.clear();
  anchorMap_.reserve(anchors.size());
  for (const auto& [anchor, page] : anchors) {
    anchorMap_.emplace_back(anchor, page);
  }

  // Close the write handle and re-open read-only so page turns can read
  // without another open round-trip. This matches loadSectionFile's
  // post-conditions (file kept open for reads).
  file.close();
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    LOG_ERR("SCT", "Failed to re-open section file for reads after build");
    // Not fatal - loadPageFromSectionFile falls back to opening on demand.
  }
  if (cssParser) {
    cssParser->clear();
  }
  return true;
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() {
  if (currentPage < 0 || static_cast<size_t>(currentPage) >= pageLut_.size()) {
    LOG_ERR("SCT", "Page %d out of LUT range (size %u)", currentPage, static_cast<unsigned>(pageLut_.size()));
    return nullptr;
  }

  // Shrike v1.7.2: if a preload task already deserialized the page we are
  // about to show, steal it and skip the SD round-trip entirely. Any running
  // preload for a different page is cancelled + joined so we don't race on
  // `file` below.
  if (fileMutex_) {
    xSemaphoreTake(fileMutex_, portMAX_DELAY);
    if (preloadedPage_ && preloadedPageNumber_ == currentPage) {
      auto hit = std::move(preloadedPage_);
      preloadedPageNumber_ = -1;
      xSemaphoreGive(fileMutex_);
      LOG_DBG("SCT", "Preload hit for page %d", currentPage);
      return hit;
    }
    // Slot has the wrong page (or nothing) - drop it before we touch `file`.
    preloadedPage_.reset();
    preloadedPageNumber_ = -1;
    xSemaphoreGive(fileMutex_);
  }
  joinPreloadTask();

  // Shrike: page offsets are resolved against the in-RAM pageLut_ populated
  // during loadSectionFile / createSectionFile, so we skip the previous
  // open + seek-to-header + read-LUT-entry + seek-to-page chain entirely.
  // If the persistent handle was closed out-of-band (very unlikely), fall
  // back to opening on demand so we remain robust.
  if (fileMutex_) {
    xSemaphoreTake(fileMutex_, portMAX_DELAY);
  }
  std::unique_ptr<Page> result;
  if (!file && !Storage.openFileForRead("SCT", filePath, file)) {
    if (fileMutex_) xSemaphoreGive(fileMutex_);
    return nullptr;
  }
  if (!file.seek(pageLut_[currentPage])) {
    LOG_ERR("SCT", "Seek to page %d (offset %u) failed", currentPage, pageLut_[currentPage]);
    if (fileMutex_) xSemaphoreGive(fileMutex_);
    return nullptr;
  }
  result = Page::deserialize(file);
  if (fileMutex_) xSemaphoreGive(fileMutex_);
  return result;
}

void Section::preloadTaskTrampoline(void* arg) {
  auto* self = static_cast<Section*>(arg);
  // The page number we should preload is stashed in preloadedPageNumber_ by
  // preloadPage() before xTaskCreate. We read it once here.
  const int target = self->preloadedPageNumber_;
  self->runPreload(target);
  // Self-delete - main thread joins via joinPreloadTask() which also nulls
  // preloadTask_.
  vTaskDelete(nullptr);
}

void Section::runPreload(int pageNumber) {
  if (preloadCancel_.load()) return;
  if (pageNumber < 0 || static_cast<size_t>(pageNumber) >= pageLut_.size()) return;

  if (!fileMutex_) return;
  xSemaphoreTake(fileMutex_, portMAX_DELAY);
  if (preloadCancel_.load()) {
    xSemaphoreGive(fileMutex_);
    return;
  }
  if (!file && !Storage.openFileForRead("SCT", filePath, file)) {
    xSemaphoreGive(fileMutex_);
    return;
  }
  if (!file.seek(pageLut_[pageNumber])) {
    xSemaphoreGive(fileMutex_);
    return;
  }
  auto page = Page::deserialize(file);
  if (!preloadCancel_.load() && page) {
    preloadedPage_ = std::move(page);
    preloadedPageNumber_ = pageNumber;
    LOG_DBG("SCT", "Preloaded page %d", pageNumber);
  }
  xSemaphoreGive(fileMutex_);
}

void Section::joinPreloadTask() {
  if (!preloadTask_) return;
  preloadCancel_.store(true);
  // The task may be blocked on fileMutex_ or deep inside SdFat - we cannot
  // reliably signal mid-read. Wait for it to complete normally; the cancel
  // flag prevents it from touching preloadedPage_ on the way out.
  TaskHandle_t h = preloadTask_;
  while (eTaskGetState(h) != eDeleted && eTaskGetState(h) != eInvalid) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  preloadTask_ = nullptr;
  preloadCancel_.store(false);
}

void Section::preloadPage(int pageNumber) {
  if (pageNumber < 0 || static_cast<size_t>(pageNumber) >= pageLut_.size()) return;

  // Already holding the right page? Nothing to do.
  if (fileMutex_) {
    xSemaphoreTake(fileMutex_, portMAX_DELAY);
    const bool alreadyHave = preloadedPage_ && preloadedPageNumber_ == pageNumber;
    xSemaphoreGive(fileMutex_);
    if (alreadyHave) return;
  }

  // Stop any stale preload, then launch a new one.
  joinPreloadTask();
  preloadedPageNumber_ = pageNumber;  // task trampoline reads this
  preloadedPage_.reset();

  // 4 KB stack, priority below the main Arduino loop (1). Pinned to core 0
  // (the only core on C3). Task self-deletes via trampoline.
  if (xTaskCreate(&Section::preloadTaskTrampoline, "shrike_preload", 4096, this,
                  tskIDLE_PRIORITY + 1, &preloadTask_) != pdPASS) {
    LOG_ERR("SCT", "Preload task create failed for page %d", pageNumber);
    preloadTask_ = nullptr;
    preloadedPageNumber_ = -1;
  }
}

void Section::invalidatePreload() {
  joinPreloadTask();
  if (fileMutex_) xSemaphoreTake(fileMutex_, portMAX_DELAY);
  preloadedPage_.reset();
  preloadedPageNumber_ = -1;
  if (fileMutex_) xSemaphoreGive(fileMutex_);
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  // Shrike: anchor map is held in RAM after loadSectionFile / createSectionFile,
  // so this lookup no longer touches the SD card at all.
  for (const auto& [key, page] : anchorMap_) {
    if (key == anchor) {
      return page;
    }
  }
  return std::nullopt;
}

std::optional<uint16_t> Section::getPageForParagraphIndex(const uint16_t pIndex) const {
  // Shrike: paragraph LUT lives in RAM after section load.
  if (paragraphLut_.empty()) {
    return std::nullopt;
  }
  uint16_t resultPage = static_cast<uint16_t>(paragraphLut_.size() - 1);
  for (size_t i = 0; i < paragraphLut_.size(); i++) {
    if (paragraphLut_[i] >= pIndex) {
      resultPage = static_cast<uint16_t>(i);
      break;
    }
  }
  return resultPage;
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) const {
  // Shrike: paragraph LUT lives in RAM after section load.
  if (page >= paragraphLut_.size()) {
    return std::nullopt;
  }
  return paragraphLut_[page];
}
