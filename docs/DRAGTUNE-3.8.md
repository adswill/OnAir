# Drag on the spectrum tunes, like SDR++ (3.8)

Branch `feat38-dragtune`. Setting: Settings > Radio > **Drag on spectrum**: *Tune (like SDR++)* (the default) or *Pan (old OnAir)*.
The same choice is in the Spectrum menu, above the click options. Key `specDrag` (0 tune, 1 pan).

## What changed

With **Tune**, on the main spectrum and waterfall:

- A left press tunes to the frequency under the pointer at once. While the button is held, a drag keeps tuning live.
- The drag snaps to the mode's channel step where the mode has one: FM 100 kHz, DMR 12.5 kHz, or the user's "Click snaps to" step.
  Otherwise it snaps to the click grid (1 kHz) or about two pixels of the zoom, whichever is coarser, on a 1-2-5 scale.
- Shift or Ctrl pressed during the drag gives fine steps: about one pixel, at most a tenth of the coarse step, never below 1 Hz.
  Shift held *before* the press still starts the Identify selection, as before.
- The radio is retuned only when the snapped frequency changes. Inside the captured band only the marker moves, the same as a click.
  With the target style ("click centres the radio"), the press does not tune. A press that does not move centres the radio on
  release, as before. A drag moves the marker live without re-centring on every step, so the view does not run away under the pointer.
- Panning:
  - A left drag on the frequency scale pans the frequency only. The scale is the strip of frequency labels along the bottom of the
    spectrum, or a look's label strip under it.
  - A right or middle drag anywhere on the spectrum or waterfall pans.
  - The wheel over the frequency scale pans by a twentieth of the view per notch.
  - Elsewhere the wheel works as View > Controls sets it, by default zoom.
  - A pan past the edge of the band still retunes the radio, now for right and middle drags too.
- The right-click menu (identify, markers) opens on a right release that did not move, because a right drag pans.
- Multi-receiver channels (VFOs): a press anywhere in a channel's band grabs that channel. The point it was grabbed by stays under the
  pointer, snapped to the zoom, with Shift or Ctrl for fine steps. Overlapping bands go to the nearest centre. Outside every band the
  press tunes the main receiver.
- General receiver: the same behaviour on its own listening channel, with the snap of its own step.

**Pan** restores the old behaviour exactly: a left drag pans, a click tunes on release, and the right-click menu opens on the press.

Kept unchanged:
- The FM filter edges, the general receiver's filter edges and extra channels, and the recording tune box. These are grabbed before
  the drag to tune and keep their own drags.
- Alt+click and the Add channel button.
- The band plan strip and the names on the spectrum. A click on a name still tunes and picks its mode.
- The zoom limits from #425 and #442. The wheel pan on the scale only moves the view, which stays clamped to the band.
- The tuning-offset overlay and the ghost channel box.
- The transmit rules: every tune goes through the existing `spectrumTune()` path, and no new checks were added.

## Touch (Android and the touch interface)

The plots share their code with the Android build. A one-finger drag still pans, because touch has no other way to scroll, and a tap
still tunes on release. A long press on the spectrum or waterfall is now held for tuning: dragging after the long press tunes live. A
long press released without moving opens the spectrum's menu as before (on the waterfall it tunes there). A plot claims the long
press each frame (`touch::claimLongPress`). Everywhere else a long press is still a right click.

## Where it applies

- Done: the main spectrum and waterfall (app/gfx/plots.cpp, app/gfx/plot.cpp), multi-receiver VFOs (app/vfo/vfo_ui.cpp), the general
  receiver (app/general/general_ui.cpp), and touch (app/ui/touch.cpp).
- Not changed, with the reason for each:
  - The airband channel spectrum and the antenna, sweep and survey plots have no click-to-tune. A drag pans them as before.
  - The fast-scan panorama tunes to the nearest detected hit, across a span much wider than the radio captures. A live drag there
    would retune the radio on every step.
  - The QO-100 views and the spectrum explorer (specx) have their own drag gestures (dial, region select).
  - The 3D waterfall uses the drag to move its camera.

## SDR++ reference

SDR++ (`core/src/gui/widgets/waterfall.cpp`, GPL-3) was read as a reference for behaviour only. No code was copied. In SDR++:
- A left press or drag on the FFT or waterfall sets the selected VFO, rounded to its snap interval.
- A press on another VFO selects it.
- A drag or the wheel on the frequency scale pans the view by a twentieth of its width per notch, and moves the centre past the band edge.

OnAir adds:
- right and middle drag pans;
- Shift or Ctrl fine steps;
- a snap step that follows the zoom when the mode has no channel step.

## Tests

- `tests/app/test_drag_tune.cpp` (ctest `drag_tune`) passes locally. It covers:
  - hit testing of the plot body and both kinds of scale strip, with a sweep over every row;
  - the press action for every button and area, in both settings and on touch;
  - the 1-2-5 steps and the snap steps for FM, DMR and no channel step, fine and coarse, with a zoom sweep from 0.1 Hz/px to 1 MHz/px;
  - the retune filter, checked by retune counts over a simulated FM drag;
  - the VFO grab, including overlap, zero width, the grab offset and snapping;
  - the menu rule and the wheel pan.
- `tests/app/run_drag_tune_gui.cmake` (ctest `drag_tune_app`, macOS) runs a renamed copy of the program with `--hidden --mute
  --drag-selftest`, with its own HOME, data and config folders and user-defaults domain (deleted after). It injects these mouse gestures
  on the spectrum of a stopped pager receiver and checks the tuned frequency and the view after each one:
  - a press then a drag (tune, view still);
  - a scale drag (pan);
  - a right drag and a middle drag (pan);
  - a drag with Shift held after the press (fine steps);
  - with Pan selected: a left drag (pan) and a click (tune).

  If the spectrum is not on screen in the default layout, the test prints SKIP. Under the overnight limits it was **not run locally**
  (no app build). It runs only in CI.
- Only syntax was checked locally (`clang++ -fsyntax-only`): plot.cpp, plots.cpp, touch.cpp, vfo_ui.cpp, general_ui.cpp, prefs.cpp,
  settings_window.cpp, main.cpp and main_start.cpp. `test_settings` passes with the new row (88 rows).
- No live radio was used. The touch long-press path was syntax-checked only, with no device test.
