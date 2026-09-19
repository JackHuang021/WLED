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
# Both patches are idempotent and report what they did.  Delete this script (and its
# extra_scripts entry) once the pinned TFT_eSPI release contains the upstream fixes --
# it then finds nothing to do and says so, rather than breaking.
Import("env")

import sys
from pathlib import Path

PATCHES = [
  (
    "SPI port (GPSPI2, not the flash controller)",
    "#define SPI_PORT SPI2_HOST",
    """#if ESP_ARDUINO_VERSION_MAJOR < 3
  #define SPI_PORT SPI2_HOST
#else
  #define SPI_PORT 2
#endif""",
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
  ),
]


def _header_paths():
  """Every TFT_eSPI C3 processor header in this environment's libdeps directory.

  Libraries installed from the registry sit directly under libdeps as `TFT_eSPI`, but
  one installed from git gets a `TFT_eSPI@src-<hash>` suffix, so match either.
  """
  libdeps = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV")
  return sorted(libdeps.glob("*TFT_eSPI*/Processors/TFT_eSPI_ESP32_C3.h"))


def _patch(path):
  """Applies every patch to one header.  Returns a list of (description, result)."""
  text = path.read_text(encoding="utf-8")
  results = []
  for description, broken, fixed in PATCHES:
    if fixed in text:
      results.append((description, "already"))
    elif broken in text:
      text = text.replace(broken, fixed, 1)
      results.append((description, "patched"))
    else:
      results.append((description, "unknown"))
  path.write_text(text, encoding="utf-8")
  return results


_state = {"done": False}


def apply_patch(*_):
  """Patch TFT_eSPI if its header is installed.  Returns False while it is not."""
  if _state["done"]:
    return True

  headers = _header_paths()
  if not headers:
    return False  # dependency not installed yet -- the build middleware retries

  for header in headers:
    for description, result in _patch(header):
      if result == "unknown":
        sys.stderr.write(
          f"\nTFT_eSPI: {header} matches neither the broken nor the fixed form of\n"
          f"  {description}\n"
          "This script has gone stale -- check whether the pinned version still needs\n"
          "the ESP32-C3 workarounds (see the comment at the top of\n"
          "pio-scripts/tft_espi_c3_fixes.py).\n\n"
        )
        env.Exit(1)
      if result == "patched":
        sys.stderr.write(f"TFT_eSPI: patched {description} in {header}\n")

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
