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
  struct BookInfo {
    std::string title;
    std::string author;
  };
  std::vector<BookInfo> bookInfos;

  // Data loading
  void loadFiles();
  // Shrike: fill bookInfos[] after `files` is populated. Cheap — one stat +
  // one short fread per epub. Non-EPUB entries are skipped.
  void loadBookInfos();
  size_t findEntry(const std::string& name) const;

 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/")
      : Activity("FileBrowser", renderer, mappedInput), basepath(initialPath.empty() ? "/" : std::move(initialPath)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
