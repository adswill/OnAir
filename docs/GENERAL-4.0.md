# General receiver 4.0: the new screen

The General receiver (family 55) has its own screen in 4.0. The spectrum and the waterfall fill almost the whole window. Every control sits
in one sidebar on the left, in folding sections that are all on screen at once, with no tabs. The layout follows the idea SDR++ made popular:
one menu on the left and the plots everywhere else. The code and artwork are OnAir's own.

Code: `app/general/general_screen.cpp` (the screen), `app/general/general_ui.cpp` (the receiver's controls and the VFO),
`app/general/general_layout.h` (pure logic, tested), `app/general/general_panels.h` (the interfaces between them),
`app/gfx/view_span.h` (the zoom rule), `app/ui/freq_digits.h` (the readout digits). Tests: `tests/general40/test_general_layout.cpp`
(111 checks) and `tests/general/test_general_rx.cpp` (the new demodulators).

## Before (3.9)

- In the default Sidebar layout the General receiver shared the window with the app sidebar (Source, Gain, Tuner, Mode, Display, Tools,
  Log), a status pane, the right-hand CHANNEL list and the analysis row at the bottom. The spectrum was its own **Overview** tab.
- The demodulators and all the other settings were in a second tab, **Listen**. It held long rows of controls and a second, smaller
  spectrum. The rows wrapped in a narrow window and were cramped.
- The Receiver details, History, Memory, Frequencies and a second mode were further tabs.

## After (4.0)

```
+----------------------------------------------------------------------------------+
| menu bar                                                                          |
| [>] 0000145.500.000 Hz [NFM 12.5 k]  Modes  summary        ADC level   volume     |  shared top bar
+---------------------+------------------------------------------------------------+
| > Status            | [=] step < 12.5 kHz > [x] Snap  Centre  |||||||| SNR 23 dB  |  General bar
| v Source            | +--------------------------------------------------------+ |
|   radio, rate, gain |  spectrum (frequency axis, dB axis, VFO band)            | |
|   tuner             |------------------ drag to resize ------------------------ |
| v Radio             |  waterfall                                               | |
|   NFM WFM AM SAM DSB|                                                          | |
|   USB LSB CW CW-L RAW|                                                         | |
|   Auto, bandwidth,  |                                                          | |
|   squelch, AGC ...  | +--------------------------------------------------------+ |
| v Audio             | > MORE  receiver details, history, memories, second mode   |  drawer (folded)
| > Display ...       |                                                             |
+---------------------+------------------------------------------------------------+
| status strip                                                                      |
+----------------------------------------------------------------------------------+
```

- **Spectrum and waterfall**: the shared plot, as large as the window allows. Drag the bar between them to resize. The split is saved.
  The wheel zooms around the mouse and a drag pans, as View > Controls sets it. The frequency axis and the dB axis are always drawn.
  The Zoom / Max / Min sliders are on the right, and the corner buttons (Spectrum, Waterfall, labels, Signals, the tuning-style toggle)
  hold every display setting.
- **Sidebar**: drag its edge to resize it (double click for the default width). The button at the left of the General bar or
  **Ctrl+B** (Cmd+B on a Mac) hides and shows it. When the window leaves less than 360 points for the plots, the sidebar hides itself;
  the button then shows it anyway, narrower. Which sections are open is saved.
- **Drawer**: under the waterfall. It is folded by default. Opened, it has the Receiver, History, Memory, Frequencies and second-mode
  tabs, with the mode action bar (next step, Auto setup, Reset this mode) above them. Drag the bar above it to resize it.
- **General bar**: the step arrows and the step size, **Snap**, **Centre**, and a compact channel level meter with the SNR and the
  squelch state.
- **Top bar**: the demodulator and its filter as a button right of the frequency digits (for example `NFM 12.5 k`). A click opens the
  demodulator grid and the filter presets. The frequency digits are the shared SDR++ style readout: scroll a digit, click its upper or
  lower half, type a number over it, or double click to type the whole frequency.

Settings: the layout is saved in the preference `gen40Layout`, for example `v=1;w=300;o=1;f=0.420;d=0;dh=240;s=f`. Unknown keys are skipped,
so later versions can add keys. Display > **4.0 General layout** turns the screen off and goes back to the 3.9 layout, which keeps
working. Its Listen tab has a **Use the 4.0 layout** button. The screen is used in the new interface's Sidebar and Scope layouts.
The faceplate, tiles, docking, classic and touch interfaces keep their own layouts.

## The sidebar sections

| Section | What is in it (all of it was in 3.9 too, unless marked new) |
|---|---|
| Status | the status lines (RX lamp, summary, network-radio sample losses, auto-detect, transcripts) |
| Source | radio, sample rate, source options (file, synthetic, sound card), gain, tuner, radio settings |
| Radio | demodulator grid (new: DSB, RAW, CW-L, SAM as a button), Auto from the band plan (new, off), filter presets and a bandwidth slider (new), squelch Off / Level / Auto with Set and the meter, AGC or fixed gain, FM de-emphasis, CW pitch, AM Auto quality, its noise reduction, enhanced treble, the AM detector, the noise blanker and its level |
| Audio | volume, mute, the speaker settings (output device), audio clean-up and voice inversion buttons, high-pass / low-pass / notch filters, the neural noise remover (NFM, WFM, AM) |
| Display | peak hold, spectrum and waterfall dB ranges with Auto, layout, look, reset, palette, tuner style; FFT size, averaging and waterfall speed (new here; they were only in the corner popups), the 4.0 layout switch |
| Recording | record WAV / FLAC / MP3, the recordings folder, one file per transmission and its count, the REC time and path, errors, pre-roll, hang, channel IQ |
| Bookmarks | Memory and Frequencies windows, ham voice segments by IARU region (with the region under Advanced), the AM broadcast bands and MW presets |
| Signals | What is this signal? (bandwidth, shape, ranked guesses with Use), the signals spotted automatically (Spot signals, Auto-decode, Decode / Ignore, Advanced) |
| Channels | more channels from the same capture: add / remove, each one's demodulator, level, Listen (swap) and per-transmission recording |
| Mode | the mode list (to leave General), the decoder options when the mode has them |
| Tools | the tool windows and the look, accent, typography, data folder, time display, text size, touch interface |
| Log | verbose, save, the log lines |

## Choosing the demodulator

1. **Sidebar, Radio**: a grid of buttons with the one in use filled: NFM, WFM, AM, SAM, DSB, USB, LSB, CW, CW-L, RAW. SAM is AM with
   the synchronous detector. It is the same setting as Synchronous in the AM detector list.
2. **Top bar**: the button by the frequency names the demodulator and the filter. A click opens the same grid and the filter presets.
3. **Right click on the VFO band** in the spectrum or the waterfall opens a small menu with the same grid, the presets, the bandwidth slider
   and Auto. A right click elsewhere still opens the spectrum's own menu (Identify, markers).
4. **Keyboard** (not while typing). Alt is Option on a Mac:

   | Key | Demodulator | Key | Demodulator |
   |---|---|---|---|
   | Alt+N | NFM | Alt+U | USB |
   | Alt+W | WFM | Alt+L | LSB |
   | Alt+A | AM | Alt+C | CW |
   | Alt+S | SAM | Alt+X | CW-L |
   | Alt+D | DSB | Alt+R | RAW |

   Ctrl+B (Cmd+B) shows or hides the sidebar. These keys were not used before. Alt+M (mute) and Alt+Up / Alt+Down (volume) are left
   as they were, and so are 1-9, Space, M, F, I and C. The list is also in the tooltip of the demodulator button and in the "(?) shortcuts"
   line of the Radio section.
5. **Auto from the band plan** is off by default. When it is on and the frequency moves into another band, the demodulator and the filter
   are set for that band. The band comes from the active band plan, or from built-in ranges when the plan has nothing there:
   - FM broadcast: WFM 180 kHz.
   - AM broadcast, MW, LW and the shortwave broadcast bands: AM.
   - Airband: AM, 6 kHz above 118 MHz.
   - PMR and marine: NFM.
   - Amateur bands, by segment: CW in the CW sub-bands; voice is LSB below 10 MHz and USB above it (60 m is USB); FM in the FM parts of 10 m,
     6 m, 2 m and 70 cm; otherwise USB.

   Inside one band nothing is changed again, so a demodulator you pick yourself stays. TV, DAB, ISM and unknown ranges are left alone.
   The rules are in `general_layout.h` (`autoPick`) and are tested.
6. **Each demodulator remembers** its own bandwidth and step (as in 3.9) and, new in 4.0, its own squelch (off / level / auto and the
   level), AGC and fixed gain. Switching back restores them. A profile from before 4.0 has only the shared keys (`genSqOn`, `genAgc` ...).
   Every demodulator starts from those, and the shared keys are still written, so 3.9 reads its settings back.

There is no FM stereo here. WFM in the General receiver is mono, as before. The FM broadcast mode has stereo and RDS.

### New demodulators (core)

`GenDsb`, `GenRaw` and `GenCwl` are added after `GenCw`. The first six keep their numbers, so saved settings, memories, the remote API and
the web and Android clients see no change.

- **DSB**: both sidebands, no carrier. The product detector sits at the dial. It has no carrier recovery, so tune exactly onto the missing
  carrier (switch Snap off or use a fine step).
- **RAW**: no demodulation. The channel's in-phase part is the audio, and recordings save the channel IQ by default (Channel IQ).
- **CW-L**: CW with the beat note on the other side of the carrier. The note falls as the signal rises, which moves a neighbour out of the way.

rigctl: `CWR` selects CW-L, `DSB` selects DSB, and CW-L is reported as `CWR`.

## The VFO

- It is drawn on both plots as a shaded passband with a centre line, as before.
- **Drag its body** (inside the band, or its centre line for USB / LSB) to tune. The VFO keeps the point where you grabbed it. Earlier
  a press jumped the centre to the mouse.
- **Drag an edge** to change the filter. USB and LSB have only the outer edge.
- **Click elsewhere** to tune there.
- **The wheel over the band** steps the VFO by the step. A band wider than 40 % of the plot leaves the wheel to the zoom.
- **Right click** opens the demodulator menu.
- **Snap** (General bar, on by default): clicks and drags land on a multiple of the step. Off, they land exactly where the mouse is.
  The step arrows and the wheel always step from the nearest step.
- A band narrower than 2.5 handle widths on screen can only be moved: zoom in to drag its edges, or use the bandwidth slider.

## Zoom (all modes)

- The **Zoom slider** and the **zoom keys** (View > Controls) zoom around the **tuned frequency** (the VFO, or the channel being
  listened to) and keep it in the middle of the view.
- Only the view moves: the radio's centre frequency, and so the hardware, stays where it is.
- Near the edge of the captured band the view stops at the edge instead of showing empty space. A tuned frequency outside the capture
  gives the nearest edge.
- The **mouse wheel over the plots** still zooms around the mouse pointer, as before.

Rule: `viewspan::zoomAround()` in `app/gfx/view_span.h`, tested in `general_layout`.

## The tuning-style toggle (all modes)

The two-part switch at the top right of the spectrum (a two-way arrow and a target) is now **one** button. Its icon shows the style in use,
and it is filled with the accent colour for centre tuning. A press switches to the other style. The tooltip names the current style and
what a press does.

## Small windows, high DPI, themes

- Every size is in points times the text scale (`gUi`), so high DPI and the text size setting scale the screen.
- The sidebar hides itself below about 360 + 210 points of width. The sidebar controls take the column's width and wrap (`flowNext`).
- The colours are the theme's (`kRail`, `kEdge`, the accent), so Dev A / B, Studio, Mono Pro, Daylight and the light skin apply.
  The compact and clean typography options only change the fonts, which the layout measures.

## What was verified

- `general_layout`: 111 checks. They cover layout text round trips, old, damaged and foreign text, clamping, the sidebar width at 1600,
  640, 500 and 400 points, the toggle, the VFO hit test (edges, body, the USB / LSB centre line, tiny bands), drag and snap (12.5 kHz,
  8.33 kHz, 9 kHz at LW, 6 GHz), edge widths, wheel steps, the readout digits (places, typing, a trackpad's fractions, carry, limits),
  the Auto rules and the zoom rule (centred, repeated steps, edges, outside the band, NaN). It was built and run locally with clang.
- The DSB / RAW / CW-L tests in `test_general_rx` and the app build: GitHub (see the pathway entry for the run).
- All of it is simulated: no live signal was received through the new screen.
