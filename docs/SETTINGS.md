# Settings

Menu bar > **Settings** (or Ctrl+, / Cmd+,) opens one window with the preferences a user wants to control. It does not replace the old menus; both read and write the same fields, so a change in one shows in the other.

## How it works

- Left: categories, with the number of settings that differ from the default. Top: a search box that looks across every category (words in the label, help, key and category; every word must match).
- Right: the rows of the category. A `*` before a label marks a setting that differs from its default; **Reset** sets that one back, **Reset category** the whole category. Expert knobs are under the category's **Advanced** foldout (shown inline, tagged, in a search).
- Changes apply at once and are saved with the other preferences (app/state/prefs.cpp). A row marked "restart needed" applies at the next start (Font look).
- Touch interface: larger rows. Keyboard: Ctrl+, opens it, the search box has focus, Tab walks the rows.

## Categories

| Category | What it holds |
|---|---|
| General | Language, Modes shown (link to the existing dialog), low power mode, power when hidden, reduce CPU when hidden, Discord status |
| Appearance | Colour palette, light / high contrast, own accent colour, layout, text size, font look, time display, touch interface, performance overlay |
| Spectrum and waterfall | Waterfall colour map and range, contrast curve, spectrum fill/grid/peaks/readout/line, averaging, max-hold fall, dBm |
| Radio | Spectrum click behaviour, snapping, automatic channel width |
| Audio | Video pop-out on top, subtitles |
| Recording and files | IQ recording format, data folder (open / copy path) |
| Decoding | FM mono / stereo / de-emphasis / RDS meaning / squelch defaults, teletext page, preferred TV language |
| Location | Station latitude and longitude (used by the maps, passes, ADS-B, distances) |
| Network and updates | Update check, beta versions, automatic download, update source and mirror, UDP output |
| Privacy | Exactly what leaves the computer |
| Advanced | Save log, export / import settings, reset every setting |

The table of rows is `app/ui/settings_table.h` (no ImGui, tested); the binding of each row to the field that holds it is in `app/ui/settings_window.cpp`. A row with no field behind it is not shown, and `settings: no field for <key>` is printed on stderr, so there are no dead toggles.

## Privacy

The update check, when on, sends one HTTPS request to api.github.com (or the mirror you chose) with the header `User-Agent: OnAir/<version>`. GitHub therefore sees your IP address and the app version. Sending your station to the map server and the Discord status are opt-in and off until you turn them on.

## Export and import

Advanced > Export writes `onair-settings-export.txt` in the data folder: one `key=value` per line. Import reads it back; unknown keys and bad numbers are ignored and numbers are clamped to their limits.

## Not done yet (asked for, no code behind it, so not shown)

Startup mode (last / ask / specific), default device and sample-rate defaults, gain strategy, PPM default, bias-T default, direct sampling, USB buffers, thread count, audio output device and latency, mute on start, filename template, auto-delete after N days, disk-space guard, MQTT / remote API port and auth, map layers and trails, quiet hours, keyboard rebinding (View > Controls has it), a verbose-logging switch. They need new fields wired into the engine; add the row to the table and a binding when the field exists.

## Tests

`test_settings`: unique keys, defaults inside limits, search across categories, reset, changed count, export / import round trip, tolerant import.
