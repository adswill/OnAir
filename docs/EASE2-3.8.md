# First-use helpers, batch 2 (3.8)

Items E16 to E30 of the ease-of-decoding list. Pure logic is in `core/src/ease` with tests in `tests/ease`; shared screens are in `app/ease`.

Rules kept: an automatic action runs at most once and is remembered, only while the user has not chosen a value, and stops when the user tunes by hand or presses Stop. Nothing here transmits.

| Item | What it does | Where |
|---|---|---|
| E16 Radiosonde | Shows the launch rule of thumb (near 11:15 and 23:15 UTC) and the time to the next window. "Start by itself at the next launch window" is off until ticked, starts once per window, never over a running receiver | `app/sonde/sonde_ui.cpp` |
| E17 Inmarsat-C, Aero | The first visit with a position picks the highest satellite and shows the pointing, with Undo. A saved choice or a frequency tuned by hand is left alone | `app/inmc`, `app/aero` |
| E18 NAVTEX | Station table: letter to name and distance, nearest stations with their next slot, station groups ordered by distance | `core/src/ease/navtex*.cpp` |
| E19 Weather fax | "Follow the schedule" tunes two minutes before each broadcast of a region or station. A chart under way is never cut off | `app/marine/marine_ui.cpp` |
| E20 DSC MF/HF | "Listen on ... kHz USB" for each working frequency a call names | `app/marine/marine_ui.cpp` |
| E21 Codec warning | One line on the mode's own screen with the install guide; Hide is remembered per mode. It uses `modeCodecMissing`, which `feat38-auto` makes real, so it shows nothing until that branch is merged | `app/ease/ease_ui.cpp` |
| E22 Regional modes | "Not used in your region, for recordings or travel" on ATSC, ATSC 3.0, ISDB-T, ISDB-S, DTMB, HD Radio, CDR, UAT, SAME, LBJ, KLW, TDCS; sorted last in their group, never hidden | `ease.cpp`, `app/ui/mode_picker.cpp` |
| E23 ISM, Sub-GHz | "Auto band": 30 s per regional band, settles where decodes arrive, samples the others. The ISM band buttons are hidden while it is on | `AutoBand` |
| E24 Repeaters | "Repeaters near me" for DMR, D-STAR, Yaesu, M17, from a CSV the user imports. See below | `core/src/ease/repeaters.cpp` |
| E25 Pagers | "Find pagers" over the known frequencies of the region; on a radio of 8 Msps or more with nothing heard for 30 s, Whole span is offered once | `app/pager/pager_ui.cpp` |
| E26 DRM | "Find DRM" visits six short wave bands with the AM census and lists DRM-shaped channels; a census row can open DRM | `app/drm/drm_ui.cpp`, `app/shortwave/am_smart_ui.cpp` |
| E27 DVB-S, DVB-C | After lock, the transponders of the network's own NIT, with a Tune button. Satellite frequencies are mapped through the LNB from the transponder being received | `core/src/ease/nit.cpp` |
| E28 Align | Level bar and peak hold for QO-100, DVB-S, GK-2A, GOES. The tone is off until ticked and is never saved | `app/ease/ease_ui.cpp` |
| E29 Empty states | UAT, DME, VDL4, NFC (NFC names the few-centimetre range) | each mode's UI |
| E30 Station | A blank callsign or locator in PSK Reporter, WSPRnet and ham setup is filled once from the others and the station position. A field the user cleared stays clear | `stationFill` |

## Data sources and what is not verified

- **Repeater lists:** nothing is bundled. Since the ISSUES3 work "Repeaters near me" can also download from Open Repeater on request (user's own API key; see docs/ISSUES3-3.8.md for the terms) next to the CSV import. RepeaterBook's terms need written permission for bulk use and redistribution, and RadioID's terms could not be read, so neither is used.
- **NAVTEX table:** from the Wikipedia "List of Navtex stations", which flags its own missing citations. Oostende and Piombino on 490 kHz are left out because no position is listed. The Cullercoats 490 kHz longitude is corrected to west.
- **E22 country lists** are from general knowledge of who uses each standard, not from a checked register. A missing country only shows the tag; the mode stays usable.
- **Sonde times** are a rule of thumb; sites differ.
- **Tested by unit tests only, on simulated inputs:** launch windows, satellite picks, NAVTEX slots and distances, schedule logic, auto band, frequency hunt, CSV import, NIT sections built byte by byte, peak hold, station fill. Every changed screen compiles, but none was run in the app, on air or with a real radio. The Find DRM result depends on the census's guess from channel shape, which was not checked against a real DRM signal, and it needs a capture of at least 500 kHz.
