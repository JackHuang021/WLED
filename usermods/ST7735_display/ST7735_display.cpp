// ST7735 80x160 SPI TFT (0.96" module) connected to WLED.
//
// Shows the same information as usermods/ST7789_display, re-flowed for a much
// smaller panel. The layout is deliberately a first pass - it is portrait only.
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

#define DISPLAY_WIDTH   TFT_WIDTH
#define DISPLAY_HEIGHT  TFT_HEIGHT

// 6x8 GLCD font, so this is the number of characters that fit on one line at
// text size 1. The layout is portrait, 80 px wide -> 13 characters.
#define CHARS_PER_LINE  (DISPLAY_WIDTH / 6)

/*
 * Row positions for the 80x160 portrait panel. The clock keeps its two rows
 * reserved even when NTP is off, so the layout does not shift around.
 */
#define Y_DATE       0
#define Y_CLOCK     12
#define Y_SSID      32
#define Y_IP        44
#define Y_BRI       56
#define Y_SIG       68
#define Y_MODE      84
#define Y_PALETTE   96
#define Y_FX       108
#define Y_CURRENT  120

// Extra chars (+1) for the null terminator, sized for the longest line we build.
#define LINE_BUFFER_SIZE 24

// How often we check whether anything we display has changed.
#define USER_LOOP_REFRESH_RATE_MS 1000

// Turn the backlight off after this long without any change.
#define DISPLAY_IDLE_OFF_MS (5UL * 60UL * 1000UL)

TFT_eSPI tft = TFT_eSPI(TFT_WIDTH, TFT_HEIGHT); // Invoke custom library

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
    uint8_t rotation = 0;             // 0 or 2, both portrait
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

    void backlight(bool on) {
      if (TFT_BL >= 0) digitalWrite(TFT_BL, on ? HIGH : LOW);
    }

    // Print one body line, centred, without advancing a stored cursor.
    void printCentered(const String &text, uint8_t y, uint16_t color) {
      String line = text;
      if (line.length() > CHARS_PER_LINE) line = line.substring(0, CHARS_PER_LINE);
      center(line, CHARS_PER_LINE);
      tft.setTextColor(color);
      tft.setTextSize(1);
      tft.setCursor(0, y);
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

      // Date on top, with the AM/PM marker appended - the clock row is only 80 px
      // wide, so there is no room for a separate suffix next to it.
      sprintf_P(lineBuffer, PSTR("%s %2d"), monthShortStr(month(localTime)), day(localTime));
      if (useAMPM) strcat_P(lineBuffer, isAM ? PSTR(" AM") : PSTR(" PM"));
      printCentered(String(lineBuffer), Y_DATE, TFT_SILVER);

      // Clock, text size 2 -> 12x16 px per character.
      sprintf_P(lineBuffer, PSTR("%2d:%02d"), (useAMPM ? showHour : hourCurrent), minuteCurrent);
      tft.setTextSize(2);
      tft.setTextColor(TFT_WHITE);
      tft.setCursor((DISPLAY_WIDTH - strlen(lineBuffer) * 12) / 2, Y_CLOCK);
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

      // Network name
      tft.setTextColor(TFT_GREEN);
      tft.setCursor(0, Y_SSID);
      line = knownSsid.substring(0, CHARS_PER_LINE);
      // Print `~` char to indicate that SSID is longer, than our display
      if (knownSsid.length() > CHARS_PER_LINE) line = line.substring(0, CHARS_PER_LINE - 1) + '~';
      center(line, CHARS_PER_LINE);
      tft.setTextSize(1);
      tft.print(line.c_str());

      if (apActive) {
        // Print AP IP and password while the AP is up.
        tft.setCursor(0, Y_IP);
        tft.print("AP IP:");
        tft.print(knownIp);
        tft.setCursor(0, Y_BRI);
        line = "Pass:";
        line += apPass;
        if (line.length() > CHARS_PER_LINE) line = line.substring(0, CHARS_PER_LINE);
        tft.print(line.c_str());
      } else {
        printCentered(knownIp.toString(), Y_IP, TFT_GREEN);

        // brightness
        sprintf_P(lineBuffer, PSTR("Bri %3d%%"), (int)bri * 100 / 255);
        printCentered(String(lineBuffer), Y_BRI, TFT_WHITE);

        // Signal quality, colour coded. WiFi.RSSI() reads 0 while disconnected, which
        // would show up as a misleading 0%, so leave the row blank until we have a link.
        if (WLED_CONNECTED) {
          int quality = getSignalQuality(WiFi.RSSI());
          sprintf_P(lineBuffer, PSTR("Sig %3d%%"), quality);
          uint16_t sigColor = (quality < 10) ? TFT_RED : (quality < 25) ? TFT_ORANGE : TFT_GREEN;
          printCentered(String(lineBuffer), Y_SIG, sigColor);
        }
      }

      // Effect name
      char nameBuffer[CHARS_PER_LINE + 1];
      extractModeName(knownMode, JSON_mode_names, nameBuffer, CHARS_PER_LINE);
      printCentered(String(nameBuffer), Y_MODE, TFT_CYAN);

      // Palette name
      extractModeName(knownPalette, JSON_palette_names, nameBuffer, CHARS_PER_LINE);
      printCentered(String(nameBuffer), Y_PALETTE, TFT_YELLOW);

      // Effect speed and intensity
      sprintf_P(lineBuffer, PSTR("Spd%3d Int%3d"), knownEffectSpeed, knownEffectIntensity);
      printCentered(String(lineBuffer), Y_FX, TFT_SILVER);

      // Estimated milliamp usage (needs the LED type to be set in LED prefs to be
      // a reasonable estimate).
      sprintf_P(lineBuffer, PSTR("Cur %umA"), BusManager::currentMilliamps());
      printCentered(String(lineBuffer), Y_CURRENT, TFT_ORANGE);
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

      // TEMPORARY DIAGNOSTIC - remove once the display is confirmed working.
      // Pinpoints which call blocks if the board hangs during usermod setup.
      DEBUG_PRINTLN(F("ST7735: entering tft.init()"));
      tft.init();
      DEBUG_PRINTLN(F("ST7735: tft.init() returned"));

      // Clip rather than wrap: a line one character too long would otherwise wrap
      // and scramble every row below it.
      tft.setTextWrap(false);
      appliedRotation = rotation;
      tft.setRotation(appliedRotation);

      // TEMPORARY DIAGNOSTIC - remove once the display is confirmed working.
      // A lit-but-uniformly-black panel cannot tell "nothing reaches the panel" apart
      // from "everything is drawn outside the address window". Full-screen primaries
      // settle it: ANY colour at all proves init + SPI + geometry end to end, while
      // staying black points at the wiring or the panel itself.
      const uint16_t sweep[] = { TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE };
      const char *sweepNames[] = { "RED", "GREEN", "BLUE", "WHITE" };
      for (uint8_t i = 0; i < 4; i++) {
        DEBUG_PRINTF_P(PSTR("ST7735: fillScreen(%s)\n"), sweepNames[i]);
        tft.fillScreen(sweep[i]);
        delay(600);
      }

      // Then markers that only line up if colstart/rowstart match the module: a border
      // drawn clipped or off-centre means the address window is shifted.
      DEBUG_PRINTLN(F("ST7735: fillScreen(BLACK) + geometry markers"));
      tft.fillScreen(TFT_BLACK);
      tft.drawRect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, TFT_WHITE);
      tft.fillRect(0, 0, 6, 6, TFT_RED);
      tft.fillRect(DISPLAY_WIDTH - 6, 0, 6, 6, TFT_GREEN);
      tft.setTextDatum(TL_DATUM);
      tft.setTextColor(TFT_WHITE);
      tft.setTextSize(2);
      tft.setCursor(4, DISPLAY_HEIGHT / 2 - 8);
      tft.print("WLED");
      DEBUG_PRINTLN(F("ST7735: markers drawn"));

      #if defined(CONFIG_IDF_TARGET_ESP32C3)
        // SPI2_CLOCK_REG. With clk_equ_sysclk (bit 18) clear, the effective clock is
        // 80 MHz / (clkdiv_pre + 1) / (clkcnt_n + 1), so a divider that never got
        // programmed shows up here instead of as a mystery.
        DEBUG_PRINTF_P(PSTR("ST7735: SPI2 clock reg = %08x\n"),
          (unsigned)*(volatile uint32_t *)(0x60024000UL + 0x0C));
      #endif

      backlight(true);

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
      oappend(F("addInfo('")); oappend(String(FPSTR(_name)).c_str()); oappend(F(":rotation',1,'','the layout is portrait only');"));
      // Both portrait orientations, so the screen can be flipped for mounting.
      oappend(F("dd=addDropdown('")); oappend(String(FPSTR(_name)).c_str()); oappend(F("','rotation');"));
      oappend(SET_F("addOption(dd,'0',0);"));
      oappend(SET_F("addOption(dd,'2 (flipped)',2);"));
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
        rotation = (newRotation == 2) ? 2 : 0;
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
