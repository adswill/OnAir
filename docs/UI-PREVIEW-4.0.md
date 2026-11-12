# Hover previews of the appearance choices (4.0)

When you hover a choice that changes how OnAir looks, a tooltip shows a small picture of the interface with that choice. Moving the
keyboard focus to the item (arrow keys in a menu or combo list) shows it too. The picture is drawn live with ImGui draw lists, so it
needs no image files and always matches the current palettes. Nothing changes until you pick the item.

Turn it off in Settings > Appearance > **Previews of the looks** (saved as `uiPreviews`, on by default). It is also off in the touch
interface, because a touch screen has no hover.

## Where it appears

| Choice | Where | What the picture shows |
|---|---|---|
| Colour palette (Terminal, Instrument, Mono, Dev A, Dev B, Studio, Mono Pro, Daylight) | Settings > Appearance; View menu (palette list and View > Look); side panel View > Look | The whole window in that palette's colours, corner radius and spacing |
| Dark / Light / High contrast | Settings > Appearance; View menu; side panel; touch menu sheet (no preview there) | The window after the Light or High contrast pass: dark on white, or black, white and yellow with 2 px outlines |
| Layout of the interface (Sidebar, Scope, Tiles, Faceplate, Scope: Cinema, Scope: Meters, Scope: Dock, Panel) | Settings > Appearance; View menu | That layout: side panel and tabs, ribbon and big scope, tiles, the chassis with display and knobs, the overlay, the meter grid, the bottom dock, the mode keys with lamps |
| Classic / Unified layout | Settings > Appearance (tick box); View > Layout; side panel View > Layout | Unified: spectrum and waterfall above the decoded tabs. Classic: the tabs on top |
| Font look (Current, Clean, Compact) | Settings > Appearance; View menu; side panel | Text size (13 or 14 px) and Inter vs. monospaced (rounded word bars vs. one block); in the classic interface also its spacing. The note says it applies after a restart |
| Text size presets 80 % to 200 % | View > Text size | Everything inside the window at that size |
| Touch interface | Settings > Appearance; View menu; side panel | Big digits, 48 px rows and a bottom dock of big buttons |
| Easy mode | Settings > General | The Simple screen of a mode: a big Start button and the key controls instead of the tabs |
| Classic interface | View > Classic interface | The classic toolbar, tabs and colours |
| Waterfall colour map (all maps) | Settings > Spectrum and waterfall; side panel Palette; the plot's Waterfall popup (both lists) | A gradient strip of the map and a small spectrum and waterfall in it. Colour-blind-safe maps say so under the picture |
| Trace colour | the plot's settings popup | The spectrum and waterfall with that trace colour |
| Tuner style (Classic, Simple) | side panel; the plot's settings popup | The tuned band as a shaded band or as the red centre line with grey edges |

The accent colour picker (Own accent colour) already shows its colour, so it has no picture of its own. Its colour is used in every
other picture.

## How it works

- `app/ui/palette_data.h`: the ten base colours of each palette, and each palette's corner radius and spacing, as plain data.
  `loadTheme()` in `app/ui/ui2_theme.cpp` now reads its colours from this table, so the preview and the real interface use one source.
- `app/ui/ui_preview_logic.h` (pure, no ImGui): `Look` holds the appearance settings in use; `with(look, option, value)` applies the
  hovered value to a copy; `describe(look)` returns a `Desc`: the colours as the screen shows them (after the Light pass of
  `light_theme.cpp`, which turns every shape's lightness over and keeps text at 5:1, or the High contrast colours), the metrics, the
  layout and flags. `previewFor(cur, option, value)` is both in one call.
- `app/ui/ui_preview_draw.cpp`: draws a `Desc` as a mock window: title bar with three lamps, the layout's body (side panel with section
  headings, fields, a tick box and a button; tab row; spectrum with grid, trace, fill and tuned band; waterfall in the colour map; meters,
  lamps, knobs, chassis), and a status bar. Text is drawn as bars, so the High contrast pass (it recolours glyphs only) leaves the picture
  alone.
- `app/ui/ui_preview.cpp`: `uiprev::item(app, option, value)` after the menu item / combo row / radio button / tick box shows the tooltip
  when the item is hovered, or when it has the keyboard focus and the keyboard is in use (`NavCursorVisible`).
- Light frames: when OnAir is in Light (or Daylight), the whole frame is turned over after drawing. The picture is drawn pre-turned
  (`forFrame`), and because the turn-over is its own exact inverse in 8-bit integers, a Dark preview still shows dark (tested).
- High DPI: the picture is 260 x 160 px times the display scale (`gUi`). Inside it, the real interface is shown at half size, with the
  hovered text size and touch scale applied on top of the display scale.

## Developer page

`dect2 --previewgallery --hidden --shot gallery.png` draws every picture of `uiprev::kGallery` on one page (36 pictures: palettes,
themes, layouts, fonts, text sizes, touch, Easy mode, tuner styles, a trace colour and three colour maps) instead of the main window.

## Tests

- `ui_preview` (`tests/app/test_ui_preview.cpp`): the palette table matches the colours `loadTheme()` used before; every palette and
  layout gives a different picture; Light, High contrast and Daylight colours and their contrast ratios; the Light pass undone exactly
  for 46 000 colours; font look and text size metrics; touch rows 48 px; the colour map each palette brings (Studio Magma, Mono Pro
  Amber, Daylight Paper, Navy heat elsewhere); the hovered option changes nothing else; and every combination of interface x palette x
  theme x layout x font look x touch x Easy mode x text size (13 824) has legible text, a visible accent and the right layout.
- `ui_preview_render` (`tests/app/test_ui_preview_render.cpp`): ImGui records the draw lists of every gallery picture and a small CPU
  rasteriser turns them into pixels. Checked: the picture covers its rectangle, has at least 12 colours, the title bar has the look's
  panel colour, waterfall pixels are colours of the chosen map, the strip's middle is the map's middle, a Dark preview drawn while the app
  is Light comes out dark after the Light pass, High contrast has a white outline, and the picture at 2x display scale has a title bar
  twice as tall. `ONAIR_UIPREVIEW_PNG=1` prints the gallery as a base64 PNG.

## Limits

- The picture is a schematic of each layout, not a screenshot: the panels are in the right places with the right colours and spacing,
  the content is mock data.
- Inter is only loaded when Clean or Compact is in use, so the font look is shown by its size and word shapes, not the real glyphs.
- Easy mode changes the decoded-view area; layouts without one (Scope, Cinema, Faceplate, Panel, Meters) look the same with it.
