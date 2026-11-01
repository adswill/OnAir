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

See the tail of docs/PATHWAY.md: pilots HFDL and DMR with their own `command`; the other modes use the generic view and are converted one family per commit.
