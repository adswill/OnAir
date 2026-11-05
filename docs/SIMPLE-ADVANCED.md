# Simple / Advanced view per mode (3.6)

Owner request: the overall window is too busy. Every mode gets a switch between two views of the same data.

| | Simple | Advanced |
|---|---|---|
| Top of the window | spectrum and waterfall (unchanged) | spectrum and waterfall (unchanged) |
| Under it | ONE "Command" view: the mode header (lock lamp, Record state, heard, counts, SNR, frequency), a line of the mode's summary and its meters (coloured read-outs), the key controls, then the live data tables side by side | the mode's tabs as before (Live, items, Map, Receiver, Scan, History ...), all detail |
| Sub-view strip (Live / Map / Table ...) | hidden, the first view (Live) is shown | shown |

The switch is two small pills ("Simple", "Advanced") at the right end of a row above the tabs / the Command view.

## Contract

`ModeUi` (app/app.h) has two new optional members:

- `command`: `void (*)(App&)`, the body of the Command view. Use `modepanel::panes({...})` to lay out the live data tables.
- `noSimple`: a mode that keeps only its tabs.

A mode that sets nothing still gets a Simple view: the generic Command view is the summary line, the meters and the mode's own `tab` on its first view (`kit::modeNav` draws no pills while the Command view is drawn and keeps the first item). So every mode built on `ModeUi` + `kit::modeNav` has it from day one; the 22 modes that use `modeNav` need no change. A mode improves it by adding a `command`.

Layout helper (`app/ui/kit/mode_panel.h`): `modepanel::panes({{id, title, draw, weight, minHeight}, ...}, minPaneWidth)` puts titled scrolling panes side by side when each gets at least `minPaneWidth`, wraps to rows when it does not, and stacks them in a narrow window. The widths are the pure function `layoutPanes()` (`mode_panel_logic.h`, tested in `tests/app/test_mode_panel.cpp`).

Key controls that already sit in the toolbar (frequency, gain) are not repeated. Controls that belong to the mode (HFDL band and channels) go in a `kit::collapsingSection` at the top of `command`.

## Persistence and default

- Pref `uiLevel.<family>` (or `uiLevel.<family>.<sub>` for a row of a shared family), value `simple` or `advanced`.
- Default: decided once at start (`modepanel::startup`, main_start.cpp) into `setupUiLevelProfile`. A profile that already has settings keeps Advanced (the tabs it knows); a new profile (no settings at all) starts in Simple. The owner can change this default; one line in `startup`.

## Not changed

Other layouts (classic, touch, the dock faces) call `mainTabs()` too, so they get the switch with it. The side list (`list`) stays in the right panel. TV modes (DVB, ATSC, DAB, FM ...) have no `ModeUi` and keep their screens. The switch does not touch the radio or the decoder; only the drawing differs, so no mode's decoding is affected.

## Rolled out

Own `command` view (summary header, key controls, live tables side by side; Advanced unchanged): HFDL, DMR (pilots), then ADS-B, AIS, ACARS, VDL2, pagers, APRS / packet, LoRa mesh (Meshtastic, MeshCom, LoRa APRS, LoRaWAN by entry), TETRA, P25, NXDN, D-STAR, dPMR, M17, radiosondes, Inmarsat-C, Inmarsat Aero, Iridium, Orbcomm, and the picture modes (Meteor LRPT, GOES, HRPT, GK-2A, Elektro, FY-3 AHRPT / MPT, FY-4 and FY-2): picture with its controls beside the signal readout and the product list.

Not converted: the TV and radio modes without a `ModeUi` (DVB-T/T2, DVB-S/S2, DAB, FM, ATSC, ISDB-T keep their screens; they have no Simple view), DRM and Fusion (their first view is already one merged screen), and the small single-view modes, which use the generic Command view. A narrow pane wraps its toolbars (`sameLineIf`), so a mode's `command` can use any table function of its tab.

The pager keyword ticker is in the Messages view, which the Simple view shows; the voice **Watch** views (DMR, P25, NXDN, TETRA) are Advanced-only sub-views because they need a typed entry.

## The action row (3.7, feat37-ease2)

Under the Simple / Advanced switch, above the Command view and above the tabs of every mode (also the modes without a Simple view), `modeActionBar()` (`app/ui/kit/mode_actions.cpp`) draws one row:

- the **"what do I do next" line** (`nextStepLine()`, `app/ui/next_step_logic.h`): always one line in the same place. Grey while nothing needs doing, green "Receiving well.", amber with a one-click fix when a step helps. A mode whose lock the app cannot read says "Listening. This mode does not report a lock ..." and never "no signal".
- **Auto setup**: Auto gain (radios with a gain), the mode's own bandwidth and sample rate (clears a custom width / rate), and the fine frequency: for a capture of 0.6 MHz or less, the strongest peak within a tenth of the capture (at most 25 kHz) of the tuned frequency, when it stands 8 dB over the median there. A wide capture keeps its frequency and says so.
- **Reset this mode**: frequency, bandwidth and sample rate back to the mode's defaults (`ModeTuning`), custom width and chosen rate cleared. Gain, the audio and the other modes are not touched.
- both leave a one-line note and **Undo** for 12 seconds (restores frequency, rate, bandwidth, gain, Auto gain and the custom width).

The tuner / decoder / synthetic-signal controls of a mode live in the top bar and the side panels, which do not depend on the Simple / Advanced switch, so every parameter stays reachable; Advanced adds the tabs (Receiver, Stream, Scan, History, the mode's other views).
