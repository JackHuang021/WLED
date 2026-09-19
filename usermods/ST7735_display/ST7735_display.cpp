// ST7735 80x160 SPI TFT (0.96" module) connected to WLED.
//
// Shows the same information as usermods/ST7789_display, re-flowed for a much
// smaller panel. The panel is mounted landscape (160x80) and the layout is
// deliberately a first pass - see Y_DATE and friends below.
//
// TFT_eSPI is configured entirely from build flags (see readme.md), because the
// tab variant, geometry and pinout of these modules cannot be probed at runtime.
#include "wled.h"
#include <TFT_eSPI.h>
#include <SPI.h>

/*
 * TFT_eSPI gets its geometry, driver and pins from the build flags documented in
 * readme.md. The generic CI environments in .github/workflows/usermods.yml do not
 * set any of them and fall back to TFT_eSPI's bundled User_Setup.h, so the guards
 * below warn and substitute -1 rather than failing the build - a build that cannot
 * see the panel still has to link.
 */
#ifndef USER_SETUP_LOADED
  #warning "ST7735_display: USER_SETUP_LOADED is not defined, TFT_eSPI will use its own User_Setup.h. See usermods/ST7735_display/readme.md."
#endif

#if defined(USER_SETUP_LOADED) && (!defined(TFT_WIDTH) || !defined(TFT_HEIGHT))
  #error "ST7735_display: USER_SETUP_LOADED is set but TFT_WIDTH/TFT_HEIGHT are missing."
#endif

#ifndef TFT_WIDTH
  #define TFT_WIDTH  80
#endif
#ifndef TFT_HEIGHT
  #define TFT_HEIGHT 160
#endif
#ifndef TFT_SCLK
  #define TFT_SCLK -1
#endif
#ifndef TFT_MOSI
  #define TFT_MOSI -1
#endif
#ifndef TFT_DC
  #define TFT_DC -1
#endif
#ifndef TFT_CS
  #define TFT_CS -1
#endif
#ifndef TFT_RST
  #define TFT_RST -1   // no reset pin: TFT_eSPI issues a software reset instead
#endif
#ifndef TFT_BL
  #define TFT_BL -1    // no backlight control pin
#endif

/*
 * TFT_WIDTH/TFT_HEIGHT are the portrait geometry TFT_eSPI is constructed with, and
 * what the tab variant's colstart/rowstart assume, so they have to stay portrait.
 * The panel is mounted landscape, i.e. rotation 1 or 3, which is the orientation the
 * layout below is drawn for.
 */
#define PANEL_WIDTH   TFT_HEIGHT
#define PANEL_HEIGHT  TFT_WIDTH

// The layout splits that 160 px into two columns of 80 px - the portrait width - so
// each column holds the 13 characters the portrait layout had.
#define COLUMN_WIDTH     (PANEL_WIDTH / 2)
#define RIGHT_COLUMN_X   COLUMN_WIDTH

// 6x8 GLCD font, so this is the number of characters that fit in one column at
// text size 1.
#define CHARS_PER_COLUMN  (COLUMN_WIDTH / 6)

/*
 * Row positions, left column first. The clock keeps its two rows reserved even when
 * NTP is off, so the layout does not shift around.
 *
 * This is a first pass at the arrangement, not a finished layout.
 */
#define Y_DATE       0   // left column
#define Y_CLOCK     10   // left: text size 2, so 16 px tall
#define Y_SSID      30   // left
#define Y_IP        42   // left
#define Y_BRI       54   // left
#define Y_SIG       66   // left

#define Y_MODE       0   // right column
#define Y_PALETTE   16   // right
#define Y_FX        32   // right
#define Y_CURRENT   48   // right

// Extra chars (+1) for the null terminator, sized for the longest line we build.
#define LINE_BUFFER_SIZE 24

// How often we check whether anything we display has changed.
#define USER_LOOP_REFRESH_RATE_MS 1000

// Turn the backlight off after this long without any change.
#define DISPLAY_IDLE_OFF_MS (5UL * 60UL * 1000UL)

TFT_eSPI tft = TFT_eSPI(TFT_WIDTH, TFT_HEIGHT); // Invoke custom library

/*
 * The init table compiled into TFT_eSPI 2.5.43 is the legacy ST7735R one (Adafruit's
 * values), and the tab variant only covers the address window and the inversion
 * command. It is close enough to light the panel up but it is not what this module
 * wants: the frame rate, the inversion mode, the power sequence and the gamma curve
 * are all different, which shows up as poor contrast, wrong brightness or buzzing
 * pixels. The vendor's own sequence for this panel is re-issued here, in the
 * vendor's order, right after tft.init() has done the reset and the sleep-out.
 *
 * Source: the vendor's STM32 example for this panel, the `TFT==96` branch of
 * `40-tft_0_96/APP/tftlcd/tftlcd.c` (its tftlcd.h pins `#define TFT 96`). That branch
 * carries two sequences, the older one commented out directly above the live one; a
 * loose `0.96寸初始化BOE+ST7735S.c` that ships with the module has a third, different
 * set again. This is the one the vendor actually compiles. Its power registers sit
 * much closer to what TFT_eSPI had (0xC0 = 62 02 04 against TFT_eSPI's A2 02 84 and
 * the loose file's 0E 0E 04); the loose file's values blanked the panel outright.
 *
 * Ordering: reset, sleep out, configure, and only then display on. TFT_eSPI already
 * did the reset (0x01/0x11 with their delays), so the register writes are re-issued
 * here in the vendor's order, including its 0x20 and its trailing 0x29. 0x36 is left
 * out because setRotation() writes it a few lines later - and it has to, since the
 * vendor's portrait value 0x08 does not apply to the landscape rotations this usermod
 * uses; 0x2A/0x2B are the tab variant's, and 0x3A is TFT_eSPI's own (same value).
 *
 * Format: TFT_eSPI's commandList() - count, then (command, argument count, args).
 */
static const uint8_t PROGMEM PANEL_INIT[] = {
  15,                                             // commands in the list
  0x20, 0,                                        // inversion off - this panel never inverts
  0xB1, 3, 0x05, 0x3A, 0x3A,                      // frame rate: normal mode
  0xB2, 3, 0x05, 0x3A, 0x3A,                      //             idle mode
  0xB3, 6, 0x05, 0x3A, 0x3A, 0x05, 0x3A, 0x3A,    //             partial mode
  0xB4, 1, 0x03,                                  // dot inversion
  0xC0, 3, 0x62, 0x02, 0x04,                      // power sequence
  0xC1, 1, 0xC0,
  0xC2, 2, 0x0D, 0x00,
  0xC3, 2, 0x8D, 0x6A,
  0xC4, 2, 0x8D, 0xEE,
  0xC5, 1, 0x0E,                                  // VCOM
  0xE0, 16, 0x10, 0x0E, 0x02, 0x03, 0x0E, 0x07, 0x02, 0x07,
            0x0A, 0x12, 0x27, 0x37, 0x00, 0x0D, 0x0E, 0x10,  // gamma, positive
  0xE1, 16, 0x10, 0x0E, 0x03, 0x03, 0x0F, 0x06, 0x02, 0x08,
            0x0A, 0x13, 0x26, 0x36, 0x00, 0x0D, 0x0E, 0x10,  // gamma, negative
  0x3A, 1, 0x05,                                  // 16-bit colour, unchanged
  0x29, 0                                         // display on
};

// Pins claimed through PinManager. -1 entries are treated as "not present" and
// skipped, so an unconnected RST or BL costs nothing.
static const PinManagerPinType displayPins[] = {
  { TFT_CS,   true },
  { TFT_DC,   true },
  { TFT_RST,  true },
  { TFT_BL,   true },
  { TFT_SCLK, true },
  { TFT_MOSI, true }
};

class St7735DisplayUsermod : public Usermod {
  private:
    static const char _name[];

    bool enabled = false;             // false if the pins could not be claimed
    bool initDone = false;
    uint8_t rotation = 1;             // 1 or 3, both landscape
    uint8_t appliedRotation = 0;

    unsigned long lastUpdate = 0;
    unsigned long lastRedraw = 0;
    bool displayTurnedOff = false;

    // needRedraw marks if redraw is required to prevent often redrawing.
    bool needRedraw = true;
    // Next variables hold the previous known values to determine if redraw is required.
    String knownSsid = "";
    IPAddress knownIp;
    uint8_t knownBrightness = 0;
    uint8_t knownMode = 0;
    uint8_t knownPalette = 0;
    uint8_t knownEffectSpeed = 0;
    uint8_t knownEffectIntensity = 0;
    uint8_t knownMinute = 99;
    uint8_t knownHour = 99;

    // Pad a line with leading spaces so it is centred in `width` characters.
    void center(String &line, uint8_t width) {
      int len = line.length();
      if (len < width) for (byte i = (width - len) / 2; i > 0; i--) line = ' ' + line;
      for (byte i = line.length(); i < width; i++) line += ' ';
    }

    // Clip to one column. Both columns are drawn on the same rows, so a line that ran
    // one character long would print straight into the other column.
    String fitToColumn(const String &text) {
      return (text.length() > CHARS_PER_COLUMN) ? text.substring(0, CHARS_PER_COLUMN) : text;
    }

    void backlight(bool on) {
      if (TFT_BL >= 0) digitalWrite(TFT_BL, on ? HIGH : LOW);
    }

    // Print one body line into a column, without advancing a stored cursor.
    void printColumn(const String &text, uint8_t x, uint8_t y, uint16_t color, bool centered = true) {
      String line = fitToColumn(text);
      if (centered) center(line, CHARS_PER_COLUMN);
      tft.setTextColor(color);
      tft.setTextSize(1);
      tft.setCursor(x, y);
      tft.print(line.c_str());
    }

    /**
     * Display the current date and time: a small date line and a large clock.
     */
    void showTime() {
      if (!ntpEnabled) return;
      char lineBuffer[LINE_BUFFER_SIZE];

      updateLocalTime();
      byte minuteCurrent = minute(localTime);
      byte hourCurrent   = hour(localTime);
      knownMinute = minuteCurrent;
      knownHour   = hourCurrent;

      byte showHour = hourCurrent;
      bool isAM = false;
      if (useAMPM) {
        if (showHour == 0) {
          showHour = 12;
          isAM = true;
        } else if (showHour > 12) {
          showHour -= 12;
          isAM = false;
        } else {
          isAM = true;
        }
      }

      // Date on top, with the AM/PM marker appended - the column is only 80 px wide,
      // so there is no room for a separate suffix next to it.
      sprintf_P(lineBuffer, PSTR("%s %2d"), monthShortStr(month(localTime)), day(localTime));
      if (useAMPM) strcat_P(lineBuffer, isAM ? PSTR(" AM") : PSTR(" PM"));
      printColumn(String(lineBuffer), 0, Y_DATE, TFT_SILVER);

      // Clock, text size 2 -> 12x16 px per character, centred in the left column.
      sprintf_P(lineBuffer, PSTR("%2d:%02d"), (useAMPM ? showHour : hourCurrent), minuteCurrent);
      tft.setTextSize(2);
      tft.setTextColor(TFT_WHITE);
      tft.setCursor((COLUMN_WIDTH - strlen(lineBuffer) * 12) / 2, Y_CLOCK);
      tft.print(lineBuffer);
    }

    void drawScreen() {
      char lineBuffer[LINE_BUFFER_SIZE];

      tft.fillScreen(TFT_BLACK);
      // Always draw from the top-left corner; the splash screen uses the same
      // datum so there is nothing to reset, but being explicit avoids a whole
      // class of "everything is drawn off-centre" bugs.
      tft.setTextDatum(TL_DATUM);

      showTime();

      String line;

      // Left column: the network side.

      // Network name
      line = knownSsid.substring(0, CHARS_PER_COLUMN);
      // Print `~` char to indicate that SSID is longer, than our display
      if (knownSsid.length() > CHARS_PER_COLUMN) line = line.substring(0, CHARS_PER_COLUMN - 1) + '~';
      printColumn(line, 0, Y_SSID, TFT_GREEN);

      if (apActive) {
        // Print AP IP and password while the AP is up. The "AP IP:" and "Pass:" labels
        // do not fit alongside their values in 13 characters, so the IP goes on the row
        // the station IP would use, and the signal row - free here, since there is no
        // WiFi signal to report - carries the label instead.
        printColumn(knownIp.toString(), 0, Y_IP, TFT_GREEN);
        line = "Pass:";
        line += apPass;
        printColumn(line, 0, Y_BRI, TFT_GREEN, false);
        printColumn(F("AP mode"), 0, Y_SIG, TFT_ORANGE);
      } else {
        printColumn(knownIp.toString(), 0, Y_IP, TFT_GREEN);

        // brightness
        sprintf_P(lineBuffer, PSTR("Bri %3d%%"), (int)bri * 100 / 255);
        printColumn(String(lineBuffer), 0, Y_BRI, TFT_WHITE);

        // Signal quality, colour coded. WiFi.RSSI() reads 0 while disconnected, which
        // would show up as a misleading 0%, so leave the row blank until we have a link.
        if (WLED_CONNECTED) {
          int quality = getSignalQuality(WiFi.RSSI());
          sprintf_P(lineBuffer, PSTR("Sig %3d%%"), quality);
          uint16_t sigColor = (quality < 10) ? TFT_RED : (quality < 25) ? TFT_ORANGE : TFT_GREEN;
          printColumn(String(lineBuffer), 0, Y_SIG, sigColor);
        }
      }

      // Right column: what the strip is doing.

      // Effect name
      char nameBuffer[CHARS_PER_COLUMN + 1];
      extractModeName(knownMode, JSON_mode_names, nameBuffer, CHARS_PER_COLUMN);
      printColumn(String(nameBuffer), RIGHT_COLUMN_X, Y_MODE, TFT_CYAN);

      // Palette name
      extractModeName(knownPalette, JSON_palette_names, nameBuffer, CHARS_PER_COLUMN);
      printColumn(String(nameBuffer), RIGHT_COLUMN_X, Y_PALETTE, TFT_YELLOW);

      // Effect speed and intensity
      sprintf_P(lineBuffer, PSTR("Spd%3d Int%3d"), knownEffectSpeed, knownEffectIntensity);
      printColumn(String(lineBuffer), RIGHT_COLUMN_X, Y_FX, TFT_SILVER);

      // Estimated milliamp usage (needs the LED type to be set in LED prefs to be
      // a reasonable estimate).
      sprintf_P(lineBuffer, PSTR("Cur %umA"), BusManager::currentMilliamps());
      printColumn(String(lineBuffer), RIGHT_COLUMN_X, Y_CURRENT, TFT_ORANGE);
    }

  public:
    /*
     * setup() is called once at boot. WiFi is not yet connected at this point.
     */
    void setup() override {
      if (!PinManager::allocateMultiplePins(displayPins, sizeof(displayPins) / sizeof(displayPins[0]), PinOwner::UM_ST7735Display)) {
        DEBUG_PRINTLN(F("ST7735: pin allocation failed, display disabled."));
        return;
      }

      // Backlight on as early as possible - the panel is unreadable without it and
      // nothing below depends on the pin being idle first.
      if (TFT_BL >= 0) {
        pinMode(TFT_BL, OUTPUT);
        digitalWrite(TFT_BL, HIGH);
      }

      // WLED and TFT_eSPI share the one global SPI object on ESP32-C3, and
      // SPIClass::begin() returns early once the bus is up *without* applying the
      // pins it was handed. WLED starts its SPI from the LED preferences before
      // usermod setup() runs, so if HW SPI pins were ever configured there,
      // TFT_eSPI's own begin() would silently do nothing and the panel would stay
      // dark. Release the bus so the display can claim it - SPI-attached LED types
      // cannot be used at the same time as this usermod.
      if (spi_sclk >= 0 || spi_mosi >= 0) {
        DEBUG_PRINTLN(F("ST7735: releasing WLED's SPI bus (SPI LEDs are not supported alongside this display)."));
        SPI.end();
      }

      tft.init();

      // Tune the panel to the values its vendor ships, before setRotation() writes
      // MADCTL - see PANEL_INIT above. Everything from here on assumes it landed.
      tft.commandList(PANEL_INIT);

      // Clip rather than wrap: a line one character too long would otherwise wrap
      // and scramble every row below it.
      tft.setTextWrap(false);
      appliedRotation = rotation;
      tft.setRotation(appliedRotation);

      enabled = true;
      initDone = true;
    }

    /*
     * loop() is called continuously.
     *
     * The panel is only redrawn when one of the displayed values actually changed,
     * so this stays cheap even though loop() runs hundreds of times per second.
     */
    void loop() override {
      if (!enabled) return;

      if (millis() - lastUpdate < USER_LOOP_REFRESH_RATE_MS) return;
      lastUpdate = millis();

      // Blank the backlight after 5 minutes with nothing changing. A running clock
      // redraws once a minute, which would keep resetting this timer, so the auto-off
      // only applies while there is no clock on screen - see readme.md.
      if (!ntpEnabled && !displayTurnedOff && millis() - lastRedraw > DISPLAY_IDLE_OFF_MS) {
        backlight(false);
        displayTurnedOff = true;
      }

      IPAddress currentIp = apActive ? WiFi.softAPIP() : WLEDNetwork.localIP();

      // Check if values which are shown on the display changed from the last time.
      if ((((apActive) ? String(apSSID) : WiFi.SSID()) != knownSsid) ||
          (knownIp != currentIp) ||
          (knownBrightness != bri) ||
          (knownEffectSpeed != strip.getMainSegment().speed) ||
          (knownEffectIntensity != strip.getMainSegment().intensity) ||
          (knownMode != strip.getMainSegment().mode) ||
          (knownPalette != strip.getMainSegment().palette) ||
          (ntpEnabled && ((knownMinute != minute(localTime)) || (knownHour != hour(localTime)))))
      {
        needRedraw = true;
      }

      if (!needRedraw) return;
      needRedraw = false;

      if (displayTurnedOff) {
        backlight(true);
        displayTurnedOff = false;
      }
      lastRedraw = millis();

      // Update last known values.
      #if defined(ESP8266)
        knownSsid = apActive ? WiFi.softAPSSID() : WiFi.SSID();
      #else
        knownSsid = apActive ? String(apSSID) : WiFi.SSID();
      #endif
      knownIp = currentIp;
      knownBrightness = bri;
      knownMode = strip.getMainSegment().mode;
      knownPalette = strip.getMainSegment().palette;
      knownEffectSpeed = strip.getMainSegment().speed;
      knownEffectIntensity = strip.getMainSegment().intensity;

      drawScreen();
    }

    /*
     * addToJsonInfo() can be used to add custom entries to the /json/info part of the JSON API.
     */
    void addToJsonInfo(JsonObject& root) override {
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");

      JsonArray displayArr = user.createNestedArray(F("ST7735")); //name
      displayArr.add(enabled ? F("installed") : F("disabled"));   //value
    }

    /*
     * addToConfig() adds custom persistent settings to the cfg.json file in the "um" (usermod) object.
     */
    void addToConfig(JsonObject& root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      // These pins are compiled into the TFT_eSPI library and are only listed here
      // so the settings page can show which GPIOs the display occupies.
      JsonArray pins = top.createNestedArray("pin");
      pins.add(TFT_CS);
      pins.add(TFT_DC);
      pins.add(TFT_RST);
      pins.add(TFT_BL);
      pins.add(TFT_SCLK);
      pins.add(TFT_MOSI);
      top["rotation"] = rotation;
    }

    void appendConfigData() override {
      // The pin numbers are compiled into TFT_eSPI, so they are shown for reference
      // only. They are still worth listing: WLED greys out every pin named here, which
      // stops one of them being handed to a relay or a LED bus in the UI before this
      // usermod has had a chance to claim it.
      oappend(F("addInfo('")); oappend(String(FPSTR(_name)).c_str()); oappend(F(":pin[]',0,'','(compile-time) SPI CS');"));
      oappend(F("addInfo('")); oappend(String(FPSTR(_name)).c_str()); oappend(F(":pin[]',1,'','(compile-time) SPI DC');"));
      oappend(F("addInfo('")); oappend(String(FPSTR(_name)).c_str()); oappend(F(":pin[]',2,'','(compile-time) SPI RST');"));
      oappend(F("addInfo('")); oappend(String(FPSTR(_name)).c_str()); oappend(F(":pin[]',3,'','(compile-time) SPI BL');"));
      oappend(F("addInfo('")); oappend(String(FPSTR(_name)).c_str()); oappend(F(":pin[]',4,'','(compile-time) SPI SCK');"));
      oappend(F("addInfo('")); oappend(String(FPSTR(_name)).c_str()); oappend(F(":pin[]',5,'','(compile-time) SPI MOSI');"));
      oappend(F("addInfo('")); oappend(String(FPSTR(_name)).c_str()); oappend(F(":rotation',1,'','the layout is landscape only');"));
      // Both landscape orientations, so the screen can be flipped for mounting.
      oappend(F("dd=addDropdown('")); oappend(String(FPSTR(_name)).c_str()); oappend(F("','rotation');"));
      oappend(SET_F("addOption(dd,'1',1);"));
      oappend(SET_F("addOption(dd,'3 (flipped)',3);"));
    }

    /*
     * readFromConfig() is called when settings are loaded (at boot, and again when
     * settings are saved). It is called BEFORE setup(), so `initDone` gates the
     * part that talks to the display.
     */
    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      if (!top.isNull()) {
        uint8_t newRotation = top["rotation"] | 0;
        // A cfg.json written by the portrait build holds 0 or 2; both land on 1 here.
        rotation = (newRotation == 3) ? 3 : 1;
        if (initDone && rotation != appliedRotation) {
          tft.setRotation(rotation);
          appliedRotation = rotation;
          lastRedraw = millis();
          needRedraw = true;
        }
      }
      return true;
    }

    uint16_t getId() override {
      return USERMOD_ID_ST7735_DISPLAY;
    }
};

const char St7735DisplayUsermod::_name[] PROGMEM = "ST7735";

static St7735DisplayUsermod st7735_display;
REGISTER_USERMOD(st7735_display);
