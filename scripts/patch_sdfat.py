"""
PlatformIO pre-build script: patch SdFat's SdSpiCard for a mutex-leak bug.

Problem:
  SdSpiCard::readSectors() in the ENABLE_DEDICATED_SPI compilation path has
  an asymmetric fail handler:

      fail:
        return false;

  On every read error (CRC, timeout, bad card response, etc.), it returns
  *without* calling spiStop(). Because cardCommand() set m_spiActive = true
  at entry by calling spiStart() -- which calls SPI.beginTransaction() and
  takes both the SPI paramLock and spi->lock mutexes -- those mutexes stay
  held on the faulting task's TCB indefinitely.

  Any subsequent SD I/O from a DIFFERENT task will observe m_spiActive ==
  true, skip the spiStart() on entry, do its transfer, and on its matching
  spiStop() call SPI.endTransaction() which does xSemaphoreGive() from the
  wrong task. FreeRTOS then asserts:

      assert failed: xTaskPriorityDisinherit tasks.c:5156
      (pxTCB == pxCurrentTCBs[0])

  writeSectors() in the same file does the right thing -- its fail handler
  calls spiStop() -- so the asymmetry is clearly a library bug.

  This manifests in Shrike as intermittent crashes whenever the SD card has
  a transient read hiccup while two tasks (e.g. ActivityManagerRender and
  the main loop, or the async section builder) are both doing SD I/O.
  "Clear cache and reopen book" is a reliable trigger because it does heavy
  interleaved read/write with short StorageLock-protected windows.

Fix:
  Make the readSectors fail path call spiStop() before returning, matching
  the writeSectors fail path (line 731).

Applied idempotently -- safe to run on every build.
"""

Import("env")
import os


def patch_sdfat(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    for env_dir in os.listdir(libdeps_dir):
        path = os.path.join(
            libdeps_dir, env_dir, "SdFat", "src", "SdCard", "SdSpiCard", "SdSpiCard.cpp"
        )
        if os.path.isfile(path):
            _apply_readsectors_spistop_fix(path)


def _apply_readsectors_spistop_fix(filepath):
    MARKER = "// Shrike patch: spiStop() on readSectors fail"
    with open(filepath, "r") as f:
        content = f.read()

    if MARKER in content:
        return  # already patched

    # The asymmetric fail handler in readSectors. The one in writeSectors
    # already calls spiStop(); we must NOT touch that one. Anchor the match
    # on readSectors' trailing #endif -> fail: return false; block which is
    # unique in the file (writeSectors' block has spiStop() on the fail line).
    OLD = (
        "  return readStop();\n"
        "#endif\n"
        "fail:\n"
        "  return false;\n"
        "}"
    )

    NEW = (
        "  return readStop();\n"
        "#endif\n"
        "fail:\n"
        "  " + MARKER + "\n"
        "  spiStop();\n"
        "  return false;\n"
        "}"
    )

    if OLD not in content:
        print(
            "WARNING: SdFat readSectors fail-handler patch target not found in %s "
            "-- library may have been updated" % filepath
        )
        return

    content = content.replace(OLD, NEW, 1)
    with open(filepath, "w") as f:
        f.write(content)
    print("Patched SdFat: spiStop() on readSectors fail: %s" % filepath)


# Run immediately at script import time (before compilation).
patch_sdfat(env)
