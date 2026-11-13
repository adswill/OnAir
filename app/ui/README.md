# app/ui

Shared look and widgets: ImGui widgets, theme and contrast, the toolbar (toolbar*.cpp, statusbar.cpp, the mode table and selector), the new interface shell (ui2*.cpp), the classic layout (draw_ui.cpp), ui_common (helpers several screens share), scaling, icons, the mascot, the pop-out video window, the first-run wizard and the layout checker.

Same topic folder as core/src/<topic>/; see ARCHITECTURE.md. Mode folders hold one decoder mode each.

## Touch interface

`App::touchUi` (saved as `touchUi`; `--touch` on the command line, View side panel > Touch interface, the "Touch" button of the classic top bar) is a theme / layout mode, not a second set of panels. The panels, tabs and widgets are the ones of the normal interface; touch.cpp raises their metrics and touch_shell.cpp arranges them.

| File | What it does |
|---|---|
| `touch_logic.h` | The pure parts, unit tested (`tests/app/test_touch.cpp`): `Keypad` entry and `parseFrequency()`, the `Gestures` recogniser (pinch scale, pan, long press, swipe, fling), `Kinetic` scroll decay |
| `touch.cpp` | Metrics (frame height, spacing, scrollbar and slider grab >= 48 px at the display scale; `touchScale`, default 1.35x, on top of the text size), the per-frame gesture feed, long press = right click, tooltips only after a 0.4 s hold, drag / fling scrolling of every child window and table, bottom sheets and full-screen windows |
| `touch_sheets.cpp` | The numeric keypad, the tuning step picker (1 Hz .. 10 MHz), the full-screen searchable mode picker (tiles in the group colours), the menu sheet (text size, theme, TX view, language, back to the desktop interface), the OS on-screen keyboard button (Windows TabTip, Linux onboard / squeekboard) |
| `touch_shell.cpp` | The shell: top bar (menu, mode chip, state), the body (main tabs or the TX panel; the status line on top in portrait), and the bottom dock (Start / Stop, - freq + step, Gain, Volume, Tuner, More) |

How the widgets pick it up without being copied: `third_party/imgui` has four small hooks (see the block in `imgui.h`; all null / 0 when the touch interface is off): a least row height for `Selectable` / menu / combo rows and `SmallButton`, a tooltip gate, a window placement hook (popups become bottom sheets, titled floating windows fill a display narrower than 1000 px) and an end-of-child hook for drag scrolling. `applyMetrics()` only ever raises the style sizes, so it runs every frame after the theme (ui2_theme.cpp); main.cpp bumps `gStyleEpoch` when the touch mode changes so the theme is rebuilt from the scale-1 base. `tabItem()` reports the tabs so a swipe can pick the next one; plots (gfx/plot.cpp) zoom and pan with `GestureFrame` pinch data.

Multi-touch: GLFW delivers one pointer (the touch is emulated as a mouse), so pinch needs a backend that calls `touch::feedPointer()` for each finger; until then a plot zooms with the wheel / trackpad and the Zoom, Max and Min sliders (now finger-sized) and pans with one finger. Everything else (long press, swipe, fling, keypad) works from the single pointer.

Dev: `--touchsheet keypad[:7.1] | step | modes | menu | gain | vol | tuner | more[1-3]` opens a sheet at the start (for `--shot`), `--touchscale 1.0 .. 2.0`, `--touchsize 0 .. 4` (control size, Small .. Extra large; docs/TOUCH-SIZE-4.1.md).

Screens converted: the shell (dock, top bar, mode picker, keypad, step, gain, volume, tuner, more, menu), the main tabs and the TX panel (through the scaled metrics and drag scrolling), settings and every popup (bottom sheets), results tables (drag scroll, 48 px rows). Left as scaled-up desktop layouts: the map and ADS-B full-screen windows (pan with one finger, buttons for zoom), the Signal workbench, per-mode panels with fixed pixel widths (they stay usable, some columns are tight in portrait), the scanner, passes and gallery windows (full screen on a narrow display, desktop layout inside).

First start on a small screen (#139): when the primary monitor's usable area is 1100 points wide or less (or 560 high or less: 800 x 480, 1024 x 600, 720 x 720) and the user never chose, OnAir starts in the touch interface once (`touch::autoTouch()` in `touch_logic.h`, main.cpp; saved as `touchAutoSeen`). View > Touch interface turns it off and the choice is kept. The saved `touchUi` key is not a choice by itself (every save writes it), so an upgraded install gets the automatic start once too (`touch::touchChosen()`).
