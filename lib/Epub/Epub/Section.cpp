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

// Shrike v1.8.1: RAII wrapper around Section::fileMutex_ so every early return
// from a mutex-holding block automatically releases the mutex. The previous
// v1.8.0 code had ~30 manual xSemaphoreTake / xSemaphoreGive call sites spread
// across 10+ early-return branches; one missed give() or an extra give() (e.g.
// on an error branch that also gave earlier) causes FreeRTOS to assert in
// xTaskPriorityDisinherit when the wrong task ends up issuing the give. Using
// a scoped lock makes the pairing impossible to get wrong and removes the
// "give without matching take" class of bug entirely.
class SectionLock {
 public:
  explicit SectionLock(SemaphoreHandle_t mutex) : mutex_(mutex) {
    if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  }
  ~SectionLock() {
    if (mutex_) xSemaphoreGive(mutex_);
  }
  // Manual early release for the rare case where we want to drop the lock
  // before the scope ends (e.g. before a long blocking parse call). Further
  // destruction becomes a no-op.
  void release() {
    if (mutex_) {
      xSemaphoreGive(mutex_);
      mutex_ = nullptr;
    }
  }
  SectionLock(const SectionLock&) = delete;
  SectionLock& operator=(const SectionLock&) = delete;

 private:
  SemaphoreHandle_t mutex_;
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
  // Shrike v1.8.0: cancel and join any background builder first. It holds the
  // write handle to `file` and must run to a safe exit point before we free
  // the mutex it uses. joinBuildTask() waits for the task to self-delete.
  joinBuildTask();
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

uint32_t Section::onPageComplete(std::unique_ptr<Page> page, const uint16_t paragraphIndex) {
  // Shrike v1.8.1: hold fileMutex_ across the ENTIRE serialize+flush+LUT
  // publish so a concurrent loadPageFromSectionFile (main thread) can never
  // observe the file cursor mid-write. Previously only the LUT push was
  // protected, which meant the main thread could seek the shared handle
  // between the builder's file.position() and the first serialize write,
  // corrupting both the read AND the builder's subsequent cursor. This also
  // makes the pageLut_ size monotonically consistent with bytes on disk.
  SectionLock lock(fileMutex_);
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", pageCount);
    return 0;
  }
  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", pageCount);
    return 0;
  }
  // Force the file cache to flush so a concurrent reader that seeks to this
  // page's offset sees the full serialized record. flush() on HalFile drops
  // down to SdFat::FsFile::sync() which is cheap (at most a single FAT
  // cluster flush).
  file.flush();
  LOG_DBG("SCT", "Page %d processed", pageCount);

  pageLut_.push_back(position);
  paragraphLut_.push_back(paragraphIndex);
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
                                const uint8_t imageRendering, const std::function<void()>& /*popupFn*/) {
  // Shrike v1.8.0: the old indexing popup has been retired. createSectionFile
  // now runs the same body that the async builder uses, but synchronously on
  // the caller's thread. popupFn is kept in the signature for ABI stability
  // with older call sites and is intentionally ignored.
  const BuildParams p{fontId,         lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                      viewportHeight, hyphenationEnabled, embeddedStyle,       imageRendering};
  return buildSectionFileBody(p);
}

bool Section::buildSectionFileBody(const BuildParams& p) {
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";

  // Reset any lingering LUT state from a prior failed attempt so the async
  // reader's pageLut_.size() grows monotonically from zero.
  {
    SectionLock lock(fileMutex_);
    pageLut_.clear();
    paragraphLut_.clear();
    anchorMap_.clear();
    pageCount = 0;
  }

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // Retry logic for SD card timing issues
  bool success = false;
  uint32_t fileSize = 0;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
    if (buildCancel_.load()) return false;
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

  // Open the section output file for write under the mutex so a racing reader
  // does not see a half-open handle. Subsequent writes remain serialized by
  // onPageComplete acquiring the mutex around each flush.
  bool opened = false;
  {
    SectionLock lock(fileMutex_);
    if (file) file.close();
    opened = Storage.openFileForWrite("SCT", filePath, file);
  }
  if (!opened) {
    return false;
  }
  writeSectionFileHeader(p.fontId, p.lineCompression, p.extraParagraphSpacing, p.paragraphAlignment, p.viewportWidth,
                         p.viewportHeight, p.hyphenationEnabled, p.embeddedStyle, p.imageRendering);

  // Derive the content base directory and image cache path prefix for the parser
  size_t lastSlash = localPath.find_last_of('/');
  std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  std::string imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  CssParser* cssParser = nullptr;
  if (p.embeddedStyle) {
    cssParser = epub->getCssParser();
    if (cssParser) {
      if (!cssParser->loadFromCache()) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
    }
  }

  ChapterHtmlSlimParser visitor(
      epub, tmpHtmlPath, renderer, p.fontId, p.lineCompression, p.extraParagraphSpacing, p.paragraphAlignment,
      p.viewportWidth, p.viewportHeight, p.hyphenationEnabled,
      // Page callback: onPageComplete serializes + flushes to disk and publishes
      // the offset into pageLut_/paragraphLut_ under the mutex. A concurrent
      // loadPageFromSectionFile() can pick the page up as soon as this returns.
      [this](std::unique_ptr<Page> page, const uint16_t paragraphIndex) {
        this->onPageComplete(std::move(page), paragraphIndex);
      },
      p.embeddedStyle, contentBase, imageBasePath, p.imageRendering, nullptr /* popupFn */, cssParser);
  Hyphenator::setPreferredLanguage(epub->getLanguage());
  success = visitor.parseAndBuildPages();

  Storage.remove(tmpHtmlPath.c_str());
  if (!success) {
    LOG_ERR("SCT", "Failed to parse XML and build pages");
    {
      SectionLock lock(fileMutex_);
      file.close();
      Storage.remove(filePath.c_str());
    }
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }

  // All pages written and pageLut_ / paragraphLut_ are live. Write the on-disk
  // trailers (page LUT, anchor map, paragraph LUT) under the mutex so a
  // concurrent reader sees either the pre-trailer or post-trailer state but
  // not a torn one. The file reopen at the end switches to read-only.
  bool reopened = false;
  bool trailerOk = false;
  {
    SectionLock lock(fileMutex_);

    const uint32_t lutOffset = file.position();
    bool hasFailedLutRecords = false;
    for (const uint32_t off : pageLut_) {
      if (off == 0) {
        hasFailedLutRecords = true;
        break;
      }
      serialization::writePod(file, off);
    }

    if (hasFailedLutRecords) {
      LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
      file.close();
      Storage.remove(filePath.c_str());
      // lock is released by RAII
    } else {
      // Write anchor-to-page map for fragment navigation (e.g. footnote targets)
      const uint32_t anchorMapOffset = file.position();
      const auto& anchors = visitor.getAnchors();
      serialization::writePod(file, static_cast<uint16_t>(anchors.size()));
      for (const auto& [anchor, page] : anchors) {
        serialization::writeString(file, anchor);
        serialization::writePod(file, page);
      }

      const uint32_t paragraphLutOffset = file.position();
      serialization::writePod(file, static_cast<uint16_t>(paragraphLut_.size()));
      for (const uint16_t pIdx : paragraphLut_) {
        serialization::writePod(file, pIdx);
      }

      // Patch header with final pageCount, lutOffset, anchorMapOffset, and paragraphLutOffset
      file.seek(HEADER_SIZE - sizeof(uint32_t) * 3 - sizeof(pageCount));
      serialization::writePod(file, pageCount);
      serialization::writePod(file, lutOffset);
      serialization::writePod(file, anchorMapOffset);
      serialization::writePod(file, paragraphLutOffset);

      // Publish anchor map to RAM (pageLut_ and paragraphLut_ were already filled
      // incrementally by onPageComplete during streaming).
      anchorMap_.reserve(anchors.size());
      for (const auto& [anchor, page] : anchors) {
        anchorMap_.emplace_back(anchor, page);
      }

      // Close the write handle and re-open read-only so page turns can read
      // without another open round-trip. This matches loadSectionFile's
      // post-conditions (file kept open for reads).
      file.close();
      reopened = Storage.openFileForRead("SCT", filePath, file);
      trailerOk = true;
    }
  }
  if (!trailerOk) {
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }
  if (!reopened) {
    LOG_ERR("SCT", "Failed to re-open section file for reads after build");
    // Not fatal - loadPageFromSectionFile falls back to opening on demand.
  }
  if (cssParser) {
    cssParser->clear();
  }
  return true;
}

bool Section::beginAsyncSectionFileBuild(const int fontId, const float lineCompression,
                                         const bool extraParagraphSpacing, const uint8_t paragraphAlignment,
                                         const uint16_t viewportWidth, const uint16_t viewportHeight,
                                         const bool hyphenationEnabled, const bool embeddedStyle,
                                         const uint8_t imageRendering) {
  // If a prior build task is still around, join it first.
  joinBuildTask();

  buildCancel_.store(false);
  buildDone_.store(false);
  buildError_.store(false);
  buildTaskExited_.store(false);
  buildParams_ = BuildParams{fontId,         lineCompression, extraParagraphSpacing, paragraphAlignment,
                              viewportWidth, viewportHeight, hyphenationEnabled,    embeddedStyle,
                              imageRendering};

  // 8 KB stack - the Expat parser plus nested TextBlock layout pushes deeper
  // frames than the 4 KB preload task. Priority below the main Arduino loop
  // so the UI stays responsive. The task self-deletes via the trampoline;
  // joinBuildTask() waits for that.
  if (xTaskCreate(&Section::buildTaskTrampoline, "shrike_build", 8192, this, tskIDLE_PRIORITY + 1, &buildTask_) !=
      pdPASS) {
    LOG_ERR("SCT", "Build task create failed");
    buildTask_ = nullptr;
    buildDone_.store(true);
    buildError_.store(true);
    buildTaskExited_.store(true);
    return false;
  }
  return true;
}

void Section::buildTaskTrampoline(void* arg) {
  auto* self = static_cast<Section*>(arg);
  self->runBuild();
  // Publish a sentinel so the main thread knows the task is done even if it
  // failed before ever reaching a page.
  self->buildDone_.store(true);
  // Shrike v1.8.1: mark the task as fully exited BEFORE vTaskDelete so
  // joinBuildTask can observe a monotonic "safe to drop handle" signal.
  // After vTaskDelete the TCB memory is freed by the IDLE task and the
  // handle is no longer usable.
  self->buildTaskExited_.store(true);
  vTaskDelete(nullptr);
}

bool Section::runBuild() {
  const bool ok = buildSectionFileBody(buildParams_);
  if (!ok) buildError_.store(true);
  return ok;
}

void Section::joinBuildTask() {
  if (!buildTask_) return;
  buildCancel_.store(true);
  // Shrike v1.8.1: poll the atomic the task sets before vTaskDelete. Using
  // eTaskGetState(handle) on a task that has already self-deleted is
  // undefined - the TCB memory is freed by the IDLE task and may be reused
  // for a different task, giving a bogus answer that either hangs the wait
  // or returns too early. The atomic is set by the task on its own stack
  // right before vTaskDelete, so once we see it set the handle is safe to
  // drop.
  while (!buildTaskExited_.load()) {
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  buildTask_ = nullptr;
  buildCancel_.store(false);
}

bool Section::waitForBuildComplete() {
  if (!buildTask_) return buildDone_.load() && !buildError_.load();
  while (!buildDone_.load()) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return !buildError_.load();
}

bool Section::waitForPageAvailable(int pageNumber) {
  if (pageNumber < 0) return false;
  auto snapshot = [this, pageNumber]() {
    SectionLock lock(fileMutex_);
    return static_cast<size_t>(pageNumber) < pageLut_.size();
  };
  // Fast path: already present.
  if (snapshot()) return true;

  // No in-flight build? Nothing to wait for.
  if (!buildTask_) return false;

  // Poll. The parser emits pages as it layouts lines, which happens in bursts
  // between SD reads; 5 ms is short enough to feel instant on the first page
  // and doesn't steal meaningful CPU from the build task.
  while (!buildDone_.load()) {
    vTaskDelay(pdMS_TO_TICKS(5));
    if (snapshot()) return true;
  }
  // Build finished; final check.
  return snapshot();
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() {
  // Shrike v1.8.1: read pageLut_ size under the mutex to avoid a race with
  // the async builder's onPageComplete push_back (which can reallocate the
  // vector and invalidate indexing). Keep the snapshot small - just the
  // bounds check and offset lookup.
  uint32_t pageOffset = 0;
  {
    SectionLock lock(fileMutex_);
    if (currentPage < 0 || static_cast<size_t>(currentPage) >= pageLut_.size()) {
      LOG_ERR("SCT", "Page %d out of LUT range (size %u)", currentPage, static_cast<unsigned>(pageLut_.size()));
      return nullptr;
    }
    pageOffset = pageLut_[currentPage];
  }

  // Shrike v1.7.2: if a preload task already deserialized the page we are
  // about to show, steal it and skip the SD round-trip entirely. Any running
  // preload for a different page is cancelled + joined so we don't race on
  // `file` below.
  {
    SectionLock lock(fileMutex_);
    if (preloadedPage_ && preloadedPageNumber_ == currentPage) {
      auto hit = std::move(preloadedPage_);
      preloadedPageNumber_ = -1;
      LOG_DBG("SCT", "Preload hit for page %d", currentPage);
      return hit;
    }
    // Slot has the wrong page (or nothing) - drop it before we touch `file`.
    preloadedPage_.reset();
    preloadedPageNumber_ = -1;
  }
  joinPreloadTask();

  // Shrike: page offsets are resolved against the in-RAM pageLut_ populated
  // during loadSectionFile / createSectionFile, so we skip the previous
  // open + seek-to-header + read-LUT-entry + seek-to-page chain entirely.
  // If the persistent handle was closed out-of-band (very unlikely), fall
  // back to opening on demand so we remain robust.
  //
  // Shrike v1.8.0: during an async build the same `file` handle is held open
  // for O_RDWR by the builder. We can seek and read through it, but we MUST
  // restore the write position afterwards so the builder's next serialize()
  // call lands at the end of file, not in the middle of the page region we
  // just read from.
  SectionLock lock(fileMutex_);
  if (!file && !Storage.openFileForRead("SCT", filePath, file)) {
    return nullptr;
  }
  const bool buildInFlight = (buildTask_ != nullptr);
  const uint32_t savedPos = buildInFlight ? file.position() : 0;
  if (!file.seek(pageOffset)) {
    LOG_ERR("SCT", "Seek to page %d (offset %u) failed", currentPage, pageOffset);
    return nullptr;
  }
  std::unique_ptr<Page> result = Page::deserialize(file);
  if (buildInFlight) {
    file.seek(savedPos);  // hand the write cursor back to the builder
  }
  return result;
}

void Section::preloadTaskTrampoline(void* arg) {
  auto* self = static_cast<Section*>(arg);
  // The page number we should preload is stashed in preloadedPageNumber_ by
  // preloadPage() before xTaskCreate. We read it once here.
  const int target = self->preloadedPageNumber_;
  self->runPreload(target);
  // Shrike v1.8.1: signal completion to joinPreloadTask before vTaskDelete
  // (see buildTaskTrampoline comment - same rationale).
  self->preloadTaskExited_.store(true);
  vTaskDelete(nullptr);
}

void Section::runPreload(int pageNumber) {
  if (preloadCancel_.load()) return;
  if (!fileMutex_) return;

  SectionLock lock(fileMutex_);
  if (preloadCancel_.load()) return;
  if (pageNumber < 0 || static_cast<size_t>(pageNumber) >= pageLut_.size()) return;
  if (!file && !Storage.openFileForRead("SCT", filePath, file)) return;
  if (!file.seek(pageLut_[pageNumber])) return;
  auto page = Page::deserialize(file);
  if (!preloadCancel_.load() && page) {
    preloadedPage_ = std::move(page);
    preloadedPageNumber_ = pageNumber;
    LOG_DBG("SCT", "Preloaded page %d", pageNumber);
  }
}

void Section::joinPreloadTask() {
  if (!preloadTask_) return;
  preloadCancel_.store(true);
  // The task may be blocked on fileMutex_ or deep inside SdFat - we cannot
  // reliably signal mid-read. Wait for it to complete normally; the cancel
  // flag prevents it from touching preloadedPage_ on the way out.
  //
  // Shrike v1.8.1: poll the task's own "exited" atomic instead of
  // eTaskGetState on a handle that may be freed. See joinBuildTask.
  while (!preloadTaskExited_.load()) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  preloadTask_ = nullptr;
  preloadCancel_.store(false);
}

void Section::preloadPage(int pageNumber) {
  if (pageNumber < 0 || static_cast<size_t>(pageNumber) >= pageLut_.size()) return;

  // Shrike v1.8.0: skip preloading while the async section builder is still
  // running. The builder has the SD card pegged and the preload task would
  // just contend for fileMutex_ without helping latency; normal e-ink refresh
  // already gives the builder its own time slice.
  if (buildTask_ && !buildDone_.load()) return;

  // Already holding the right page? Nothing to do.
  {
    SectionLock lock(fileMutex_);
    if (preloadedPage_ && preloadedPageNumber_ == pageNumber) return;
  }

  // Stop any stale preload, then launch a new one.
  joinPreloadTask();
  preloadedPageNumber_ = pageNumber;  // task trampoline reads this
  preloadedPage_.reset();
  preloadTaskExited_.store(false);

  // 4 KB stack, priority below the main Arduino loop (1). Pinned to core 0
  // (the only core on C3). Task self-deletes via trampoline.
  if (xTaskCreate(&Section::preloadTaskTrampoline, "shrike_preload", 4096, this,
                  tskIDLE_PRIORITY + 1, &preloadTask_) != pdPASS) {
    LOG_ERR("SCT", "Preload task create failed for page %d", pageNumber);
    preloadTask_ = nullptr;
    preloadedPageNumber_ = -1;
    preloadTaskExited_.store(true);
  }
}

void Section::invalidatePreload() {
  joinPreloadTask();
  SectionLock lock(fileMutex_);
  preloadedPage_.reset();
  preloadedPageNumber_ = -1;
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
