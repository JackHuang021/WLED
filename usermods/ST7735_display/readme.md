# ST7735 80x160 TFT display

Drives a 0.96" ST7735 SPI TFT (80x160) from WLED. It shows the same information
as the [ST7789 usermod](../ST7789_display/), re-flowed for the much smaller panel:

* Current date and time;
* Network SSID;
* IP address, or AP IP and password while the access point is up;
* Brightness;
* WiFi signal strength;
* Selected effect and palette;
* Effect speed and intensity;
* Estimated current in mA.

## Hardware

| signal | description |
|---|---|
| `TFT_MOSI` | SPI data in (SDA) |
| `TFT_SCLK` | SPI clock (SCL) |
| `TFT_CS`   | chip select |
| `TFT_DC`   | data/command (RS) |
| `TFT_BL`   | backlight, optional |
| `TFT_RST`  | reset, optional — leave it out if the module ties RESET to 3.3V |

There is no MISO: these panels are write-only, which is why `TFT_MISO` is set to -1.

## Library used

[Bodmer/TFT_eSPI](https://github.com/Bodmer/TFT_eSPI), pulled in automatically as a
dependency of this usermod's `library.json`.

The pinned 2.5.43 release cannot be used on the ESP32-C3 with arduino-esp32 v3.x — the
board reboots in a loop before the panel shows anything. The build therefore patches
two defects out of its C3 processor header, through
`pio-scripts/tft_espi_c3_fixes.py`, which the `wled_c3p` environment adds to
`extra_scripts`. **An environment that wants this usermod on a C3 has to add that script
too**; see [Troubleshooting](#troubleshooting) for what it does and when it can be
dropped.

## Setup

The driver, geometry and pinout of these modules cannot be detected at runtime, so
everything is configured at build time through build flags. Everything below goes
into the `build_flags` of the environment you are building, and the usermod itself
into `custom_usermods`:

```ini
build_flags = ${env:your_env.build_flags}
  ;; 0.96" ST7735 80x160 SPI TFT
  -D USER_SETUP_LOADED=1
  -D ST7735_DRIVER=1
  -D ST7735_REDTAB160x80=1
  ;-D TFT_RGB_ORDER=TFT_BGR   ;; uncomment if red and blue are swapped
  -D TFT_WIDTH=80
  -D TFT_HEIGHT=160
  -D TFT_MOSI=8
  -D TFT_SCLK=7
  -D TFT_CS=6
  -D TFT_DC=20
  -D TFT_BL=10
  -D TFT_RST=-1
  -D TFT_MISO=-1
  -D LOAD_GLCD=1
  -D SPI_FREQUENCY=27000000

custom_usermods =
  ${env:your_env.custom_usermods}
  ST7735_display
```

`TFT_WIDTH` / `TFT_HEIGHT` are given in **portrait** orientation, which is the
convention TFT_eSPI expects, and they stay portrait in the build flags even though the
panel is mounted landscape: the tab variant's offsets are portrait values and
TFT_eSPI swaps them itself once `setRotation()` picks a landscape rotation.
`SPI_FREQUENCY` is held at 27 MHz because TFT_eSPI warns that ST7735 panels misbehave
above that. The pins above are an example — this fork's board uses the ones in
`platformio_override.ini`.

The tab is the module's, not a guess: the vendor's own example for this panel
addresses columns 24..103 and rows 0..159 (`CASET 0x0018..0x0067`, `RASET
0x0000..0x009F`), i.e. `colstart = 24`, `rowstart = 0` — the REDTAB160x80 values.

The frame rate, inversion mode, power sequence and gamma are **not** build flags:
TFT_eSPI 2.5.43 compiles the legacy ST7735R (Adafruit) values, which light this panel
up but do not match it. The usermod re-issues the vendor's block through
`tft.commandList()` right after `tft.init()` — see `PANEL_INIT` in
[ST7735_display.cpp](ST7735_display.cpp) for the values and why they are ordered that
way. Those values are taken from the `TFT==96` branch of the vendor's own STM32 demo
(`40-tft_0_96/APP/tftlcd/tftlcd.c`, selected by `#define TFT 96` in its `tftlcd.h`),
which is the newer of the two sequences that file carries. Beware of a loose
`0.96寸初始化BOE+ST7735S.c` that ships alongside the same module: its power registers
are different again and blank this panel.

The pins and rotation are also reported on the usermod settings page. The pin values
are read-only there — they are compiled into TFT_eSPI, so changing them in the UI has
no effect on which GPIOs are used. Listing them still matters: WLED greys out every
pin the usermod names, which stops one of them being assigned to a relay or a LED bus
in the UI before this usermod gets a chance to claim it.

## Layout

The panel is mounted **landscape (160x80)**, so the rotation setting offers only 1 and
3 — the two landscape orientations, 180° apart, so the screen can be flipped for
mounting. Either is a runtime change (the MADCTL register is rewritten), so no rebuild
is needed to try the other one.

160 px is 26 characters at text size 1, but 80 px of height is only half the height the
portrait arrangement had, so the layout is split into two columns of 13 characters:

| left column | right column |
|---|---|
| date | effect name |
| clock (text size 2) | palette name |
| SSID | effect speed / intensity |
| IP address, or the AP's IP and password | estimated current in mA |
| brightness | |
| WiFi signal strength | |

Both columns are drawn on the same rows, so every line is clipped to 13 characters.
That clipping is what stops one column printing over the other; long effect and palette
names are cut rather than wrapped.

This is a first pass at the arrangement. The row positions are one block of `#define`s
near the top of [ST7735_display.cpp](ST7735_display.cpp), so re-flowing it is a matter
of moving those numbers.

## SPI bus

WLED and TFT_eSPI share the single global SPI object on ESP32-C3, and
`SPIClass::begin()` returns early once the bus is up *without* applying the pins it
was handed. WLED starts its SPI from the LED preferences before usermod `setup()`
runs, so if HW SPI pins are ever set there, TFT_eSPI's own `begin()` would silently
do nothing and the panel would stay dark.

To avoid that, `setup()` releases the bus (`SPI.end()`) whenever WLED has SPI
configured, and logs a line to that effect at boot. The consequence is that
**SPI-attached LED types cannot be used at the same time as this display** — leave
the SPI pin fields empty in LED Preferences.

## Backlight

The backlight is switched off after 5 minutes without any change, and comes back on
as soon as something changes.

Note that a running clock is itself a change once a minute, which would keep resetting
that timer, so **the automatic off only applies while NTP is disabled**. With NTP
enabled the display stays on continuously — which is normally what you want from a
clock. Drop the `TFT_BL` build flag entirely if you would rather the backlight never
be touched at all.

## Troubleshooting

**The image is offset or wrapped.** The "tab" variant is wrong. This is the single
most likely thing to be off, because different 80x160 ST7735 modules wire the panel
to the controller differently. TFT_eSPI has exactly two variants for this size:

| define | colstart | rowstart | inversion |
|---|---|---|---|
| `ST7735_REDTAB160x80` (the default above) | 24 | 0 | none |
| `ST7735_GREENTAB160x80` | 26 | 1 | sends `TFT_INVON` |

Swap which one is defined and rebuild. The variant is baked into the library at
compile time and cannot be selected at runtime. Two columns of shift is the visible
symptom, plus a 2 px wide band of whatever the controller had in RAM along one edge.

**Red and blue are swapped.** That is a separate setting from the tab. The colour
order the panel wants is in the vendor's `MADCTL` byte: `0x08`, the BGR bit with no
mirroring, which is also TFT_eSPI's default for this tab, so normally nothing needs
to change. If it does, the switch is `-D TFT_RGB_ORDER=1` — note the *literal* 1,
not `TFT_BGR`: TFT_eSPI 2.5.43 tests `#if (TFT_RGB_ORDER == 1)` in
`TFT_Drivers/ST7735_Defines.h` and never defines `TFT_BGR` as a number, so
`-D TFT_RGB_ORDER=TFT_BGR` is silently a no-op that leaves you on BGR.

**The board reboots in a loop before anything reaches the panel** — `rst:0x8
(TG1WDT_SYS_RST)`, with the saved PC inside `TFT_eSPI::writecommand()`. That is not a
wiring fault: it is one of the two TFT_eSPI 2.5.43 defects on the ESP32-C3 with
arduino-esp32 v3.x, both patched out by `pio-scripts/tft_espi_c3_fixes.py`. If you
build for a C3 in an environment that does *not* add that script to `extra_scripts`,
you get this.

### 1. It talks to the wrong SPI peripheral

The C3 has a single general-purpose SPI at `DR_REG_SPI2_BASE` (0x60024000). It is not
the only SPI near a `0x6000_2xxx` address: the flash controller is at 0x60002000.
arduino-esp32 v3 resolves the register macros in `soc/spi_reg.h` through a
`REG_SPI_BASE(i)` defined in `soc/soc.h` as

```c
#define REG_SPI_BASE(i) (((i)==2) ? (DR_REG_SPI2_BASE) : (DR_REG_SPI0_BASE - ((i) * 0x1000)))
```

so port index **2** reaches the general-purpose SPI and index **1** lands on the flash
controller. TFT_eSPI 2.5.43 writes `#define SPI_PORT SPI2_HOST`, and `SPI2_HOST` is the
enum value 1, so every register it touches by hand goes to the flash controller. It
then spins on `SPI_UPDATE` in that register, which never clears, the watchdog never
gets fed, and the chip resets. Nothing reaches the panel at all: the first thing
`init()` does with a `TFT_RST` pin defined is `writecommand(0x00)`, which is where it
dies.

Upstream now uses the literal `2` when `ESP_ARDUINO_VERSION_MAJOR >= 3` (commit
`c00d8f4e`, "Fix DMA on ESP32 C3 and S3 for board packages 3.x.x"), but that is not in
any release the registry serves.

### 2. It aliases MISO onto the MOSI pin

The 2.5.43 C3 header rewrites `TFT_MISO` from -1 to `TFT_MOSI`, so the panel's MISO ends
up on the same GPIO as its MOSI. arduino-esp32 v3's `SPIClass::begin()` then does:

```c
if (_miso >= 0 && !spiAttachMISO(_spi, _miso)) goto err;   // claims the pin for MISO
if (_mosi >= 0 && !spiAttachMOSI(_spi, _mosi)) goto err;   // same pin: fails
...
err: log_e("Attaching pins to SPI failed."); return false;
```

The first attach succeeds, the second cannot claim a pin that is already taken, so
`begin()` detaches MOSI, reports the failure and returns false — and TFT_eSPI never
checks the return value. The bus is left half set up: clock running, MOSI unattached,
so nothing reaches the panel. These modules are write-only, so `TFT_MISO` belongs at -1;
the patch deletes the alias (the condition also names the S2, but S2 builds use
`TFT_eSPI_ESP32.h`, so that part is dead here anyway).

### About the patch script

It is idempotent, reports each fix it applies at build time, and stops the build with a
clear message if a header it expects to patch changes shape — which is the signal to
bump the dependency and drop the script. Note that the header lives in `.pio/libdeps/`
and is **deleted and re-extracted by any clean build or dependency reinstall**, so
editing it by hand is not a fix that survives; that is exactly what this script is for.

**The board will not boot at all.** Check which GPIO the display shares with a
strapping pin. On the ESP32-C3, GPIO2 and GPIO8 **must be high at reset**
(`wled00/json.cpp:1149`, which flags both as `PIN_CAP_BOOTSTRAP`), and GPIO9 low
means "enter download mode". A display data line on GPIO8 — a very common choice for
MOSI — is fine once the board is running, because TFT_eSPI only drives it after
`init()`. But if the module has anything pulling that line low, the board can fail to
start. A 10k pull-up to 3.3V on the line is cheap insurance.

**Nothing on screen, but the build is fine.** Check the `PIN ALLOC: FAIL` lines in
the boot log (`-D WLED_DEBUG` enables them). The usermod gives up and disables
itself if any of its pins is already claimed by another peripheral — `GET /json/info`
will then report `"ST7735": "disabled"`.

**The screen is blank white or glowing with no content.** That is usually the
backlight working but the panel not being initialised, i.e. the driver define is
wrong — check that `ST7735_DRIVER` is set.

## Limitations

* **Landscape only.** The layout is drawn for 160x80, so the rotation setting offers
  only 1 and 3. Portrait (0 or 2) would need its own row positions — a single column
  of 13 characters over 160 px of height rather than the two columns below — and a
  branch to pick between them, which nothing currently does.
* The layout is a first pass and is expected to be revised — see
  [Layout](#layout) for how it is currently arranged.
