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

// Create /Library if it doesn't exist and migrate legacy /books or /book into
// place if /Library is absent. Safe to call repeatedly; no-ops once /Library
// exists. Call once early in setup(), after Storage is mounted.
void ensureLibraryRoot();

}  // namespace Shrike
