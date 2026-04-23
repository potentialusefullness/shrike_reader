"""
PlatformIO pre-build script: instrument Arduino-ESP32 SPIClass transactions
for cross-task mutex debugging in Shrike.

Problem:
  v1.8.3 and v1.8.4 both assert at SPI::endTransaction ->
  xTaskPriorityDisinherit tasks.c:5156 (pxTCB == pxCurrentTCBs[0]).
  The paramLock is taken by one task and given by another. We don't know
  WHO is doing the mis-paired take/give -- SdFat's spiStart/spiStop is
  the only obvious caller from our code, but the ESP32 APB change callback
  (_on_apb_change in esp32-hal-spi.c) also grabs spi->lock around
  setCpuFrequencyMhz() calls. Without tracing every begin/endTransaction
  we can't see the actual sequence.

Fix:
  Insert a call to an extern "C" hook `shrike_spi_trace(const char* ev)` at
  the top of SPIClass::beginTransaction (before SPI_PARAM_LOCK) and at the
  top of SPIClass::endTransaction. The hook is implemented in
  lib/Logging/SpiTraceHook.cpp and writes a task-tagged line to the RTC
  ring buffer via LOG_INF. When SHRIKE_MUTEX_TRACE is NOT defined the hook
  expands to an empty stub, so non-Shrike builds are unaffected.

  We also add a `#include <cstddef>` shim and an extern "C" declaration of
  the hook at the top of SPI.cpp.

Applied idempotently via a marker comment.
"""

Import("env")
import os


def patch_arduino_spi(env):
    framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    if not framework_dir:
        return
    path = os.path.join(framework_dir, "libraries", "SPI", "src", "SPI.cpp")
    if os.path.isfile(path):
        _apply_spi_trace_hook(path)


def _apply_spi_trace_hook(filepath):
    MARKER = "// Shrike patch: SPI transaction trace hook"
    with open(filepath, "r") as f:
        content = f.read()

    if MARKER in content:
        return  # already patched

    changed = False

    # 1. Inject extern "C" decl near top of file (after the includes).
    HEADER_OLD = '#include "SPI.h"'
    HEADER_NEW = (
        '#include "SPI.h"\n'
        + MARKER
        + "\n"
        "extern \"C\" void shrike_spi_trace(const char* ev) __attribute__((weak));\n"
        "static inline void _shrike_spi_trace(const char* ev) {\n"
        "  if (shrike_spi_trace) shrike_spi_trace(ev);\n"
        "}\n"
    )
    if HEADER_OLD in content and HEADER_NEW not in content:
        content = content.replace(HEADER_OLD, HEADER_NEW, 1)
        changed = True
    else:
        print(
            "WARNING: Arduino SPI patch couldn't find '%s' to inject header" % HEADER_OLD
        )
        return

    # 2. Hook beginTransaction.
    BEGIN_OLD = (
        "void SPIClass::beginTransaction(SPISettings settings) {\n"
        "  SPI_PARAM_LOCK();"
    )
    BEGIN_NEW = (
        "void SPIClass::beginTransaction(SPISettings settings) {\n"
        "  _shrike_spi_trace(\"beginTx_enter\");\n"
        "  SPI_PARAM_LOCK();\n"
        "  _shrike_spi_trace(\"beginTx_locked\");"
    )
    if BEGIN_OLD in content:
        content = content.replace(BEGIN_OLD, BEGIN_NEW, 1)
        changed = True
    else:
        print("WARNING: beginTransaction anchor not found in %s" % filepath)

    # 3. Hook endTransaction.
    END_OLD = (
        "void SPIClass::endTransaction() {\n"
        "  if (_inTransaction) {\n"
        "    _inTransaction = false;\n"
        "    spiEndTransaction(_spi);\n"
        "    SPI_PARAM_UNLOCK();"
    )
    END_NEW = (
        "void SPIClass::endTransaction() {\n"
        "  _shrike_spi_trace(\"endTx_enter\");\n"
        "  if (_inTransaction) {\n"
        "    _inTransaction = false;\n"
        "    spiEndTransaction(_spi);\n"
        "    _shrike_spi_trace(\"endTx_preUnlock\");\n"
        "    SPI_PARAM_UNLOCK();\n"
        "    _shrike_spi_trace(\"endTx_unlocked\");"
    )
    if END_OLD in content:
        content = content.replace(END_OLD, END_NEW, 1)
        changed = True
    else:
        print("WARNING: endTransaction anchor not found in %s" % filepath)

    if changed:
        with open(filepath, "w") as f:
            f.write(content)
        print("Patched Arduino SPI: added shrike_spi_trace hooks: %s" % filepath)


# Run immediately at script import time (before compilation).
patch_arduino_spi(env)
