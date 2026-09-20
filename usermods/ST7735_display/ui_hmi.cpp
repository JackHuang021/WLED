#include "ui_hmi.h"

/*
 * Button state machine.
 *
 * One sample per call. The shape of it mirrors wled00/button.cpp so that the
 * same muscle memory works, but the actions are the three the screen offers
 * instead of WLED's defaults:
 *
 *   short press   -> POWER_TOGGLE   (after the double-press window)
 *   double press  -> MODE_NEXT
 *   long press    -> BRI_STEP, then BRI_STEP every repeat interval
 *   hold >= 5 s   -> ESCAPE_AP      (on release)
 *   hold >= 10 s  -> ESCAPE_RESET   (on release)
 *
 * The escape hatches are reimplemented here rather than left to WLED because
 * ST7735_display returns true from handleButton() and thereby suppresses
 * button.cpp's own handling of button 0 entirely - see readme.md.
 */
HmiAction HmiGesture::update(bool pressed, uint32_t now)
{
  if (pressed) {
    if (!_pressedBefore) {        // rising edge
      _pressedTime = now;
      _longFired = false;
      _repeats = 0;
      // _waitTime is deliberately left alone: if a short press is still inside
      // its window, this press is the second half of a double press.
    }
    _pressedBefore = true;

    if (!_longFired) {
      if (now - _pressedTime < HMI_LONG_PRESS_MS) return HmiAction::NONE;
      // Long press. It cancels any pending short/double press, and flips the
      // brightness direction - there is only one button, so the direction is a
      // memory the user builds up rather than something they can see first.
      _longFired = true;
      _waitTime = 0;
      _dir = (_dir > 0) ? -1 : 1;
      _repeatAt = now;
      return HmiAction::BRI_STEP;
    }

    uint16_t interval = (_repeats < HMI_STEP_SLOW_UNTIL) ? HMI_REPEAT_SLOW_MS : _repeatFastMs;
    if (now - _repeatAt < interval) return HmiAction::NONE;
    _repeatAt = now;
    _repeats++;
    return HmiAction::BRI_STEP;
  }

  // ---- released ----
  if (_pressedBefore) {
    _pressedBefore = false;
    uint32_t held = now - _pressedTime;

    if (_longFired) {
      // A held press is judged on total duration, exactly as button.cpp does.
      // The 5 s and 10 s thresholds are checked here, so a long hold that was
      // adjusting brightness still reaches the escape hatch on release.
      _longFired = false;
      _repeats = 0;
      if (held >= HMI_ESCAPE_RESET_MS) return HmiAction::ESCAPE_RESET;
      if (held >= HMI_ESCAPE_AP_MS)    return HmiAction::ESCAPE_AP;
      return HmiAction::NONE;
    }

    if (held < HMI_DEBOUNCE_MS) return HmiAction::NONE;   // contact noise

    if (_waitTime) {              // the second press of a double press
      _waitTime = 0;
      return HmiAction::MODE_NEXT;
    }
    if (_doubleMs == 0) return HmiAction::POWER_TOGGLE;   // double press disabled: fire now

    _waitTime = now;              // hold the action until the window closes
    return HmiAction::NONE;
  }

  // ---- idle: nothing pressed ----
  if (_waitTime && (now - _waitTime > _doubleMs)) {
    _waitTime = 0;
    return HmiAction::POWER_TOGGLE;
  }
  return HmiAction::NONE;
}
