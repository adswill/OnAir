# Mode list: tinted categories (4.0)

Owner order: tint the whole background of each category in the left-hand mode list with that category's colour, and highlight the selected mode
row clearly, with hover and keyboard focus distinct from the selection. Branch `feat40-trackui`.
Code: `app/ui/ui2_shell.cpp` (`modeList`), the colour maths in `app/ui/row_tint.h`, test `tests/app/test_row_tint.cpp` (ctest `row_tint`).

## Before (3.9)

- Category heading: a 2.5 px colour bar at the left edge, the name in the dim text colour, the member count in a faint chip in the category colour.
- Mode rows: plain pane background; a hollow circle, filled with a small accent dot for the selected mode; the name in the bright text colour
  when selected. Hover: a 4 % white overlay. Keyboard focus: nothing drawn (the rows are invisible buttons).

## After (4.0)

- Every heading and its mode rows sit on a background mixed from the pane and the category colour: heading 20 %, mode rows 10 %. The fills reach
  over the item spacing, so a category reads as one block; a thin untinted line separates categories.
- Selected mode: 34 % mix, a 3 px bar in the theme accent at the left edge, the name in semibold (drawn twice where the typography has no
  semibold face) and its second line in the normal text colour. The radio circles are gone.
- Hover: a 17 % mix (a heading under the pointer: 26 %). Keyboard focus (when ImGui navigation is visible): a 1 px accent outline, never a fill,
  so focus, hover and selection are three different looks.
- The fold arrow, count and the folded group's current mode stay where they were; they are drawn in colours checked against the tint.

## Contrast in every theme

The fills are drawn opaque (the mix is computed, not alpha-blended), because the Light theme turns every vertex colour's lightness over after
the frame is built (`light_theme.cpp`) and an opaque colour turns over exactly. `row_tint.h`:

- `safeAlpha` cuts a mix back only where the brightest text would fall under 7:1 on it (pale categories on dark panes), so there is room for the
  normal and dim text below it.
- `liftText` moves a text colour towards the brightest text just far enough to hold 4.5:1 (WCAG AA) on its row, as shown after the Light or
  High contrast pass; text that already holds it is left exactly as the theme has it.

The test runs the 17 category colours and a 216-colour grid over the panes and text colours of all eight palettes (Instrument, Mono, Dev A,
Dev B, Studio, Mono Pro, Daylight, Terminal, plus black panes) in Dark, Light and High contrast, for heading, mode, hover and selected rows:
50 328 combinations, every text at 4.5:1 or better. It also checks that the real category colours stay visibly tinted (heading at least 12 %,
mode rows at least 7 %) and that the selected row is at least 10 points stronger than a plain row. Result on this branch: 0 failures.

## Not checked yet

No screenshots: the Mac was on battery (no local app build). To be taken on mains with a renamed app copy and isolated settings, in Dark,
Light, High contrast and Daylight.
