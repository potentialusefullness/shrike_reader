// Implementation of the weak hook that patch_arduino_spi.py calls from
// SPIClass::beginTransaction / endTransaction. Defined only when the
// SHRIKE_SPI_TRACE build flag is set; otherwise the hook stays undefined
// and the weak stub in the patched SPI.cpp becomes a no-op.
//
// v1.8.7: Upgraded from passive logging to active cross-task detection.
// Previous versions merely logged task names for every begin/end. In v1.8.7
// we record the TaskHandle_t at beginTx_locked (when paramLock is known to
// be held by the current task) and on endTx_preUnlock (just before
// SPI_PARAM_UNLOCK gives the mutex) we compare the current task to that
// owner. If they mismatch we have a smoking gun: something is about to give
// a mutex it does not own, which is exactly the xTaskPriorityDisinherit
// assert. We log a PANIC-level line first (so crash_report.txt captures
// the guilty pair) and then abort() so the panic backtrace points to the
// guilty endTransaction caller instead of the generic FreeRTOS assert.
//
// Evidence gathered ahead of v1.8.7 shows EInkDisplay.cpp calls
// SPI.beginTransaction / SPI.endTransaction 5 times directly from the
// render task, while SdFat calls the same pair from loopTask and
// shrike_preload under StorageLock. All three share a single SPIClass
// paramLock. The v1.8.6 crash decoded to FsCache::sync triggering writeSector
// on shrike_preload, which must have raced EInk. This diagnostic build
// will confirm which direction the race runs (EInk's begin paired with
// SdFat's end, or vice versa).

#include "Logging.h"

#ifdef SHRIKE_SPI_TRACE

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdlib.h>  // abort

// The owner recorded at the most recent beginTx_locked event. We read/write
// this without a critical section because (a) the SPIClass paramLock is a
// mutex that already serialises begin/end against other begins/ends, and
// (b) on the faulting run we only need ONE correct capture before the
// cross-task give happens for the assert to fire. If something weird leaves
// the value stale between unrelated transactions the worst case is a false
// positive log line, not a missed detection.
static TaskHandle_t s_spi_owner = nullptr;
static const char* s_spi_owner_name = "<none>";

// Retain the last-known offender so crash_report.txt clearly fingers it even
// if the abort path loses the LOG_ERR line in a ring-buffer eviction.
static volatile TaskHandle_t s_spi_last_giver = nullptr;
static const char* s_spi_last_giver_name = "<none>";

extern "C" void shrike_spi_trace(const char* ev) {
  const TaskHandle_t current = xTaskGetCurrentTaskHandle();
  const char* task = pcTaskGetName(nullptr);
  if (task == nullptr) task = "?";

  // On beginTx_locked the mutex is held by the current task -- record it.
  // We use a string compare instead of a pointer compare on ev because the
  // weak hook receives the literal from SPI.cpp and we want to key on a
  // stable phase, not the literal address.
  if (ev[0] == 'b' && ev[1] == 'e' && ev[2] == 'g' && ev[7] == 'l') {
    // "beginTx_locked"
    s_spi_owner = current;
    s_spi_owner_name = task;
    LOG_INF("SPI", "lock t=%s h=%p", task, current);
    return;
  }

  if (ev[0] == 'e' && ev[1] == 'n' && ev[2] == 'd' && ev[7] == 'p') {
    // "endTx_preUnlock" -- this is the last chance to abort before the
    // paramLock is given. If current != owner, the give will trigger
    // xTaskPriorityDisinherit. Log loudly and abort here so the panic
    // backtrace points at the guilty SPIClass::endTransaction caller.
    if (s_spi_owner != nullptr && current != s_spi_owner) {
      s_spi_last_giver = current;
      s_spi_last_giver_name = task;
      LOG_ERR("SPI", "XTASK-GIVE! owner=%s(h=%p) giver=%s(h=%p)",
              s_spi_owner_name, s_spi_owner, task, current);
      // LOG_ERR lands in the RTC ring buffer which is already drained to
      // Serial by the logging core when ENABLE_SERIAL_LOG is set. We avoid
      // pulling MySerialImpl into lib/Logging's dependency graph here --
      // SpiTraceHook is only built into Logging and must not introduce a
      // back-edge to HalSerial.
      abort();
    }
    // Matched owner: clear it so any subsequent unmatched end reads as
    // "<none>" rather than the previous task's stale handle.
    s_spi_owner = nullptr;
    s_spi_owner_name = "<none>";
    LOG_INF("SPI", "unlock t=%s h=%p", task, current);
    return;
  }

  // Other phases (beginTx_enter, endTx_enter, endTx_unlocked) are ignored
  // at LOG_INF to keep the ring buffer breathable. Flip to LOG_DBG if you
  // need the full sequence back.
}

#endif  // SHRIKE_SPI_TRACE
