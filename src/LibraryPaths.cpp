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

  // Look for a legacy content folder and migrate it into place.
  for (const char* legacy : LEGACY_LIBRARY_DIRS) {
    if (Storage.exists(legacy)) {
      if (Storage.rename(legacy, LIBRARY_ROOT)) {
        LOG_INF(TAG, "migrated legacy folder %s -> %s", legacy, LIBRARY_ROOT);
        return;
      }
      LOG_ERR(TAG, "rename %s -> %s failed; creating fresh %s instead",
              legacy, LIBRARY_ROOT, LIBRARY_ROOT);
      break;
    }
  }

  // No legacy folder (or migration failed) - create a fresh one.
  if (Storage.mkdir(LIBRARY_ROOT)) {
    LOG_INF(TAG, "created %s", LIBRARY_ROOT);
  } else {
    LOG_ERR(TAG, "mkdir %s failed", LIBRARY_ROOT);
  }
}

}  // namespace Shrike
