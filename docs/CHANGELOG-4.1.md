# OnAir 4.1

OnAir 4.1 adds 5 new receivers: 393 entries in MODES.md in total, 391 in the app's mode list (it has no row for Plugins or Mode Builder). It adds support for the Aaronia SPECTRAN V6 and a native SDRplay RSP1 / MSi2500 driver, RX888 MkII rates up to 130 Msps, a map layer for your own position, CPU savings in many modes, better sensitivity in more than a dozen receivers, a sensitivity table for every mode, diagnostic codes in log messages to make bug reports easier to trace, and the fixes that were planned as a separate fix release after 4.0; they are part of 4.1.

**Measured** numbers come from OnAir's own generated test signals, not from over-the-air recordings, unless a line says so. Read **Known limits** at the end before relying on a new feature.

## Highlights

- 5 new receivers: Golay paging (GSC), STL link, ESP-NOW, SCADA radio telemetry (Modbus RTU, DNP3) and Robust Packet (RPR, physical layer).
- Support for the Aaronia SPECTRAN V6 / V6 ECO (receive and transmit), a native driver for the SDRplay RSP1 and Mirics MSi2500 radios, RX888 MkII rates up to 130 Msps.
- Sensitivity: a measured table of every mode, and lower thresholds for CubeSat links, e-Czas, VDL4, KLW, TDCS, ATSC 3.0, DRM and more.
- Your own position on every map, optional range rings, and distance and bearing to the pointer.
- Lower CPU use in about 20 modes with identical decodes.
- Diagnostic codes in log messages, to make bug reports easier to trace; a bug report now also names an old crash file by the version that wrote it.
- Fixes from your reports (#496, #495, #491, #488, #470, #436 and more), including the ones planned as a fix release after 4.0.
- Robust Packet (RPR), new: finds RPR bursts, the speed level and the symbol clock on HF; the frames are not decoded yet (the coding is not public; a recording is needed).
- Your own picture, video and sound in the synthetic test signal of the TV, analog TV, FM, AM and SSTV modes (DAB, DRM, HD Radio and ATSC 3.0 keep the built-in content: OnAir has no encoder for them).
- VDL2 ground stations stay where they are: a position needs two agreeing reports and a corrupted one is ignored; they are named after the nearest airport; the map has Ground stations and Coverage switches.
- Clearer AM: the noise reduction leaves much less "warbling" (musical noise index 1.57 to 0.98 on generated signals) and the leveller no longer pumps.
- Workspaces remember the window size, position and maximised state.
- Radiosonde upload: the station beacon uses the dish symbol and says "tracker", so radiosondy.info lists the receiver; frames SondeHub refuses are counted as refused and its reason is shown in the feed status.
- Discord requests: SITOR keeps its text across retunes and has Save and Clear; FM smart checks sit below the RDS details; WEFAX hand phasing and the JFX Kagoshima station; a Pluto+ RX1/RX2 and TX1/TX2 choice (not yet tested on hardware).

## Sensitivity

Every figure is from simulated signals (see the docs named in the last column). "Before" is the 4.0 receiver on the same signal and seeds.

| Receiver | Change | Result | Details |
|---|---|---|---|
| CubeSat narrow channels | differential detectors beside the existing ones | about 2 dB; AX100 Mode 6 unchanged | docs/SENSITIVITY-4.1-ALL.md |
| e-Czas | coherent detector beside the phase-step one | 18 to 16 dB | docs/SENSITIVITY-4.1-ALL.md |
| VDL4 | Gaussian channel filter, 1-bit differential detector, sync fit from the training sequence | 16 to 14 dB; a further 4 to 6 dB at the first measurement | docs/SENSITIVITY-4.1-ALL.md |
| KLW, TDCS | gap fixes found with the sensitivity table | 4 to 6 dB | docs/SENSITIVITY-4.1-ALL.md |
| ATSC 3.0 | channel state through the convolutional time interleaver | about 1 to 2 dB in a Rayleigh channel | docs/SENSITIVITY-4.1-TV.md |
| DRM | common phase error correction (opt-in, `DECT2_DRM_CPE=1`) | with 1 Hz phase noise, 64-QAM 28 to 24 dB, 16-QAM 12 to 10 dB | docs/SENSITIVITY-4.1-TV.md |
| MRZ radiosondes | both halves of every Manchester bit | about 1 to 1.5 dB | docs/SENSITIVITY-4.1.md |
| Zigbee | second choice of the least sure symbols when the FCS fails | about 0.5 dB | docs/SENSITIVITY-4.1.md |
| Aero | band and carrier search every 0.25 s at the start | 3 s trials at 3 dB: 0 to 100 % decoded | docs/SENSITIVITY-4.1.md |
| FM | threshold options (see the page); the NFM PLL demodulator was measured worse and is not the default | | docs/SENSITIVITY-4.1.md |

- **A table of every mode** (docs/SENSITIVITY-4.1-ALL.md): each mode's threshold in AWGN, with a clock and carrier error, and off centre, from its own test signal, with the gap to theory where a theory figure exists. It also shows where more work is needed.
- **Measured and not kept**: ATSC 1.0 in TU6 (no gain; two attempts reverted), ISDB-T first-order inter-carrier interference removal (the limit at 50 to 70 Hz Doppler is the pilot interpolation), and a 50 ppm clock penalty in DVB-T, DVB-T2, DTMB and ATSC that turned out to be a test artefact, not a receiver loss.
- **Fixes with the work**: the FSK modem and DCR weak-signal gates, the Iridium repair, an FT8 / FT4 CQ shape check (fewer wrong texts), Wi-Fi 802.11b DSSS timing at resampled radio rates (24 to 26 Msps).

## New modes

- **Golay paging (GSC)**: GSC pager messages (docs/GSC-4.1.md). Simulated signals only.
- **STL link (studio-to-transmitter)**: composite or mono analogue studio-to-transmitter audio, with automatic channel width and centre (docs/STL-4.1.md). Simulated signals only.
- **ESP-NOW**: ESP8266 / ESP32 packets on a 2.4 GHz Wi-Fi channel on the Wi-Fi demodulator, devices and payloads (#500). Receive only; needs 20 MHz of bandwidth.
- **SCADA radio telemetry (Modbus, DNP3)**: Modbus RTU and DNP3 over utility data radios with AFSK, 2-FSK and 4-FSK (docs/SCADA-4.1.md). Receive only; check local law. Simulated signals only.

## New features

- **Devices**: the Aaronia SPECTRAN V6 / V6 ECO, receive and transmit, through Aaronia's RTSA API (loaded at run time, not shipped; docs/SPECTRAN-V6.md). A native SDRplay RSP1 / Mirics MSi2500 driver over USB on desktop and Android (#503). RX888 MkII sample rates above 64 Msps, up to the ADC clock (docs/RX888-RATES-4.1.md). The Pluto+ RX and TX antenna port can be chosen.
- **Maps**: your own position on every map with optional range rings, and distance and bearing to the pointer (docs/MY-LOCATION-MAPS-4.1.md); the aircraft card on the VDL2, HFDL and ACARS maps (#394); a TETRA map from LIP location reports (#505); one home marker per map; map labels hide when zoomed out; the OpenStreetMap tile policy is followed.
- **TV**: picture aspect ratio for analogue TV (#504); play of North Korean muxes with inverted PIDs and T2-MI recordings; Android TV additions: analogue TV on the phone, a full-screen player and a larger touch control size (docs/ANDROID-PLAYER-4.1.md).
- **Satellites and HF**: Meteor AVHRR 221 false colour (#499); your own frequencies in the band hopping of the FT modes (#501); VHF wireless microphone bands in the microphone finder (#487).
- **Video generator** (by peterdewitte): an RTMP input for OBS Studio, so OBS streams picture and sound to `rtmp://<this PC>:1935/live` without a virtual camera or audio cable (#493, #490); several video generators as the programmes of one multiplex (#494, #492).
- **OOK**: an on-time / off-time scatter plot for OOK bursts (docs/OOK-SCATTER-4.1.md).
- **Mode Builder**: a Signal tab with a histogram and a PSK constellation (docs/MODEBUILDER-PLOTS-4.1.md).
- **Sensors**: iMet-54 and MRZ soft decisions; Sub-GHz sensors report at once and get a band selector; the band plan, region and channel pickers of the IoT modes retune the receiver.
- **Install**: the Nix flake is brought up to date (docs/NIXOS.md); SPDX and copyright headers on the OnAir-owned source files.

### Requests from Discord

- **SITOR**: the text is kept across retunes, with Save and Clear buttons.
- **FM**: the smart checks are shown below the RDS information.
- **WEFAX**: a hand phasing slider with Auto and Reset, and JFX (Kagoshima) in the station list.
- **Pluto+**: a choice of the RX and TX antenna port. This is **not tested on a Pluto+**: it was only checked against the driver's own tests.
- **Bug reports**: an old crash file is named by the version that wrote it, so a report does not blame the running version for it.

## Performance

CPU seconds per second of signal, measured on GitHub runners with the same decodes before and after (docs/CPU-4.1.md has the table of every mode and the figures).

- **KAVB**: about 40 % less CPU.
- **BGAN** pilot search: about 50 % less. **ISM sensors and Sigfox**: about 14 % less.
- **NPR, ExpressLRS, DVB-C, DVB-C2, DVB-SH, SD3, Wi-SUN, ISDB-S, GOES**: cheaper searches, filters and Viterbi loops.
- **RX888**: a plain float mixer for the undecimated rates.
- Some changes were measured, did not help, and were taken back (pipelined loops in the weather-satellite and X-band receivers, the in-order FIR sums).

## Interface

- Font sizes were scaled twice at UI scales above 100 % (found in #502); footer text, the Passes header, the Controls table, the Data packs table and the credit line no longer clip or run off the edge.
- Calmer, evenly spaced mode group colours; the slider grab is darker than the value drawn over it; auto-height panes no longer cut off a row; the Simple / Advanced switch is centred on the mode action row; the menu bar's right-hand items are centred vertically.
- The WEFAX phase slider scales with the UI.
- The Simplified Chinese catalogue is complete and reviewed (4516 entries, by thalorex, #515).

## Fixes

- **#496** OnAir Web did nothing when Play was pressed on a server started without `--autostart`. A listener who may tune now starts the receiver (setting `web_listener_start`, on by default); a failed start shows the radio's reason to every listener, and the page tunes again when the first samples arrive.
- **#495** Hamlib was missing from the Windows release. Hamlib is now loaded at run time (`libhamlib-4.dll` on Windows, searched in the program folder, the codecs folders and PATH); without the DLL the "Add Hamlib rig..." window says what to copy where. OnAir does not ship the DLL.
- **#491** On a fresh install the first-run tour and "Get me listening" opened at once and the tour could not be skipped. Only one guide is in front at a time now.
- **#488** "Hear this channel" kept playing the old frequency after a click on the spectrum or a frequency typed inside the captured band. The audio monitor now follows the decoder.
- **#470** Raspberry Pi 5 drew with llvmpipe (software). On Linux, when desktop OpenGL gives a software renderer and a GPU exists, OnAir tries OpenGL ES 3.0 through EGL and keeps it when it reaches the GPU. `ONAIR_GL=desktop` or `ONAIR_GL=gles` forces one (docs/RASPBERRY_PI.md).
- **#436** Full RDS on Android and desktop; the GPU in use is named in Settings; the rtl_tcp reader thread has a higher priority.
- **Linux start-up errors**: launched from the menu (Linux Mint), a fatal start error only went to the terminal. It now shows a dialog and writes `~/.config/onair/startup-error.txt` (`ONAIR_NO_DIALOG=1` turns the dialog off).
- **Airspy HF+**: a sample rate too wide for the radio stopped the stream; the radio now keeps streaming at the rate it had and says why the rate was refused.
- **COSPAS-SARSAT**: the false-burst search could use more than its processor budget in a storm of steady tones.
- **Sub-GHz and IoT**: band plan and channel changes retune the receiver; Sub-GHz sensors report at once.
- **Mirics**: one libusb context in the driver. **Network**: an unread request is drained before a refused connection closes.
- **Wi-Fi**: 802.11b DSSS demodulation starts on the correlation peak and tolerates a symbol of timing error; fixes at 24 to 26 Msps.
- **Wide capture**: a pinned or listened-to entry is never left dropped.
- **Zigbee**: the BPSK frame path is kept as it was. **ISDB-T, DRM**: corrections in the new estimators (see Sensitivity).
- **CubeSat**: one Mobitex or AX100 Mode 6 frame heard by several demodulators counts once.
- **VHF wireless microphones**: a narrower channel filter so a microphone 50 kHz away does not beat with the wanted one.
- **Maps**: VDL4 view selector, the OpenStreetMap tile policy, labels at low zoom.
- **Diagnostic codes**: log messages carry diagnostic codes to make bug reports easier to trace; the bug report names an old crash file by its version.

## Contributors

Pull requests merged since 4.0:

- **#502** footer text clipped at UI scales above 100 %: m6c7l (the same fix also corrected font scaling in 17 more places).
- **#509** DTMB: the carrier mode is selected from the system information; **#510** DTMB: counters can no longer overflow during continuous reception; **#511** DVB / ISDB: broadcast text uses per-stream encoding contexts: gfdfdxc.
- **#515** Simplified Chinese catalogue, complete and reviewed: thalorex.
- **#493** RTMP input and **#494** multi-programme multiplex of the video generator: peterdewitte.

Thanks to everyone who reported the issues named above, and to the Discord users whose requests are listed under "Requests from Discord".

## Known limits

Please read these before relying on a new feature.

- **Almost everything new was tested on simulated signals only.** This holds for all 5 new receivers, every sensitivity figure above, the CPU figures (measured on GitHub runners) and the new maps.
- **Not tested on hardware**: the SPECTRAN V6 driver (against a fake RTSA API only), the native RSP1 / MSi2500 driver, RX888 rates up to 130 Msps (against a fake FX3 only; USB 3 and a good cable are needed), the Pluto+ port choice, and the Android TV player additions.
- **#470 (GLES fallback)** was not run on a Raspberry Pi: whether EGL reaches the GPU is not known. If `eglinfo -B` also shows llvmpipe, the cause is outside OnAir.
- **#495 (Hamlib DLL)** was not tried on a Windows PC or with a real rig; the tests use Hamlib's dummy rig.
- #496, #491 and #488 were checked with tests on simulated signals and on GitHub runners, not on the reporters' hardware.
- **DRM common-phase correction** is off by default because one DRM test (`drm_smooth`, mode E, ITU channels 5 and 6) fails with it.
- **ESP-NOW**: long OFDM frames with a fast sender clock are a known failure of the test.
- **ISDB-T Doppler, ATSC 1.0 in TU6**: no gain was found (see Sensitivity).
- **Tests**: some tests on the Windows CI shards still have load-dependent timing flakes, as before.
