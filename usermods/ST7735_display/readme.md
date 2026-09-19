# ST7735 80x160 TFT display

Drives a 0.96" ST7735 SPI TFT (160x80, landscape) from WLED, and puts the board's
single BOOT button to work as a control for the three things worth having without
a phone: on/off, brightness and effect.

The screen is two **persistent status bars** with a **main area** between them:

* Top bar — time, date, brightness as a sun icon plus a percentage, and WiFi
  signal strength as bars at the right edge;
* Main area — the selected effect and palette;
* Bottom bar — IP address (or the AP's address and password while the access
  point is up), and the estimated current draw in mA.

There is no menu: both bars are always there and the button acts directly on what
they show. The reasoning (and the alternatives that were rejected) is in
[docs/hmi.md](../../docs/hmi.md).

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
mounting. **3 is the default**; pick 1 if the picture comes up upside-down.

Either is a runtime change — the MADCTL register is rewritten and the screen is
repainted — so no rebuild is needed to try the other one. Note that the setting is
**persisted**, so a value already in `cfg.json` wins over the compiled-in default:
changing the default in the source only affects a board whose `um.ST7735` block has no
`rotation` key yet. To flip an existing board, use the dropdown on the settings page.

160 px is 26 characters at text size 1 (6x8 GLCD font), and 80 px is ten rows. Two
one-line status bars frame a 56 px main area:

```
   0 ┌──────────────────────────────────────────────────────────────┐
     │ 14:32  9/19              ☀ 65%                  ▁▃▅█         │  time, date, brightness, signal
  11 ├──────────────────────────────────────────────────────────────┤
  26 │                        Blink                                 │  effect name, size 2
  46 │                       Rainbow                                │  palette
  68 ├──────────────────────────────────────────────────────────────┤
  71 │ 192.168.1.42                                1234 mA          │  address, current draw
  79 └──────────────────────────────────────────────────────────────┘
```

The split is by how often a value changes and who can change it. The top bar holds
what the button edits and what moves on its own; the bottom bar holds the two
things that are slow, wide, and only worth reading occasionally.

Each bar is 11 px rather than the 8 the font needs, because the two icons on the
top bar are 11x11. Below that size a sun cannot have both a round core and rays
clear of it, which is what it takes to stop it reading as a cross.

**Signal strength is four bars** — the one icon nobody needs a label for: 2 px
wide with a 1 px gap, 4/6/8/10 px tall, lit from the left as `quality` crosses 25,
50, 75 and 100. Without a link there is nothing to measure, so they are drawn in
the trough colour rather than not drawn: the icon stays recognisable.

**Brightness is a sun**, not a bar. The core is a filled disc of radius 2 and stays
that size; the level only lengthens the rays. All eight of them are always drawn,
one pixel clear of the core: a single pixel each up to 25% brightness, two past it,
which takes the sun out to the edges of its 11x11 box. Leaving the four diagonals
for a higher tier, as the first version did, put a disc with only the four axis
rays on screen across the middle of the range — and that is a cross, not a sun. At
zero it is a hollow ring, so "off" still reads as a brightness icon. Both icons are
drawn from primitives rather than stored as bitmaps because the sun takes the live
LED colour, the same one the brightness overlay's bar uses. The percentage 2 px
away carries the value the icon can only approximate.

Every field is a **fixed width**, and is cleared to that width before it is
drawn — a value that shrinks (`192.168.1.42` → `No link`) would otherwise leave
the tail of the longer one behind it. Long names are cut rather than wrapped
(`textWrap` is off), because a wrapped line would print over the row below.

Each region is repainted on its own, and only when something in it changed: the
clock ticking does not disturb the effect name two rows below it. The current draw
is the one field that changes every second; it is right-aligned so that the `mA`
stays put while the number grows to its left.

**Degraded states**, all of which are handled:

| state | what changes |
|---|---|
| station connected | address in the bottom bar in green, bars lit on the top bar |
| access point up | address in the bottom bar turns orange and is joined by the AP password |
| no link | `No link` in orange, no bars lit |
| NTP off | clock shows `--:--` and the date field is left empty |

The row positions, field widths and colours are one block of `#define`s and
`static const uint16_t` near the top of
[ST7735_display.cpp](ST7735_display.cpp), so re-flowing the arrangement is a
matter of moving those numbers.

## Button

The board has one button (the C3's BOOT button on GPIO9), and this usermod takes
it over. Its gestures mirror WLED's own so the timing feels the same:

| gesture | action | when it fires |
|---|---|---|
| short press | on / off | 350 ms after release — the double-press window has to close first |
| double press | next effect | on the second release |
| long press | brightness, stepping every 150 ms while held | 600 ms after pressing down |
| hold 5–10 s | start the access point | on release |
| hold 10 s + | factory reset | on release |

**Brightness direction alternates**: each long press flips it, so the first one
brightens and the next dims. Since the sign cannot be known before pressing, the
direction is shown **the moment the long press starts** — that is what the `+` /
`-` next to the percentage in the main area is for. The first few steps are finer
(4) than the rest (`hmiStep`, 16) so the bottom of the range can be set precisely.

### Operation views

Pressing a button replaces the main area with a view of what is being changed —
the effect name for a double press, `ON`/`OFF` for a short press, a full-width bar
and percentage while the brightness is adjusted. It is **not a screen you have to
leave**: it disappears on its own 1.5 s after the last press. Nothing needs
confirming, because every change takes effect immediately.

### The escape hatches are reimplemented, not delegated

`handleButton()` returning true makes `wled00/button.cpp` skip its own handling of
button 0 **entirely** — including the 5 s AP and 10 s factory-reset holds. Those
are reimplemented in [ui_hmi.cpp](ui_hmi.cpp) with the same thresholds.

Handing the press back to WLED part-way through instead does *not* work, and it is
worth knowing why: `button.cpp` only advances a button's press state while it owns
that button ([button.cpp:304](../../wled00/button.cpp#L304)), so a press it has
been ignoring looks like a **fresh** press the moment it is handed back. The
release would then measure a duration of nearly zero and neither threshold would
ever be reached. If you change the takeover logic, keep the escape hatches in
`HmiGesture` — they are the only software route back from a bad Wi-Fi
configuration, alongside holding GPIO9 down while the board resets.

One consequence to be aware of: a hold that reaches the 5 s AP threshold has also
been adjusting brightness for the previous 4.4 s, so the strip will be at full
brightness when the AP comes up. Brightness is not persisted, so a reboot (or the
preset that gets applied afterwards) restores it.

### What is given up

While the display usermod owns button 0:

* The three button 0 macros on the **Time settings** page do nothing. Button
  indices other than 0 are untouched.
* WLED's default actions for that button — short-press toggle, long-press random
  colour — are replaced by the table above. The random colour is the only one
  with no direct replacement.
* No `button/0` message is published to MQTT, because that publish lives inside
  the handlers that no longer run. State changes still notify normally, since
  everything here goes through `stateUpdated(CALL_MODE_BUTTON)`.
* Effects only cycle forwards — one button has no gesture left for "previous".

Set `hmi` to `false` on the usermod settings page to hand the button back to WLED
completely; the display keeps working.

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
  only 1 and 3. Portrait (0 or 2) would need its own row positions and field widths —
  a single column of 13 characters over 160 px of height instead of two status bars
  — and a branch to pick between them, which nothing currently does.
* **No menu.** Effect palette, speed and intensity are not reachable from the
  button; they need the web UI. The three gestures are spent on on/off, brightness
  and effect selection, and they are not on screen either — see below.
  [docs/hmi.md](../../docs/hmi.md) works through what a menu would cost and how it
  would be added if these turn out to be needed.
* **The SSID is no longer on screen.** The bottom bar has room for the address, not
  the network name; it is on the settings page instead.
* **Brightness, effect and on/off are volatile**, as they are everywhere else in
  WLED: they survive until the next reboot unless a preset is saved. The button
  cannot save one.
* **The screen cannot be switched off.** With `TFT_BL` at -1 there is no backlight
  pin to turn down, and a colour TFT's backlight is a continuous drain. The
  automatic blanking described under [Backlight](#backlight) is a no-op on such a
  module. If the display is meant to sit somewhere dark, this is worth solving in
  hardware before anything else.
* **The bottom bar is full.** Its two fields are sized for the worst case they can
  actually reach (a 15-character IPv4 address, `65535 mA`), which leaves 16 px
  between them. The top bar has 40 px of slack in the middle, so it is the one
  with room to grow.
* **The AP password displaces the current draw**, not the address: while the
  access point is up the bottom bar becomes its address and password, because
  joining it is the only thing anyone is doing at that moment.
* **Speed, intensity, preset and frame rate are not on screen.** They are in the
  web UI. The main area has the room for them if they turn out to be missed.
