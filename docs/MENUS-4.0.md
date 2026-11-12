# Menus in 4.0: Tools, Mode and View

The grouping data of the Tools menu and the Mode menu's chunking live in `app/ui/menu_groups.h` (no ImGui). `app/ui/ui2_shell.cpp` draws the
menus from it. `tests/app/test_menu_groups.cpp` (ctest `menu_groups`) checks that every item of the old flat Tools menu appears exactly once,
that no group has fewer than 2 or more than 9 items, and that the Mode menu chunks cover any category size exactly, in order, within 25 modes.
Hover a group title and its list opens beside it (arrow keys work as in every ImGui menu). Nothing was removed; every shortcut text,
tooltip and checked state is kept. Only the drop-down menus changed. Validated by syntax check and the unit test only; not yet seen on screen.

## Tools

Before: one flat list of 48 items. After: a search entry (filters across all groups), 6 items at the top level, then 9 groups.

| Where | Items |
|---|---|
| Top level | Frequencies... (Ctrl+Shift+F), Show Frequencies as a tab, Memory... (Ctrl+M), Map, Recordings..., **Plugin store...** (new: opens the plugin window on its Store tab, the marketplace) |
| Analysis and measurement | Signal Lab..., Protocol lab..., RF fingerprint, Everything in view, Insights |
| Scanning and hunting | Sweep scanner, Scanner (highly experimental), Interference hunter, Direction finder, Passive radar, Multi-radio trunking |
| Propagation and planning | Propagation..., Antenna designer..., Antenna rotator..., My station |
| Satellites and scheduling | Passes, Scheduler..., SatNOGS transmitters, Images |
| Voice, messages and archives | Transcripts ..., Voice keys..., Conversations..., Messages..., Search everything heard, Datasets..., Data packs..., Backup and restore... |
| Data feeds and network | Data feeds..., EchoLink..., Wireshark..., LAN swarm..., Integrations... |
| Automation and extending | Watchlists..., Automations..., Plugins..., Mode Builder... |
| Transmit and generate | Transmit..., TX studio..., Digital video generator..., nRF24 / ESP32 radio... |
| Calibration and hardware | Calibrate frequency..., Station health, Reception doctor |

Plugins... (the installed list) stays in Automation and extending; the Store tab is also reachable from there and from the new top-level entry.
`View > Plugins` no longer exists as such: the plugin window is opened from Tools.

## Mode (menu bar)

Before: all modes in one flat list with separators between categories. After: one submenu per category of the Mode Browser (`kGroupNames`,
rows in table order), titled `Category  (count)`, with `*` after the title when the current mode is inside. A category with more than 25 modes
gets a second level of consecutive chunks of at most 25 titled `First mode  ...  Last mode  (count)`. Decision: the mode table has no
sub-family column, so chunks follow the table order (which groups related modes). The keys 1-9 still pick the first nine listed modes in
table order. "Browse all modes..." and "Modes shown..." stay below. Categories hidden by "Modes shown" stay hidden.

## View

Before (flat, about 40 entries): Layout >, Look >, the eight arrangements, Text size >, Touch interface, Docking >, Restore the default view,
Reset layout, Reset spectrum, Full screen, eight colour schemes, Dark / Light / High contrast, Own accent colour, Font look, Time display,
Timestamps, Language >, Controls, Performance overlay, Verbose logging, Power >, Low-power drawing, Show in Discord, Show the frequency, Classic interface.
The colour schemes and Dark / Light / High contrast were two lists with one item checked in each, with no hint that they are separate settings.

After (9 entries):

| Entry | Contents |
|---|---|
| Layout > | "Screen arrangement" (Sidebar, Scope, Tiles, Faceplate, Scope: Cinema / Meters / Dock, Panel); "Panels" (Classic / Unified, Ctrl+Shift+L); Touch interface; Docking >; Classic interface |
| Theme > | "Colour scheme" (the eight palettes, one checked); "Brightness" (Dark / Light / High contrast, one checked); Own accent colour |
| Text > | Text size (slider, presets, Ctrl + / - / 0); Font look (restart note) |
| Time > | Time display group; Timestamps in logs and exports group |
| Language > | unchanged |
| Controls (keys and mouse)... | unchanged |
| Power and system > | Performance overlay, Verbose logging, Power >, Low-power drawing, Show in Discord, Show the frequency |
| Reset view > | Restore the default view (Ctrl+Shift+M), Reset layout, Reset spectrum, each with a tooltip saying what it resets |
| Full screen | F11 |

The old `Look >` submenu was a subset of the colour schemes (Classic = Instrument palette, Studio, Mono Pro, Daylight, Dev A, Dev B) and is gone;
every value stays reachable in Theme. The Discord items moved into Power and system instead of Settings, so nothing is lost from the menu.
Hover previews (feat40-uipreview, `app/ui/ui_preview.*`) did not exist on pub when this was written: the place for the hook is marked
by a comment at the top of `viewMenu()` in `ui2_shell.cpp` (call it right after each radio item's MenuItem).

Help texts elsewhere that name old menu paths (for example "View > Map", "View > Passes") were already out of date before 4.0 and are not
rewritten here; only the "View > Look" hint after the Dev A switch now says "View > Theme".

## Translations

New titles go through `tr()` / `N_()`. German entries were added to `app/i18n/de.txt` and listed in `app/i18n/unreviewed/de.txt`
(machine translated); other languages show the English text until translated.
