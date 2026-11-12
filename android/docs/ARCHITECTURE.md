# OnAir Android: architecture (0.3.0 base)

The base the USB (`android-usb-030`), UI (`android-ui-030`) and modes (`android-modes-030`) work builds on. Build steps and signing are
in [../README.md](../README.md).

## Modules

```
                       +-----------------------------------------------------------+
                       |  UI (Compose, main thread)                                |
                       |  MainActivity -> ui/OnAirApp (shell, nav) -> Receiver /   |
                       |  Panels (ModePanels) / Modes / Radio / Settings / Log ... |
                       +------------------------+----------------------------------+
                          collectAsStateWithLifecycle |   ^ intents (vm.tune(), vm.toggle() ...)
                                                  v   |
                       +------------------------------+----------------------------+
                       |  ui/ReceiverViewModel  (one per activity, survives        |
                       |  rotation; owns the Waterfall bitmap, main thread only)   |
                       +------------------------------+----------------------------+
                                                      |
   +------------------+        +----------------------v----------------------------+
   | service/         | start  |  data/ReceiverRepository  (process singleton,     |
   | ReceiverService  |<-------|  AppGraph)                                        |
   | foreground,      | state  |  state: StateFlow<ReceiverState>   (phase, mode,  |
   | wake + Wi-Fi     |------->|    tuning, freq, caps, error, file)               |
   | locks, notif.    |  stop  |  settings: StateFlow<Settings>   (memory, then    |
   +------------------+------->|    settings/SettingsStore -> DataStore)           |
                               |  spectrum / telemetry / agcGain  (from engine)    |
                               +----------------------+----------------------------+
                                     post / call      |   ^ StateFlows, callbacks
                                                      v   |
                               +----------------------+----------------------------+
                               |  engine/ReceiverEngine  (thread "onair-receiver") |
                               |  FIFO command queue + poller (spectrum, telemetry,|
                               |  C++ log, "engine stopped by itself")             |
                               +----------------------+----------------------------+
                                                      |
                               +----------------------v----------------------------+
                               |  engine/OnAirCore (typed) -> OnAirNative (JNI)    |
                               +----------------------+----------------------------+
                                                      |  libonair.so
                               +----------------------v----------------------------+
                               |  native/src/jni_onair.cpp, results_json.cpp       |
                               |  -> dect2core (core/, rust/ of this repository)   |
                               +---------------------------------------------------+

   diag/AppLog (structured log, all threads)   diag/CrashLog (crash guard, crash.log, share via FileProvider)
   model/ (immutable types shared by every layer)   AppGraph (process-wide objects, created in OnAirApplication)
```

## Data flow

- **Down (actions).** A composable calls a `ReceiverViewModel` function, which calls the repository. The repository updates its
  in-memory state at once (so the UI reacts in the same frame), schedules the settings write, and queues the native work on the
  engine. Example: `tune(mhz)` → `ReceiverState.freqMhz` now → `Settings.freqMhz[mode]` written to DataStore in the background →
  `engine.post("retune") { if (it.running()) it.retune(hz) }`.
- **Up (state).** The engine thread publishes spectrum lines (`SpectrumFrame`), telemetry (`Telemetry`) and the AGC gain as
  StateFlows and appends the C++ log to `AppLog`. The repository exposes them unchanged next to its own `state` and `settings`. The
  view model turns spectrum frames into waterfall rows on the main thread and re-publishes the frame (`vm.frame`) after that, so a
  redraw always sees a bitmap that matches the trace.
- **Single source of truth.** `ReceiverState` (session) and `Settings` (preferences) in the repository. Composables hold only UI
  state: open dialogs, text being typed, slider positions while dragging, zoom, selected tab. Sliders commit on release; text
  fields commit after a pause (`Endpoint` in ReceiverTab).
- **Persistence.** `SettingsStore` (Jetpack DataStore, `datastore/settings.preferences_pb`). The 0.2.x SharedPreferences file `onair`
  is migrated once by `SharedPreferencesMigration` with the same key names; the `schema` key (now 1) is for later migrations. The
  repository reads the stored settings once at start and then treats its in-memory copy as authoritative; writes are conflated.

## Threading rules

1. **Every JNI call runs on the receiver thread** (`ReceiverEngine`, thread `onair-receiver`), through `engine.post(name) { core -> }`
   (fire and forget) or `engine.call(name) { core -> }` (suspend until done, returns the value). Never call `OnAirNative` or
   `OnAirCore` anywhere else; the C++ bridge has one engine and a control mutex, and `nativeStart` may block for seconds.
2. **Commands are serialised.** One FIFO queue, one thread: start, stop, retune, mode change can never overlap or overtake each
   other. "Stop while starting" simply runs after the start returns. A posted block decides with the engine's *current* state
   (`if (it.running()) ...`), not with a value captured earlier.
3. **Multi-step operations** (start, stop, mode change, source change, closing an old file) run under the repository's `ops` mutex
   (fair, FIFO), on `Dispatchers.Default`. Inside them, the phase goes `IDLE → STARTING → RUNNING → STOPPING → IDLE`; a mode
   change while running keeps `STARTING` between its stop and start, so the foreground service does not end in between.
4. **The main thread never blocks.** Intents return at once. Compose reads StateFlows with `collectAsStateWithLifecycle`; the
   spectrum view reads its frame inside the draw phase, so a new line only redraws the canvases.
5. **The waterfall bitmap is main-thread only** (written by the view model, drawn by Compose).
6. Engine callbacks (`onFailure`, `onEngineStopped`) run on the receiver thread: keep them short, touch only thread-safe things
   (`MutableStateFlow.update`, `AppLog`, `CrashLog`).
7. **Exceptions to rule 1:** `RadioNative.nativeAudioPush` (the sound-card recording thread) and `tv/TvNative` (the TV player's thread
   `onair-tv` and the TV controller's poll). Both touch only their own locked buffers in the bridge and the engine's packet tap and
   service snapshot, which the engine guards itself; neither starts, stops or retunes anything. The TV player (`tv/TvPlayer.kt`)
   keeps MediaCodec and AudioTrack on its own thread; `surfaceDestroyed` waits (at most a second) until it let go of the Surface.
8. Polling cadence: 35 ms while running with an activity visible (spectrum + AGC), 100 ms running in the background (no spectrum),
   350 ms idle; telemetry and the C++ log every 350 ms.

## Lifecycle

- `OnAirApplication.onCreate`: crash guard first, then `AppGraph`, then the repository (receiver thread, settings, `nativeInit`, mode
  table, saved mode, source capabilities).
- `MainActivity` holds nothing; `onStart`/`onStop` tell the repository whether spectrum lines are wanted. Rotation and re-creation
  change nothing (state is in the repository; the view model keeps the waterfall).
- `ReceiverRepository.start()` (from the UI, so the start is allowed) starts `ReceiverService`. The service goes to the foreground
  (type `mediaPlayback`), holds a partial wake lock and a Wi-Fi lock, shows mode/frequency/state with a Stop action, and stops
  itself when the phase is back to `IDLE`. Screen off, app in the background, activity finished: the session goes on until Stop
  (button or notification) or the engine stops by itself. `START_NOT_STICKY`: after a process kill there is nothing to resume.
- Files from the system picker stay open in the repository (`/proc/self/fd/N` for the engine) until the next pick; the old descriptor
  is closed only after any running session let go of it.

## Diagnostics

- `AppLog`: time, level, tag, message; also to logcat as `OnAir/<tag>`. Tags: `app`, `engine`, `core` (C++ log lines, levels guessed
  from the text), `service`, `settings`, `crash`; new work adds `usb`, `mode:<name>`. The Log tab filters by level, follows, clears
  and shares.
- `CrashLog`: uncaught exceptions (any thread), failed engine commands and failed starts go to `files/crash.log` (64 KB cap) with a
  header (version, build type, phone, Android, ABI). A crash the user has not seen yet shows a banner at the next start (tap clears
  it). Sharing copies the app log and the crash log into `cache/share/` and sends them through `FileProvider`
  (`${applicationId}.files`); nothing else is exposed. Native crashes (SIGSEGV in the core) are not caught on this side yet:
  logcat/tombstones only.

## Permissions

`ui/Permissions.kt` (`AppPermission`, `rememberPermissionRequest`): a permission is asked only if the manifest declares it and the
Android version has it. Declared now: INTERNET, FOREGROUND_SERVICE(+_MEDIA_PLAYBACK), WAKE_LOCK, POST_NOTIFICATIONS (asked at the
first start; the receiver runs either way). For later: RECORD_AUDIO (audio-input modes) and ACCESS_FINE_LOCATION (direction finding)
are in the enum but not declared; USB radios use `UsbManager.requestPermission` per device (not a runtime permission).

## Build variants

`debug`: id suffix `.debug`, version suffix `-debug`, no R8, starts without `libonair.so` (warning). `release`: R8 + resource
shrinking (`proguard-rules.pro` keeps `OnAirNative` and every `native` method), signed when a key is supplied, refuses to build
without `libonair.so`. Version name/code from the repository's `CMakeLists.txt` (see README).

## How modes plug in

Today every mode runs inside the desktop engine; the app selects it and shows its results generically:

1. **Mode list.** `Modes.kt` + `res/values/modes.xml` are generated by `tools/gen_modes.py` from the desktop table
   (`app/toolbar.cpp`: family, group, accent colour, protocol id). Re-run it after the desktop list changes. `ReceiverState.stdOf()`
   maps a family to the engine standard (as `engineStd` in the desktop's `app_state.cpp`); a protocol id (sub-mode) wins over it.
2. **Tuning.** `nativeModeTable` / `nativeSetMode` give a `Tuning` per mode (default/min/max frequency, rate, bandwidth, minimum rate).
   The repository clamps and remembers the frequency per mode.
3. **Results.** `nativeTelemetry` returns JSON built by `native/src/results_json.cpp`: `summary`, `cards` (title/value pairs),
   `tables` (desktop column names), plus mode-specific objects (`fm`, `general`). `Telemetry.parse` turns it into typed models;
   `ResultsTab` draws cards and tables for every mode, a map for tables with positions, and dedicated panels for FM and the general
   receiver.
4. **A mode with its own screen or controls** (the modes agent):
   - C++: add its fields to `results_json.cpp` under a key of its own (e.g. `"adsb": {...}`), and any setter as a new `FN(...)` in
     `jni_onair.cpp`.
   - Kotlin: the `external fun` in `OnAirNative.kt` (run `tools/check_jni.py`), a typed method in `OnAirCore`, a data class in
     `model/` parsed in `Telemetry.parse`, an intent in the repository that posts the call (and stores its setting in `Settings` if it
     persists), the function in the view model, then a panel: a class implementing `ui/panels/ModePanel` in a file of its own,
     registered with a `ModePanelProvider` listed in
     `app/src/main/resources/META-INF/services/io.github.adswill.onair.ui.panels.ModePanelProvider` (or `ModePanels.register(...)` at
     start-up). The panel host shows it as a tab next to the generic Results panel for the modes its `appliesTo` accepts; no UI file
     changes. Small additions to the generic view can still go into `ResultsTab` (`when (t.standard)`).
   - Never call the new native function outside an engine block; never parse JSON outside `model/`.
5. **Sources**: `SourceKind` + the bridge's `enum Source` + `makeDevice` in `jni_onair.cpp`. USB radios, network radios and the
   sound card (kinds 4, 5, 6) are the radio-access layer, described in [USB.md](USB.md): `usb/` (UsbManager permission and hot-plug
   flow, `UsbRadioController`, device filter, picker dialog, `RadioNative` JNI = `native/src/jni_radio.cpp`), `netradio/` (network
   radio list, add-dialog model, opt-in mDNS), `audio/` (AudioRecord), all held by `data/RadioRepository` inside the repository.
   `RadioNative` calls follow rule 1 (receiver thread), except `nativeAudioPush` from the recording thread.

## UI layer (0.3.0 rework, `ui/`)

- **Theme** (`Theme.kt`, `OnAirIcons.kt`, `Widgets.kt`): the desktop's palette, type scale, fonts (Inter, JetBrains Mono from
  `third_party/fonts`), corners and spacing as a Material3 theme; `docs/STYLE.md` maps every desktop name to its token.
- **Shell** (`ui/AppShell.kt`): `AppLayout.of(width, height)` picks the layout: phone portrait = bottom bar, spectrum on top and the
  panels under a draggable split; landscape and tablets = navigation rail and two panes (spectrum left, panels right). Edge to edge
  (`enableEdgeToEdge`, `WindowInsets.safeDrawing`). The back stack is `ui/nav/Navigator` (pure rules in `BackStack`, unit tested):
  top-level destinations replace everything above the receiver, others are pushed, Back pops then leaves. Deep links
  `onair://tune?freq=...&mode=...` (`ui/nav/DeepLink`, manifest intent filter) arrive in `MainActivity` and are handed to the shell once.
- **Screens**: `ui/receiver` (frequency readout with tap-to-type and step buttons, mode chip, lock lamp, meters, run button, spectrum
  and waterfall, sound strip with volume, mute, squelch and record), `ui/panels` (the extension point above), `ui/modes` (grouped,
  searchable picker with favourites and the two filters; pure filtering in `ModeFilter`), `ui/radio` (sources, saved network radios,
  add dialog with opt-in mDNS discovery, tuner), `ui/settings` (grouped, searchable), `ui/log`, `ui/about` (credits from the
  repository's credit files), `ui/onboarding` (first-run guide like the desktop wizard; lessons from `data/lessons/*.json`).
  Each screen has a stateless `...Content` composable; `ui/Previews.kt` previews them.
- **Interface preferences** (`ui/UiPrefs.kt`, DataStore file `ui`): favourites, hidden modes, filters, centre tuning, step, first-run
  state, font look, reduced motion, saved network radios. Kept apart from `Settings` (the receiver's), same rules (memory first,
  conflated writes). `UiGraph` holds it; `UiHooks` is where the USB and recorder sides plug their objects in (`UsbRadios`, `Recorder`).
- **Spectrum performance**: the waterfall is one bitmap ring written row by row (`Waterfall.push`, no allocation per frame); a frame
  is published with `specVersion` and `Waterfall.commit()` (`version`), and both canvases read those in the draw phase, so a new line
  redraws two canvases and recomposes nothing. Paths are reused; meters are refreshed 4 times a second; on battery or power saving
  redraws are capped at 30 fps; the engine already stops spectrum lines when no activity is visible.
- **Assets from the repository** (`syncOnAirAssets` in `app/build.gradle.kts`): `data/lessons/*.json`, `tools/credits_data.tsv`,
  `tools/contributors.tsv`, `app/panels/supporters_data.h`, `LICENSES/OFL-1.1.txt`, copied into the APK so desktop and phone show the
  same lessons and credits.

## Not verified here

Written and checked by reading only (no device, no full Gradle/NDK build in this step): the Kotlin compiles (the `android-*` branch CI
job checks this), the DataStore migration of an existing 0.2.x install, the foreground service on Android 14/15 (type
`mediaPlayback` start rules, notification permission denied), audio with the screen off over hours, R8 release build with JNI, the
macOS CI native build (`brew` headers + NDK 27 + Rust Android target), and signing from secrets. The UI rework (android-ui-030) was also
written without a local Gradle build: layouts on real phones, tablets and foldables, TalkBack, 200 % text, mDNS discovery and the
30 fps cap on battery are unchecked on a device.
