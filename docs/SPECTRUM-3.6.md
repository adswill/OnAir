# Spectrum and waterfall controls, 3.6 notes

Done in feat36-spectrum (pure maths in app/gfx/tune_drag.h, tests in tests/app/test_tune_drag.cpp):
- Wheel zoom keeps the frequency under the pointer fixed (it already did in plot.cpp; the missing part was the tuner marker). Within 10 px of the
  tuner marker the marker is the fixed point (zoomAnchor / zoomAbout).
- Target click ("Click centres the radio"): why it felt weird. (1) A click was ignored when it equalled the tuned frequency, so a signal tuned
  but off-centre could not be centred by clicking it, and the grid snap made nearby clicks equal too (targetClickActs now compares with the radio
  centre). (2) After the retune the zoomed view stayed where it was, so the signal was not in the middle (centreView). (3) Double click reset the
  zoom, so a double click on a signal tuned it and threw the zoom away. Double click now only tunes; Home (bound now) shows the whole band.
- Mode offsets: modes with a ModeTuning tuneOffsetHz keep the radio centre offset from the dial on purpose (DC spike off the signal), so with a
  full-span view the signal sits tuneOffsetHz from the axis centre. Zoomed views are centred exactly on the signal.

Bigger ideas, not done:
- One view model object (lo, hi, band, marker) replacing the two retune handlers (setupFreqAxis and specFreqViewUpdate) that both adjust the view.
- Animated recentre after a target click (ease the view instead of jumping).
- Cursor readout box in every look (not only the looks), with offset from the tuner marker and bandwidth.
- Snap-to-peak on click (nearest local maximum within a few pixels).
- Optional "true centre" target for offset modes (signal on the DC spike), per mode, off by default.
- Zoom-in/out keys bound by default (+/-), pan with Shift+arrows.
