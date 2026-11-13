# Issues #463, #435, #442 (4.0, branch feat40-issues2)

Owner's rule: no phone, no radio, no local builds (battery). Everything below was checked on GitHub only; CI results are at the end.
Nothing was run on a real phone or with a real CDR or DTMB signal.

## #463: CDR on Android locks but plays no sound

**Cause (two things).**

1. The phone never asked the CDR receiver to play anything. The desktop's CDR panel calls `CdrReceiver::setListen(id)` every frame
   (the selected service, else the first audio service); the Android app has no CDR screen and never called it, so `listenId` stayed 0
   and the DRA+ units were thrown away before any decoder.
2. CDR sound is DRA+, which OnAir does not ship (docs/CODECS.md: user-installed add-on `libonair_dra`). On the desktop the add-on goes in
   a codecs folder; the phone had no codecs folder, no way to put a file there, and nothing on screen said why it was silent.

**Fix.**

- Core: `setListen(-1)` means "the first audio service", resolved by the receiver from its service table (`cdr_rx.cpp listenWant()`).
  The bridge uses -1 by default and re-applies it on every start.
- JNI: `nativeCdrListen(id)` and `nativeCodecs(dir)` (sets the app's `files/codecs` as the extra codec folder, `draRescan()`, returns
  found / path / problem). Called at start-up and after an import.
- `results_json.cpp`: CDR has its own exporter now: state, SNR, carrier error, transmission mode, LDPC, network, a **Sound** card
  ("silent: the DRA+ add-on is not installed" / loaded / waiting), the services table (kind, DRA+ format, rate, language, text, playing)
  and a `cdr` object for the app.
- Android: new **Sound** tab on CDR (`CdrPanel.kt`): the add-on state in plain words (amber when missing, green when loaded), the build
  steps, **Import add-on (.so)** (system file picker, copied into `files/codecs/libonair_dra.so`; Android does not run code from shared
  storage), **Remove add-on**, and the service list with Automatic / Listen.
- docs/CODECS.md: new section "DRA+ on Android" with the NDK build (`tools/addons/dra`, arm64-v8a, android-28). android.yml builds the
  add-on with the NDK on every Kotlin check as a test of those steps (continue-on-error, never uploaded or packaged).

**What the user must do:** build `libonair_dra.so` for arm64 with the Android NDK on a computer (docs/CODECS.md, "DRA+ on Android"),
copy it to the phone, CDR mode > Sound tab > Import add-on (.so).

## #435: DTMB on Android

**Crashes.** All crash entries in the four attached crash files are the same Compose failure:
`ArrayIndexOutOfBoundsException: length=80 (or 320); index=-2` in `G.p` (ComposerImpl) under `J1.a` (ComposableLambdaImpl) during a
frame's recomposition. No mapping file was kept, so the frames are not named; the pattern matches an early return out of an inline
composable lambda, which the Compose compiler of Kotlin 2.0.21 does not always balance. `ResultsTab` had exactly that
(`if (t == null) { ...; return@Column }`, t goes null on every mode change and start), and the RDS, Tables, Aircraft, Vessels and
Messages panels had `return` inside their `Column { }`. All six are now plain if / else / when. This is the most likely cause, not a
proven one (no symbolicated trace, no phone). Recommendation for the integrator: keep `mapping.txt` of release builds as an artifact so
the next crash report can be read.

**"No channels, no parameters".** The attached 3.7 log shows the SpyServer link (Airspy R2, 10 Msps, Wi-Fi to the phone) losing
0.6 to 2.8 s of signal every 10 s: 24 s of 70 s in one session. The DTMB signal is PN945 with a **fixed** header phase, 4QAM 0.4,
interleaver 720. With a fixed phase the receiver cannot tell how many frames a gap took ("samples lost: frame grid moved by -3443
symbols, 1 frame(s) erased" dozens of times a second), so the time deinterleaver (about 0.3 s deep) is fed out of step after every gap
and nothing decodes; the PC on a wire did not lose samples.

- SpyServer source: the server drops IQ messages its client cannot take, and their sequence numbers show it. The missing messages are now
  pushed as zeros of their length and counted as lost at the source (`IqRing::noteLost`), so every receiver keeps its time line. Other
  message types that share the counter are not taken for drops; a gap of more than a second is a restart and is not filled.
  Whether every SpyServer version numbers its messages this way was not verified on a real server; the fake server in test_netradio does.
- DTMB receiver: a header that is one constant magnitude over its whole length (a zero-filled gap after the front end's DC removal and
  derotation) is erased and coasted over without counting towards "no PN header" (up to about 2 s). Measured on the "wifi link" case
  (the user's parameters, 3 % of the signal lost in gaps of 2 .. 10 ms): gaps removed 0.16 of the packets; zero-filled before this change
  0.46 with the lock lost and found 11 times; zero-filled now **0.92, lock found once**, no wrong packet. "zeros for 100 ms" now costs
  1788 packets and keeps the lock.
- DTMB on the phone now shows what the desktop shows: frame header and phase, carriers, system information (mapping, rate, interleaver),
  payload rate, LDPC ok / bad, TS packets, frames without a usable header, codewords dropped (phone too slow), carrier and clock offset,
  echo span. The service list and picture were already on the 4.0 Watch tab (feat40-android).
- Every mode: cards "Signal lost on the network link" / "Radio delivers x % of the sample rate" / "Signal dropped (phone too slow)" and
  an amber line on top of the results when the link is losing signal, telling the user to use a wired link, a lower rate, or USB.
- The real cure on the phone is the radio on USB: 4.0 adds the Airspy R2 over OTG (feat40-android).

Still open: DRA sound of DTMB services on the phone (the Watch tab says "no sound" for DRA; the core's `TvDraDecoder` could feed it with
the same add-on); AVS+ pictures (no decoder on Android).

## #442: bugs

| Item | State on integ-40 | Fix |
|---|---|---|
| Scroll-wheel zoom-in crash | fixed in 3.7 (wheel limits, user confirmed) | - |
| Zoom-out makes the dB axis tiny | fixed in 3.7 (19ff484): the wheel stops at the slider range, -160 .. +20 dB, 10 dB minimum span; frequency span limited to the sample rate | - |
| FM right panel shifts when RDS data comes and goes | still there: rows (now playing, flags, call letters, subcarriers, multipath, TMC) appear and disappear and text wraps, moving the saved stations under the mouse | the RDS block keeps the tallest height it reached on this frequency (reset on retune or stop), `fm_ui.cpp` |
| Mode A/C full screen button | still there: `adsbFullMap` cancelled itself unless the ADS-B screen's own view was Map or Radar; its default is the Live table (the #235 fix only covered users who had used the ADS-B map) | Mode A/C always uses the map view, `adsb_map_ui.cpp` |
| Tools > Images > Choose... opens no folder picker (Windows) | still there: `openFolderDialog()` on Windows returned an empty string | shell folder browser (`SHBrowseForFolderW`, new-style dialog with an edit box), `platform_posix.cpp`; fixes every folder "Choose..." on Windows |
| OnAir logo not shown on the main window (Windows) | still there: the .rc has the icon as resource 1; GLFW gives its window class the icon named `GLFW_ICON`, so the title bar and taskbar showed the default icon | `GLFW_ICON ICON "OnAir.ico"` in `packaging/windows/onair.rc` |

Not verified on Windows by hand (no Windows machine): the folder dialog and the icon were compiled on the Windows CI runner only.

### Feature ideas in #442 and #463 (not built, for the owner)

- FM saved stations: sort the table by frequency or name (#442).
- Hover help for every mode: what it is, frequency range, whether the connected SDR can reach it and its sample rate; satellite facts
  (launch date, operator) like the M key menu (#442).
- Hover help on the FM Carrier / Stereo / RDS lamps; make the M key menu easier to find (#442).
- DAB: keep the ensembles found by a scan so a restart does not need a new scan (#442).
- Android RDS: AF list, CT clock and the other full RDS fields (#463, already noted as a request).
- Android IQ recording (#463): already in the app (Record button, CS8 + SigMF into the app folder, `nativeRecStart`); nothing to build,
  the reply says where it is.
- Android DTMB: DRA and AVS+ decoding through add-ons (#463).

## Tests

| Test | What | Validated on |
|---|---|---|
| `native_netradio` (test_netradio) | fake SpyServer leaves out every fifth IQ message and sends CLIENT_SYNC every third: silence in whole 1024-sample messages, counted as lost at the source; syncs alone cause no fill | simulated server on localhost |
| `dtmb_faults` "wifi link" | PN945 fixed phase, 4QAM 0.4, interleaver 720, -110 Hz, 8 ppm, 25 dB, 5 gaps a second of 2 .. 10 ms; gaps removed (printed: 0.16) vs filled with zeros (must keep decoding, no wrong packet, at least 0.85 of the packets, lock found once; measured 0.92) | simulated signal |
| android.yml | JNI table, generated tables current, Kotlin compile, JVM tests, debug APK with libonair.so, DRA+ add-on NDK build | GitHub runners |

No phone, emulator, Windows desktop, CDR or DTMB signal was used.

## CI results

| Run | What | Result |
|---|---|---|
| android.yml 43198580494 (90929fc) | debug APK with libonair.so (new JNI calls, results_json CDR/DTMB/loss, cdr_rx) | pass |
| android.yml 43207001215 (712a5b6) | JNI table, generated tables, Kotlin compile, JVM unit tests, DRA+ add-on NDK build (symbols checked) | pass |
| quick.yml 43207706185 (712a5b6, Linux) | dect2 app build; dtmb_faults | app built; dtmb_faults failed its first threshold (0.46 < 0.5), which led to the DTMB coasting fix; native_netradio not run (wrong target name) |
| quick.yml 43214511990 / 43215392986 (Linux, verbose) | native_netradio, dtmb_faults | pass (figures above) |
| quick.yml 43218158764 (Windows, MSYS2 UCRT64) | dect2.exe with the folder dialog and onair.rc (GLFW_ICON); usb_fd | pass (built and linked; dialog and icon not seen on a screen) |
