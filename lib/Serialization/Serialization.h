#pragma once
#include <HalStorage.h>

#include <iostream>

namespace serialization {

// Maximum length accepted by readString for a single std::string. Real-world
// content in Shrike cache files: href/title/word are all well under 2 KB, and
// the occasional base64 CSS selector or long metadata field fits under 8 KB.
// A corrupted cache file can easily produce a uint32 length in the billions,
// which makes std::string::resize() throw bad_alloc and abort the firmware
// (see v1.8.5 crash: TextBlock::deserialize -> readString -> operator new).
// Capping at 8 KB lets us reject garbage before we call resize().
static constexpr uint32_t MAX_SERIALIZED_STRING_LEN = 8 * 1024;

template <typename T>
static void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
static void writePod(FsFile& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

template <typename T>
static void readPod(std::istream& is, T& value) {
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

template <typename T>
static void readPod(FsFile& file, T& value) {
  file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T));
}

// readPodChecked returns false if the read came up short. Intended for the
// Page/Section/TextBlock deserializer chain where a truncated cache file
// must abort deserialization instead of silently returning zeros.
template <typename T>
static bool readPodChecked(FsFile& file, T& value) {
  return file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T)) == sizeof(T);
}

static void writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  writePod(os, len);
  os.write(s.data(), len);
}

static void writeString(FsFile& file, const std::string& s) {
  const uint32_t len = s.size();
  writePod(file, len);
  file.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

// readString (void overload): legacy callers. On a length > MAX_SERIALIZED_STRING_LEN
// we clear the string rather than calling resize() with a bogus size; this
// prevents bad_alloc and the uncaught-exception abort, but the file cursor
// is NOT advanced by the bogus length, so subsequent reads will be
// misaligned. Callers that care about corruption should prefer the bool
// overload (readStringChecked) below.
static void readString(std::istream& is, std::string& s) {
  uint32_t len = 0;
  readPod(is, len);
  if (len > MAX_SERIALIZED_STRING_LEN) {
    s.clear();
    return;
  }
  s.resize(len);
  if (len) is.read(&s[0], len);
}

static void readString(FsFile& file, std::string& s) {
  uint32_t len = 0;
  readPod(file, len);
  if (len > MAX_SERIALIZED_STRING_LEN) {
    s.clear();
    return;
  }
  s.resize(len);
  if (len) file.read(reinterpret_cast<uint8_t*>(&s[0]), len);
}

// readStringChecked: returns false on bounds violation, short read, or any
// other failure. Callers in the preload hot path (Page/TextBlock/Section)
// propagate this upward so a corrupt cache file fails fast and can be
// invalidated instead of aborting the firmware.
static bool readStringChecked(FsFile& file, std::string& s) {
  uint32_t len = 0;
  if (file.read(reinterpret_cast<uint8_t*>(&len), sizeof(len)) != sizeof(len)) {
    return false;
  }
  if (len > MAX_SERIALIZED_STRING_LEN) {
    s.clear();
    return false;
  }
  s.resize(len);
  if (len && file.read(reinterpret_cast<uint8_t*>(&s[0]), len) != (int)len) {
    return false;
  }
  return true;
}

}  // namespace serialization
