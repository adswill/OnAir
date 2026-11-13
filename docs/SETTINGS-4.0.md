# Settings in 4.0

Menu bar > **Settings** (Ctrl+, by default). 4.0 adds a keyboard and mouse editor, five new pages, profiles, a search that highlights what it found
and leads into the key editor, and about 70 new settings. Every new row is wired to the code that uses it: the window shows a row only when a
field or a preferences key is bound to it (`binds()` in `app/ui/settings_window.cpp`; rows without one are listed on stderr), so there are no dead switches.

Files: `app/ui/settings_table.h` (the rows), `app/ui/settings_logic.h` (search, highlight, reset, export rules), `app/ui/settings_window.cpp`
(the window and the bindings), `app/ui/settings_apply.cpp` (what the settings do outside the window), `app/ui/settings_profile.h` (profiles),
`app/ui/input_map.h` / `.cpp` (the action registry and the key editor), `app/ui/notify_logic.h` (quiet hours), `app/adsb/tile_url.h` (tile server).

## What is new

- **Pages:** General, Appearance, **Units**, Spectrum and waterfall, Radio, Audio, Recording and files, Decoding, Location, **Keyboard and mouse**,
  **Notifications**, Network and updates, Privacy, **Performance**, Plug-ins and scripts, **Profiles**, Advanced. Power, GPU and the performance overlay
  moved to Performance, time display to Units; their keys did not change.
- **Search:** filters across every page; the matched words are marked in the label (accent colour on a tint). When keyboard actions match too, the
  result line says how many and **Show them** opens the key editor with the same search.
- **Reset this page** (was Reset category) and **Reset every setting** (Advanced) now also resets the keys.
- **Profiles** (Profiles page): save every setting of the window plus all key bindings under a name (`<data folder>/profiles/<name>.onairprofile`),
  switch to one, delete one, export one to a file, import a file as a profile. An older "Export settings" file imports as a profile too. The
  current profile's name is shown (prefs `settingsProfile`). Custom rows (start-up mode, output device, the dialogs behind links) are not in a profile.
- **Compatibility:** every 3.9 key is unchanged; an older settings file loads and the new rows keep their defaults (tested). The saved keys
  (`ctlKeys`) keep their 3.9 format; second keys are added as `name.2=`. Nothing was renamed, so no migration was needed.

## Keyboard and mouse

One action registry (`inputmap::Action`, `defs()`): every shortcut that was written into the code in 3.9 is an action with a saved name, a group,
a scope and up to two keys. The code asks `inputmap::pressed(Action)` instead of testing keys.

- **Editor** (Settings > Keyboard and mouse, also View > Controls): Set / Set second record a key or the middle / side mouse button (press-to-record,
  Esc cancels), Clear, Default per action, Reset everything (asks first). A search box filters by action, group or key text.
- **Conflicts:** two actions that would fire on the same key press are marked orange with a tooltip naming the other; a warning line counts them,
  and recording a key that clashes says so at once. Scopes: Global, Plot (mouse over the spectrum or waterfall), TV picture, FT8 screen. Global and
  Plot clash; a panel scope does not clash with Global: while the TV picture or the FT8 screen is shown its keys win (the tooltip says which).
  This also fixes two 3.9 double fires: F6 in the FT8 screen toggled the unified side column as well as calling CQ, and Ctrl+Shift+L also opened
  the FT8 log's add dialog (Ctrl+L matched with any modifiers).
- **Refused keys:** left and right mouse buttons, Esc (closes dialogs, stops the transmitter), Tab, a modifier alone.
- **Menus and tooltips** show the user's keys (`inputmap::shortcut()`, `inputmap::tip()`), with Cmd / Option names on a Mac.
- **Import / export:** a readable text file, one action per line (`fullscreen = F11, Alt+Enter`), plus the wheel rows, step and notches.
  Import starts from the defaults, so a file with a few lines changes only those; lines it cannot read are reported.
- **Mouse:** the wheel table per area (spectrum, waterfall) and modifier (zoom both / frequency / level, tune, pan, scroll back, nothing), the tuning
  step and notches per step (from 3.9), the middle and side buttons as bindable keys.
- Not in the registry (fixed on purpose): Esc everywhere, the transmitter's Space (push to talk) and Esc, dialog Enter / Esc, list navigation keys,
  the dock's Ctrl+Alt+arrow commands, the FT8 log's Ctrl+Z / Ctrl+Y, the 3D waterfall's keys.

## Settings that do something outside the window (settings_apply.cpp and friends)

| Setting | Where it acts |
|---|---|
| Start-up mode / frequency | `settingsStartup()` after the preferences, `settingsFrame()` on frame 2 (a plain start only; start flags win) |
| Start the receiver when OnAir starts | `settingsFrame()` at frame 30, the last radio, only when it is a radio |
| After a crash, restore the last session | `workspaceSessionStart()` is not offered (Never) or answered at once (Always) |
| Confirm before quitting | the window close is held back and a dialog asks (button, Alt+F4, File > Exit) |
| IQ file name | `dect2::setRecordingNamePattern()` (core/src/source/iq_record.cpp); empty keeps the 3.9 name |
| Split IQ recordings / stop below free space | `settingsFrame()` every 2 s: a new part at the size; stop with a log line and a banner when the disk is short |
| Output device, AGC and squelch for new modes | `app/panels/audio_control_ui.cpp`: the device of modes without their own; the first-use defaults |
| Notifications | `app/panels/automation_sink.cpp`: system notification, banner and its time, sounds, quiet hours |
| Map tile server | `app/adsb/adsb_map.cpp`: the template, a cache folder per server (OpenStreetMap keeps `maps/osm`) |
| Default range, range keys step | `app/gfx/plots.cpp` (Reset, the range keys) and `app/state/prefs.cpp` (a bad saved range) |
| Aircraft units | the ADS-B screen follows at once (`adsbui::unitsFromPrefs()`); HFDL reads the key each time |

## Decisions (owner asleep)

- **Privacy defaults:** the update check and the online map stay on by default as in 3.9; switching them off for everyone would blank the maps of
  every existing install. Everything else that sends data is off by default (Discord, map server, feeds). Owner's call if this should change.
- Not added in this pass (each needs new code behind it first, and a row only exists when the code reads it): decoder thread count, GPU device
  choice, FFT window and overlap, a frequency digit-grouping option (feat40-general is changing the frequency display), temperature units, a proxy.
- The 3.9 key `unitsImperial` (written by the setup wizard) is read by nothing; the Units page uses the keys the aircraft screens read (`adsbUnits`,
  `adsbSpeedUnit`).

## Tests

- `tests/app/test_input_map.cpp`: defaults equal the 3.9 keys, no default conflicts, scopes and shadowing, conflicts through second keys,
  `assign()` refusals and clash reports, the Cmd and Shift rules, persistence round trip with second keys, a 3.9 saved text, the export file
  round trip of every action, import garbage, the spec parser, menu and tooltip texts, the editor search.
- `tests/app/test_settings.cpp`: the new pages and searches, the 3.9 keys kept, float choices, an older export file, search highlight spans,
  profile compose / split / file names / round trip, the tile server template and cache name, quiet hours.
- `tests/source/test_iq_record.cpp`: the file name pattern (tokens, UTC, unsafe characters, the default, read-back of rate and format).
- German texts for the new strings in `app/i18n/de.txt` (machine translation, listed in `app/i18n/unreviewed/de.txt`).
- Built and run on GitHub (quick.yml) only; the GUI itself was not run (Mac on battery).

## All settings

### General

| Key | Setting | Kind | Default | 4.0 |
|---|---|---|---|---|
| `uiLang` | Language | Custom |  |  |
| `easyMode` | Easy mode | Custom |  |  |
| `regionPreset` | Region preset | Custom |  |  |
| `setupRun` | First-run setup | Link |  |  |
| `modeVisLink` | Modes shown | Link |  |  |
| `startMode` | Start-up mode | Custom |  | new |
| `startFreqMode` | Start-up frequency | Choice | The last frequency | new |
| `startFreqMhz` | Fixed start-up frequency (MHz) | Float | 100 | new |
| `autoStartRx` | Start the receiver when OnAir starts | Bool | off | new |
| `sessionRestore` | After a crash, restore the last session | Choice | Ask | new |
| `confirmExit` | Confirm before quitting | Choice | Never | new |
| `discordOn` | Show in Discord | Bool | off |  |
| `discordFreq` | Show the frequency in Discord | Bool | on |  |

### Appearance

| Key | Setting | Kind | Default | 4.0 |
|---|---|---|---|---|
| `uiTheme` | Colour palette | Choice | Dev A (graphite) |  |
| `themeMode` | Light or high contrast | Custom |  |  |
| `accent` | Own accent colour | Custom |  |  |
| `uiVariant` | Layout of the interface | Choice | Sidebar |  |
| `layoutUnified` | Unified layout | Bool | off |  |
| `uiTextScale` | Text size | Float | 1 |  |
| `uiTypography` | Font look | Custom |  |  |
| `restoreView` | Restore the default view | Custom |  |  |
| `touchUi` | Touch interface | Bool | off |  |
| `touchScale` | Touch text size | Float | 1.35 |  |

### Units

| Key | Setting | Kind | Default | 4.0 |
|---|---|---|---|---|
| `timeDisplay` | Time display | Custom |  |  |
| `adsbUnits` | Aircraft units | Choice | Aviation (kt, ft, nm) | new |
| `adsbSpeedUnit` | Metric speed | Choice | km/h | new |

### Spectrum and waterfall

| Key | Setting | Kind | Default | 4.0 |
|---|---|---|---|---|
| `wfMap3` | Waterfall colours | Choice | Navy heat |  |
| `wfAutoFloor` | Waterfall range follows the noise floor | Bool | on |  |
| `wfGamma` | Waterfall contrast curve | Float | 1 |  |
| `wfSat` | Waterfall colour intensity | Float | 1 |  |
| `wfBright` | Waterfall brightness | Float | 0 |  |
| `wfContrast` | Waterfall contrast | Float | 1 |  |
| `wfClock` | Clock time on the waterfall | Bool | off |  |
| `wfAvg` | Average slow rows | Bool | off |  |
| `wfLowPass` | Waterfall noise reduction | Bool | off |  |
| `specAutoDb` | Spectrum range follows the noise floor | Bool | on |  |
| `specPerHz` | Levels in dB per Hz | Bool | on |  |
| `specDbm` | Show dBm instead of dBFS | Bool | off |  |
| `specFill` | Fill under the spectrum line | Bool | on |  |
| `specGrid` | Grid | Bool | on |  |
| `specPeaks` | Peak markers | Bool | off |  |
| `specReadout` | Level readout at the cursor | Bool | on |  |
| `specLine` | Spectrum line thickness | Float | 1.2 |  |
| `specAvgSec` | Spectrum averaging (seconds) | Float | 0.08 |  |
| `holdDecay` | Max-hold fall (dB per second) | Float | 4.5 |  |
| `specFft` | FFT size | Choice | Automatic | new |
| `wfRate` | Waterfall speed | Choice | Every frame | new |
| `specHold` | Max-hold line | Bool | off | new |
| `minHold` | Min-hold line | Bool | off | new |
| `specNfMarker` | Noise floor line | Bool | off | new |
| `specLabels` | Station names on the spectrum | Bool | on | new |
| `traceColor` | Spectrum line colour | Choice | Soft white | new |
| `holdColor` | Max-hold line colour | Choice | Grey (default) | new |
| `specSmoothTrace` | Smooth the spectrum line | Choice | As the look | new |
| `specAxisLabels` | Frequency scale | Choice | As the look | new |
| `specLabelSize` | Scale text size | Float | 1 | new |
| `specSmoothZoom` | Smooth zoom | Bool | on | new |
| `wfFloorBelow` | Waterfall floor below the noise (dB) | Float | 4 | new |
| `wfFloorSpan` | Waterfall range (dB) | Float | 55 | new |
| `specDefLo` | Default range: bottom (dB) | Float | -110 | new |
| `specDefHi` | Default range: top (dB) | Float | -30 | new |
| `dbKeyStep` | Range keys step (dB) | Float | 5 | new |

### Radio

| Key | Setting | Kind | Default | 4.0 |
|---|---|---|---|---|
| `tuningMode` | Spectrum click | Choice | Retune the radio |  |
| `centreTuning` | Click centres the radio | Bool | off |  |
| `specDrag` | Drag on spectrum | Choice | Tune (like SDR++) |  |
| `dragMovesTuned` | Dragging the band moves the tuned frequency | Bool | off |  |
| `specSnapHz` | Click snaps to a multiple of (Hz) | Int | 0 |  |
| `bwAuto` | Measure the channel width by itself | Bool | on |  |
| `agcOn` | Automatic gain | Bool | off | new |
| `dcRemove` | Remove the DC spike | Bool | off | new |
| `iqCorrect` | Correct IQ imbalance | Bool | off | new |
| `offsetTune` | Tune off-centre by itself | Bool | off | new |
| `spurNotch` | Notch fixed spurs | Bool | off | new |
| `nbOn` | Noise blanker | Bool | off | new |
| `nbThr` | Noise blanker threshold | Float | 8 | new |
| `nbWidthUs` | Noise blanker width (us) | Float | 20 | new |

### Audio

| Key | Setting | Kind | Default | 4.0 |
|---|---|---|---|---|
| `popTop` | Video pop-out stays on top | Bool | on |  |
| `subsOn` | Show subtitles | Bool | on |  |
| `surroundLayout` | TV sound speakers | Choice | Automatic (the device's speakers) |  |
| `dolbyPassthrough` | Pass Dolby audio through to my receiver (bitstream, experimental) | Bool | off |  |
| `audioDevDefault` | Output device | Custom |  | new |
| `aclDefAgc` | Audio AGC for new modes | Choice | Off | new |
| `aclDefSq` | Squelch for new modes | Choice | Off | new |
| `aclDefSqDb` | Squelch opens at (dB) | Float | -50 | new |
| `fmBass` | FM bass (dB) | Int | 0 | new |
| `fmTreble` | FM treble (dB) | Int | 0 | new |
| `fmWarm` | FM warm sound | Bool | off | new |
| `fmAudioBw` | FM audio bandwidth | Choice | Full (15 kHz) | new |
| `smoothGaps` | Hide short drop-outs | Bool | off | new |
| `audioRate` | Sound card rate (Hz) | Choice | 48000 | new |
| `iecDevice` | Passthrough output | Text | (empty) |  |

### Recording and files

| Key | Setting | Kind | Default | 4.0 |
|---|---|---|---|---|
| `recFormat` | IQ recording format | Choice | 8-bit |  |
| `recordingsFolder` | Recordings folder | Custom |  |  |
| `dataFolder` | Data folder | Custom |  |  |
| `recNamePattern` | IQ file name | Text | (empty) | new |
| `recSplitMb` | Split IQ recordings every (MB) | Int | 0 | new |
| `recMinFreeMb` | Stop recording below free space (MB) | Int | 200 | new |
| `recSigmf` | SigMF metadata beside IQ files | Bool | on | new |
| `audioRecFmt` | Audio recording format | Choice | WAV | new |
| `audioRecGate` | Record audio only while there is sound | Bool | off | new |
| `audioRecSplit` | One audio file per transmission | Bool | off | new |
| `audioRecKeep` | Keep recording audio across retunes | Bool | off | new |

### Decoding

| Key | Setting | Kind | Default | 4.0 |
|---|---|---|---|---|
| `fmMono` | FM: force mono | Bool | off |  |
| `fmStereo` | FM stereo | Choice | Auto |  |
| `fmDeemph` | FM de-emphasis | Choice | 50 us |  |
| `fmRegion` | RDS meanings | Choice | Auto |  |
| `fmSquelch` | FM squelch (dB SNR) | Int | 0 |  |
| `staleVdl2` | Remove after: VDL2 | Choice | Custom |  |
| `staleAcars` | Remove after: ACARS | Choice | Custom |  |
| `staleHfdl` | Remove after: HFDL | Choice | Custom |  |
| `staleAdsb` | Remove after: ADS-B | Choice | Custom |  |
| `staleAis` | Remove after: AIS | Choice | Custom |  |
| `staleUat` | Remove after: UAT | Choice | Custom |  |
| `staleInmsat` | Remove after: Inmarsat | Choice | Custom |  |
| `staleCustomMin` | Custom remove-after time (minutes) | Int | 120 |  |
| `ttxPage` | Teletext start page | Int | 100 |  |
| `tvLang` | Preferred TV language | Text | (empty) |  |
| `tvBufMb` | TV pause buffer (MB) | Int | 256 | new |

### Location

| Key | Setting | Kind | Default | 4.0 |
|---|---|---|---|---|
| `adsbLat` | Station latitude | Float | 0 |  |
| `adsbLon` | Station longitude | Float | 0 |  |

### Keyboard and mouse

| Key | Setting | Kind | Default | 4.0 |
|---|---|---|---|---|
| `keysEditor` | Keys, mouse buttons and wheel | Custom |  | new |

### Notifications

| Key | Setting | Kind | Default | 4.0 |
|---|---|---|---|---|
| `notifySystem` | System notifications | Bool | on | new |
| `notifyBanner` | Banner in OnAir | Bool | on | new |
| `notifyBannerSec` | Banner time (seconds) | Int | 10 | new |
| `notifySound` | Alert sounds | Bool | on | new |
| `notifyQuiet` | Quiet hours | Bool | off | new |
| `notifyQuietFrom` | Quiet from (minutes after midnight) | Int | 1320 | new |
| `notifyQuietTo` | Quiet until (minutes after midnight) | Int | 420 | new |

### Network and updates

| Key | Setting | Kind | Default | 4.0 |
|---|---|---|---|---|
| `updCheck` | Check for updates | Bool | on |  |
| `updBeta` | Include beta versions | Bool | off |  |
| `updAuto` | Download updates by itself | Bool | off |  |
| `updSrc` | Update source | Choice | Automatic |  |
| `updMirrorOn` | Use the mirror when GitHub cannot be reached | Bool | off |  |
| `updMirror` | Mirror address | Text | (empty) |  |
| `udpHost` | UDP output host | Text | (empty) |  |
| `udpPort` | UDP output port | Int | 0 |  |
| `udpRtp` | UDP output as RTP | Bool | off |  |
| `mapTileUrl` | Map tile server | Text | (empty) | new |
| `mapCapMb` | Map tile cache size (MB) | Int | 500 | new |
| `mapKeepTiles` | Keep map tiles between runs | Bool | on | new |

### Privacy

| Key | Setting | Kind | Default | 4.0 |
|---|---|---|---|---|
| `privacyInfo` | What leaves this computer | Custom |  |  |
| `adsbMap` | Fetch online map tiles | Bool | on | new |

### Performance

| Key | Setting | Kind | Default | 4.0 |
|---|---|---|---|---|
| `powerLowMode` | Low power mode | Choice | Automatic (on battery or hot) |  |
| `powerHidden` | Save power when hidden | Choice | On battery |  |
| `reduceCpuHidden` | Reduce CPU when hidden | Bool | on |  |
| `gpuGlobal` | Use GPU where available | Bool | on |  |
| `gpuModes` | GPU per mode | Custom |  |  |
| `compute` | DVB error correction on | Choice | Automatic | new |
| `perfOverlay` | Performance overlay | Bool | off |  |

### Plug-ins and scripts

| Key | Setting | Kind | Default | 4.0 |
|---|---|---|---|---|
| `pluginsLink` | Plug-ins | Link |  |  |
| `automationLink` | Scripts and rules | Link |  |  |

### Profiles

| Key | Setting | Kind | Default | 4.0 |
|---|---|---|---|---|
| `profiles` | Settings profiles | Custom |  | new |

### Advanced

| Key | Setting | Kind | Default | 4.0 |
|---|---|---|---|---|
| `verboseLog` | Verbose logging | Bool | off | new |
| `ffmpegLibs` | FFmpeg libraries | Custom |  |  |
| `optLibs` | Optional libraries and codecs | Custom |  |  |
| `sdrconnectLog` | Log SDRconnect websocket traffic | Custom |  |  |
| `saveLog` | Save the log | Custom |  |  |
| `exportImport` | Export or import settings | Custom |  |  |
| `resetAll` | Reset every setting | Custom |  |  |


## Default keys

| Saved name | Action | Group | Scope | Key | Second key |
|---|---|---|---|---|---|
| `start` | Start or stop the receiver | Receiver | Global | Space | - |
| `mode1` | Mode 1 of the list | Modes | Global | 1 | - |
| `mode2` | Mode 2 of the list | Modes | Global | 2 | - |
| `mode3` | Mode 3 of the list | Modes | Global | 3 | - |
| `mode4` | Mode 4 of the list | Modes | Global | 4 | - |
| `mode5` | Mode 5 of the list | Modes | Global | 5 | - |
| `mode6` | Mode 6 of the list | Modes | Global | 6 | - |
| `mode7` | Mode 7 of the list | Modes | Global | 7 | - |
| `mode8` | Mode 8 of the list | Modes | Global | 8 | - |
| `mode9` | Mode 9 of the list | Modes | Global | 9 | - |
| `dbup` | Spectrum range up 5 dB | Spectrum | Plot | Ctrl+Up | - |
| `dbdown` | Spectrum range down 5 dB | Spectrum | Plot | Ctrl+Down | - |
| `dbwide` | Spectrum range wider | Spectrum | Plot | Ctrl+Shift+Up | - |
| `dbnarrow` | Spectrum range narrower | Spectrum | Plot | Ctrl+Shift+Down | - |
| `dbreset` | Spectrum range back to the default | Spectrum | Plot | Ctrl+Home | - |
| `pgup` | View one width up in frequency | View | Global | Ctrl+PageUp | - |
| `pgdown` | View one width down in frequency | View | Global | Ctrl+PageDown | - |
| `tuneup` | Tune up by the step | Tuning | Plot | Right | - |
| `tunedown` | Tune down by the step | Tuning | Plot | Left | - |
| `zoomin` | Zoom in (frequency) | View | Plot | - | - |
| `zoomout` | Zoom out (frequency) | View | Plot | - | - |
| `zoomreset` | Show the whole band | View | Plot | Home | - |
| `panleft` | Pan the view left | View | Plot | - | - |
| `panright` | Pan the view right | View | Plot | - | - |
| `stepnext` | Larger tuning step | Tuning | Plot | Up | - |
| `stepprev` | Smaller tuning step | Tuning | Plot | Down | - |
| `wfpause` | Pause or resume the waterfall | View | Plot | - | - |
| `mute` | Mute or unmute the sound | Receiver | Global | - | - |
| `settings` | Open or close Settings | Windows | Global | Ctrl+, | - |
| `palette` | Command palette | Windows | Global | Ctrl+K | - |
| `modepicker` | Mode picker | Windows | Global | M | - |
| `memory` | Memory panel | Windows | Global | Ctrl+M | - |
| `freqdb` | Frequencies database | Windows | Global | Ctrl+Shift+F | - |
| `clips` | Radio clips | Windows | Global | C | - |
| `controls` | Keyboard and mouse settings | Windows | Global | - | - |
| `layout` | Classic or Unified layout | View | Global | Ctrl+Shift+L | - |
| `restoreview` | Restore the default view | View | Global | Ctrl+Shift+M | - |
| `dock` | Docking on or off | View | Global | Ctrl+Alt+D | - |
| `fullscreen` | Full screen | View | Global | F11 | Alt+Enter |
| `videoonly` | Video only (picture over the whole screen) | View | Global | F | - |
| `identify` | Identify tool (click a signal to identify it) | View | Global | I | - |
| `unicol` | Unified layout: side column | View | Global | F6 | - |
| `textbigger` | Larger text | View | Global | Ctrl+= | Ctrl+Keypad + |
| `textsmaller` | Smaller text | View | Global | Ctrl+- | Ctrl+Keypad - |
| `textreset` | Text size back to 100 % | View | Global | Ctrl+0 | Ctrl+Keypad0 |
| `recordiq` | Start or stop an IQ recording | Receiver | Global | - | - |
| `tvpause` | Pause or play the TV picture | TV | Tv | Space | - |
| `tvrecord` | Record the TV service | TV | Tv | R | - |
| `hamguide` | Guide tab | FT8 / FT4 / WSPR | Ham | F1 | - |
| `hamwizard` | Station setup | FT8 / FT4 / WSPR | Ham | F2 | - |
| `hamlog` | Log tab | FT8 / FT4 / WSPR | Ham | F3 | - |
| `hamcleardx` | Stop and clear the DX call | FT8 / FT4 / WSPR | Ham | F4 | - |
| `hammap` | Map tab | FT8 / FT4 / WSPR | Ham | F5 | - |
| `hamcq` | Call CQ | FT8 / FT4 / WSPR | Ham | F6 | - |
| `hamanswer` | Answer the best caller | FT8 / FT4 / WSPR | Ham | F7 | - |
| `hamspots` | Spots tab | FT8 / FT4 / WSPR | Ham | F8 | - |
| `hamaddlog` | Add a contact to the log | FT8 / FT4 / WSPR | Ham | Ctrl+L | - |
| `hamfindlog` | Search the log | FT8 / FT4 / WSPR | Ham | Ctrl+F | - |
