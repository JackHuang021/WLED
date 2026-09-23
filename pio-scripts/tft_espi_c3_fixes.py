# TFT_eSPI 2.5.43 cannot drive a display on the ESP32-C3 with arduino-esp32 v3.x.
# Two independent defects have to be patched out of its C3 processor header before it
# works at all; both put the board into a watchdog-reset loop inside tft.init().
# Full explanation: usermods/ST7735_display/readme.md.
#
# 1. Wrong SPI peripheral.  TFT_eSPI pokes the SPI registers by hand through
#    soc/spi_reg.h's SPI_*_REG(SPI_PORT) macros and defines `#define SPI_PORT SPI2_HOST`,
#    where SPI2_HOST is the enum value 1.  arduino-esp32 v3 resolves those macros through
#    soc/soc.h's
#        #define REG_SPI_BASE(i) (((i)==2) ? DR_REG_SPI2_BASE : (DR_REG_SPI0_BASE - i*0x1000))
#    so index 1 lands on the flash controller (0x60002000) and only index 2 reaches the
#    general purpose SPI (0x60024000).  TFT_eSPI then spins on SPI_UPDATE in the wrong
#    peripheral's register.  Upstream uses the literal 2 once ESP_ARDUINO_VERSION_MAJOR
#    >= 3; this patch is a verbatim copy of that fix.
#
# 2. MISO aliased onto the MOSI pin.  Upstream rewrites TFT_MISO from -1 to TFT_MOSI for
#    the C3 (and S2).  arduino-esp32 v3's SPIClass::begin() then attaches MISO to that
#    GPIO first and fails to attach MOSI to the same one, hits its `goto err`, and returns
#    false -- which TFT_eSPI never checks.  The bus is left half configured (clock up,
#    MOSI detached) and the panel gets nothing.  These panels are write-only, so TFT_MISO
#    belongs at -1.  The condition also names the S2, but S2 builds use TFT_eSPI_ESP32.h
#    (see the dispatch in TFT_eSPI.h), so the block is dead here and is dropped outright.
#
# 3. Startup timing, trimmed out of the ST7735 init table.  The firmware works without this
#    -- it is the largest single piece of the ~5 s this board takes to put a first frame on
#    the panel.  tft.init() walks the legacy ST7735R table, whose waits total ~0.9 s: 150 ms
#    after the hardware reset (TFT_eSPI.cpp), then in Rcmd1 150 ms after SWRESET and 500 ms
#    after SLPOUT, then 100 ms after DISPON in Rcmd3.  Every one of those registers is
#    re-issued by the usermod's own PANEL_INIT immediately afterwards, in the vendor's
#    order, so the panel is configured for real only once the waits are already over.
#    The three numbers below are cut to 120/120/20 ms -- still well clear of what an
#    ST7735S asks for after a reset or a sleep-out, which is 120 ms, and the display-on
#    wait is slack the controller does not use.  If a panel ever comes up blank on a cold
#    boot, these are the first numbers to put back; they are the only ones here that trade
#    margin for speed.
#
# Fatality is per patch: a patch is fatal when the firmware's behaviour depends on it.  The
# two C3 defects are fatal because the board watchdog-resets without them.  The timing trims
# are not -- if a future TFT_eSPI reshapes the table the build should still produce working
# firmware, just a slower one, so those warn and carry on.  A stale fatal patch stops the
# build rather than shipping a firmware that resets forever.
#
# Every patch is idempotent and reports what it did.  Delete this script (and its
# extra_scripts entry) once the pinned TFT_eSPI release contains the upstream fixes --
# it then finds nothing to do and says so, rather than breaking.
Import("env")

import sys
from pathlib import Path

C3_PROCESSOR_HEADER = "*TFT_eSPI*/Processors/TFT_eSPI_ESP32_C3.h"
ST7735_INIT_TABLE    = "*TFT_eSPI*/TFT_Drivers/ST7735_Init.h"

# (glob, patches), where a patch is (description, broken text, fixed text, fatal).
# The glob also matches a git-installed library, which gets a `TFT_eSPI@src-<hash>`
# suffix rather than sitting directly under libdeps as `TFT_eSPI`.
TARGETS = [
  (
    C3_PROCESSOR_HEADER,
    [
      (
        "SPI port (GPSPI2, not the flash controller)",
        "#define SPI_PORT SPI2_HOST",
        """#if ESP_ARDUINO_VERSION_MAJOR < 3
  #define SPI_PORT SPI2_HOST
#else
  #define SPI_PORT 2
#endif""",
        True,
      ),
      (
        "MISO left at -1 instead of aliased onto the MOSI pin",
        """    #if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32S2)
      #if (TFT_MISO == -1)
        #undef TFT_MISO
        #define TFT_MISO TFT_MOSI
      #endif
    #endif""",
        """    // Left at -1: these panels are write-only, and aliasing MISO to the MOSI pin
    // makes SPIClass::begin() fail to attach MOSI and return false (unchecked).
    // See pio-scripts/tft_espi_c3_fixes.py.""",
        True,
      ),
    ],
  ),
  (
    ST7735_INIT_TABLE,
    [
      (
        "SWRESET waits 120 ms, not 150 ms",
        "      150,                    //     150 ms delay\n",
        "      120,                    //     120 ms after SWRESET - trimmed, see"
        " pio-scripts/tft_espi_c3_fixes.py\n",
        False,
      ),
      (
        "SLPOUT waits 120 ms, not 500 ms",
        "      255,                    //     500 ms delay\n",
        "      120,                    //     120 ms after SLPOUT - trimmed, see"
        " pio-scripts/tft_espi_c3_fixes.py\n",
        False,
      ),
      (
        "DISPON waits 20 ms, not 100 ms",
        "      100 };                  //     100 ms delay\n",
        "       20 };                  //      20 ms after DISPON - trimmed, see"
        " pio-scripts/tft_espi_c3_fixes.py\n",
        False,
      ),
    ],
  ),
]

_STALE_HINT = (
  "This script has gone stale -- check whether the pinned version still needs\n"
  "the ESP32-C3 workarounds (see the comment at the top of\n"
  "pio-scripts/tft_espi_c3_fixes.py).\n\n"
)


def _matching_paths(pattern):
  """Every TFT_eSPI file in this environment's libdeps directory matching `pattern`."""
  libdeps = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV")
  return sorted(libdeps.glob(pattern))


def _patch(path, patches):
  """Applies every patch to one file.  Returns a list of (description, result, fatal).

  A result is "already" when the fixed text is present, "patched" when the broken text
  was found and replaced, and "unknown" when the file carries neither form.
  """
  text = path.read_text(encoding="utf-8")
  results = []
  for description, broken, fixed, fatal in patches:
    if fixed in text:
      results.append((description, "already", fatal))
    elif broken in text:
      text = text.replace(broken, fixed, 1)
      results.append((description, "patched", fatal))
    else:
      results.append((description, "unknown", fatal))
  path.write_text(text, encoding="utf-8")
  return results


def _report(path, description, result, fatal):
  """Writes one patch's outcome.  Returns False if the build should stop."""
  if result == "patched":
    sys.stderr.write(f"TFT_eSPI: patched {description} in {path}\n")
    return True
  if result != "unknown":
    return True

  sys.stderr.write(
    f"\nTFT_eSPI: {path} matches neither the broken nor the fixed form of\n"
    f"  {description}\n"
  )
  if fatal:
    sys.stderr.write(_STALE_HINT)
    return False
  sys.stderr.write(
    "This patch only trims startup time, so the build is carrying on without it\n"
    "and the display will take longer to light up. Re-check the timing trims at the\n"
    "top of pio-scripts/tft_espi_c3_fixes.py against the new table.\n\n"
  )
  return True


_state = {"done": False}


def apply_patch(*_):
  """Patch TFT_eSPI if it is installed.  Returns False while it is not."""
  if _state["done"]:
    return True

  if not _matching_paths(C3_PROCESSOR_HEADER):
    return False  # dependency not installed yet -- the build middleware retries

  for pattern, patches in TARGETS:
    paths = _matching_paths(pattern)
    if not paths:
      sys.stderr.write(f"\nTFT_eSPI: nothing matched {pattern}.\n")
      if any(fatal for _, _, _, fatal in patches):
        sys.stderr.write(_STALE_HINT)
        env.Exit(1)
      continue
    for path in paths:
      for description, result, fatal in _patch(path, patches):
        if not _report(path, description, result, fatal):
          env.Exit(1)

  _state["done"] = True
  return True


def ensure_patched(node=None):
  """Build middleware: retries on every source until the header is there.

  Reaching TFT_eSPI's own source means the dependency is definitely installed, so a
  header that is still missing at that point is a bug in this script rather than a
  matter of timing -- fail the build instead of silently flashing a firmware that
  watchdog-resets.
  """
  if not apply_patch() and node is not None and "TFT_eSPI" in str(node):
    sys.stderr.write(
      f"\nTFT_eSPI: compiling {node} but no C3 processor header was found under\n"
      f"{Path(env.subst('$PROJECT_LIBDEPS_DIR')) / env.subst('$PIOENV')}.\n"
      "This script is not patching anything -- refusing to build a firmware that\n"
      "will not work on the ESP32-C3.  See the comment at the top of\n"
      "pio-scripts/tft_espi_c3_fixes.py.\n\n"
    )
    env.Exit(1)
  return node


# Best effort at configure time, so an incremental build gets a patched header (and a
# stale script fails loudly) before anything is compiled.
apply_patch()

# And again just before sources are compiled, which is what covers a clean build: the
# dependency is installed after the configure step, so the attempt above finds nothing.
env.AddBuildMiddleware(ensure_patched)
