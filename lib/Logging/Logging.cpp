#include "Logging.h"

#include <string>

// Max length of a single line built by logPrintf. This is a stack buffer only,
// the persistent ring below packs lines back-to-back regardless of length.
#define MAX_ENTRY_LEN 256

// Size of the char-packed persistent ring buffer (bytes). Holds many short
// MTX_TRACE entries: at ~60 bytes/line this fits ~65 lines, 4x more than the
// previous 16-slot design. Sized to keep the overall RTC_NOINIT footprint
// identical to the old layout (16 * 256 = 4096).
#define LOG_RING_BYTES 4096

// Persistent ring buffer in RTC slow memory. Lines are written back-to-back,
// each line null-terminated. On wrap we overwrite the oldest bytes; the reader
// (getLastLogs) walks from the tail forward and skips the first partial line.
RTC_NOINIT_ATTR char logRing[LOG_RING_BYTES];
// Byte offset of the next write. Always in 0..LOG_RING_BYTES-1.
RTC_NOINIT_ATTR uint32_t logHead;
// True once the ring has wrapped at least once.
RTC_NOINIT_ATTR uint32_t logWrapped;
// Magic word written alongside logHead to detect uninitialized RTC memory.
// RTC_NOINIT_ATTR is not zeroed on cold boot, so logHead may appear in-range
// by chance even though logRing is garbage. The magic value is only set by
// clearLastLogs(), so its absence means the buffer was never initialized.
RTC_NOINIT_ATTR uint32_t rtcLogMagic;
static constexpr uint32_t LOG_RTC_MAGIC = 0xDEADBEEF;

static inline bool logStateValid() {
  return rtcLogMagic == LOG_RTC_MAGIC && logHead < LOG_RING_BYTES && logWrapped <= 1;
}

static void resetRing() {
  memset(logRing, 0, sizeof(logRing));
  logHead = 0;
  logWrapped = 0;
  rtcLogMagic = LOG_RTC_MAGIC;
}

// Append one byte to the ring, handling wrap.
static inline void ringPutByte(char b) {
  logRing[logHead] = b;
  logHead++;
  if (logHead >= LOG_RING_BYTES) {
    logHead = 0;
    logWrapped = 1;
  }
}

void addToLogRingBuffer(const char* message) {
  // If the magic is wrong or state is out of range (RTC_NOINIT_ATTR garbage
  // on cold boot), clear the entire buffer so subsequent reads are safe.
  if (!logStateValid()) {
    resetRing();
  }
  // Write the message bytes plus a terminating NUL so getLastLogs() can walk
  // line boundaries. Newlines inside messages are preserved as-is.
  size_t len = strnlen(message, MAX_ENTRY_LEN);
  for (size_t i = 0; i < len; i++) {
    ringPutByte(message[i]);
  }
  ringPutByte('\0');
}

// Since logging can take a large amount of flash, we want to make the format string as short as possible.
// This logPrintf prepend the timestamp, level and origin to the user-provided message, so that the user only needs to
// provide the format string for the message itself.
void logPrintf(const char* level, const char* origin, const char* format, ...) {
  va_list args;
  va_start(args, format);
  char buf[MAX_ENTRY_LEN];
  char* c = buf;
  // add timestamp, level and origin
  {
    unsigned long ms = millis();
    int len = snprintf(c, sizeof(buf), "[%lu] [%s] [%s] ", ms, level, origin);
    // error while writing => return
    if (len < 0) {
      va_end(args);
      return;
    }
    // clamp c to be in buffer range
    c += std::min(len, MAX_ENTRY_LEN);
  }
  // add the user message
  {
    int len = vsnprintf(c, sizeof(buf) - (c - buf), format, args);
    if (len < 0) {
      va_end(args);
      return;
    }
  }
  va_end(args);
  if (logSerial) {
    logSerial.print(buf);
  }
  addToLogRingBuffer(buf);
}

std::string getLastLogs() {
  if (!logStateValid()) {
    return {};
  }
  std::string output;
  output.reserve(LOG_RING_BYTES);
  // If the ring has not wrapped, valid data is simply [0..logHead).
  // If it has wrapped, valid data is [logHead..LOG_RING_BYTES) + [0..logHead),
  // but the first segment starts in the middle of whatever line was being
  // overwritten -- skip up to the first NUL to realign on a line boundary.
  if (!logWrapped) {
    for (size_t i = 0; i < logHead; i++) {
      char ch = logRing[i];
      if (ch == '\0') {
        // Lines are stored NUL-terminated for safety; output as nothing
        // (the format string already includes '\n' before the NUL).
        continue;
      }
      output.push_back(ch);
    }
    return output;
  }

  // Wrapped: start at logHead, scan forward until the first NUL to skip the
  // partial line that got clobbered, then dump everything after it.
  size_t i = logHead;
  bool aligned = false;
  size_t scanned = 0;
  while (scanned < LOG_RING_BYTES) {
    char ch = logRing[i];
    if (!aligned) {
      if (ch == '\0') {
        aligned = true;
      }
    } else if (ch != '\0') {
      output.push_back(ch);
    }
    i++;
    if (i >= LOG_RING_BYTES) {
      i = 0;
    }
    scanned++;
  }
  return output;
}

// Checks whether the RTC log state is consistent. Returns true if corruption
// is detected, in which case rtcLogMagic is still invalid. Callers (e.g.
// HalSystem::begin on the panic-reboot path) must call clearLastLogs() after
// a true result to fully reinitialize the ring before getLastLogs() is used.
bool sanitizeLogHead() {
  if (!logStateValid()) {
    logHead = 0;
    logWrapped = 0;
    return true;
  }
  return false;
}

void clearLastLogs() {
  resetRing();
}
