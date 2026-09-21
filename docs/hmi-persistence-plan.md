# ST7735 HMI: the brightness goes back into the preset

> **Status: implemented (2026-09-21)** in
> [ST7735_display.cpp](../usermods/ST7735_display/ST7735_display.cpp). Everything below was traced
> against this tree on that date, and re-traced from scratch rather than carried over from the
> earlier draft of this document — see *Superseded* below. WLED moves, so re-verify a citation
> before trusting it. Related: [hmi.md](hmi.md) (the HMI design this extends),
> [readme.md](../usermods/ST7735_display/readme.md) (the user-facing description).

## Context

The ST7735 usermod's button HMI changes three things: on/off (short press), effect (double press)
and brightness (long press). A brightness dialled in that way did not stick. Walking away from the
preset and back brought the preset's own level back, and a power cycle brought the startup
brightness. The user's report:

> 修改亮度后，修改的亮度值不会保存到 preset 中，下次开机或者重新切换到该 preset，亮度又变成了原来的值

Nothing is wrong with the usermod here — this is WLED's model. State (`bri`, `effectCurrent`,
`on`, the segments) lives in RAM and reaches flash only when a preset is saved, so a preset is a
snapshot and anything adjusted since applying one is outside every snapshot. The button wrote `bri`
and called `stateUpdated()`, and the file on disk was never touched.

Decisions taken with the user:

| Question | Choice |
|---|---|
| Where the adjustment is stored | **Written back into the preset the strip is currently in** — overwriting it |
| What triggers a save | **Only the brightness long press.** Short press and double press are not saved |
| No preset currently selected | **Nothing is written.** There is nothing to write into |
| Goal | Walking back to that preset uses the new brightness |

### Superseded

An earlier version of this document proposed the opposite trade: a **dedicated preset slot**
(default 250), written on any of the three gestures, applied at boot, superseding WLED's own boot
preset — the shape upstream `usermod_v2_auto_save` ships. The user chose overwriting the active
preset instead, because it is the preset they already use and already switch between, and a second
slot would be a parallel copy of the strip state that they then have to keep in mind. Everything
from that draft that depended on the dedicated slot — the boot restore, `restoreQueued`, the
`setup()` existence check against *Error 12*, the factory-reset deferral, the `hmiSavePreset` key —
is gone. What survives is the WLED-internals work, re-verified below.

## Verified against the code

Each of these decides whether the feature works at all, so each was traced rather than assumed.

**1. `currentPreset` is unusable — the write-back keys off the usermod's own `presetCursor`.**
This was got wrong on the first attempt, in a way worth recording, because the wrong answer looks
right if you only trace half the path.

The first version had it that `currentPreset` survives a brightness step, so the write-back could
read it. The brightness-step half of that is true: `led.cpp:92-93` is
`if (bri != briOld || stateChanged) { if (stateChanged) currentPreset = 0; ... }`, and a brightness
step enters that outer branch (`bri` moved) while `stateChanged` is false — neither the assignment
nor the `setValuesFromFirstSelectedSeg()` inside it (`led.cpp:8`, segment → globals) raises the
flag, and `stateUpdated()` clears it again on the way out (`led.cpp:137`). A short press *does*
clear it, because `toggleOnOff()` sets `stateChanged` outright (`led.cpp:53`).

What that misses is that **`currentPreset` is already 0 by then, for every preset the button
applies.** `handlePresets()` assigns it at `presets.cpp:201` and calls `stateUpdated()` at `:208` —
one statement in between — and `deserializeState()` has just raised `stateChanged` for the
brightness that moved (`json.cpp:380`) and for the segments (`json.cpp:363`). So `led.cpp:93` zeroes
it before `handlePresets()` returns. WLED patches this up only on the `pd` path the web UI's
`setPreset()` uses, and says so in as many words at `json.cpp:527`: *"stateUpdated() will clear the
preset, so we need to restore it after"*. `applyPreset()` — which is what the HMI's double press
calls — is the queued path, and has no such restore.

The symptom was total: `armStateSave()` bailed on `!currentPreset` and no write ever happened. The
fix is `presetCursor`, which the usermod has always maintained for exactly this reason, and which
is also what the screen prints as `Preset 3/8`. That makes the rule stateable: **the preset the
screen names is the preset a long press re-levels.** The cursor follows the web UI as well (a
preset clicked there arrives as `pd`, which restores `currentPreset`, so the existing sync in
`loop()` catches it), and a playlist is skipped outright by fact 6.

The comment in the source, which the first version had "corrected" into the wrong claim, now records
the real reason.

**2. The value written is the new brightness, not a stale one.** `stateUpdated()` ends with
`if (bri > 0) briLast = bri;` (`led.cpp:117`), and `serializeState()` writes
`root["bri"] = briLast` rather than `bri` (`json.cpp:651`). Without that line the write-back would
save the previous level. Load-bearing, and easy to lose in a refactor of `stateUpdated()`.

**3. `savePreset()` with no name renames the preset.** The name falls back to
`sprintf_P(saveName, "Preset %d", index)` (`presets.cpp:227-231`), which `doSaveState()` then writes
into `"n"` (`presets.cpp:49-50`). The existing name has to be read off the file first, so a save
that cannot read it is skipped rather than allowed to rename.

**4. An empty `saveobj` is what includes the brightness.** `presets.cpp:246`:
`includeBri = sObj["ib"].as<bool>() || sObj.size()==0 || index==255`, so the two-argument call
resolves `includeBri = true` and the preset carries `on` + `bri`.

**5. The write is queued, not performed.** `savePreset()` only sets `presetToSave`;
`handlePresets()` → `doSaveState()` does the serialising and the LittleFS write on a later pass,
with `strip.suspend()`/`resume()` around it. `savePreset()` never touches the JSON lock (only
`doSaveState()` does), so calling it from a usermod `loop()` is safe.
`presetNeedsSaving()` (`presets.cpp:25`) reports whether something is already queued.
`writeObjectToFileUsingId()` (file.cpp) seeks to the single `"<id>":` entry and rewrites that
entry's byte range, space-padding when the new JSON is shorter — it is not a whole-file rewrite.

**6. A playlist preset must be skipped.** `currentPlaylist` (`wled.h:724`, `int16_t`, −1 when
none) is set to the preset id when a playlist is loaded (`playlist.cpp:145`), cleared by
`unloadPlaylist()`, and `applyPreset()` calls `unloadPlaylist()` first (`presets.cpp:129`). So
`currentPlaylist >= 0` reliably marks "the current preset is a playlist", and
`presets.cpp:201` sets `currentPreset` for playlists too (`changePreset` is true for them). Writing
a state into that slot would replace the playlist with whatever the strip is doing now.

**7. The write-back is a whole-state overwrite, not a brightness patch.** `serializeState()` writes
`on`, `bri`, `transition`, `bs` and every segment, and there is no public API that patches one key
of a preset. This is accepted rather than worked around: when nothing else has changed since the
preset was applied, the result is the preset's original content plus the new brightness, which is
what was asked for.

## Implementation

All of it is in [ST7735_display.cpp](../usermods/ST7735_display/ST7735_display.cpp); no WLED core
file changes.

- **`armStateSave()`** — called from `BRI_STEP`, *inside* the existing `if (next != bri)` guard, so
  a step absorbed by the clamp at 1 or 255 arms nothing. Returns without arming when `hmiSave` is
  off, when `presetCursor` is 0, or when `currentPlaylist >= 0`. Otherwise it captures
  `presetCursor` into `stateSavePreset` and raises `stateSavePending`. Capturing the id at press
  time rather than reading it when the write fires is what stops a preset applied from elsewhere
  before the view retires from redirecting the write into its own slot.
- **The trigger is the brightness view retiring**, in `loop()`: `stateSavePending && view.overlay()
  != HmiOverlay::BRIGHTNESS && !isButtonPressed(0)` → `fireStateSave()`. The moment the main area
  goes back to the idle layout is the moment the user has finished, and it is on screen, so the save
  is something they can watch rather than a delay they have to take on trust. A run of presses
  collapses for free: each one puts the view back up, which holds the write off.
- **`fireStateSave()`** guards, in order: `presetCursor != stateSavePreset` → drop (something moved
  the strip out of that preset); `bri == 0` → drop (see below); `presetNeedsSaving()` → drop (a save
  the user queued from the web UI is sitting in the same single global slots and ours would
  overwrite it whole); `getPresetName()` failing → stay pending and retry, up to
  `HMI_SAVE_NAME_TRIES`, then give up. On success, `savePreset(stateSavePreset, name.c_str())`.
  The call sits *below* the 1 s redraw gate, deliberately: a failed name lookup then retries about a
  second later instead of spinning, and the write itself is not on any path that has to be prompt.
- **Three gestures drop the pending write**: `POWER_TOGGLE`, `ESCAPE_AP` and `ESCAPE_RESET`. None of
  them moves `presetCursor`, so the fire-time guard cannot catch any of them, and each would
  otherwise write something nobody asked for — an `on: false` state for the short press, and full
  brightness for the two holds, which have been stepping brightness for ~4.4 s before they trigger.
  The short press is the one that matters for the user's stated scope — it is why the button does
  not persist on/off at all: a preset that has gone dark because of one press is hard to diagnose,
  and turning the strip off is not a decision about the preset's brightness but must not become one.
  `bri == 0` at fire time is checked as well, because a nightlight fade can reach zero without the
  user touching anything.
- **`isButtonPressed(0)` in that condition is not redundant** with the view being gone. It is what
  keeps a brightness view configured to expire instantly (`hmiOverlayMs` 0) from writing out of the
  middle of a hold. It is also why `setup()` now disables the HMI when **no button is configured at
  all** — `isButtonPressed()` indexes the button vector with no bounds check (`button.cpp:99`),
  because WLED only ever calls it from the button handler. The old code left `hmiEnabled` true in
  that case, which was harmless while the HMI was only ever reached through `handleButton()`.
- **Config**: `hmiSave` (bool, default `true`). `readFromConfig()` clears a pending write when the
  switch is turned off there, because it runs again on every settings save.

### Why not a settle delay

The first implementation used a `hmiSaveDelay` timer (default 10 s) instead of the view retiring.
That number was inherited from the `usermod_v2_auto_save` draft this design superseded, and it never
had a reason here. That usermod watches **global state**, so a web UI slider drag is dozens of
changes with no end marker and a time window is the only way to batch them. This one watches a
**button**, which reports its own release — and the view it raises is itself the end marker.

What the delay bought was collapsing a run of presses into one write, and the view does that for
free, because every press restarts its dwell. What it cost was real: a power cut in the window
after letting go lost the adjustment, which is the symptom this whole feature exists to remove. The
user asked why it was 10 seconds; there was no good answer, so the key is gone and the trigger is
the visible event.

## What is deliberately not added

- **No save on the other two gestures.** A short press to off would write `on: false` into the
  preset, so the preset goes dark — one press, and a light that will not come on until you look at
  the Presets page. The user chose brightness only.
- **No dedicated slot, no boot restore, no "Saved" overlay.** See *Superseded*.
- **No factory-reset deferral.** Dropping the pending write on `ESCAPE_RESET` covers the reachable
  case. The residual — a save already queued *and* undrained when the 10 s hold fires, so that
  `handlePresets()` recreates `/presets.json` from it after `WLED_FS.format()` — is not coded
  against: `handlePresets()` drains the queue every pass, and a write-back is armed only when a view
  retires. It would take the JSON lock being held for ten seconds.
- **No dedup against the last-saved state.** The view retiring already collapses the case that
  matters; a power on→off→on cycle costs one extra write.
- **No suppression during nightlight.** A write landing mid-fade captures an arbitrary `bri` and the
  preset does not carry `nl` (`serializeState` only writes it outside `forPreset`). Documented rather
  than coded, because silently not saving is the more confusing failure.
- **No second persistence mechanism for the reboot case.** See *Known limit*.

## Known limit: the reboot half needs a boot preset

The feature makes the level survive a **preset switch**. It does not by itself make it survive a
**reboot**: without a boot preset the strip comes up at `briS` (`wled.cpp:685`), the startup
brightness on the LED preferences page. Setting *Apply preset N at boot* to the preset holding the
level closes that, and nothing else is needed — the write-back has already put the level in it.

This is a configuration answer rather than a code one on purpose. Presets are WLED's only
persistence, and the alternative — a private file or a dedicated slot restored from `setup()` —
is a second copy of the strip state with its own precedence rules, which is exactly the design the
user rejected. The readme states it plainly under
[The brightness is saved back into the preset](../usermods/ST7735_display/readme.md#the-brightness-is-saved-back-into-the-preset)
and in the `hmiSave` help text on the settings page.

## Docs updated with it

- [usermods/ST7735_display/readme.md](../usermods/ST7735_display/readme.md) — new
  *The brightness is saved back into the preset* subsection under `## Button`; the false
  *"Presets are read, never written"* line; the AP-escape paragraph, which used to claim a reboot
  restores the level; the *Limitations* bullet, which now names the boot-preset caveat; and the
  "a preset cannot be created from the button" bullet, which is still true but no longer said of a
  preset that cannot be modified either.
- [hmi.md](hmi.md) — §6.6 *改动是否持久* row, §6.7 key table (one new key, plus a note that
  `hmiOverlay` now also sets when the write happens), §9's *需要在设备上新建预设*
  trigger, now marked as half satisfied. §3.4 item 1 explained why the cursor is the usermod's own
  by saying a brightness press clears `currentPreset`; that is not what clears it (fact 1), so the
  reason is corrected now that the write-back depends on the cursor. The §6.7 table also had two key
  names that did not match `addToConfig()` (`hmiDoubleMs`, `hmiOverlayMs`); those are corrected,
  with a note on the member-name/JSON-key difference.

## Verification

This fork's rule is that the user builds and flashes; no `pio` run. Manual checklist on the device
(`wled_c3p`, `-D WLED_DEBUG` is already on, so every step has a serial line):

1. **No preset, no write** — with nothing selected, long press to change brightness and let the
   screen come back: no `saving brightness to preset` line, `/presets.json` untouched.
2. **Write-back** — double press into a preset, long press to change brightness, release: the line
   appears **as the brightness view drops and the main screen returns**, the preset's **name
   unchanged** in the web UI (the regression test for fact 3), its brightness the new one.
3. **Switch back** — double press away and all the way round: the level is the one just saved.
4. **One write per adjustment, not per step** — a 2.5 s hold produces exactly **one** serial line,
   and it comes when the view retires, not while the button is down.
5. **A run of presses is one write** — hold, release, hold again, release (waiting less than the
   view's dwell each time): exactly one line, after the last view drops.
6. **Reboot** — set *Apply preset N at boot* to that preset and reboot: level and effect come back.
   Without a boot preset it does not, which is the documented behaviour, not a bug.
7. **On/off does not pollute the preset** — short press off while in a preset, switch back: the
   preset is still an `on` preset at its previous level.
8. **A short press drops a pending write** — long press to change brightness, then short press off
   *before* the brightness view has dropped: no save line, and the preset is still an `on` preset.
   This is the explicit clear, not the fire-time guard: `presetCursor` does not move on a power
   toggle, so the guard alone would let an `on: false` through here.
9. **A preset switch drops a pending write** — long press to change brightness, then double press
   before the view drops: no write, and the next preset is not overwritten.
10. **A playlist is skipped** — double press into a playlist preset, long press to change
    brightness, let the view drop: no save line, the playlist intact.
11. **The AP hold does not persist** — hold 5 s to bring up the AP (strip ramps to full), release,
    reboot: the strip comes back at the **saved** level, not full brightness.
12. **The switch works** — turn `hmiSave` off, use the button: no save line. Also flip it off *while*
    an adjustment is pending and confirm nothing is written.
