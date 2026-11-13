# Touch control size 4.1 (#139)

The touch interface (`onair --touch`, View > Touch interface) was cluttered at 640 x 480: two dock rows of 55 px and a 53 px top bar left
about 250 px for the panels. 4.1 adds a control size and gives short screens a tighter frame.

## The setting

Settings > Appearance > **Touch control size** (key `touchSize`, 0 .. 4), also View > Touch control size and the touch Menu sheet
(Control size), and `--touchsize 0..4` for tests:

| Size | Factor | Target at display scale 1 |
|---|---|---|
| Small | 0.70 | 34 px (never under 32 px) |
| Compact | 0.85 | 41 px |
| Default | 1.00 | 48 px (the size it always had) |
| Large | 1.20 | 58 px |
| Extra large | 1.40 | 67 px |

The factor scales the hit target (`touch::target()`), the paddings and spacings `applyMetrics` raises the style to, the top bar and the dock
rows, the keypad, step and mode picker sheets (they use `target()`), and the mode chip's width. It applies at once: `main.cpp` rebuilds the
style when the size changes, because `applyMetrics` only ever raises sizes. The text size of the touch interface stays its own setting.

## Short screens

`touch_logic.h shellLayout()` decides the frame from the display size, the display scale and the factor. Under 600 px of display-scale
height the top bar is 0.95 targets (was 1.1) and a dock row 1.0 (was 1.15); when eight buttons fit across with the smallest (- and +) at
0.8 of a target, the dock is one row without the Step button, which moves to More (Step: ...). The status line above the tabs stays for
portrait screens only. Large displays (1000 px wide and more) are laid out exactly as before.

Panel height at 640 x 480, display scale 1 (from the unit test): before 253 px; now Small 359, Compact 344, Default 330, Large 246,
Extra large 198 (Large and Extra large go back to two dock rows because the buttons no longer fit one row).

## Tests

`tests/app/test_touch.cpp controlSize()` (touch_logic): factors grow, Default = 1, clamping, the 32 px floor, names; 1920 x 1080 and a
portrait tablet unchanged; 640 x 480 one row with Step in More and at least 70 px more for the panels; panel height never grows with a
larger size; one-row buttons stay 0.8 of a target wide; 800 x 480 one row, 320 x 240 two, a tiny display never negative. The gui_rules,
i18n (de and lt translations added, listed as unreviewed) and settings tests run on the CI. Not tested on a 640 x 480 screen by hand.
