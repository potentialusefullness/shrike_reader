#include "LibraryPaths.h"

#include <HalStorage.h>
#include <Logging.h>

namespace Shrike {

namespace {

constexpr const char* TAG = "LibraryPaths";

// Temporary name used to force a case-changing rename on FAT/exFAT, which
// treat /Library and /library as the same entry. Rename A -> TMP -> a so the
// on-disk stored name is guaranteed to match LIBRARY_ROOT's casing.
constexpr const char* CASE_SHUFFLE_TMP = "/.library_rename_tmp";

// True when LIBRARY_ROOT and `legacy` refer to the same on-disk entry on a
// case-insensitive filesystem (i.e. the only difference is letter case).
bool isCaseVariantOfRoot(const char* legacy) {
  const char* a = legacy;
  const char* b = LIBRARY_ROOT;
  while (*a && *b) {
    const char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
    const char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + 32) : *b;
    if (ca != cb) {
      return false;
    }
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

}  // namespace

void ensureLibraryRoot() {
  // If any legacy folder that is a pure case variant of LIBRARY_ROOT (e.g.
  // /Library vs /library) is sitting on disk, force a case-changing rename
  // via a temporary name so the stored folder name actually becomes lowercase
  // on FAT/exFAT. Without the two-hop shuffle a same-name rename can be a
  // no-op and leave the old casing in place.
  for (const char* legacy : LEGACY_LIBRARY_DIRS) {
    if (!isCaseVariantOfRoot(legacy)) {
      continue;
    }
    if (!Storage.exists(legacy)) {
      continue;
    }
    // Skip shuffle if the stored casing already matches (strcmp is case-
    // sensitive; exists() was case-insensitive). We cannot easily query the
    // stored casing, so we always attempt the shuffle - it is idempotent.
    if (Storage.exists(CASE_SHUFFLE_TMP)) {
      Storage.removeDir(CASE_SHUFFLE_TMP);
    }
    if (Storage.rename(legacy, CASE_SHUFFLE_TMP) &&
        Storage.rename(CASE_SHUFFLE_TMP, LIBRARY_ROOT)) {
      LOG_INF(TAG, "re-cased library folder to %s", LIBRARY_ROOT);
      return;
    }
    LOG_ERR(TAG, "case-rename %s -> %s via %s failed", legacy, LIBRARY_ROOT, CASE_SHUFFLE_TMP);
    // Fall through to the regular migration loop below as a best-effort.
    break;
  }

  // Already present? Nothing more to do.
  if (Storage.exists(LIBRARY_ROOT)) {
    return;
  }

  // Look for a non-case-variant legacy content folder and migrate it into
  // place. If no legacy folder exists we leave /library absent on purpose so
  // that Browse Files falls back to the SD root (see libraryHasContent).
  for (const char* legacy : LEGACY_LIBRARY_DIRS) {
    if (isCaseVariantOfRoot(legacy)) {
      continue;  // handled by the case-shuffle block above
    }
    if (Storage.exists(legacy)) {
      if (Storage.rename(legacy, LIBRARY_ROOT)) {
        LOG_INF(TAG, "migrated legacy folder %s -> %s", legacy, LIBRARY_ROOT);
        return;
      }
      LOG_ERR(TAG, "rename %s -> %s failed; leaving %s alone", legacy, LIBRARY_ROOT, legacy);
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
