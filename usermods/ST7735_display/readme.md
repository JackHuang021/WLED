# ST7735 80x160 TFT display

Drives a 0.96" ST7735 SPI TFT (160x80, landscape) from WLED, and puts the board's
single BOOT button to work as a control for the three things worth having without
a phone: on/off, brightness and preset.

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
     │ 14:32  2026-09-20        ☀ 65%                  ▁▃▅█         │  time, date, brightness, signal
  11 ├──────────────────────────────────────────────────────────────┤
  26 │                        Blink                                 │  effect name, size 2
  46 │                       Rainbow                                │  palette
  68 ├──────────────────────────────────────────────────────────────┤
  71 │ 192.168.1.42                                      1234 mA │  address, current draw
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

**The sun describes the level, not the power state.** Switching the strip off
drops `bri` to 0, but the icon stays at the level the strip would come back on
at and only the number beside it falls to `0%`. A ring next to a `0%` is the one
combination that reads as a fault — the icon looks broken rather than dimmed —
and the level is the more useful of the two things to have on screen while the
strip is dark.

Every field is a **fixed width**, and is padded with spaces to that width before
it is drawn — a value that shrinks (`192.168.1.42` → `No link`) would otherwise
leave the tail of the longer one behind it. The fields that end at the right edge
pad in front of the text instead, so it is the last character that is pinned to
the corner. The padding is doing the clearing:
the font is opaque, so writing the whole field covers exactly what a clear would
have, without the flash that a clear-then-draw between two SPI transactions
shows on a panel this small. A field whose padded string matches the one already
on screen is skipped outright, which is what keeps the current draw — the one
field that changes every second — from repainting itself over digits that did
not move.

For that reason a status bar is not **cleared** before it is drawn. Its fields
are padded to their full width and the two icons paint their own boxes, so
between them they cover most of the bar, and the columns they leave are ones
nothing else ever draws in.

The exception is the bottom bar's second layout. Its two layouts cannot share
columns — the AP needs an address *and* a password, and `Pass:` plus even a short
password is wider than the 54 px the current draw gives up — so switching
between them wipes the bar rather than painting over it. That happens when the
access point comes up or goes away: once per session, on a change the user made
deliberately, which is why it can afford the clear that the every-second
repaint cannot.

Within either layout the fields stay clear of each other, and that is not a
matter of taste: the font is opaque, so a field that reaches into another's
columns paints its padding across them. A 15-character address under a
right-aligned readout loses its tail that way — `192.168.1.42` comes out as
`192.168.1.`. The address is given the 90 px the longest IPv4 address takes and
the readout the 54 px the longest mA figure takes, 16 px apart, and two
`static_assert`s in the source stop either of them growing back over the other.

The centred lines cannot work that way. Where a centred string starts depends on
how long it is, so it cannot be padded out to a fixed width: a string that
shrinks vacates cells on its left that only a wipe takes back. Those lines are
cleared and redrawn when their text changes and skipped outright when it does
not, which is what keeps the ON/OFF line from flashing on the slow path between
one state and the next.

Anything that wipes the panel (`fillScreen`, a rotation change) resets that
cache, and a wipe of part of it — the main area, which has three layouts to swap
between — forgets the fields it covered. A full wipe also forgets which of the
bottom bar's two layouts was on screen, which is only ever needed to decide
whether swapping them has to wipe first.

Long names are cut rather than wrapped (`textWrap` is off), because a wrapped
line would print over the row below.

Each region is repainted on its own, and only when something in it changed: the
clock ticking does not disturb the effect name two rows below it. The current draw
is the one field that changes every second; it is right-aligned so that the `mA`
stays put while the number grows to its left. The brightness bar is repainted
only over the pixels between its old end and its new one: a held button steps it
about seven times a second, and refilling its whole width each time is the
largest flicker the screen produces.

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
| double press | next preset | on the second release |
| long press | brightness, stepping every 150 ms while held | 600 ms after pressing down |
| hold 5–10 s | start the access point | on release |
| hold 10 s + | factory reset | on release |

### The double press walks presets

Not effects, and the reason is arithmetic: there are around 220 effects compiled
in, so stepping one at a time is useless for reaching the one you want. Presets
are the set you already curated and named on the **Presets** page, so the same
gesture gets you between the handful of looks you actually use.

* It walks **existing** presets in id order and **skips the gaps** left by deleted
  ones, wrapping from the last back to the first.
* Applying a preset applies **whatever was saved in it** — palette, colour and
  brightness included, not just the effect. That is what a preset is; if you want
  the button to change nothing but the effect, save the preset without the
  brightness ticked.
* With **no presets saved at all**, the double press falls back to the next
  effect, so the gesture is never dead. The title reads `Effect` instead of
  `Preset` when this is what is happening.
* A preset saved without a name shows the effect it switches the strip to.
  Naming them on the Presets page is worth the ten seconds.
* There is still **no "previous preset"** — one button has no gesture left for it.
  See `docs/hmi.md` §3.4 for why presets are the answer here and what the
  alternatives cost.

Choosing a preset does not modify it — but a long press afterwards does; see
[below](#the-brightness-is-saved-back-into-the-preset).

**Brightness starts brighter, and the readout is what reverses it**: the first long
press of an adjustment brightens, and while the percentage is still on screen
pressing again reverses it and dims. Once the readout has dropped, the next long
press means "brighter" again — the direction is a property of one adjustment, not
a mode the button sits in, so it never carries over from the previous one. The
window for the reversal is `hmiOverlayMs` (1.5 s by default), the same clock as
the readout itself. Since the sign cannot be known before pressing, it is shown
**the moment the long press starts** — that is what the `+` / `-` next to the
percentage in the main area is for. The first few steps are finer (4) than the
rest (`hmiStep`, 16) so the bottom of the range can be set precisely.

### The brightness is saved back into the preset

WLED keeps its state — brightness, effect, colours, on/off — in RAM, and a preset
is the only thing that writes it to flash. So a preset is a snapshot, and until
now a brightness dialled in on the button was in no snapshot at all: switching
away and back brought the preset's own level back, and a reboot brought the
startup brightness.

What the button does about that is **the narrowest thing that makes the level
stick**: a long press on brightness writes the new level back into the preset the
strip is *in*, at the moment it is in it.

* Only the **long press** does it. A short press (on/off) and a double press are
  not saved — you turn the strip off to go to bed far more often than you mean to
  re-level a preset, and a preset that has gone dark because of one is hard to
  diagnose. A short press or a double press *before the brightness view has dropped*
  cancels the write the long press had queued, so pressing the button again is how
  you take it back.
* It writes into the preset the screen is showing, the `N` of `Preset N/M`, so that
  is the one a long press re-levels. There has to **be** one: at boot, and until the
  double press has walked into a preset, the button has nothing to save to and
  quietly does nothing.
* `hmiOverlayMs` is therefore also how long the level waits before it is written —
  it is the dwell of the brightness view. Shortening it makes the save happen
  sooner, at the cost of the readout disappearing sooner.
* A preset whose slot holds a **playlist** is left alone. Writing a state into it
  would replace the playlist with whatever the strip is doing right now.
* The write lands **when the brightness view drops and the main screen comes
  back** — not on release, and not after a delay you have to count. That moment is
  already on screen, so you can watch the screen go back to normal and know the
  level is stored. It also collapses a run of presses for free: each one puts the
  view back up, which holds the write off, so a run of steps lands as one write
  at the end rather than one per press.
* What goes in is the **whole current state**, not the brightness on its own —
  WLED has no way to patch one key of a preset. In practice that is the same
  thing, because nothing else has changed since the preset was applied. If
  something has, it goes in too, and the preset becomes a snapshot of now.
* The preset keeps its **name**. A save that cannot read the name back off the
  file is skipped rather than allowed to rename it.

To survive a **reboot**, set *Apply preset N at boot* under
[LED preferences](../../wled00/data/settings_leds.htm) to the preset you keep the
level in. Without a boot preset the strip still comes up at the startup brightness
— presets are the only persistence there is, and nothing here adds a second one.

Set `hmiSave` to `false` on the usermod settings page to turn the whole thing off,
in which case the button goes back to being unable to lose you anything.

### Operation views

Pressing a button replaces the main area with a view of what is being changed —
the preset name and its position for a double press, `ON`/`OFF` for a short press, a full-width bar
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
brightness when the AP comes up. That ramp is an artefact of the gesture rather
than a level anyone chose, and the same hold is how you get back from it — so it
cancels the write-back it would otherwise have queued. What you come back to is
the saved level, not full brightness.

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
* Presets only cycle forwards — one button has no gesture left for "previous".
* Once even one preset is saved, the raw effect list is no longer reachable from
  the button at all; the double press walks presets instead. Use the web UI, or
  save the effect you want as a preset.

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
  and preset selection, and they are not on screen either — see below.
  [docs/hmi.md](../../docs/hmi.md) works through what a menu would cost and how it
  would be added if these turn out to be needed.
* **A preset cannot be created from the button**, only chosen and then re-levelled
  by a long press; a new one still means the web UI. See
  [docs/hmi.md §9](../../docs/hmi.md#9-后续演进).
* **The SSID is no longer on screen.** The bottom bar has room for the address, not
  the network name; it is on the settings page instead.
* **Brightness, effect and on/off are volatile**, as they are everywhere else in
  WLED: they live in RAM until a preset is saved. A long press is the exception —
  it writes the new level back into the preset the strip is in, so switching away
  and back brings the new level with it
  ([details](#the-brightness-is-saved-back-into-the-preset)). It does not survive a
  reboot on its own: the strip boots at the startup brightness unless *Apply preset
  N at boot* points at the preset holding the level.
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
* **Speed, intensity and frame rate are not on screen.** They are in the web UI.
  The main area has the room for them if they turn out to be missed. The preset
  *name* does appear, but only while the double-press view is up; the idle main
  area still shows the effect and its palette.
