#pragma once

// Shrike: centralised SD-card paths for the user-facing Library root.
//
// The Library folder is the default starting point for Browse Files. On first
// boot we ensure it exists, and if a legacy "/book" or "/books" folder is
// present we rename it in place so the user doesn't lose their content.

namespace Shrike {

// Canonical user-facing content folder. Everything the user puts into the
// Library lives here. Internal caches still live under /.crosspoint/.
inline constexpr const char* LIBRARY_ROOT = "/Library";

// Legacy directory names to migrate from on first boot (in priority order).
inline constexpr const char* LEGACY_LIBRARY_DIRS[] = {"/books", "/book"};

// Migrate a legacy /books or /book folder into /Library if one is present and
// /Library does not yet exist. Does NOT create an empty /Library on fresh
// installs - we only want /Library to exist if the user (or migration) put
// something in it. Safe to call repeatedly; no-ops once the migration has
// happened or when there is nothing to migrate. Call once early in setup(),
// after Storage is mounted.
void ensureLibraryRoot();

// True if /Library exists and contains at least one entry. Used to decide
// whether Browse Files should open into /Library or fall back to the SD root
// so users with books loose at the root can still see them.
bool libraryHasContent();

}  // namespace Shrike
