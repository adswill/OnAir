# Touch interface on narrow screens (#139)

Under 800 px of display-scale width (640 x 480 and similar) the touch shell now:

- moves Gain and Volume from the dock into the More sheet, so Start / Stop, the frequency, Tuner and More share the dock and are wider (the dock height and the choice of one or two rows are as in docs/TOUCH-SIZE-4.1.md);
- drops the Running / Stopped word from the top bar (the Start / Stop button says it) and lets the mode chip take the rest of the bar;
- on a narrow portrait screen puts the status line above the tabs behind a switch in More (`Show the status line above the tabs`, key `touchStatusLine`, off until chosen); the same status is always in More > Status.

800 px and wider, and desktop-size displays, are laid out as before. `shellLayout()` in `app/ui/touch_logic.h` decides it (`narrow`, `secondaryInMore`, `statusFolded`, `topStateText`); tests in `tests/app/test_touch.cpp` (`touch_logic`). Nothing here was compiled into the app or looked at on a screen: syntax check only; the touch size setting (Small, 34 px targets) from 4.1 is still the way to shrink the buttons.
