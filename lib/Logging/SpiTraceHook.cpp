// Implementation of the weak hook that patch_arduino_spi.py calls from
// SPIClass::beginTransaction / endTransaction. Defined only when the
// SHRIKE_MUTEX_TRACE build flag is set; otherwise the hook stays undefined
// and the weak stub in the patched SPI.cpp becomes a no-op.
//
// Logs at LOG_INF level into the RTC-retained ring buffer so crash_report.txt
// captures the sequence of SPI transaction events across every task, not just
// the one that ultimately triggers the xTaskPriorityDisinherit assert.

#include "Logging.h"

#ifdef SHRIKE_MUTEX_TRACE

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

#endif  // SHRIKE_MUTEX_TRACE
