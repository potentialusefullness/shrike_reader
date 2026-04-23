#pragma once

// Shrike: centralised SD-card paths for the user-facing Library root.
//
// The library folder is the default starting point for Browse Files. On first
// boot we migrate any legacy capitalised /Library or older /book(s) folder
// into /library so the user doesn't lose their content.

namespace Shrike {

// Canonical user-facing content folder. Everything the user puts into the
// Library lives here. Internal caches still live under /.crosspoint/.
// Kept lowercase to match the rest of the SD layout (/.crosspoint/, etc.).
inline constexpr const char* LIBRARY_ROOT = "/library";

// Legacy directory names to migrate from on first boot (in priority order).
// /Library covers users who ran v1.6.0 - v1.7.0 where the folder was
// capitalised; /books and /book cover much older CrossPoint layouts.
inline constexpr const char* LEGACY_LIBRARY_DIRS[] = {"/Library", "/books", "/book"};

// Migrate a legacy /Library, /books or /book folder into /library if one is
// present and /library does not yet exist (or is only a case variant of the
// same on-disk entry). Does NOT create an empty /library on fresh installs -
// we only want /library to exist if the user (or migration) put something in
// it. Safe to call repeatedly; no-ops once the migration has happened or when
// there is nothing to migrate. Call once early in setup(), after Storage is
// mounted.
void ensureLibraryRoot();

}  // namespace Shrike
