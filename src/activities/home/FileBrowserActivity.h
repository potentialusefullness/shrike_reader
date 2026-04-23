#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "RecentBooksStore.h"
#include "util/ButtonNavigator.h"

class FileBrowserActivity final : public Activity {
 private:
  // Deletion
  void clearFileMetadata(const std::string& fullPath);

  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;

  bool lockLongPressBack = false;

  // Files state
  std::string basepath = "/";
  std::vector<std::string> files;

  // Shrike: parallel metadata to `files`. Populated for EPUB entries whose
  // on-SD cache is still valid (source-file size matches). Empty strings mean
  // "fall back to filename"; non-EPUB entries and directories always stay empty.
  // `percent` is the last-saved book-level reading progress (0..100) for any
  // supported reader type, or -1 when unknown (unopened, or legacy cache).
  struct BookInfo {
    std::string title;
    std::string author;
    int8_t percent = -1;
  };
  std::vector<BookInfo> bookInfos;
  // Shrike v1.7.3: parallel to bookInfos - false means the slot has never been
  // populated. loadFiles() resizes both vectors and leaves every slot "not
  // loaded"; render() calls ensureBookInfoLoaded(i) for each row it is about
  // to draw, so we only pay for the 6-10 rows currently visible instead of
  // paying for every file in the folder up front.
  std::vector<bool> bookInfosLoaded;

  // Data loading
  void loadFiles();
  // Shrike v1.7.3: populate bookInfos[index] on demand. Idempotent - early
  // returns once the slot is marked loaded. Non-book entries mark themselves
  // loaded with default-constructed BookInfo so they are not re-tried.
  void ensureBookInfoLoaded(size_t index);
  size_t findEntry(const std::string& name) const;

 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/")
      : Activity("FileBrowser", renderer, mappedInput), basepath(initialPath.empty() ? "/" : std::move(initialPath)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
