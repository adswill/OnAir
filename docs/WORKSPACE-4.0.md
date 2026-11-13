# Workspace layout (4.0)

**View > Layout > Workspace** turns the main window into a blank canvas you fill with blocks, in the spirit of SDRangel's windows. Each block is
one thing: a radio source, its spectrum, a demodulator, the audio output, a decoder, the map, the satellite tracker, the rotator and so on.
Classic and Unified stay as they were and remain the default; choosing either of them (or **View > Restore the default view**, Ctrl+Shift+M)
leaves the Workspace layout. It needs the new interface and is not used with the touch interface.

## Using it

- **Add block** (top left), or a right click on the empty canvas, lists the block types by kind. A right click puts the new block where you
  clicked.
- A block is a window: drag its title to move it, a corner or edge to resize it, the arrow to collapse it, the cross to close (remove) it.
  The cog at the right of its first row opens its settings: **Tab with ...**, **Take out of the tabs**, **Remove block**, and for a decoder
  the mode's own decoder settings.
- **Snap** (on by default): a moved or resized block settles against the edges of the blocks near it (10 points) or else on the grid
  (**grid**, 16 points; 0 for none).
- **Tabs**: drag a block by its title onto another block's title (it lights up) and let go: the two become tabs of one window. Right click a
  tab to take it out again. Tool windows (map, tracker, rotator, scanner, frequency list, bookmarks) cannot be tabs (they draw their own frame).
- **Sources and links**: every block that needs a radio has a picker in its first row (which Radio source it uses). A new block takes the
  last source added. **Links** draws a thin line from each source to the blocks that use it.
- **Workspaces**: the list next to Add block holds the saved workspaces; pick one to switch, or **New from a template**. **Save as...** saves a
  copy under a new name or renames the current one; **Delete** removes it; **Export...** writes the current workspace to a file
  (`.onairws`, plain text) and **Import...** adds the workspaces of such a file. Everything (blocks, positions, sizes, collapsed state,
  tabs, links, each block's settings, the grid) is saved by itself, a second after a change, in `<data folder>/workspaces-blocks.txt`.
  The workspace shown is remembered (`canvasCur`), and whether the Workspace layout is on (`canvasOn`).
- File > Workspaces is a different thing (a named set of one receiver's settings and open windows); it works as before.

## Templates

| Template | Blocks |
|---|---|
| Blank | nothing |
| Listener | Radio source, Spectrum and waterfall, Demodulator, Audio output |
| Aircraft | Radio source, Decoder (ADS-B), Map, Spectrum and waterfall |
| Satellite station | Radio source, Decoder (NOAA APT; pick another in the block), Satellite tracker, Antenna rotator, Spectrum and waterfall |

## Blocks

| Block | What it draws | Source |
|---|---|---|
| Radio source | Start / Stop, the frequency digits, the radio, sample rate, tuner and gain rows of the toolbar (`toolbarParts`), the source options of a file / test signal / sound card, the status line | is one: one block per receiver |
| Spectrum and waterfall | `spectrumWaterfall()` with its draggable split (kept in the block) | yes |
| Demodulator | One Multi-VFO channel (docs/MULTI-VFO.md) on the source: on, demodulator, offset in kHz from the source's frequency, MHz, bandwidth, squelch, volume, pan, mute, level | yes |
| Audio output | Hear this source (one receiver is heard at a time), the volume row with the audio control (output device, AGC ...), the monitor and its outputs (the Listen group), and the source's demodulator blocks mixed in (on, volume, mute) | yes |
| Decoder | The mode list (any mode of the mode table), Pause, and the mode's results views (`mainTabs()`); the cog has the mode's decoder settings | yes |
| Transmitter | The guided TX panel (`txPanel()`) with its usual warnings; nothing is blocked | yes |
| Recorder | IQ and audio recording (the Record group of the Source panel) | yes |
| Log | The receiver's log | yes |
| History | The decoded history (`historyLogTab()`) | yes |
| Map | The Map window (aircraft, ships, sondes, satellites, stations) | tool window |
| Satellite tracker | The Passes window (passes, sky plot, Doppler, follow) | tool window |
| Antenna rotator | The rotator window: GS-232, Hamlib rotctld, ASCOM Alpaca, calibration, limits, pass tracking | tool window |
| Scanner | The scanner window | tool window |
| Frequency list | The Frequencies window | tool window |
| Bookmarks | The Memory window | tool window |
| Clock | UTC (large) and local time | none |
| Notes | Free text, saved with the workspace | none |

## Several radios

Each Radio source block drives one receiver (RX 1 ... RX 4, the receivers of the RX 1 / + strip, docs/multi-sdr.md). The first takes RX 1;
each further one takes a receiver no block uses, or opens a new one. Removing a Radio source block closes its receiver (RX 1 stays open).
Blocks drawn with another source than the one selected in the strip work as usual; their receivers keep running in the background as before.
A source block keeps its radio, mode, frequency, sample rate and gains in the workspace (the same fields as File > Workspaces); they are applied
when the workspace is loaded and the receiver is stopped.

## Decoders and modes

A receiver runs one mode. The first decoder block on a source that is not paused chooses that source's mode; changing the mode in the block
(or anywhere else) is followed by the block. A second decoder block on the same source says so and draws nothing: add another Radio source for
a second mode. When a source has no running decoder block any more (the block is paused, removed or moved to another source) the source switches
to the General receiver at the same frequency: its decoder stops, the spectrum, waterfall and demodulator blocks go on. Loading a workspace
with no decoder on a source leaves the source's mode alone.

## Performance

A collapsed block and a tab that is not shown draw nothing of their body. Decoders run only while a decoder block is attached to the source and
not paused (above). Nothing else is added per frame: the canvas reads the windows' positions and draws the links.

## Limits

- No panning or zooming of the canvas: blocks live inside the OnAir window (as with docking, no separate operating-system windows).
- Tool windows exist once each (one Map block, one Tracker block ...) and show the receiver selected in the RX strip; their close button
  removes the block. The rotator window sizes itself, so only its position is kept.
- Two blocks that draw the same panel for two receivers at once share some of that panel's view state (zoom of the plots, the last tab):
  the same limit as switching receivers in the strip.
- Snapping and tab docking are by mouse; there are no keyboard commands for them.
- Tested: the model (tests/app/test_canvas.cpp, `ctest -R canvas`): add / remove / link, a block whose source was removed, tab groups, snapping,
  receiver choice, the decoder mode steps, the file round trip with odd text, decimal commas, CR LF, unknown kinds and broken links, several
  workspaces in one file, unique names, templates. The drawing code was compiled on GitHub; it was not run on a screen here (the laptop was on
  battery).

## Code

`app/canvas/canvas_model.h/.cpp` (the model, no ImGui), `app/canvas/canvas_ui.cpp` (drawing), `app/canvas/canvas.h` (the entry points). Shared
files touched: `ui2.cpp` (the body is the canvas when it is on), `ui2_shell.cpp` (View > Layout entry, Restore the default view),
`ui2_dock.cpp` (docking is off while the canvas is on), `diag_ui.cpp` (the Record and Listen groups became `recordGroupUi()` / `listenGroupUi()`),
`main_start.cpp` (`--workspace 0..3 | file`: start on a template or a file, for screenshots; not saved).
