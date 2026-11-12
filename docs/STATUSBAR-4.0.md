# Status bar, top bar and menu bar items (4.0)

Owner orders (2026-11-12): make the bottom status strip "look a lot less AI, more professional, devvy" (job C); use the empty space in the
top bar and pack the rows above every mode's tabs (job D); give the menu bar's right-hand items a gap above and below and a Discord button
of the same height (job E). Branch `feat40-trackui`. Screenshots are owed: the Mac was on battery, so no local app build; they are to be
taken on mains with a renamed app copy and isolated settings.

## C. The bottom status bar

Code: `app/ui/ui2_shell.cpp` (`statusLine`, `statusStripBlockHeight`), `app/ui/status_strip.h` (pure helpers, test
`tests/app/test_status_strip.cpp`, ctest `status_strip`), the receiver tabs in `app/state/receivers.cpp` (`strip`).

### Before (3.9)

- Five rounded cards with a border, each with a spaced-out upper-case caption (FREQUENCY, MODE, RECEIVER, STATE, CPU) above a large value
  (1.3 x the text size, bold monospaced). The frequency to the kilohertz, the first three digits in the accent colour.
- CPU: a 44 px bar, the percentage and a 34 px sparkline.
- After the cards, on the text line: the signal indicator, the "too slow" fix, `x.xxx Msps`, `level x dBFS`, `load x%`, `dropped ...`
  and the last log line, separated by rules. The mute button at the right edge.
- On a narrow window the cells collapsed in a fixed order: CPU caption, receiver, all captions, CPU, state, mode.
- Receiver tabs below: rounded chips with a border, the selected one filled.

### After (4.0)

- One thin row, the height of a frame (follows the text size and the compact typography). No cards, no captions, no oversized fonts.
- Left to right: state as a small coloured dot and a word (Stopped, Channel scan, Running, Searching, Locked: the last two from the mode's own
  summary line, as the top bar colours it), the frequency to the hertz in groups of three (`522.000 000 MHz`, `1 090.000 000 MHz`),
  `mode DVB`, `RX 1`; while running the signal indicator, `rate 10.000 Msps`, `level -32.1 dBFS`, `load 41%`, `dropped ...`; then `cpu 9%`
  with a one-line sparkline. Labels in the muted text colour and the text font, values in the monospaced font at normal weight, a 1 px rule
  in the edge colour between segments.
- Right side: the "too slow" fix while it applies, the last log line (cut with "..." to fit), the clock in the zone chosen in Settings (UTC or
  local), the mute button.
- Narrow windows: segments drop out from the right by priority (`statusstrip::fit`): state and frequency never; then receiver first, level and
  load, rate, signal, CPU, dropped samples, mode last. The clock goes under 520 px.
- Behaviour kept: the segments had no click actions before and have none now; the CPU tooltip, the load / dropped tooltips (`lossTooltip`),
  the signal tooltip and the mute click are unchanged; the mode segment's tooltip is the mode's one-line description, the state's says what
  it means. The "too slow" warning and its Fix / Undo buttons and the log line are the same calls as before.
- Receiver tabs: flat, no frames; the selected tab on the frame colour with a 2 px accent line on top, hover a faint fill, a thin rule
  between tabs; the row is 4 px thinner. The run dot, speaker, close button, right-click menu, Ctrl+1..9 and the + button are unchanged.

Tests: `groupedMhz` (grouping on both sides of the dot, clamping) and `fit` (wide shows all, priority-0 segments never dropped, monotonic
as the window narrows, never drops a more important segment before a less important one, ties drop from the right). The old cell-collapse
`plan` was removed with its test, since nothing else used it.

## D. The top bar and the rows above every mode's tabs

Code: `app/ui/ui2_shell.cpp` (`topBar`, `quickStrip`), `app/ui/kit/mode_actions.cpp` (`modeActionBar`), `app/ui/kit/mode_panel.cpp`
(`levelBar`, `levelBarWidth`, `commandView`), `app/ui/main_tabs.cpp`, `app/panels/diag_ui.cpp` (`nextStepLine`, `recordCompact`),
`app/ui/mode_hints.cpp` (`modeHintsLine`, `modeAntennaHint`). The General receiver's own top bar (`app/general/*`) is not touched.

### Before (3.9)

- Top bar: run button, frequency digits, Modes button, the mode name with its state under it, then an empty gap up to the ADC level meter
  and the volume.
- Above the tabs of every mode, stacked, each on its own row with the right side empty: Simple / Advanced (right-aligned, modes with a
  Simple view), the next-step line (e.g. "Stopped. Press Start to listen." in amber), its fix button (Start or Auto gain), the hints (first
  start, next pass, satellite, navigation), the antenna hint ("Antenna: a vertical or dipole, quarter wave 14 cm."), then Auto setup and
  Reset this mode with the Undo note. Four or five rows in every mode.

### After (4.0)

- Top bar: the gap now holds flat segments, right-aligned against the meter, in the status bar's style: `rec` (a red dot; while recording
  a square, `m:ss` and the size, click to stop), `Memory` (the quick list: favourites, the ten most recent, Store current..., the Memory
  panel), `offset +12.500 kHz` (listening frequency against the radio centre), `bw 8.000 MHz` (the mode's channel bandwidth). They drop
  from the right by priority when the window narrows (bw, offset, Memory, rec last) and never overlap the mode summary. Nothing was
  removed from its old place: Record IQ (with its format menu) stays in the side panel, the Memory menu in the menu bar; the sample rate
  is in the status bar.
- Above the tabs: one row. Left: a coloured dot, the next-step line on one line (cut with "..." to fit; the whole text in the tooltip) and
  its fix button (Start, Auto gain) right after it. Middle: the antenna hint as muted text, cut to fit or shortened to "Antenna" when room
  is short, the whole hint in the tooltip. Right: the Undo note and Undo (for 12 s after Auto setup or Reset), Auto setup, Reset this mode,
  then Simple / Advanced. When the pane is narrower than the next step needs (220 px) plus the rest, the next step takes the first row and
  the antenna hint and buttons a second row. The other hints keep their own rows and appear only when they have something to say.
- The Simple view (Command view) uses the same row, so Simple / Advanced moved into it there too; nothing is drawn twice.

Long texts checked by reading the code paths, not on screen yet: "Listening. This mode does not report a lock: watch the list for
decoded items." is cut to the space left of the antenna hint and buttons and shown whole in the tooltip.
