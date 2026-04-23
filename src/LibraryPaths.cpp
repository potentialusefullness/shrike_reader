#include "LibraryPaths.h"

#include <HalStorage.h>
#include <Logging.h>

namespace Shrike {

namespace {
constexpr const char* TAG = "LibraryPaths";
}

void ensureLibraryRoot() {
  // Already present? Nothing to do.
  if (Storage.exists(LIBRARY_ROOT)) {
    return;
  }

  // Look for a legacy content folder and migrate it into place. If no legacy
  // folder exists we leave /Library absent on purpose so that Browse Files
  // falls back to the SD root (see libraryHasContent).
  for (const char* legacy : LEGACY_LIBRARY_DIRS) {
    if (Storage.exists(legacy)) {
      if (Storage.rename(legacy, LIBRARY_ROOT)) {
        LOG_INF(TAG, "migrated legacy folder %s -> %s", legacy, LIBRARY_ROOT);
        return;
      }
      LOG_ERR(TAG, "rename %s -> %s failed; leaving %s alone",
              legacy, LIBRARY_ROOT, legacy);
      return;
    }
  }
}

bool libraryHasContent() {
  if (!Storage.exists(LIBRARY_ROOT)) {
    return false;
  }
  // listFiles enumerates entries under the given path. Cap at 1 - we only
  // care whether at least one entry is present.
  const auto entries = Storage.listFiles(LIBRARY_ROOT, 1);
  return !entries.empty();
}

}  // namespace Shrike
