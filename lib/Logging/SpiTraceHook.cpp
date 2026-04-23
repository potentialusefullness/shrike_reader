// Implementation of the weak hook that patch_arduino_spi.py calls from
// SPIClass::beginTransaction / endTransaction. Defined only when the
// SHRIKE_MUTEX_TRACE build flag is set; otherwise the hook stays undefined
// and the weak stub in the patched SPI.cpp becomes a no-op.
//
// Logs at LOG_INF level into the RTC-retained ring buffer so crash_report.txt
// captures the sequence of SPI transaction events across every task, not just
// the one that ultimately triggers the xTaskPriorityDisinherit assert.

#include "Logging.h"

// v1.8.6: the SPI transaction hook was introduced in v1.8.5 to diagnose the
// xTaskPriorityDisinherit mutex crash. It fires on every beginTransaction /
// endTransaction, which during library enumeration produces hundreds of
// lines per second and floods the RTC ring buffer. We split it onto its own
// SHRIKE_SPI_TRACE flag (separate from SHRIKE_MUTEX_TRACE) so future
// diagnostic builds can toggle just the SPI noise without losing the
// higher-value Section / ActivityManager / PowerManager MTX_TRACE lines.
// Default is OFF now that the v1.8.5 run confirmed loopTask's SPI
// transactions are all well-paired.
#ifdef SHRIKE_SPI_TRACE

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern "C" void shrike_spi_trace(const char* ev) {
  // pcTaskGetName(nullptr) returns the current task's name (up to 15 chars
  // by default -- FreeRTOS configMAX_TASK_NAME_LEN). We log one short line
  // per SPI transaction boundary so the ring captures who took the paramLock
  // and who gave it back.
  const char* task = pcTaskGetName(nullptr);
  LOG_INF("SPI", "%s t=%s", ev, task ? task : "?");
}

#endif  // SHRIKE_SPI_TRACE
