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
  -D ST7735_GREENTAB160x80=1
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
convention TFT_eSPI expects. `SPI_FREQUENCY` is held at 27 MHz because TFT_eSPI
warns that ST7735 panels misbehave above that.

The pins and rotation are also reported on the usermod settings page. The pin values
are read-only there — they are compiled into TFT_eSPI, so changing them in the UI has
no effect on which GPIOs are used. Listing them still matters: WLED greys out every
pin the usermod names, which stops one of them being assigned to a relay or a LED bus
in the UI before this usermod gets a chance to claim it.

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

| define | colstart | rowstart |
|---|---|---|
| `ST7735_GREENTAB160x80` (the default above) | 26 | 1 |
| `ST7735_REDTAB160x80` | 24 | 0 |

Swap which one is defined and rebuild. The variant is baked into the library at
compile time and cannot be selected at runtime.

**Red and blue are swapped.** That is a separate setting from the tab:
`ST7735_GREENTAB160x80` sends `TFT_INVON` but does not touch the MADCTL colour
order, so add

```ini
  -D TFT_RGB_ORDER=TFT_BGR
```

to `build_flags` and rebuild.

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

* **Portrait only.** The layout is laid out for 80 px of width. It fits about ten
  rows; using rotation 1 or 3 (landscape, 160x80) would need a different layout,
  which is why the rotation setting only offers the two portrait values.
* The layout is a first pass and is expected to be revised.
