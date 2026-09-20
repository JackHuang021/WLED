# ST7735 HMI: persist screen-side adjustments across reboot

> **Status: plan only — not implemented, deliberately deferred (2026-09-20).**
> Everything below was traced against this tree on that date, including the line references and the
> WLED internals it depends on. WLED moves, so re-verify before implementing rather than trusting a
> citation. Related: [hmi.md](hmi.md) (the HMI design this extends).

## Context

The ST7735 usermod's button HMI ([ST7735_display.cpp](../usermods/ST7735_display/ST7735_display.cpp)) changes
three things — on/off (short press), effect (double press) and brightness (long press) — by writing to WLED's
global state (`bri`, `effectCurrent`) and calling `stateUpdated(CALL_MODE_BUTTON)`. WLED state is volatile by
design: it lives in RAM and reaches flash only when a preset is saved. So a power cycle throws the adjustments
away and the strip comes back at WLED's startup brightness and default effect. The readme lists this as a known
limitation and [hmi.md](hmi.md) §6.6 states it as a deliberate decision.

The goal is the opposite: **what was adjusted on the screen comes back after a power cycle.**

Decisions taken with the user:

| Question | Choice |
|---|---|
| What triggers a save | **Only the three button gestures.** Web UI / app / preset changes are not saved. |
| Where the state is stored | **A WLED preset slot** (default 250), written with WLED's own `savePreset()`. |
| Boot precedence | **The auto-saved state wins** over WLED's "Apply preset N at boot". |
| Strip switched off, then power cycled | **Comes back off** — faithful restore. |

Storing it as a preset rather than a private file means the *whole* strip state is preserved (effect, palette,
colours, speed, intensity, brightness, on/off) by WLED's own `serializeState()`/`deserializeState()`, it is
visible and editable in the web UI, and the restore rides WLED's boot-preset path instead of being a second
implementation of it. Upstream ships the same shape in
[usermod_v2_auto_save](../usermods/usermod_v2_auto_save/) — same mechanism, different trigger (it watches global
state; this watches the HMI).

## Implementation

All of it goes in [ST7735_display.cpp](../usermods/ST7735_display/ST7735_display.cpp); no other source file
changes.

### 1. Config keys (three new, persisted in `cfg.json` under `um.ST7735`)

| Key | Member | Default | Valid | Meaning |
|---|---|---|---|---|
| `hmiSave` | `bool hmiSave` | `true` | — | off = no auto-save **and** no boot restore |
| `hmiSavePreset` | `uint8_t hmiSavePreset` | `250` | 1..250 | the slot the auto-save uses |
| `hmiSaveDelay` | `uint16_t hmiSaveDelayS` | `10` | 2..3600 | settle time, seconds |

Member names deliberately avoid `savePreset` — that is the name of a global function this code has to call.

- `addToConfig()` — three lines beside the existing `top["hmi"] = hmiEnabled;`.
- `readFromConfig()` — mirror the file's existing `rotation` idiom, where any out-of-range value falls back to
  the default rather than being clamped:
  ```cpp
  uint8_t  newSlot = top["hmiSavePreset"] | 0;
  hmiSavePreset = (newSlot >= 1 && newSlot <= 250) ? newSlot : 250;
  uint16_t newDelay = top["hmiSaveDelay"] | 0;
  hmiSaveDelayS = (newDelay >= HMI_SAVE_DELAY_MIN && newDelay <= HMI_SAVE_DELAY_MAX) ? newDelay : HMI_SAVE_DELAY_DEFAULT;
  ```
  Clamping into 1..250 instead would turn a stored `0` into slot **1** and overwrite whatever preset the user
  keeps there. The fallback form cannot do that.
- `readFromConfig()` must also **disarm a live deadline** — it runs again on every settings save
  (wled00/set.cpp), so turning `hmiSave` off or shortening the delay while a save is pending has to take
  effect: `if (initDone && !hmiSave) stateSaveAt = 0;`
- `appendConfigData()` — three `addInfo()` strings in the style of the existing `hmi*` keys, saying: where the
  state is saved and that the slot is overwritten on every gesture, that only the button triggers it, that it
  supersedes WLED's own boot preset, and that another auto-save usermod using the same slot will fight it.

### 2. Arming the save (button side)

New members: `unsigned long stateSaveAt = 0;` (0 = nothing pending) and a helper:

```cpp
void armStateSave(unsigned long now) {
  if (hmiSave && hmiSavePreset) stateSaveAt = now + (unsigned long)hmiSaveDelayS * 1000UL;
}
```

Called from `handleButton()`'s switch, in the three cases that actually change the strip:

- `POWER_TOGGLE` — `toggleOnOff()` always changes `bri`.
- `MODE_NEXT` — always changes the effect.
- `BRI_STEP` — **inside the existing `if (next != bri)` guard**, so a step that clamps at 1 or 255 does not arm
  a pointless write.

Two actions *clear* the deadline instead:

- `ESCAPE_AP`. A 5 s hold to start the access point has also been stepping brightness every 150 ms for the
  previous 4.4 s, so the strip ends up at full brightness. The readme documents that ramp as a side effect that
  a reboot clears; persisting it would make the recovery gesture permanently re-level the strip. Clear it.
- `ESCAPE_RESET` — see §6, which needs more than a clear.

The repeated `BRI_STEP`s of one long press are absorbed by the settle window: each pushes the deadline out, so
a 2.5 s hold writes once, not fifteen times.

### 3. Performing the save (loop side)

In `loop()`, after `unsigned long now = millis();` and **before** the early-return gate at
`if (!hmiUrgent && !overlayExpired && (now - lastUpdate < USER_LOOP_REFRESH_RATE_MS)) return;` — so the write is
not tied to the redraw cadence, and never runs before `setup()` has set `enabled`:

```cpp
if (stateSaveAt && (long)(now - stateSaveAt) >= 0) {
  if (presetNeedsSaving()) {            // someone (the web UI) already has a save queued
    armStateSave(now);                  // do not stomp it; try again after the next settle
  } else {
    stateSaveAt = 0;
    saveStateToPreset();
    if (!presetNeedsSaving()) DEBUG_PRINTLN(F("ST7735: state save was rejected."));
  }
}
```

The `presetNeedsSaving()` guard on both sides matters, because `savePreset()` gives no feedback:

- **Before** — `presetToSave`, `saveName`, `includeBri`, `saveLedmap` and friends are single global slots
  (wled00/presets.cpp). A save the user triggered from the web UI earlier in the same pass is still queued when
  our deadline fires, and our call would overwrite it — silently losing the user's save.
- **After** — `savePreset()` returns early without setting `presetToSave` if either PSRAM allocation fails
  (wled00/presets.cpp), and the deadline has already been consumed. `presetNeedsSaving()` immediately after is
  the only way to tell; log it rather than pretending it worked.

The comparison is `(long)(now - stateSaveAt) >= 0`, not `now >= stateSaveAt`, so it survives the 49.7-day
`millis()` rollover. The `stateSaveAt &&` term is load-bearing: `(long)(now - 0) >= 0` is true at boot.

`saveStateToPreset()` calls `savePreset(hmiSavePreset, "ST7735 auto")` — a fixed name, because a timestamp
(what upstream auto_save does) reads as `~ 01-01 00:00 ~` on a device whose clock has not synced yet.

This is a *queue* call, not a write: `savePreset()` sets `presetToSave` and returns; WLED's own `handlePresets()`
→ `doSaveState()` does the serialisation and the LittleFS write in the main loop, with `strip.suspend()`/
`resume()` around it. Passing no `saveobj` selects the same branch the core's own `PS=` handler uses, which
resolves `includeBri = true` — that is what makes the preset carry `on` + `bri`.

Flash wear is bounded better than it first looks: `writeObjectToFile()` (wled00/file.cpp) seeks to the single
`"<id>":` entry and rewrites only that entry's byte range, space-padding when the new JSON is shorter. It is
not a whole-file rewrite; growth (and therefore an append) happens only when the state serialises longer than
the previous save.

Deliberately **not** done here: calling this from `handleButton()`. `handleIO()` runs before `strip.service()`
(wled00/wled.cpp) and the file already documents that the button callback must only update state, never do I/O.

### 4. Restoring at boot

At the end of `setup()`, after `initDone = true`:

```cpp
if (hmiSave && hmiSavePreset) {
  String presetName;
  if (getPresetName(hmiSavePreset, presetName)) {
    DEBUG_PRINTF_P(PSTR("ST7735: restoring saved state from preset %u\n"), hmiSavePreset);
    applyPreset(hmiSavePreset, CALL_MODE_INIT);
    restoreQueued = true;
  }
}
```

- `getPresetName()` is an **existence check, not a formality**: applying a preset that does not exist sets
  `errorFlag = ERR_FS_PLOAD` (12), which surfaces as an *"Error 12: Preset not found"* toast in the web UI on
  the next `/json/state`. On a first boot, or after a factory reset, that would be every boot. `getPresetName()`
  takes the global JSON buffer lock (a recursive mutex with no network dependency, so it is safe this early)
  and returns false cleanly for a missing preset.
- `CALL_MODE_INIT` matches how WLED queues its own boot preset, and keeps boot from looking like a user change:
  it suppresses the UDP notify and the interface push.
- Precedence works because `applyPreset()` is a pure write to a **single-slot** queue, and
  `UsermodManager::setup()` runs *after* `beginStrip()` queued the configured boot preset — so ours is the write
  that survives. This is the same mechanism upstream `usermod_v2_auto_save` relies on.
- Calling `handlePresets()` directly from `setup()` is **not** an acceptable shortcut: it would run
  `UsermodManager::onStateChange()` into every usermod, including ones whose `setup()` has not run yet.
- Restore sits behind `enabled` — a device whose pin allocation failed never gets here, so the user's boot
  preset wins there. With no display there is no HMI, so there is nothing to be consistent with.

### 5. Holding the first paint until the restore lands

`bootPreset` defaults to 0, so the usual path is *not* WLED's `setup()` drain block: the queued apply waits for
the first `handlePresets()` in `loop()`, which runs **after** the usermod's `loop()`. Without a guard the
usermod paints one full frame of pre-restore state (effect 0, `briS`) and then, because it clears `hmiUrgent`
and sets `lastUpdate`, the redraw gate holds that wrong picture for up to a second.

So `restoreQueued` is set when the restore is queued, and `loop()` returns *before* the redraw gate while it is
set — without touching `lastUpdate` or `hmiUrgent` — so the real paint is the pass after the preset has landed.
It is cleared when the restore has visibly happened (`currentPreset == hmiSavePreset`, which `handlePresets()`
sets for any preset carrying `on`/`bri`) or after a ~2 s timeout, so a restore that somehow never lands cannot
leave the screen blank.

Nothing else in the display needs invalidating: the dirty flags start true, `knownBrightness`/`mainMode`/
`mainPalette` start at sentinel values, and the 5-minute blanking keys off `lastRedraw`, which a save does not
touch.

### 6. Factory reset versus a save that is already queued

Clearing `stateSaveAt` on `ESCAPE_RESET` covers the normal case, but not a save that is *already* queued and
still waiting: `doSaveState()` returns early without clearing `presetToSave` when it cannot get the JSON lock,
and in realtime mode `handlePresets()` is skipped entirely. If one is outstanding when the 10 s hold fires,
`WLED_FS.format()` wipes `presets.json` and the very next `handlePresets()` recreates it from the queued save —
a factory reset that silently undoes itself.

There is no API to cancel a queued save, so the reset defers instead: if `presetNeedsSaving()`, set
`resetPending` and do `WLED_FS.format(); doReboot = true;` on a later pass once the queue has drained (the lock
retry makes that milliseconds). `doReboot` is not acted on until well after the usermod's `loop()`, so deferring
costs nothing.

### 7. What is deliberately *not* added

- **No "Saved" overlay.** `HmiView`'s three overlays are feedback for a press; this fires 10 s after the user
  stopped touching anything.
- **No dedup against the last-saved state.** The settle window already collapses the only case that matters;
  a power on→off→on cycle costs one extra write.
- **No suppression during nightlight.** A save landing mid-fade captures an arbitrary `bri` (the preset does not
  carry `nl` — `serializeState` only writes it outside `forPreset`). Documented rather than coded, because
  silently not saving is the more confusing failure.

## Verified against the code

Traced before writing this plan, because each one decides whether the feature works at all:

- `WLED::setup()` order is `beginStrip()` → `UsermodManager::setup()` →
  `if (bootPreset > 0) { handlePresets(); … }` (wled00/wled.cpp). `beginStrip()` only *queues* the boot preset,
  so the usermod's queue write wins.
- The usermod's `loop()` runs before `handlePresets()` in the same iteration, and `enabled` cannot be true
  before `setup()` — so the placement in §3 satisfies "never skipped, never before presets are loaded".
- `serializeState()` writes `on` + `briLast`; `deserializeState()` re-derives the pair through `toggleOnOff()`.
  The off-state round-trip is native, not something to special-case — the "comes back off" choice falls out of
  the mechanism.
- The restored brightness is **not** silently overridden afterwards: `turnOnAtBoot`/`briS` are applied inside
  `beginStrip()`, strictly before the preset, and `applyFinalBri()` only mirrors whatever `bri` then is.
- The upstream `usermod_v2_auto_save` readme's claim that "WLED doesn't respect the brightness of the preset
  being auto loaded" is **stale** — it described a `briS` write that ran after the queue and before the drain in
  a 2021 version of `beginStrip()`; that write now happens before the queueing. Its remaining true half is the
  one we want: the usermod does supersede the configured boot preset.
- `savePreset()` itself never touches the JSON lock (only `doSaveState()` does), so calling it from a usermod
  loop is safe.

Two limits that are documented rather than fixed, both outside this usermod's reach:

- Precedence is over **WLED's own** boot preset. The queue is one global, so any *other* usermod that calls
  `applyPreset()` from its `setup()` after ours wins unconditionally — `usermod_v2_auto_save` is the in-tree
  example, and it also defaults to slot 250.
- A save that fails on `ERR_FS_QUOTA`/`ERR_FS_GENERAL` leaves `errorFlag` set, because `doSaveState()` ignores
  `writeObjectToFileUsingId()`'s result and `serializeState(..., forPreset=true)` never clears it.

## Docs to update when this is implemented

- [usermods/ST7735_display/readme.md](../usermods/ST7735_display/readme.md)
  - New `### Adjustments survive a reboot` subsection under `## Button`: what is saved, the settle delay, the
    slot, the boot restore, the precedence (and its limit), the three settings keys, and the "switched off is
    restored as off" behaviour.
  - `## Limitations`: replace the *"Brightness, effect and on/off are volatile … The button cannot save one"*
    bullet.
  - The AP-escape note (~line 275) says *"Brightness is not persisted, so a reboot … restores it"* — no longer
    true once `ESCAPE_AP` clears the deadline.
- [hmi.md](hmi.md)
  - §6.6 table: the *"改动是否持久"* row currently reads **默认不持久** — rewrite for the new behaviour and its
    button-only scope.
  - §6.7 table: add the three keys.
  - §9: *"需要『保存为预设』来固化当前状态（WLED 的改动易失是个真实的痛点）"* is listed as a trigger for building
    a menu — it is now satisfied, so it should be marked as such rather than left standing.
  - §10 item 6's "must read `/presets.json` asynchronously" caveat still holds for *displaying* a preset name;
    it does not apply to the `setup()` existence check, so it needs no change.

## Verification

This fork's rule is that the user builds and flashes; no `pio` run. Manual checklist on the device
(`wled_c3p`, `-D WLED_DEBUG` is already on, so every step has a serial line):

1. **First boot after flashing** — no preset 250 exists: no restore line in the log, no *"Error 12: Preset not
   found"* toast in the web UI, and the strip comes up exactly as it does today.
2. **Settle and write** — double press (next effect), stop touching the button, wait >10 s. One save appears;
   `/presets.json` has a slot 250 named `ST7735 auto`; the web UI lists it. A long brightness hold must produce
   **one** save after release, not one per step.
3. **Restore** — power cycle. The effect and brightness come back, and the panel paints them on its first frame
   rather than showing the startup state first (§5).
4. **Precedence** — set *Apply preset N at boot* to a different preset, power cycle, confirm the auto-saved
   state wins.
5. **Off is restored as off** — short press to switch off, wait for the save, power cycle: strip stays dark.
   Short press again and `briLast` returns it to the level it was at.
6. **Only the button saves** — change brightness/effect from the web UI, wait, power cycle: the strip comes back
   at the preset's state, not the web UI state.
7. **The AP hold does not persist** — hold 5 s to start the AP (strip ramps to full), release, wait >10 s,
   power cycle: the strip comes back at the *saved* level, not full brightness.
8. **Factory reset** — hold 11 s. After the reboot `presets.json` is gone and slot 250 has not been recreated.
9. **The switch works** — set `hmiSave` to false on the settings page, change things with the button, power
   cycle: today's behaviour. Also flip it off *while* a save is pending and confirm nothing is written.
