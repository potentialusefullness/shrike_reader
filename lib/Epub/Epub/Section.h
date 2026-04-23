#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
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

  // Shrike v1.7.2: single-slot next-page preload. While the e-ink refresh for
  // page N is running (~1s of idle CPU), a background task reads N+1 off the
  // SD card and deserializes it into preloadedPage_. When the user advances,
  // loadPageFromSectionFile() steals the slot instead of hitting the SD card.
  // SdFat is not thread-safe, so fileMutex_ serialises any access to `file`
  // that crosses tasks. Invalidated on any currentPage jump, section unload,
  // or font/layout change (which recreates the Section entirely).
  SemaphoreHandle_t fileMutex_ = nullptr;
  TaskHandle_t preloadTask_ = nullptr;
  std::atomic<bool> preloadCancel_{false};
  // Shrike v1.8.1: set by the preload task itself right before vTaskDelete.
  // joinPreloadTask() polls this instead of eTaskGetState(preloadTask_),
  // which is unsafe after the TCB has been freed and possibly reused for a
  // different task. Using an atomic flag makes the join self-contained.
  std::atomic<bool> preloadTaskExited_{true};
  std::unique_ptr<Page> preloadedPage_;
  int preloadedPageNumber_ = -1;

  static void preloadTaskTrampoline(void* arg);
  void runPreload(int pageNumber);
  void joinPreloadTask();

  // Shrike v1.8.6: the cached section file (created by createSectionFile or
  // the async builder) is an opaque binary blob. A truncated or corrupted
  // file -- e.g. left behind by a prior crash mid-write -- can trip the
  // deserializer into calling std::string::resize() with a bogus uint32 and
  // abort the firmware (v1.8.5 crash in TextBlock::deserialize). Callers
  // invoke this helper after Page::deserialize() returns nullptr on a cache
  // file that is NOT currently being written by the async builder; it
  // closes `file`, removes the corrupt path, and resets in-RAM LUTs so the
  // next loadSectionFile() / createSectionFile() cycle rebuilds from source.
  // Returns true if the file was actually removed.
  bool invalidateCorruptCache(const char* site);

  // Shrike v1.8.0: background section-file build. On a cache miss the parser
  // runs in this task and streams pages to disk one-by-one. pageLut_ grows as
  // each page is persisted, so the main thread can start rendering page 0 as
  // soon as it lands - no blocking, no indexing popup. Subsequent page turns
  // either hit pages already written or wait briefly for the task to catch up.
  TaskHandle_t buildTask_ = nullptr;
  std::atomic<bool> buildCancel_{false};
  std::atomic<bool> buildDone_{false};   // true once task exits (success or failure)
  std::atomic<bool> buildError_{false};  // true if the parse/stream failed
  // Shrike v1.8.1: set by the build task itself right before vTaskDelete.
  // joinBuildTask() polls this instead of eTaskGetState(buildTask_), which
  // is unsafe after the TCB has been freed. buildDone_ covers logical
  // completion but the task is still executing the trampoline tail; this
  // flag is the real "task finished, handle safe to drop" signal.
  std::atomic<bool> buildTaskExited_{true};

  // Parameters captured for the background build task. Only valid while
  // buildTask_ is running.
  struct BuildParams {
    int fontId;
    float lineCompression;
    bool extraParagraphSpacing;
    uint8_t paragraphAlignment;
    uint16_t viewportWidth;
    uint16_t viewportHeight;
    bool hyphenationEnabled;
    bool embeddedStyle;
    uint8_t imageRendering;
  };
  BuildParams buildParams_{};

  static void buildTaskTrampoline(void* arg);
  bool runBuild();
  void joinBuildTask();

  // Shared body of the section-file build. Used by both the synchronous
  // createSectionFile() and the background buildTask_ path. Does the streaming
  // HTML, header write, parse loop, LUT/anchor/paragraph write and reopen.
  bool buildSectionFileBody(const BuildParams& p);

  // Load the three LUTs from `file` into the in-RAM vectors. Assumes `file` is
  // open and positioned anywhere; restores position undefined afterwards. The
  // header must have been read already so the three LUT offsets are known.
  bool readLutsFromFile(uint32_t pageLutOffset, uint32_t anchorMapOffset, uint32_t paragraphLutOffset);

  void writeSectionFileHeader(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                              uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                              bool embeddedStyle, uint8_t imageRendering);
  uint32_t onPageComplete(std::unique_ptr<Page> page, uint16_t paragraphIndex);

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  // Constructor and destructor are out-of-line because unique_ptr<Page> is a
  // member and Page is only forward-declared in this header. Putting them in
  // the .cpp (which includes Page.h) keeps Page's full type out of callers.
  explicit Section(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer& renderer);
  ~Section();
  bool loadSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                       uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                       uint8_t imageRendering);
  bool clearCache() const;
  bool createSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                         uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                         uint8_t imageRendering, const std::function<void()>& popupFn = nullptr);

  // Shrike v1.8.0: kick off a background section-file build. Returns true once
  // the build task has been spawned; the caller must then poll via
  // waitForPageAvailable() to know when page N is ready to render. Returns
  // false if the task could not be created (check buildFailed()). All params
  // mirror createSectionFile(); no popup is ever shown.
  bool beginAsyncSectionFileBuild(int fontId, float lineCompression, bool extraParagraphSpacing,
                                  uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                                  bool hyphenationEnabled, bool embeddedStyle, uint8_t imageRendering);

  // Shrike v1.8.0: block until page `pageNumber` is available in the on-disk
  // cache (i.e. pageLut_ has an entry for it and page bytes are flushed), or
  // until the async build finishes without producing that page. Returns true
  // if the page is ready to load, false if the build failed or finished short
  // of this page. Safe to call when no build is running - returns true if the
  // page already exists in the current pageLut_.
  bool waitForPageAvailable(int pageNumber);

  // Shrike v1.8.0: block until the async builder finishes. Needed by callers
  // that require the final pageCount (percent-jump resolution, spine-change
  // progress adjustments) or the fully-populated anchor map. Returns true if
  // the build completed successfully, false on error or if no build is in
  // flight.
  bool waitForBuildComplete();

  // True once the async builder has finished (successfully or not). Readers
  // can use this to stop waiting on waitForPageAvailable.
  bool buildFinished() const { return buildDone_.load(); }
  bool buildFailed() const { return buildError_.load(); }
  bool buildInProgress() const { return buildTask_ != nullptr && !buildDone_.load(); }

  std::unique_ptr<Page> loadPageFromSectionFile();

  // Shrike v1.7.2: request an async preload of the given page number into the
  // in-RAM slot. Returns immediately; the task does the SD read off the main
  // thread so the e-ink refresh window covers the I/O. If a preload is already
  // running it is cancelled first. Out-of-range page numbers are ignored.
  void preloadPage(int pageNumber);

  // Drop any preloaded page and cancel any in-flight preload task. Call after
  // any currentPage change that isn't a simple +1 page turn (anchors, jumps,
  // percent seeks) so the next loadPageFromSectionFile doesn't pick up stale
  // state.
  void invalidatePreload();

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;

  // Look up the page number for a synthetic paragraph index from XPath p[N].
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;

  // Look up the synthetic paragraph index for the given rendered page.
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;
};
