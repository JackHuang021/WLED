// ST7735 80x160 SPI TFT (0.96" module) connected to WLED.
//
// Two one-line status bars - clock, date, signal and brightness on top; address
// and current draw below - frame a main area showing what the strip is doing.
// The single BOOT button drives the three functions worth having without a
// phone: on/off, brightness and preset. There is no menu - see docs/hmi.md for
// why, and readme.md for the gestures.
//
// TFT_eSPI is configured entirely from build flags (see readme.md), because the
// tab variant, geometry and pinout of these modules cannot be probed at runtime.
#include "wled.h"
#include <TFT_eSPI.h>
#include <SPI.h>
#include "ui_hmi.h"

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
 * The panel is mounted landscape, i.e. rotation 1 or 3, which is the orientation
 * the layout below is drawn for.
 */
#define PANEL_WIDTH   TFT_HEIGHT   // 160
#define PANEL_HEIGHT  TFT_WIDTH    // 80

// 6x8 GLCD font: 6 px per character, 8 px per row.
#define CHAR_W        6
#define ROW_H         8
#define CHARS_PER_LINE (PANEL_WIDTH / CHAR_W)   // 26
#define BIG_CHAR_W    12                        // text size 2
#define BIG_CHARS     (PANEL_WIDTH / BIG_CHAR_W) // 13

// An operation view's title row is split at the halfway point, 80 px = 13 chars:
// the label on the left, the value it belongs to on the right.
#define HALF_CHARS (CHARS_PER_LINE / 2)

// Upper bound for any string we build, including the null terminator.
#define LINE_BUFFER_SIZE 32

/*
 * Two one-line status bars frame the main area, which shows either the idle
 * layout or a transient "operation view" (see HmiOverlay). The top bar carries
 * what the button edits and what changes on its own; the bottom bar carries the
 * address and the current draw.
 *
 * Each bar is 11 px rather than the 8 the font needs, because the two icons on
 * the top bar - the signal bars and the sun - are both 11x11. Anything smaller
 * cannot hold a round core with rays clear of it, which is what makes the sun
 * read as a sun instead of a cross.
 */
#define STATUS_H  11
#define Y_TOP     0
#define Y_BAR1    2                              // text row inside the top bar
#define Y_SEP1    (Y_TOP + STATUS_H)             // 11
#define Y_MAIN    (Y_SEP1 + 1)                   // 12
#define Y_SEP2    (PANEL_HEIGHT - STATUS_H - 1)  // 68
#define Y_BOTTOM  (Y_SEP2 + 1)                   // 69
#define Y_BAR2    (Y_BOTTOM + 2)                 // 71

// Main area: the effect name and palette, centred in the 56 px available.
#define Y_MAIN_BIG   (Y_MAIN + 14)    // 26 - effect name, text size 2 -> 16 px tall
#define Y_MAIN_SUB   (Y_MAIN + 34)    // 46 - palette

/*
 * Operation views: a title row, then either a large value or a progress bar.
 * The value and the bar bands do not overlap, so an overlay that leaves either
 * one behind cannot be mistaken for the other.
 */
#define Y_OVERLAY_TITLE (Y_MAIN + 6)    // 18
#define Y_OVERLAY_BIG   (Y_MAIN + 14)   // 26, text size 2 -> 16 px tall (26..41)
#define Y_OVERLAY_BAR   (Y_MAIN + 30)   // 42 (42..49)

/*
 * The one big text that is centred on the panel rather than in the main area:
 * the strip's ON/OFF state, in the power overlay and on the idle screen when
 * the strip is off. It is the whole message in both places, so it is placed
 * against the panel - (80 - 16) / 2 - rather than against the area between the
 * two bars, which is 1 px off centre and crowds the title.
 */
#define Y_POWER_BIG ((PANEL_HEIGHT - 16) / 2)   // 32, text size 2 -> 16 px tall

/*
 * Top bar, left to right: clock, date, then the sun with its percentage, and
 * the signal bars last. The two right-hand items are laid out from the edge
 * inwards: the signal bars sit against it, the brightness pair to their left.
 *
 * Inside that pair the gap is 2 px, the tightest on the bar: the number is the
 * sun's caption, not a field of its own, and the 4 px before the signal bars
 * are what keeps the two readings from running together.
 *
 * The date is ten characters with the year in it, which is what the clock and
 * the icons leave room for: 37 + 60 = 97 px against the sun's icon at 107. It
 * is written in a fixed YYYY-MM-DD shape rather than a "9/20" that grows to
 * "12/31", so it is the same ten columns on every day of the year and the field
 * beside it never has to move. No weekday: three more characters would not fit,
 * and the day of the week is the one part of a date nobody needs on a light.
 */
#define X_TIME       0
#define TIME_CHARS   6                                // "14:32 " or "11:05A"
#define X_DATE       37
#define DATE_CHARS   10                               // "2026-09-20"
#define ICON_W       11                               // signal bars and sun alike
#define ICON_H       11
#define X_SIGNAL_ICON (PANEL_WIDTH - ICON_W)          // 149
#define X_BRI_PCT     (X_SIGNAL_ICON - 5 - 4 * CHAR_W) // 120
#define X_BRI_ICON    (X_BRI_PCT - 2 - ICON_W)        // 107

/*
 * Bottom bar. Normally: the address on the left, the current draw against the
 * right edge. Two fields, and they must not overlap - the one drawn second
 * paints its padding, and the font is opaque, so a field that reaches into
 * another's columns erases the tail of it. That is what a 15-character address
 * sitting under a right-aligned readout does: "192.168.1.42" comes out as
 * "192.168.1." with the last three characters painted over by the spaces in
 * front of the mA.
 *
 * With the address given every column an IPv4 address can need and the readout
 * given every one a 16-bit mA figure can, there are 16 px to spare between them
 * (90..106), and nothing else ever draws on this bar. The two static_asserts
 * below are that arithmetic, kept where a change to either field has to walk
 * past them.
 */
#define X_LINK        0
#define LINK_CHARS    15      // "255.255.255.255" is the longest address there is - 90 px
#define MA_CHARS       9      // "65535 mA" is the longest readout there is - 54 px

/*
 * The AP is the one screen this bar cannot fit on: its two values are the
 * address to browse to and the password to join with, and "Pass:" plus even a
 * short password is wider than the 54 px the readout gave up. So it is a second
 * layout in different columns, and the bar is wiped when the two swap - once
 * per AP session rather than once a second, and on a change the user made
 * deliberately, which is why it can afford the clear the other layout cannot.
 *
 * Both of its fields are cut to fit, an address of the lengths WLED hands out
 * ("4.3.2.1", "192.168.4.1") and the default "wled1234" password with room to
 * spare. A longer password is cut like any other over-long string; it is also
 * on the settings page, which is where someone who has to join this way ends up
 * anyway.
 */
#define AP_IP_CHARS   11      // "192.168.4.1" - 66 px
#define X_AP_PASS     (PANEL_WIDTH - AP_PASS_CHARS * CHAR_W)  // 70
#define AP_PASS_CHARS 15      // "Pass:" and ten characters of password - 90 px

static_assert(X_LINK + LINK_CHARS * CHAR_W <= PANEL_WIDTH - MA_CHARS * CHAR_W,
              "bottom bar: the readout is drawn over the address");
static_assert(X_LINK + AP_IP_CHARS * CHAR_W <= X_AP_PASS,
              "bottom bar: the AP password is drawn over the AP address");

// Brightness overlay: direction sign sits between the title and the percentage.
// The overlay keeps its value against the right edge; only the top bar gave that
// slot to the signal icon.
#define X_OVERLAY_PCT (PANEL_WIDTH - 4 * CHAR_W)      // 136
#define X_OVERLAY_DIR (X_OVERLAY_PCT - 24)            // 112

// How often we check whether anything we display has changed.
#define USER_LOOP_REFRESH_RATE_MS 1000

// Turn the backlight off after this long without any change.
#define DISPLAY_IDLE_OFF_MS (5UL * 60UL * 1000UL)

// Brightness step: fine at first, then the configured hmiStep once the user is
// clearly holding the button down.
#define HMI_STEP_FINE 4

/*
 * The screen's colour vocabulary. Named by meaning rather than by hue so that
 * the two regions cannot drift apart, and so a colour is only ever changed in
 * one place. The rules behind the choices - which colours are legible in a 1 px
 * stroke on a black background, and the one case where a live LED colour is used
 * instead - are in docs/hmi.md, "配色规范".
 */
static const uint16_t HMI_C_BG      = TFT_BLACK;
static const uint16_t HMI_C_LABEL   = TFT_SILVER;
static const uint16_t HMI_C_VALUE   = TFT_WHITE;
static const uint16_t HMI_C_ACCENT  = TFT_CYAN;
static const uint16_t HMI_C_OK      = TFT_GREEN;
static const uint16_t HMI_C_WARN    = TFT_ORANGE;
static const uint16_t HMI_C_DANGER  = TFT_RED;
static const uint16_t HMI_C_OFF     = 0x7BEF;   // mid grey, for the OFF state
static const uint16_t HMI_C_BAR_BG  = 0x2104;   // progress bar trough

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

/*
 * A preset is WLED's own unit of "a look worth going back to", and the double
 * press walks those rather than the 200-odd entries of the mode list: the web UI
 * is already where a preset gets built, named and reordered, so what the button
 * steps through is a set the user has already chosen, and applying one brings
 * its palette, colour and brightness along with the effect.
 *
 * Which ids exist is kept in a bitmap rather than asked of getPresetName(). That
 * call reads the file every time and cannot tell "no such preset" from "the JSON
 * buffer was busy" - it answers false to both - so it would have the button stop
 * on gaps left by deleted presets. 250 bits is 32 bytes, and the bitmap is
 * rebuilt from one read of presets.json when the file changes.
 */
#define HMI_PRESET_ID_MAX 250                      // 255 is WLED's temporary preset
#define HMI_PRESET_BYTES  ((HMI_PRESET_ID_MAX + 8) / 8)
// How many times a preset name is asked for before giving up on it. A failure
// means either a busy JSON buffer or a preset saved without a name, and the two
// are indistinguishable - so the retry is bounded, or an unnamed preset would
// read the file once a second forever.
#define HMI_PRESET_NAME_TRIES 3

// Asking for the preset's name can lose to the JSON buffer being busy, which is
// worth retrying a few times; a preset saved without a name is not. Bounded, or
// an unnamed preset would ask the filesystem forever. The retries ride the 1 s
// poll, so they are a second apart rather than back to back.
#define HMI_SAVE_NAME_TRIES 3

class St7735DisplayUsermod : public Usermod {
  private:
    static const char _name[];

    bool enabled = false;             // false if the pins could not be claimed
    bool initDone = false;
    uint8_t rotation = 3;             // 1 or 3, both landscape, 180 degrees apart
    uint8_t appliedRotation = 0;

    // Button/status-bar behaviour. Persisted in cfg.json.
    bool     hmiEnabled = true;       // false hands button 0 back to WLED
    uint16_t hmiDoubleMs = 350;       // 0 disables double press (and its delay)
    uint16_t hmiOverlayMs = 1500;     // how long an operation view stays up
    uint16_t hmiRepeatMs = HMI_REPEAT_FAST_MS; // brightness repeat interval while held
    uint8_t  hmiStep = 16;            // brightness step once past HMI_STEP_FINE
    bool     hmiSave = true;          // false = the button never writes to a preset

    HmiGesture gesture;
    HmiView    view;

    /*
     * Which presets exist, and which one the button last walked to.
     *
     * The cursor is the usermod's own rather than WLED's `currentPreset`, for two
     * reasons. handlePresets() does not apply a preset until after this usermod's
     * loop() has already drawn for the pass (wled.cpp:149 against :104), so
     * currentPreset is a pass behind the screen; and it reads 0 for every preset
     * this button applies. handlePresets() sets it (presets.cpp:201) and then calls
     * stateUpdated() on the next line but one (presets.cpp:208), which zeroes it
     * again whenever `stateChanged` is set (led.cpp:93) - and deserializeState() has
     * just set that, for a brightness that moved (json.cpp:380) and for the segments
     * (json.cpp:363) alike. WLED patches that up only on the `pd` path the web UI
     * uses (json.cpp:527 - "stateUpdated() will clear the preset, so we need to
     * restore it after"), and applyPreset() is the queued path, which has no such
     * restore. currentPreset is still read, but only as the hint that something
     * outside this button applied one.
     *
     * The write-back keys off the cursor for the same reason: there is nothing left
     * in currentPreset by the time the button is pressed. So the preset the screen
     * names is the preset a long press re-levels, and the screen is the place that
     * answers "which preset am I in". The cursor does follow the web UI - a preset
     * clicked there arrives as `pd`, which restores currentPreset (index.js
     * setPreset()) - and a playlist is skipped by the write-back outright.
     */
    uint8_t  presetBits[HMI_PRESET_BYTES] = {};
    uint16_t presetCount = 0;            // how many are set, i.e. the "N" on screen
    uint16_t presetOrdinal = 0;          // position of the cursor, 1-based
    uint8_t  presetCursor = 0;           // 0 = not in a preset
    char     presetLabel[BIG_CHARS + 1] = {};  // its name, clipped to what fits the row
    bool     presetLabelPending = false; // name not fetched yet
    uint8_t  presetNameTries = 0;
    bool     presetIndexValid = false;
    unsigned long presetIndexStamp = 0xFFFFFFFF;  // presetsModifiedTime it was built at
    uint8_t  presetCheckedId = 0;        // currentPreset already used to force a rebuild

    // The brightness write-back waiting to happen. stateSavePreset is captured when
    // the button arms it rather than read when it fires, so a preset applied from
    // elsewhere before the view retires cannot redirect the write into its slot.
    bool    stateSavePending = false;    // an adjustment waiting for the view to retire
    uint8_t stateSavePreset = 0;         // the preset the pending write belongs to
    uint8_t stateSaveTries = 0;          // bounded retries for a busy JSON buffer

    unsigned long lastUpdate = 0;
    unsigned long lastRedraw = 0;
    bool displayTurnedOff = false;

    // Redraw bookkeeping. Each region is repainted on its own, so a change to
    // the clock does not disturb the effect name three rows below it.
    // Set by the button handler to bypass the 1 s poll. Starts true so the first
    // pass paints immediately instead of waiting out a poll it has already missed.
    bool hmiUrgent = true;
    bool dirtyTop = true;
    bool dirtyBottom = true;
    bool dirtyMain = true;
    bool fullMainRedraw = true;    // repaint the whole main area, not just a field

    /*
     * What each text field last put on the panel, so an unchanged one is not
     * repainted. This is the anti-flicker half of how fields are drawn: the
     * panel has no framebuffer, so the only way to back out a wrong value is to
     * clear the field and write the right one, and a field measured in seconds
     * (the current draw) is cleared most of the times it is drawn even when the
     * number that changed is one digit.
     *
     * The rect is kept as well as the key so that a wipe can find the fields it
     * covered, and so that a field being recorded displaces any other one it
     * overlaps: two fields that share pixels cannot both be on the panel, and
     * the one that has just been written is the one that is. Without that,
     * whatever the wipe missed would be skipped as already drawn, which is how
     * a field goes missing rather than merely flickering. Every fillScreen()
     * has to call resetFieldCache().
     */
    struct FieldCache {
      uint32_t key;    // x, y and height; 0 means the slot is free
      int16_t  x, y;   // the rect the field covers - what a wipe is tested against
      uint8_t  w, h;
      uint16_t color;
      char     text[LINE_BUFFER_SIZE];
    };
    static const uint8_t FIELD_CACHE_ENTRIES = 12;
    FieldCache fieldCache[FIELD_CACHE_ENTRIES];

    // Last values painted, so we can tell what actually changed.
    IPAddress knownIp;
    bool     knownApActive = false;
    int16_t  knownQuality = -2;    // -1 = no link, -2 = never sampled
    uint8_t  knownBrightness = 255;
    // The last brightness that was not zero. Switching the strip off drops bri
    // to 0, and the sun is a picture of the level rather than of the power
    // state, so it keeps the level the strip would come back on at while the
    // percentage beside it reads 0%.
    uint8_t  lastNonZeroBri = 255;
    uint8_t  knownMinute = 99;
    uint8_t  knownHour = 99;
    uint8_t  knownMonth = 99;
    uint8_t  knownDay = 99;
    uint16_t knownYear = 0;
    // Which of the bottom bar's two layouts is on screen. Unknown until one of
    // them has been drawn, because until then there is nothing to wipe.
    bool     bottomLayoutKnown = false;
    bool     bottomIsAp = false;
    uint16_t knownMilliamps = 0xFFFF;  // BusManager::currentMilliamps() is 16-bit
    uint8_t  knownMode = 0;
    uint8_t  knownPalette = 0;

    // Idle main area, field by field: the effect name and the palette are
    // compared on their own so one changing does not repaint the other.
    uint8_t  mainMode = 255, mainPalette = 255;
    bool     mainOff = false;
    bool     mainDrawn = false;    // nothing has been painted yet
    int16_t  brightnessBarWidth = 0;   // width last painted in the brightness bar

    /*
     * Copy at most `max` characters, always terminating. The panel and the GLCD
     * font are both fixed width, so every string is clipped rather than wrapped:
     * a wrapped line would print straight over the row below it.
     */
    static void clip(const char *src, char *dst, uint8_t max) {
      uint8_t i = 0;
      while (i < max && src[i]) { dst[i] = src[i]; i++; }
      dst[i] = '\0';
    }

    /*
     * Copy `chars` characters of `text` into `dst` and pad with spaces to the
     * full field width. The GLCD font is 6x8 with an opaque background, so a
     * padded string is the whole field: a value that shrinks ("192.168.1.42" ->
     * "No link") overwrites its tail, and the padded string is what
     * fieldChanged() compares, so what is shown is what is remembered.
     */
    static void clipPadded(const char *text, char *dst, uint8_t chars) {
      uint8_t i = 0;
      while (i < chars && text[i]) { dst[i] = text[i]; i++; }
      while (i < chars) dst[i++] = ' ';
      dst[i] = '\0';
    }

    /*
     * The same, padded in front instead. A right-aligned field is one whose
     * *last* character is pinned to the edge, so a value that changes length -
     * "45 mA" today, "1200 mA" tomorrow - has to grow leftwards; padding behind
     * it would only pin the first character and leave the reading floating in
     * the middle of the screen.
     */
    static void clipPaddedLeft(const char *text, char *dst, uint8_t chars) {
      uint8_t len = 0;
      while (len < chars && text[len]) len++;
      uint8_t pad = chars - len;
      for (uint8_t i = 0; i < pad; i++) dst[i] = ' ';
      for (uint8_t i = 0; i < len; i++) dst[pad + i] = text[i];
      dst[chars] = '\0';
    }

    // Two rectangles overlap when each starts before the other ends, so a field
    // that ends at the pixel another starts on is in the clear.
    static bool overlaps(int16_t ax, int16_t ay, int16_t aw, int16_t ah,
                         int16_t bx, int16_t by, int16_t bw, int16_t bh) {
      return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
    }

    /*
     * True when the field at (x, y) no longer reads what the panel shows there,
     * and records `text` as its new content either way. The panel has no
     * framebuffer, so a field's own record of what it wrote is the only way to
     * tell an unchanged one, which needs no repaint, from a changed one.
     *
     * The rect is what the field paints, not a measurement of its text: the two
     * have to agree, because anything the rect misses is a field the cache would
     * hold on to after a wipe had taken it off the panel.
     */
    bool fieldChanged(int16_t x, int16_t y, int16_t w, int16_t h, const char *text, uint16_t color) {
      // x in the low byte, the height in the next - no field is 0 px tall - and
      // y above that. No field has a key of 0, which is the empty slot.
      uint32_t key = (uint32_t)(uint16_t)x | ((uint32_t)(uint16_t)h << 8) | ((uint32_t)(uint16_t)y << 16);

      for (uint8_t i = 0; i < FIELD_CACHE_ENTRIES; i++) {
        FieldCache &entry = fieldCache[i];
        if (entry.key != key) continue;
        if (entry.color == color && strcmp(entry.text, text) == 0) return false;
        entry.color = color;
        strcpy(entry.text, text);
        return true;
      }

      // Not on the panel yet, so there is nothing to compare against. Whatever
      // is in the way goes first: two fields that share pixels cannot both be on
      // the panel, and this is the one being written, so holding on to the other
      // would have it skip a repaint its pixels still need.
      for (uint8_t i = 0; i < FIELD_CACHE_ENTRIES; i++) {
        FieldCache &entry = fieldCache[i];
        if (entry.key == 0) continue;
        if (overlaps(x, y, w, h, entry.x, entry.y, entry.w, entry.h)) entry.key = 0;
      }

      // A slot is always free - there are more of them than there are fields any
      // one layout can put up - but if one ever is not, the draw still happens
      // and this field simply repaints every time, which is the old behaviour
      // rather than a hole.
      for (uint8_t i = 0; i < FIELD_CACHE_ENTRIES; i++) {
        FieldCache &entry = fieldCache[i];
        if (entry.key != 0) continue;
        entry.key = key;
        entry.x = x; entry.y = y; entry.w = w; entry.h = h;
        entry.color = color;
        strcpy(entry.text, text);
        break;
      }
      return true;
    }

    /*
     * One fixed-width field, painted only when it would change the panel. The
     * string handed in is already padded out to `chars` characters, so it covers
     * the field by itself and the alignment is the caller's - the two below are
     * the left- and right-aligned ways of building one.
     *
     * Two panels' worth of grid cells are what an unchanged field would cost to
     * repaint, and the fields that change on their own - the current draw every
     * second, the clock every minute - cannot be made to change any less often.
     * So an unchanged field is skipped outright, and a changed one is written
     * over in place: the padded string is the whole field, so there is no
     * clear-then-draw and therefore no flash between the two.
     */
    void paintField(const char *field, int16_t x, int16_t y, uint8_t chars, uint16_t color) {
      if (!fieldChanged(x, y, chars * CHAR_W, ROW_H, field, color)) return;

      tft.setTextColor(color, HMI_C_BG);
      tft.setTextSize(1);
      tft.setCursor(x, y);
      tft.print(field);
    }

    /*
     * Text at the left edge of a field. Takes a plain C string rather than the
     * F() macro other calls in this file use - a flash string would have to be
     * copied into a RAM String to get here, and the brightness readout redraws
     * these several times a second.
     */
    void drawField(const char *text, int16_t x, int16_t y, uint8_t chars, uint16_t color) {
      char field[LINE_BUFFER_SIZE];
      clipPadded(text, field, chars);
      paintField(field, x, y, chars, color);
    }

    /*
     * The panel is written through directly, so anything that wipes it - a
     * rotation change, a settings save that redraws the chrome - has to say so,
     * or the fields painted after it would be skipped as already on screen.
     */
    void resetFieldCache() {
      for (uint8_t i = 0; i < FIELD_CACHE_ENTRIES; i++) fieldCache[i].key = 0;
      brightnessBarWidth = 0;   // the bar went with the panel
      // The bottom bar is blank now, so whichever layout it held is gone and the
      // next one can be drawn without wiping first.
      bottomLayoutKnown = false;
    }

    /*
     * Wipe a band of the panel and forget every field that was in it. The two
     * belong together: a field the cache still believes is on screen would be
     * skipped by the redraw that the wipe was for, and the band would keep a
     * hole where that field used to be. The status bars do not need this - the
     * fields on them cover the bar between them - but the main area has three
     * different layouts to swap between and no way to cover all of them at once.
     */
    void clearBand(int16_t x, int16_t y, int16_t w, int16_t h) {
      tft.fillRect(x, y, w, h, HMI_C_BG);
      for (uint8_t i = 0; i < FIELD_CACHE_ENTRIES; i++) {
        FieldCache &entry = fieldCache[i];
        if (entry.key == 0) continue;
        if (overlaps(x, y, w, h, entry.x, entry.y, entry.w, entry.h)) entry.key = 0;
      }
      // The brightness bar is not a field - it is a run of pixels whose end
      // moves - but it went with the band like everything else on it.
      if (y < Y_OVERLAY_BAR + ROW_H && Y_OVERLAY_BAR < y + h) brightnessBarWidth = 0;
    }

    /*
     * Text ending at the panel's right edge. The field is the rightmost
     * `maxChars` of the bar and the padding goes in front of the text, so the
     * reading grows leftwards from the corner and the corner itself never moves
     * - which is what makes a value that changes width, the current draw, sit
     * still on a bar that redraws every second.
     */
    void drawFieldRight(const char *text, int16_t y, uint8_t maxChars, uint16_t color) {
      char field[LINE_BUFFER_SIZE];
      clipPaddedLeft(text, field, maxChars);
      paintField(field, PANEL_WIDTH - maxChars * CHAR_W, y, maxChars, color);
    }

    /*
     * Centred in the panel, and one line is the whole field - the panel's full
     * width rather than the 156 px the 26 characters of the font grid cover,
     * because a name that long starts two pixels in and so ends past where a
     * shorter one does.
     *
     * Where the text starts depends on how long it is, so unlike the fields
     * above this one cannot be padded out to a fixed width: a string that
     * shrinks vacates cells on its left that only a wipe takes back, and the
     * wipe has to come before the text rather than be replaced by it. It is
     * still skipped outright while it reads the same, which is what keeps the
     * ones that change on their own from flashing their whole line.
     */
    void drawCentered(const char *text, int16_t y, uint16_t color) {
      char field[LINE_BUFFER_SIZE];
      clip(text, field, CHARS_PER_LINE);
      if (!fieldChanged(0, y, PANEL_WIDTH, ROW_H, field, color)) return;

      tft.fillRect(0, y, PANEL_WIDTH, ROW_H, HMI_C_BG);
      tft.setTextColor(color, HMI_C_BG);
      tft.setTextSize(1);
      tft.setCursor((PANEL_WIDTH - (int16_t)strlen(field) * CHAR_W) / 2, y);
      tft.print(field);
    }

    void drawBigCentered(const char *text, int16_t y, uint16_t color) {
      char field[LINE_BUFFER_SIZE];
      clip(text, field, BIG_CHARS);
      if (!fieldChanged(0, y, PANEL_WIDTH, 16, field, color)) return;

      tft.fillRect(0, y, PANEL_WIDTH, 16, HMI_C_BG);
      tft.setTextColor(color, HMI_C_BG);
      tft.setTextSize(2);
      tft.setCursor((PANEL_WIDTH - (int16_t)strlen(field) * BIG_CHAR_W) / 2, y);
      tft.print(field);
    }

    /*
     * Colour for a progress bar: the colour the strip is actually outputting,
     * which is the most informative thing the bar can show. A dark colour would
     * vanish into the trough, so those fall back to white.
     */
    uint16_t levelColor() {
      uint32_t c = strip.getMainSegment().colors[0];
      if ((int)R(c) + (int)G(c) + (int)B(c) < 96) return HMI_C_VALUE;
      return tft.color565(R(c), G(c), B(c));
    }

    void backlight(bool on) {
      if (TFT_BL >= 0) digitalWrite(TFT_BL, on ? HIGH : LOW);
    }

    /*
     * Brightness as a sun rather than a bar.
     *
     * The core is a real disc, radius 2, and stays that size: a one-pixel core
     * comes out of fillCircle as a plus sign, and growing it to radius 3 would
     * close the gap the rays need. The level is carried by the rays instead, and
     * the percentage beside the icon carries the exact value.
     *
     * All eight rays always appear together, and the level only lengthens them.
     * Drawing the four cardinals on their own - which is what the earlier
     * version did between a quarter and two thirds brightness - puts a disc on
     * two axes and nothing between them, and that reads as a cross, not a sun.
     * The diagonals are what make the shape radiate, so they can never be the
     * tier that comes last.
     *
     * Drawn from primitives rather than stored as a bitmap because it takes the
     * live LED colour (levelColor), which a fixed bitmap could not.
     */
    void drawBrightnessIcon(int16_t x, int16_t y, uint8_t level) {
      tft.fillRect(x, y, ICON_W, ICON_H, HMI_C_BG);
      int16_t cx = x + ICON_W / 2, cy = y + ICON_H / 2;   // centre of an 11x11 box

      if (level == 0) {
        // Off is a hollow ring: still a brightness icon, but nothing shining.
        tft.drawCircle(cx, cy, 4, HMI_C_OFF);
        return;
      }

      uint16_t color = levelColor();
      tft.fillCircle(cx, cy, 2, color);

      // Every ray starts one pixel clear of the core at radius 4 and runs
      // outward. A dim ray is that single pixel - for the diagonals the line
      // then has two equal endpoints, which drawLine renders as one pixel - and
      // past a quarter brightness each gains a second, which takes the sun out
      // to the edges of its box.
      int16_t len   = (level > 64) ? 2 : 1;
      int16_t outer = len - 1;
      tft.drawFastHLine(cx - 4 - outer, cy, len, color);
      tft.drawFastHLine(cx + 4, cy, len, color);
      tft.drawFastVLine(cx, cy - 4 - outer, len, color);
      tft.drawFastVLine(cx, cy + 4, len, color);
      tft.drawLine(cx - 3 - outer, cy - 3 - outer, cx - 3, cy - 3, color);
      tft.drawLine(cx + 3 + outer, cy - 3 - outer, cx + 3, cy - 3, color);
      tft.drawLine(cx - 3 - outer, cy + 3 + outer, cx - 3, cy + 3, color);
      tft.drawLine(cx + 3 + outer, cy + 3 + outer, cx + 3, cy + 3, color);
    }

    /*
     * Signal strength as four bars - the one icon nobody needs a label for.
     * Without a link there is nothing to measure, but the bars are still drawn
     * in the trough colour so the icon stays recognisable instead of vanishing.
     */
    void drawSignalIcon(int16_t x, int16_t y, int16_t quality) {
      tft.fillRect(x, y, ICON_W, ICON_H, HMI_C_BG);

      uint16_t color = (quality < 10) ? HMI_C_DANGER : (quality < 25) ? HMI_C_WARN : HMI_C_OK;
      for (uint8_t i = 0; i < 4; i++) {
        // 2 px wide with a 1 px gap, 4/6/8/10 px tall, all sitting on the box's
        // bottom row.
        uint8_t h = 4 + 2 * i;
        bool lit = (quality > (int16_t)i * 25);
        tft.fillRect(x + i * 3, y + ICON_H - h, 2, h, lit ? color : HMI_C_BAR_BG);
      }
    }

    // The two rules that frame the main area. Part of the chrome, so they are
    // drawn once per screen clear rather than by the region that would redraw
    // over them anyway.
    void drawChrome() {
      tft.drawFastHLine(0, Y_SEP1, PANEL_WIDTH, HMI_C_BAR_BG);
      tft.drawFastHLine(0, Y_SEP2, PANEL_WIDTH, HMI_C_BAR_BG);
    }

    // ---- status bars ----------------------------------------------------------------

    /*
     * Neither bar is cleared before it is drawn. Every field on one is padded
     * out to the width of the field and the two icons paint their own boxes, so
     * between them they cover the bar; the few columns they leave unpainted are
     * ones nothing else ever draws in. Clearing the bar first would put back the
     * flash between the clear and the redraw, and the bottom bar is the one that
     * cannot afford it - it redraws every second the current draw moves.
     */
    void drawStatusTop() {
      char buf[LINE_BUFFER_SIZE];

      // Clock. Always six characters wide so the date alongside it never moves.
      if (ntpEnabled) {
        int h = hour(localTime);
        if (useAMPM) {
          bool am = h < 12;
          h %= 12;
          if (h == 0) h = 12;
          sprintf_P(buf, PSTR("%2d:%02d%c"), h, minute(localTime), am ? 'A' : 'P');
        } else {
          sprintf_P(buf, PSTR("%02d:%02d "), h, minute(localTime));
        }
      } else {
        strcpy_P(buf, PSTR("--:-- "));
      }
      drawField(buf, X_TIME, Y_BAR1, TIME_CHARS, HMI_C_VALUE);

      // Date. Zero-padded so it is the same ten columns on every day of the
      // year, and dropped when NTP is off, where it would be the epoch.
      if (ntpEnabled) {
        sprintf_P(buf, PSTR("%04d-%02d-%02d"), year(localTime), month(localTime), day(localTime));
      } else {
        buf[0] = '\0';
      }
      drawField(buf, X_DATE, Y_BAR1, DATE_CHARS, HMI_C_LABEL);

      // Brightness: the icon for a glance, the number for the exact value. The
      // number is left-aligned in its field rather than against the signal icon
      // the way the overlay's copy of it is, so the digits keep their distance
      // from the sun as they go from "8%" to "100%": it is the sun they caption,
      // not the edge of the bar.
      //
      // The sun is left at the level the strip would come back on at while the
      // strip is off, so switching off empties the readout instead of flattening
      // the icon: "0%" beside a ring is the one combination that reads as a
      // fault rather than as off.
      if (knownBrightness) lastNonZeroBri = knownBrightness;
      drawBrightnessIcon(X_BRI_ICON, Y_TOP, lastNonZeroBri);
      sprintf_P(buf, PSTR("%d%%"), (int)knownBrightness * 100 / 255);
      drawField(buf, X_BRI_PCT, Y_BAR1, 4, HMI_C_VALUE);

      drawSignalIcon(X_SIGNAL_ICON, Y_TOP, knownQuality);
    }

    void drawStatusBottom() {
      char buf[LINE_BUFFER_SIZE];

      // A switch between the bar's two layouts is the one thing here that needs
      // the whole bar rather than the fields: they sit in different columns, so
      // neither layout can paint over the other. It happens when the AP comes
      // up or goes away, which is rare and deliberate, and the same session that
      // shows the AP screen is the one where a wipe on it is expected.
      if (bottomLayoutKnown && bottomIsAp != apActive) {
        clearBand(0, Y_BOTTOM, PANEL_WIDTH, STATUS_H);
      }
      bottomLayoutKnown = true;
      bottomIsAp = apActive;

      if (apActive) {
        // Joining the AP needs the address and the password, and while it is up
        // nothing else on this bar comes close - the current draw moves aside
        // for them.
        drawField(WiFi.softAPIP().toString().c_str(), X_LINK, Y_BAR2, AP_IP_CHARS, HMI_C_WARN);
        char pass[LINE_BUFFER_SIZE];
        strcpy(pass, "Pass:");
        strncat(pass, apPass, AP_PASS_CHARS);   // clipPaddedLeft() cuts it to the field
        drawFieldRight(pass, Y_BAR2, AP_PASS_CHARS, HMI_C_OK);
        return;
      }

      // WiFi.RSSI() reads 0 while disconnected, so the icon on the top bar
      // would show no bars either way; the address still has to say so.
      if (knownQuality >= 0) {
        drawField(WLEDNetwork.localIP().toString().c_str(), X_LINK, Y_BAR2, LINK_CHARS, HMI_C_OK);
      } else {
        drawField("No link", X_LINK, Y_BAR2, LINK_CHARS, HMI_C_WARN);
      }

      // Right-aligned in its field, so the corner the readout ends at is the
      // same one every second while the number in front of it grows.
      sprintf_P(buf, PSTR("%u mA"), (unsigned)knownMilliamps);
      drawFieldRight(buf, Y_BAR2, MA_CHARS, HMI_C_WARN);
    }

    // ---- main area ------------------------------------------------------------------

    void drawMainIdle() {
      char buf[LINE_BUFFER_SIZE];
      bool nowOff = (bri == 0);

      // Turning off (or on) changes what the whole area is about, so it repaints
      // everything; so does the very first paint after boot.
      if (fullMainRedraw || !mainDrawn || nowOff != mainOff) {
        clearBand(0, Y_MAIN, PANEL_WIDTH, Y_SEP2 - Y_MAIN);
        mainDrawn = true;
        mainMode = 255; mainPalette = 255;
      }
      mainOff = nowOff;

      if (nowOff) {
        drawBigCentered("OFF", Y_POWER_BIG, HMI_C_OFF);
        return;
      }

      if (mainMode != knownMode) {
        // extractModeName() leaves `name` untouched when the mode is out of
        // range, so it has to start out as a valid (empty) string.
        char name[BIG_CHARS + 1] = "";
        extractModeName(knownMode, JSON_mode_names, name, BIG_CHARS);
        drawBigCentered(name, Y_MAIN_BIG, HMI_C_ACCENT);
        mainMode = knownMode;
      }

      if (mainPalette != knownPalette) {
        buf[0] = '\0';   // same reason
        extractModeName(knownPalette, JSON_palette_names, buf, CHARS_PER_LINE);
        drawCentered(buf, Y_MAIN_SUB, TFT_YELLOW);
        mainPalette = knownPalette;
      }
    }

    // Value, direction and bar of the brightness overlay. Split out because this
    // is the one overlay that is redrawn on every repeat, and repainting the
    // whole main area 7 times a second would be visible.
    void drawBrightnessOverlay() {
      char buf[LINE_BUFFER_SIZE];
      bool up = gesture.direction() > 0;

      // The sign has to appear the moment the long press starts: the direction
      // flips on every press, so this is the only warning the user gets.
      drawField(up ? "+" : "-", X_OVERLAY_DIR, Y_OVERLAY_TITLE, 1, up ? HMI_C_OK : HMI_C_WARN);

      sprintf_P(buf, PSTR("%3d%%"), (int)bri * 100 / 255);
      drawField(buf, X_OVERLAY_PCT, Y_OVERLAY_TITLE, 4, HMI_C_VALUE);

      // Only the 1 px or so between the old end and the new one is repainted.
      // Refilling the whole trough on every repeat is what makes the bar the
      // brightest thing on a flickering screen: a held button repaints it about
      // seven times a second, and each of those wipes the full width back to
      // the trough colour before drawing over it again.
      int16_t width = (PANEL_WIDTH * (int)bri) / 255;
      if (width < brightnessBarWidth) {
        tft.fillRect(width, Y_OVERLAY_BAR, brightnessBarWidth - width, ROW_H, HMI_C_BAR_BG);
      } else if (width > brightnessBarWidth) {
        tft.fillRect(brightnessBarWidth, Y_OVERLAY_BAR, width - brightnessBarWidth, ROW_H, levelColor());
      }
      brightnessBarWidth = width;
    }

    void drawMainOverlay() {
      char buf[LINE_BUFFER_SIZE];

      // A repeat of an overlay already on screen only updates its value.
      if (!fullMainRedraw && view.overlay() == HmiOverlay::BRIGHTNESS) {
        drawBrightnessOverlay();
        return;
      }

      // An overlay that is not the one already up replaces its whole area, so the
      // band goes rather than the handful of fields this view happens to use: the
      // idle layout's, or another overlay's, are behind it and none of them can
      // be reached from here to be cleared one at a time.
      clearBand(0, Y_MAIN, PANEL_WIDTH, Y_SEP2 - Y_MAIN);

      switch (view.overlay()) {
        case HmiOverlay::POWER:
          drawField("Power", 0, Y_OVERLAY_TITLE, HALF_CHARS, HMI_C_LABEL);
          drawBigCentered(bri ? "ON" : "OFF", Y_POWER_BIG, bri ? HMI_C_OK : HMI_C_OFF);
          break;

        case HmiOverlay::MODE: {
          char name[BIG_CHARS + 1] = "";   // extractModeName may not write to it
          // In a preset the counter is a position in the set of presets, so the
          // title says which set the number belongs to. Both titles are drawn into
          // the same 13-character field, so one cannot leave letters behind the
          // other.
          bool inPreset = (presetCursor != 0 && presetCount != 0);
          drawField(inPreset ? "Preset" : "Effect", 0, Y_OVERLAY_TITLE, HALF_CHARS, HMI_C_LABEL);
          if (inPreset) sprintf_P(buf, PSTR("%u/%u"), (unsigned)presetOrdinal, (unsigned)presetCount);
          else          sprintf_P(buf, PSTR("%u/%u"), (unsigned)knownMode + 1, (unsigned)strip.getModeCount());
          drawFieldRight(buf, Y_OVERLAY_TITLE, HALF_CHARS, HMI_C_LABEL);
          // A preset with a name shows it; one saved without a name, or one whose
          // name has not been read yet, shows the effect it put the strip in.
          if (inPreset && presetLabel[0]) clip(presetLabel, name, BIG_CHARS);
          else                            extractModeName(knownMode, JSON_mode_names, name, BIG_CHARS);
          drawBigCentered(name, Y_OVERLAY_BIG, HMI_C_ACCENT);
          break;
        }

        case HmiOverlay::BRIGHTNESS:
          drawField("Brightness", 0, Y_OVERLAY_TITLE, HALF_CHARS, HMI_C_LABEL);
          // The trough belongs to this view, not to the bar: on a repeat
          // drawBrightnessOverlay() paints only the pixels between where the bar
          // ended and where it ends now, which need something under them to be
          // the difference from. clearBand() above has already said the bar went
          // with the band, so the first of those repeats fills the trough in.
          tft.fillRect(0, Y_OVERLAY_BAR, PANEL_WIDTH, ROW_H, HMI_C_BAR_BG);
          drawBrightnessOverlay();
          break;

        default:
          break;
      }
    }

    // Enter an operation view. `fullMainRedraw` is only clear when the same
    // overlay is already up and just changed value.
    void showOverlay(HmiOverlay overlay, unsigned long now) {
      fullMainRedraw = (view.overlay() != overlay);
      view.show(overlay, now);
      dirtyMain = true;
      hmiUrgent = true;
    }

    /*
     * Rebuild the preset bitmap from /presets.json.
     *
     * Read straight off the filesystem rather than through WLED's JSON document:
     * that buffer is a single shared instance, and holding it for the length of a
     * parse would stall the web server, the playlist engine and handlePresets()
     * alike. All that is wanted here is the set of keys of the outermost object,
     * and those are the preset ids - everything inside a preset sits at a greater
     * depth, so a brace counter tells a key from a string that merely looks like
     * one.
     */
    void rebuildPresetIndex() {
      memset(presetBits, 0, sizeof(presetBits));
      presetCount = 0;
      presetIndexStamp = presetsModifiedTime;

      // Reading the filesystem while the strip is being written out is what
      // shows up as a glitch in the LEDs, so WLED waits the frame out before
      // every FS read it does (presets.cpp:171).
      unsigned long maxWait = millis() + strip.getFrameTime();
      while (strip.isUpdating() && millis() < maxWait) delay(1);

      File f = WLED_FS.open(FPSTR(getPresetsFileName()), "r");
      if (!f) {                    // no file at all means no presets, not a failure
        presetIndexValid = true;
        return;
      }

      uint8_t  depth = 0;          // 1 = the outermost object, where the ids live
      bool     inString = false;
      bool     escaped = false;
      bool     collecting = false; // inside a string that started at depth 1
      bool     numeric = true;     // ... and it has been digits throughout
      uint16_t value = 0;
      uint16_t pending = 0;        // a numeric key whose ':' has not been read yet

      char buf[64];
      while (f.available()) {
        size_t got = f.read((uint8_t *)buf, sizeof(buf));
        if (!got) break;
        for (size_t i = 0; i < got; i++) {
          char c = buf[i];
          if (inString) {
            if (escaped)                     { escaped = false; }
            else if (c == '\\')              { escaped = true; }
            else if (c == '"')               { inString = false; pending = (collecting && numeric) ? value : 0; collecting = false; }
            else if (collecting && numeric)  {
              if (c >= '0' && c <= '9') { value = value * 10 + (c - '0'); if (value > HMI_PRESET_ID_MAX) numeric = false; }
              else numeric = false;
            }
            continue;
          }
          if (c == '"') { inString = true; collecting = (depth == 1); numeric = true; value = 0; continue; }
          if (pending) {
            // A key only is a key if a ':' follows it, whitespace aside. Anything
            // else means that string was a value - an entry of the id array
            // inside a playlist, say - and it is dropped.
            if (c == ':') { markPreset(pending); pending = 0; continue; }
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
            pending = 0;
          }
          if (c == '{' || c == '[')       depth++;
          else if (c == '}' || c == ']')  { if (depth) depth--; }
        }
      }
      f.close();
      presetIndexValid = true;
      DEBUG_PRINTF_P(PSTR("ST7735 HMI: %u presets indexed.\n"), (unsigned)presetCount);
    }

    void markPreset(uint16_t id) {
      if (id == 0 || id > HMI_PRESET_ID_MAX) return;
      uint8_t &slot = presetBits[id >> 3];
      uint8_t  bit  = 1 << (id & 7);
      if (!(slot & bit)) { slot |= bit; presetCount++; }
    }

    // Position of `id` within the set, 1-based. 0 when it is not in the set.
    uint16_t presetOrdinalOf(uint8_t id) const {
      uint16_t n = 0;
      for (uint16_t i = 1; i <= id; i++) if (presetBits[i >> 3] & (1 << (i & 7))) n++;
      return n;
    }

    /*
     * The next preset after `from`, wrapping past the end and over the gaps left
     * by deleted presets, with the cursor's position moved onto it. 0 when there
     * is nothing to go to, which is the signal to fall back to the mode list.
     */
    uint8_t stepPreset(uint8_t from) {
      if (!presetCount) return 0;
      for (uint16_t step = 1; step <= HMI_PRESET_ID_MAX; step++) {
        uint8_t id = (uint8_t)(((from + step - 1) % HMI_PRESET_ID_MAX) + 1);
        if (presetBits[id >> 3] & (1 << (id & 7))) {
          presetOrdinal = presetOrdinalOf(id);
          return id;
        }
      }
      return 0;   // unreachable while presetCount is non-zero
    }

    // AI: below section was generated by an AI
    /*
     * Arm the write-back. Only the brightness gesture calls this, and only from
     * inside its own `next != bri` guard - a step a clamp at 1 or 255 swallowed
     * changed nothing, so it leaves no write behind either.
     *
     * Every repeat of a held button arms it again, and so does every press after it,
     * so a run of adjustments accumulates into one pending write rather than one per
     * step.
     */
    void armStateSave() {
      // The write-back replaces the preset's whole state, which is only ever right
      // for a preset the strip is actually in. The cursor is 0 when there is no such
      // preset - at boot, or after the preset it named was deleted - and the button
      // then has nothing to write to.
      if (!hmiSave || !presetCursor) return;
      // A playlist's slot holds a list of presets rather than a state, so overwriting
      // it would replace the playlist with whatever the strip is doing right now.
      if (currentPlaylist >= 0) return;
      if (!stateSavePending) stateSaveTries = 0;   // a fresh adjustment retries afresh
      stateSavePreset  = presetCursor;
      stateSavePending = true;
    }

    /*
     * Write the brightness the button just set back into the preset it was set in, so
     * that walking back to that preset brings the new level along - and so does
     * booting into it, on a device whose boot preset is that one. Nothing here gives
     * the level a persistence of its own: the preset is still the only thing on
     * flash, and a preset that is never applied again carries the level nowhere.
     *
     * savePreset() is a queue call, not a write: it records the request in WLED's
     * single global save slot and returns, and handlePresets() -> doSaveState() does
     * the serialising and the LittleFS write on a later pass, with the strip
     * suspended. Nothing here touches the filesystem.
     */
    void fireStateSave() {
      // Something moved the strip out of that preset while the view was up - a double
      // press, or a preset clicked in the web UI, which the cursor follows. There is
      // nothing to write back to then, and the state now on the strip belongs to a
      // different preset; writing it here would overwrite the wrong one.
      if (presetCursor != stateSavePreset) { stateSavePending = false; return; }

      // Never let a preset learn about "off". A short press leaves bri at 0, and a
      // nightlight fade can reach it on its own; either way serializeState() would
      // write `on: false` and the preset would come back dark on a press that was
      // only ever meant to set a level.
      if (bri == 0) { stateSavePending = false; return; }

      // presetToSave, saveName and includeBri are single globals, so a save the user
      // queued from the web UI and that has not drained yet is still sitting in them.
      // Ours would overwrite theirs whole, and silently lose it. Let it through.
      if (presetNeedsSaving()) {
        DEBUG_PRINTLN(F("ST7735: brightness not saved, another preset save is queued."));
        stateSavePending = false;
        return;
      }

      // savePreset() with no name renames the preset to "Preset <id>", so the name it
      // already has has to come back off the file first. getPresetName() answers false
      // both for a preset with no name and for a busy JSON buffer, and the two cannot
      // be told apart - so the retry is bounded and giving up means not saving, never
      // renaming. The attempt rides the 1 s poll, so the tries are a second apart.
      String name;
      if (!getPresetName(stateSavePreset, name)) {
        if (++stateSaveTries < HMI_SAVE_NAME_TRIES) return;   // stays pending
        DEBUG_PRINTF_P(PSTR("ST7735: preset %u has no readable name, brightness not saved.\n"), stateSavePreset);
        stateSavePending = false;
        return;
      }

      DEBUG_PRINTF_P(PSTR("ST7735: saving brightness to preset %u.\n"), stateSavePreset);
      savePreset(stateSavePreset, name.c_str());
      stateSavePending = false;
    }
    // AI: end

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

      tft.fillScreen(HMI_C_BG);
      resetFieldCache();
      drawChrome();

      // The button state machine only understands press/release edges, so it is only
      // offered a momentary button. Anything else keeps WLED's own handling.
      //
      // No button at all is disabled too, rather than being left as a no-op: the
      // write-back asks isButtonPressed() from loop(), and that indexes the button
      // vector without a bounds check (button.cpp:99) because WLED only ever calls it
      // from the button handler. With no button to drive the HMI there is nothing to
      // lose by saying so.
      if (hmiEnabled && (buttons.empty() ||
          (buttons[0].type != BTN_TYPE_PUSH && buttons[0].type != BTN_TYPE_PUSH_ACT_HIGH))) {
        if (buttons.empty()) DEBUG_PRINTLN(F("ST7735: no button configured - HMI disabled."));
        else DEBUG_PRINTF_P(PSTR("ST7735: button 0 is type %u, not a push button - HMI disabled.\n"), buttons[0].type);
        hmiEnabled = false;
      }
      gesture.setDoublePressMs(hmiDoubleMs);
      gesture.setRepeatMs(hmiRepeatMs);
      view.setDwellMs(hmiOverlayMs);

      enabled = true;
      initDone = true;
    }

    /*
     * loop() is called continuously. Nothing here blocks for long: the panel is
     * repainted region by region, and only when something in that region changed.
     */
    void loop() override {
      if (!enabled) return;
      unsigned long now = millis();

      // The 1 s poll is a floor, not a gate. A button press and an overlay timing
      // out both have to show up without waiting for it, or the screen feels dead
      // for up to a second after every press.
      bool overlayExpired = view.expired(now);
      if (!hmiUrgent && !overlayExpired && (now - lastUpdate < USER_LOOP_REFRESH_RATE_MS)) return;
      lastUpdate = now;
      hmiUrgent = false;

      /*
       * Presets. Both halves of this are filesystem work, so they ride the poll
       * above rather than running on every pass; the button only ever moves the
       * cursor and leaves the reading to here.
       */
      if (!presetIndexValid || presetsModifiedTime != presetIndexStamp ||
          (currentPreset && currentPreset != presetCheckedId &&
           !(presetBits[currentPreset >> 3] & (1 << (currentPreset & 7))))) {
        // The last clause is the fallback for a device with no clock: without NTP
        // presetsModifiedTime can stay 0 right through a save, so a preset id the
        // bitmap does not know is reason enough to look again. It is remembered
        // per id, because the condition is not one a rebuild clears - an id this
        // scan cannot see would otherwise re-read the file once a second forever.
        presetCheckedId = currentPreset;
        rebuildPresetIndex();
        // The set can have changed under the cursor - a preset deleted ahead of it
        // moves its position, and one deleted out from under it leaves nothing to
        // be in - so the counter is recomputed rather than read as it stands.
        if (presetCursor && !(presetBits[presetCursor >> 3] & (1 << (presetCursor & 7)))) presetCursor = 0;
        presetOrdinal = presetCursor ? presetOrdinalOf(presetCursor) : 0;
        dirtyMain = true;
      }

      // A preset applied from the web UI, a macro or a playlist moves the cursor
      // too, or the next press would step from wherever the button last left it.
      if (currentPreset && currentPreset != presetCursor) {
        presetCursor = currentPreset;
        presetOrdinal = presetOrdinalOf(currentPreset);
        presetLabelPending = true;
        presetNameTries = 0;
      }

      if (presetCursor && presetLabelPending) {
        // getPresetName() answers false both for a busy JSON buffer and for a
        // preset saved without a name, and the two cannot be told apart - so the
        // retries are bounded, or an unnamed preset would read the file once a
        // second for the rest of the session.
        String name;
        if (getPresetName(presetCursor, name)) {
          clip(name.c_str(), presetLabel, BIG_CHARS);
          presetLabelPending = false;
          dirtyMain = true;
        } else if (++presetNameTries >= HMI_PRESET_NAME_TRIES) {
          presetLabel[0] = '\0';    // falls back to the effect name
          presetLabelPending = false;
          dirtyMain = true;
        }
      }

      // Blank the backlight after 5 minutes with nothing changing. A running clock
      // redraws once a minute, which would keep resetting this timer, so the auto-off
      // only applies while there is no clock on screen - see readme.md.
      if (!ntpEnabled && !displayTurnedOff && (now - lastRedraw > DISPLAY_IDLE_OFF_MS)) {
        backlight(false);
        displayTurnedOff = true;
      }

      if (overlayExpired) {
        view.clear();
        fullMainRedraw = true;
        dirtyMain = true;
      }

      /*
       * The brightness write-back, and its trigger is the view retiring rather than
       * a count of seconds: the moment the brightness view drops and the main area
       * goes back to the idle layout is the moment the user has finished adjusting,
       * and it happens on screen, so "it saved when the screen came back" is
       * something they can watch rather than a delay they have to take on trust. It
       * also collapses a run of presses for free - each one puts the view back up,
       * which holds the write off.
       *
       * `isButtonPressed()` is not redundant with that: it is false only once the
       * finger is off, so a brightness view configured to expire instantly
       * (hmiOverlayMs 0) still cannot write from the middle of a hold. This sits
       * after the gate above, so a failed name lookup is retried about a second
       * later rather than in a tight loop.
       */
      if (stateSavePending && view.overlay() != HmiOverlay::BRIGHTNESS && !isButtonPressed(0)) {
        fireStateSave();
      }

      IPAddress currentIp = apActive ? WiFi.softAPIP() : WLEDNetwork.localIP();
      int16_t quality = WLED_CONNECTED ? getSignalQuality(WiFi.RSSI()) : -1;

      // Refresh the clock here rather than only while drawing: localTime is only
      // recomputed when this is called, so comparing it without refreshing first
      // would make the status bar's minute change undetectable.
      if (ntpEnabled) updateLocalTime();

      // Top bar: clock, date, signal, brightness.
      if (knownBrightness != bri) dirtyTop = true;
      if (ntpEnabled && (knownMinute != minute(localTime) || knownHour != hour(localTime) ||
                         knownMonth != month(localTime) || knownDay != day(localTime) ||
                         knownYear != year(localTime))) dirtyTop = true;
      if (knownQuality != quality) dirtyTop = true;

      // Bottom bar: address and current draw. This is the bar that repaints
      // every second, which is why both of its fields are cheap.
      if (knownIp != currentIp || knownApActive != apActive) dirtyBottom = true;
      uint16_t milliamps = BusManager::currentMilliamps();
      if (knownMilliamps != milliamps) dirtyBottom = true;

      // Main area. The fields inside it are diffed individually when drawing.
      if (view.overlay() == HmiOverlay::NONE) {
        if (mainMode != strip.getMainSegment().mode ||
            mainPalette != strip.getMainSegment().palette ||
            mainOff != (bri == 0)) {
          dirtyMain = true;
        }
      }

      if (!dirtyTop && !dirtyBottom && !dirtyMain) return;

      if (displayTurnedOff) {
        backlight(true);
        displayTurnedOff = false;
      }
      lastRedraw = now;

      // Update what we last painted. Done before drawing so the overlays, which
      // read the `known*` values, see the state they are describing.
      knownBrightness = bri;
      if (ntpEnabled) {
        knownMinute = minute(localTime);
        knownHour   = hour(localTime);
        knownMonth  = month(localTime);
        knownDay    = day(localTime);
        knownYear   = year(localTime);
      }
      knownIp = currentIp;
      knownApActive = apActive;
      knownQuality = quality;
      knownMilliamps = milliamps;
      knownMode = strip.getMainSegment().mode;
      knownPalette = strip.getMainSegment().palette;

      if (dirtyTop)    { drawStatusTop();    dirtyTop = false; }
      if (dirtyBottom) { drawStatusBottom(); dirtyBottom = false; }
      if (dirtyMain) {
        if (view.overlay() == HmiOverlay::NONE) drawMainIdle();
        else drawMainOverlay();
        dirtyMain = false;
        fullMainRedraw = false;
      }
    }

    /*
     * Button 0 is ours: the three gestures are handled here and WLED's own
     * handling of that button is suppressed by returning true.
     *
     * That includes the 5 s / 10 s AP and factory-reset holds, which button.cpp
     * would otherwise have run - they are reimplemented in HmiGesture. Handing
     * the press back to WLED part-way through instead does not work: button.cpp
     * only advances its press state while it owns the button (button.cpp:303),
     * so it would see a fresh press on release and never reach either threshold.
     */
    bool handleButton(uint8_t b) override {
      if (!hmiEnabled || !enabled || b != 0) return false;
      if (buttons.empty() ||
          (buttons[0].type != BTN_TYPE_PUSH && buttons[0].type != BTN_TYPE_PUSH_ACT_HIGH)) return false;

      unsigned long now = millis();
      HmiAction action = gesture.update(isButtonPressed(0), now);
      if (action == HmiAction::NONE) return true;

      switch (action) {
        case HmiAction::POWER_TOGGLE:
          // A short press drops a write-back the long press before it had armed. The
          // fire-time guards would not catch this one - the cursor does not move on a
          // power toggle - and switching off is not a decision about the preset's
          // brightness, so it must not become one.
          stateSavePending = false;
          toggleOnOff();
          stateUpdated(CALL_MODE_BUTTON);
          showOverlay(HmiOverlay::POWER, now);
          break;

        case HmiAction::MODE_NEXT: {
          // A preset carries the whole look - effect, palette, colour and however
          // much brightness the user chose to save with it - so the double press
          // walks the set built in the web UI rather than the 200-odd mode list.
          //
          // applyPreset() only records the request; handlePresets() acts on it
          // later in the same pass (wled.cpp:149). The cursor is therefore moved
          // here, and the screen reads that instead of currentPreset, which is
          // both a pass behind and cleared by any state change at all.
          uint8_t next = stepPreset(presetCursor);
          if (next) {
            applyPreset(next, CALL_MODE_BUTTON_PRESET);
            presetCursor = next;
            presetLabelPending = true;
            presetNameTries = 0;
          } else {
            // Nothing saved yet, so the gesture keeps its old meaning rather than
            // doing nothing until the user visits the web UI. The cursor goes with
            // it: with no presets to be in, a leftover id would only make the
            // first preset saved later look like the one already playing.
            presetCursor = 0;
            effectCurrent = (effectCurrent + 1) % strip.getModeCount();
            stateChanged = true;
            colorUpdated(CALL_MODE_BUTTON);
          }
          showOverlay(HmiOverlay::MODE, now);
          break;
        }

        case HmiAction::BRI_STEP: {
          // Fine steps at first, so the bottom of the range can be set precisely,
          // then larger ones once the user is clearly holding the button down.
          int step = (gesture.repeats() < HMI_STEP_SLOW_UNTIL) ? HMI_STEP_FINE : hmiStep;
          // 0 means "off"; dimming stops at 1 so the button cannot silently
          // switch the strip off.
          int next = constrain((int)bri + step * gesture.direction(), 1, 255);
          if (next != bri) {
            bri = next;
            stateUpdated(CALL_MODE_BUTTON);
            // Inside the guard so a step that clamps at 1 or 255 arms nothing: it
            // changed no brightness, so it is not worth a write.
            armStateSave();
          }
          showOverlay(HmiOverlay::BRIGHTNESS, now);
          break;
        }

        case HmiAction::ESCAPE_AP:
          // The 5 s hold has also been stepping the brightness for the previous 4.4 s,
          // so the strip is at full brightness by the time the AP comes up. That ramp
          // is a side effect of the gesture rather than a level anyone chose, and the
          // same hold is how someone gets back from it - persisting it would turn the
          // escape hatch into a permanent re-levelling of the strip. If it did not
          // persist today, it does not start now.
          stateSavePending = false;
          DEBUG_PRINTLN(F("ST7735 HMI: held 5s, starting AP."));
          WLED::instance().initAP(true);
          break;

        case HmiAction::ESCAPE_RESET:
          // The same ramp, on the gesture that is about to wipe the filesystem the
          // write-back would have landed in.
          stateSavePending = false;
          DEBUG_PRINTLN(F("ST7735 HMI: held 10s, factory reset."));
          WLED_FS.format();
          doReboot = true;
          break;

        default:
          break;
      }
      return true;
    }

    /*
     * addToJsonInfo() can be used to add custom entries to the /json/info part of the JSON API.
     */
    void addToJsonInfo(JsonObject& root) override {
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");

      JsonArray displayArr = user.createNestedArray(F("ST7735")); //name
      displayArr.add(enabled ? F("installed") : F("disabled"));   //value
      if (enabled) displayArr.add(hmiEnabled ? F("HMI on") : F("HMI off"));
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
      top["hmi"] = hmiEnabled;
      top["hmiSave"] = hmiSave;
      top["hmiDouble"] = hmiDoubleMs;
      top["hmiOverlay"] = hmiOverlayMs;
      top["hmiRepeat"] = hmiRepeatMs;
      top["hmiStep"] = hmiStep;
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
      oappend(F("addInfo('")); oappend(String(FPSTR(_name)).c_str()); oappend(F(":rotation',1,'','both settings are landscape; they are the same picture rotated 180 degrees, so this is only about which way up the module is mounted. Takes effect immediately.');"));
      // Both landscape orientations, so the screen can be flipped for mounting.
      oappend(F("dd=addDropdown('")); oappend(String(FPSTR(_name)).c_str()); oappend(F("','rotation');"));
      oappend(SET_F("addOption(dd,'3 (default)',3);"));
      oappend(SET_F("addOption(dd,'1 (rotated 180)',1);"));

      oappend(F("addInfo('")); oappend(String(FPSTR(_name)).c_str()); oappend(F(":hmi',1,'','button 0 drives the screen: short=on/off, double=next preset, long=brightness. The presets are the ones on the Presets page, walked in id order with deleted ids skipped; with none saved, the double press falls back to the next effect. On by default; while it is on, the three button 0 macros on the Time settings page do nothing.');"));
      oappend(F("addInfo('")); oappend(String(FPSTR(_name)).c_str()); oappend(F(":hmiSave',1,'','a long press on brightness writes the new level back into the preset shown on screen, so walking back to that preset - or booting into it - brings the level with it. The write happens when the brightness view drops and the main screen comes back, so it is the end of the adjustment rather than a fixed delay. Only the brightness gesture does this: short press and double press are not saved. Nothing is written while the strip is not in a preset, and a preset that holds a playlist is left alone. What goes in is the whole current state rather than the brightness on its own, so anything else changed since that preset was applied goes in with it. This is not by itself what makes a level survive a reboot: set \"Apply preset N at boot\" in LED preferences too, or the strip still comes up at the startup brightness.');"));
      oappend(F("addInfo('")); oappend(String(FPSTR(_name)).c_str()); oappend(F(":hmiDouble',1,'','double-press window in ms, matching WLED. Shortening it makes the on/off press snappier; setting it to 0 removes the delay entirely and gives up the double press.');"));
      oappend(F("addInfo('")); oappend(String(FPSTR(_name)).c_str()); oappend(F(":hmiOverlay',1,'','how long the brightness/preset readout stays on screen after the last press, in ms.');"));
      oappend(F("addInfo('")); oappend(String(FPSTR(_name)).c_str()); oappend(F(":hmiRepeat',1,'','how often the brightness steps while the button is held, in ms. The first few steps are slower (200 ms) so the low end can be set precisely.');"));
      oappend(F("addInfo('")); oappend(String(FPSTR(_name)).c_str()); oappend(F(":hmiStep',1,'','brightness step once the button has been held for a moment. The first steps are finer (4) so the low end can be set precisely.');"));
    }

    /*
     * readFromConfig() is called when settings are loaded (at boot, and again when
     * settings are saved). It is called BEFORE setup(), so `initDone` gates the
     * part that talks to the display.
     */
    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      if (!top.isNull()) {
        // 0 is the "key absent" sentinel, and a cfg.json written by the portrait
        // build holds 0 or 2 - all three land on the default, 3.
        uint8_t newRotation = top["rotation"] | 0;
        rotation = (newRotation == 1) ? 1 : 3;
        if (initDone && rotation != appliedRotation) {
          tft.setRotation(rotation);
          appliedRotation = rotation;
          // The rules are part of the chrome, and setRotation() resets the
          // viewport, so they have to be redrawn with everything else.
          tft.fillScreen(HMI_C_BG);
          resetFieldCache();
          drawChrome();
          fullMainRedraw = true;
          dirtyTop = dirtyBottom = dirtyMain = true;
        }

        hmiEnabled   = top["hmi"]        | hmiEnabled;
        hmiDoubleMs  = top["hmiDouble"]  | hmiDoubleMs;
        hmiOverlayMs = top["hmiOverlay"] | hmiOverlayMs;
        hmiRepeatMs  = top["hmiRepeat"]  | hmiRepeatMs;
        hmiStep      = top["hmiStep"]    | hmiStep;

        hmiSave      = top["hmiSave"]    | hmiSave;

        gesture.setDoublePressMs(hmiDoubleMs);
        gesture.setRepeatMs(hmiRepeatMs);
        view.setDwellMs(hmiOverlayMs);

        // This runs again on every settings save, and an adjustment can be waiting for
        // the view to retire while it does: turning the switch off has to stop that
        // write rather than let it land afterwards.
        if (initDone && !hmiSave) stateSavePending = false;
      }
      // A settings save can change what the bars have to say - NTP switched off,
      // 12- or 24-hour clock - and nothing inside them would notice until the
      // next field happened to change on its own.
      if (initDone) dirtyTop = dirtyBottom = true;
      return true;
    }

    uint16_t getId() override {
      return USERMOD_ID_ST7735_DISPLAY;
    }
};

const char St7735DisplayUsermod::_name[] PROGMEM = "ST7735";

static St7735DisplayUsermod st7735_display;
REGISTER_USERMOD(st7735_display);
