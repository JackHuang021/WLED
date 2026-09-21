// Button gestures and screen overlays for the ST7735 usermod.
//
// Deliberately free of any hardware or WLED dependency: the state machines here
// are driven by a bool ("is the button down") and a millisecond timestamp, so
// they can be exercised from a host-side loop by feeding it a timing sequence.
// Everything that touches the panel or the LED state lives in ST7735_display.cpp.
#pragma once

#include <stdint.h>

/*
 * Timing. The five constants marked WLED are copied from wled00/button.cpp, so
 * the gestures recognised here stay in step with the ones WLED itself would have
 * acted on. Do not tune them locally - a mismatch means the two implementations
 * disagree about what a "long press" is.
 */
#define HMI_DEBOUNCE_MS       50    // WLED_DEBOUNCE_THRESHOLD - shorter presses are contact noise
#define HMI_LONG_PRESS_MS    600    // WLED_LONG_PRESS       - brightness adjustment starts here
#define HMI_ESCAPE_AP_MS    5000    // WLED_LONG_AP          - hold this long, release -> AP mode
#define HMI_ESCAPE_RESET_MS 10000   // WLED_LONG_FACTORY_RESET
#define HMI_REPEAT_SLOW_MS   200    // repeat interval for the first HMI_STEP_SLOW_UNTIL steps
#define HMI_REPEAT_FAST_MS   150    // ... and after that
#define HMI_STEP_SLOW_UNTIL    5    // how many repeats run at the slow rate

enum class HmiAction : uint8_t {
  NONE = 0,
  POWER_TOGGLE,   // short press
  MODE_NEXT,      // double press
  BRI_STEP,       // long press, and once per repeat for as long as it is held
  ESCAPE_AP,      // released after >= 5 s
  ESCAPE_RESET    // released after >= 10 s
};

/*
 * Turns a button level into actions.
 *
 * The 350 ms double-press window is unavoidable: short press and double press
 * share the same leading action, so the device has to wait before it can tell
 * them apart. That wait lands on POWER_TOGGLE, which is why it can be switched
 * off entirely (setDoublePressMs(0)) at the cost of losing MODE_NEXT.
 */
class HmiGesture {
  public:
    // Call once per loop() with the current button level. Returns the action
    // this sample produced; at most one per call.
    HmiAction update(bool pressed, uint32_t now);

    // +1 while the current long press brightens, -1 while it dims. Read it
    // together with the BRI_STEP it belongs to - it flips on the long press that
    // starts each adjustment, and resetDirection() is what ends one.
    int8_t direction() const { return _dir; }

    // Repeats the current long press has already produced. 0 for the first step.
    uint8_t repeats() const { return _repeats; }

    bool doublePressEnabled() const { return _doubleMs > 0; }

    void setDoublePressMs(uint16_t ms) { _doubleMs = ms; }
    void setRepeatMs(uint16_t ms)      { _repeatFastMs = ms ? ms : HMI_REPEAT_FAST_MS; }

    /*
     * End the adjustment, so the next long press brightens instead of carrying
     * the last one's direction into it. The direction is a property of one
     * adjustment rather than a mode the button sits in, and the brightness view
     * is what an adjustment is - so the caller resets this when that view leaves
     * the screen. This class has no view to watch, deliberately.
     */
    void resetDirection() { _dir = -1; }

  private:
    bool     _pressedBefore = false;
    bool     _longFired = false;
    uint32_t _pressedTime = 0;   // when the current press started
    uint32_t _waitTime = 0;      // a short press waiting out the double-press window; 0 = none
    uint32_t _repeatAt = 0;      // when the last repeat fired
    // Starts at -1 so that the first long press flips it to +1 and brightens,
    // which is what WLED's own button 1 does (button.cpp:309). resetDirection()
    // puts it back here, which is what makes every adjustment start brighter.
    int8_t   _dir = -1;
    uint8_t  _repeats = 0;
    uint16_t _doubleMs = 350;    // WLED_DOUBLE_PRESS; 0 disables double press
    uint16_t _repeatFastMs = HMI_REPEAT_FAST_MS;
};

enum class HmiOverlay : uint8_t {
  NONE = 0,
  POWER,        // ON / OFF
  MODE,         // effect name after a double press
  BRIGHTNESS    // value and bar while the button is held
};

/*
 * How long a transient "operation view" stays on the main area. It is feedback,
 * not a screen the user has to leave: it appears when an action fires and drops
 * back to the idle layout on its own.
 */
class HmiView {
  public:
    // Every call restarts the dwell timer, so the brightness view stays up for
    // as long as the button keeps producing steps and 1.5 s past the last one.
    void show(HmiOverlay overlay, uint32_t now) {
      _overlay = overlay;
      _shownAt = now;
    }

    void clear() { _overlay = HmiOverlay::NONE; }

    HmiOverlay overlay() const { return _overlay; }

    // True once the overlay has been up for its full dwell time. The caller
    // clears it (and repaints the idle layout) when this goes true.
    bool expired(uint32_t now) const {
      return _overlay != HmiOverlay::NONE && (now - _shownAt) >= _dwellMs;
    }

    void setDwellMs(uint16_t ms) { _dwellMs = ms; }

  private:
    HmiOverlay _overlay = HmiOverlay::NONE;
    uint32_t   _shownAt = 0;
    uint16_t   _dwellMs = 1500;
};
