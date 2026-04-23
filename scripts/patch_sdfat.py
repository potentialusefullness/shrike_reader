"""
PlatformIO pre-build script: symmetrize SdFat's readSectors fail path.

Background (v1.8.4, revised):
  This patch was introduced in v1.8.4 on the theory that an unmatched
  spiStart in readSectors' fail path was leaking the SPI paramLock across
  tasks and eventually triggering

      assert failed: xTaskPriorityDisinherit tasks.c:5156

  After v1.8.4 still crashed with an identical stack trace, the analysis
  turned out to be wrong in one important detail: the readSectors fail
  branch is only reached when readData() returned false, and readData()
  already calls spiStop() on its own failure path. So this patch ends up
  inserting a second spiStop() -- which is harmless because spiStop()'s
  own `if (m_spiActive)` guard turns the second call into a no-op, but it
  is NOT the real fix.

  The patch is kept (rather than reverted) because:
    * It is idempotent and provably side-effect-free due to the m_spiActive
      guard, so it cannot introduce new bugs.
    * It does make the two fail paths (readSectors vs. writeSectors)
      symmetric, which is defensive.

  The real crash is investigated via patch_arduino_spi.py (SPI transaction
  trace hooks) and HalPowerManager MTX_TRACE in v1.8.5. Do not treat this
  script as a fix -- it is belt-and-braces only.

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
