# Bandwidth and sample-rate limits: audit and what 3.4 unlocked

Owner's rule (2026-10-28): the default stays the default, but the user has full control of the bandwidth and the sample rate in every mode. A limit may only warn; the radio's driver decides what it can really do, and the app shows its plain error and keeps the previous setting if it refuses.

Audited: the shared control code (top bar, rate list, saved rates, sources), the 165 mode-list entries (app/ mode panels, core/ receivers and engine standards, `xxxTuning()` rows), and every radio source. About 115 limits were found. This file is the table of what each one was and what happened to it.

## How the controls work now

- **Advanced row, every mode** (top bar "decoder" row or the side panel's Decoder section): `Bandwidth: 1.8 MHz (standard)` with a **Custom** tick, and `Sample rate: 2 Msps (standard)` with a **Custom** tick. Unticked, the mode runs with its own values exactly as before. Ticked:
  - Bandwidth: a list (0.1 to 20 MHz) or any typed width (`250k`, `1.5`, `12 MHz`). The plan (app/state/rx_width.h) gives the analysis passband, the radio's analogue filter and the sample rate that carries it (width / 0.9, rounded up to 0.25 Msps, never below the decoder's own floor). Nothing is clamped.
  - Sample rate: the rates the radio lists plus **Other…** for any typed rate. A typed rate goes to the driver as typed.
- **Warnings, not refusals.** A rate outside the radio's list, in a gap between its ranges, or below what the decoder needs gets a yellow `!` with the reason (`core/src/engine/rate_choice.cpp: checkManualRate`). Only a value that is not a rate (0, NaN) is rejected.
- **The driver has the last word.** If the radio refuses the rate or width, the app shows the driver's own message (`Engine::startError()`), puts the previous setting back and restarts. A rate the user chose is no longer held down by a source's own safety cap (`TuneSettings::rateChosen`): RTL-SDR (2.56 Msps and the 3.2 Msps refusal), rtl_tcp, PlutoSDR, SDRplay, bladeRF, LimeSDR, USRP, nRSP-ST. The automatic rate keeps those caps, so nobody's default changes.
- **Saved per mode** (`rxbw.<mode>` and `rxbw.<mode>.custom`; `rate.<mode>.<radio>`). Someone who saved nothing sees no change. VDL2, TETRA and TETRAPOL keep their 3.2 keys. The test signal keeps a chosen rate too (`rate.<mode>.synthetic`).

## Table

"Unlocked" = the limit is gone or only warns. "Documented" = it stays, with the reason.

### Shared

| Mode | Limit | Where | What changed |
|---|---|---|---|
| all | A typed rate outside the radio's list, in a range gap, or below the mode's need was refused | rate_choice.cpp `checkManualRate` | Unlocked: warns, goes to the driver |
| all | A saved rate that failed that check silently fell back to Auto | app_state.cpp `applyBandwidth` | Unlocked: used as saved |
| all | Rates below the mode's minimum were hidden from the rate list | toolbar_source.cpp | Unlocked: listed, with a warning when picked |
| all | Remote API refused a rate outside the radio's min / max | remote_api_app.cpp `setSampleRateHz` | Unlocked: passed on; a driver refusal comes back as the error |
| all | No way to see why a radio refused a rate | engine_start.cpp | New `Engine::startError()`; the app shows it and reverts |
| all | Bandwidth fixed by the mode, no control in most modes | app_state.cpp `applyBandwidth` (the `modeTuning` override) | Unlocked: Custom bandwidth for every mode |
| all | Custom bandwidth only for VDL2, TETRA, TETRAPOL (3.2) | rx_width.h `rxWidthMode` | Unlocked: every mode and the six classic families |
| VDL2, TETRA, TETRAPOL | Width clamped to 0.05 - 2 MHz | rx_width.h/.cpp | Unlocked: any width |
| all | The test signal could not keep a chosen rate | app_state.cpp | Unlocked |
| RTL-SDR native | Rate held to 2.56 Msps; refusal above 3.2 Msps with "use a faster radio" | source_rtlsdr.cpp | Unlocked for a chosen rate; Auto unchanged |
| rtl_tcp | Same, 2.56 / 3.2 Msps | source_rtltcp.cpp | Unlocked for a chosen rate |
| PlutoSDR | Rate clamped to 2.1 - 61.44 Msps | source_plutosdr.cpp | Unlocked for a chosen rate |
| SDRplay | Rate clamped to 62.5 ksps - 10 Msps | source_sdrplay.cpp | Unlocked for a chosen rate |
| bladeRF, LimeSDR, USRP, nRSP-ST | Rate clamped to the board's range | source_bladerf / limesdr / usrp / nrspst.cpp | Unlocked for a chosen rate |
| HackRF | Listed as 2 - 20 Msps | rate_choice.cpp `rateLimitsOf`, hackrf_usb.cpp | The list stays (what the radio is specified for); a typed rate goes to libhackrf as typed |
| Airspy, Airspy HF+ | Discrete rates only (2.5 / 10 Msps; 192 - 768 ksps) | source_airspy / airspyhf.cpp | Documented: the hardware has no other rates; a typed rate rounds up to the next one and the app says "runs at" |
| KiwiSDR, Hamlib, sound card, SpyServer | 1.008 Msps; 48 kHz; 192 kHz; maxRate / 2^n | source_*.cpp | Documented: the stream's own format |
| recording | Plays at its own rate | toolbar_source.cpp | Documented: the file says it |
| Sweep, RFI | Rate combo {best, 10, 5, 2.4 Msps} | sweep_ui.cpp, rfi_ui.cpp | Documented: automatic sweepers; the radio's own rate setting is the Advanced row of the receiver modes |

### Modes with a carrier search or channel list (wide captures)

| Mode | Limit | Where | What changed |
|---|---|---|---|
| TETRA | At most 24 carriers per capture | tetra_rx.cpp `findCarriers` | Unlocked: all carriers |
| TETRA | Searched only the middle 72 % of the capture, fixed | tetra_rx.h `kUsableBand` | Unlocked: `setSearchBand()`, "searching the middle N %" in the panel; 72 % stays the default |
| TETRA | A mixer and filter over the whole capture per carrier: 24 carriers at 20 Msps took about 5 times real time | tetra_rx.cpp `channelize` | Unlocked: shared FFT channeliser (`channelizeMany`, lora::FdChannelizer) from 4 Msps and 3 carriers; 15x less CPU for 16 carriers at 10 Msps, measured |
| TETRA | Scan span presets only 390-395 / 410-430 / 450-470 | tetra_ui.cpp | Unlocked: "380 - 400 MHz (whole range)" preset; Custom range existed |
| TETRA | A scan step used 80 % of the sample rate | tetra_ui.cpp `makePlan` | Follows the search width |
| TETRA | The default test signal had one cell | tetra_gen.cpp | A capture of 4 Msps or more gets the wide scene (12 carriers, own levels and tuning errors) |
| DMR, M17, D-STAR, NXDN, dPMR, P25, pagers, packet, MPT1327, LTR, EDACS, Yaesu (Whole span banks) | "Carriers at most" slider 1 - 32 | modeui.cpp | Unlocked: Ctrl-click types any number; the CPU limit (default 80 % of real time) still drops the weakest carrier when the processor is short, as a warning-level safeguard |
| the same banks | Hard cap 8 carriers by default | wide_modes.cpp `cfgFor` | Documented: the default; raise it in the Whole-span settings |
| TETRAPOL | One carrier at a time (the decoder is one receiver; the carrier list holds one row) | tetrapol_rx.cpp | Documented: not changed. A wide capture shows every carrier in the spectrum; click one to decode it. A bank of receivers per carrier (as the banks above) is the way to add it |
| PSK / CW / HF digital | Whole span at most 250 kHz (`kWideMaxSpanHz`), 100 kHz for PSK, one decoder per 3.4 kHz | widespan.h, psk_rx.cpp | Documented: processor budget of one decoder per slice. Raising it is a matter of CPU, not correctness |
| PSK / CW / HF digital | At most 24 PSK signals | psk_tel.h | Documented: table size |
| ACARS | 8 carriers decoded at once (16 on a wide capture), 24 table rows | acars_rx.cpp | Documented |
| VDL2, HFDL, airband, AIS | Whole span already decodes every channel in the span | receivers | Nothing to unlock |
| Aero | 6 channels | aero_rx.cpp | Documented |
| GOES DCS | 8 slots | goesdcs_live.cpp | Documented |
| QO-100 | 12 slots in the panel, 24 in the core | qo100_ui.cpp, qo100_auto.cpp | Documented |
| Radiosonde | 8 sondes at once | sonde_rx.cpp | Documented |
| GNSS | 40 tracked satellites | gnss_rx_impl.h | Documented |
| Iridium voice | 100 calls, 8 active | iridium_voice.cpp | Documented |
| eLoran | 8 stations | eloran_rx.cpp | Documented |
| multi-channel TV | At most 8 receivers; refused when the capture holds fewer than 2 slots | engine_multitv.cpp | Documented: a slot is a whole receiver; the whole-span refusal says why |
| LoRa raw / LoRaWAN / mesh | Channels that do not fit the capture are dropped | lora_bank.cpp | Documented: physics (they are outside the capture) |

### Single-value controls

| Mode | Limit | Where | What changed |
|---|---|---|---|
| PSK analyser | Channel bandwidth slider 2.5 - 200 kHz, clamped; core clamp 2.5 kHz - 50 MHz | pskdsp_ui.cpp, pskdsp_rx.cpp | Unlocked: Ctrl-click types any width; core keeps only a 100 Hz sanity floor |
| FM | Channel filter 100 - 300 kHz | fm_rx.h/.cpp, prefs.cpp, plots.cpp | Unlocked: any width from 10 kHz, bounded by what the sample rate carries |
| DVB-C | Symbol rate list only | dvbc_ui.cpp | Unlocked: free entry in Msym/s |
| DVB-T / T2, ATSC, DAB, ISDB-T, FM, DTMB, analog TV | Channel width from a fixed list or a fixed value | app.h `kBw`, app_state.cpp | Unlocked through Custom bandwidth (capture bandwidth, rate and radio filter). The receiver's own channel-width choice (8 / 7 / 6 MHz etc.) stays beside it. DVB-T / T2 "Auto" bandwidth detection is on when Custom is off |
| Pager, packet / APRS | Channel filter 12.5 or 25 kHz | pager_ui.cpp, packet_ui.cpp | Documented: decoder channel filters, unchanged; the capture bandwidth is now free |
| LoRa raw, LoRa sat | LoRa chirp bandwidth from a list | loraraw_ui.cpp, lorasat_ui.cpp | Documented: the chirp bandwidth is a protocol parameter, not a capture limit |
| AM | 2.5 - 10 kHz audio filter | am_proc.cpp | Documented: audio filter |
| FM, CQUAM, Olivia | Audio bandwidth lists | fm_ui.cpp, cquam_ui.cpp, hfdig_kbd_ui.cpp | Documented: audio filters, not RF |
| HaLow, mesh 2.4 GHz LoRa, GNSS, HD Radio AM, Meteor, AIS pairs, sondes L-band | A fixed rate for the tune | app_state.cpp | Default unchanged; Custom bandwidth / rate override them |

### Decoder floors (documented, warn only)

About 110 receivers return "not ready" when the rate is below `minSampleRate` (for example ISDB-S 39 Msps, DVB-C 7 Msps, analog TV 7.9 Msps, VDL2 and TETRA a few hundred ksps). That is the decoder's arithmetic (a symbol cannot be recovered from fewer samples than it needs), not a limit on the user: the rate is sent to the driver anyway, the top bar warns `below N Msps this decoder may not work`, and the receiver's own log line says why it idles. Test-signal generators (AIS 0.2 - 21 Msps, GNSS 2 - 21, DAB, DMR 1 - 20.5, ATV 1 - 64, FPV 16) refuse rates outside their design range; real radios are not affected.

## Wide captures that decode every carrier (measured)

TETRA on a HackRF-like wide capture (tests/tetra/test_tetra_wide.cpp, synthetic: carriers 11.5 dB apart, each with its own tuning error up to 4 kHz, in noise at 18.5 dB for the weakest):

| Sample rate | Carriers in the capture | Found and decoded (cell identity, SYSINFO, main carrier) | Time to decode one 2.3 s capture |
|---|---|---|---|
| 5 Msps | 8 | 8 | 0.3 s |
| 10 Msps | 16 | 16 | 0.5 s |
| 20 Msps | 24 | 24 | 0.8 s |

(M-series laptop, one capture of 2.3 s of signal; the decoder runs on its own thread.) The shared channeliser cuts 16 carriers out of 1.2 s of a 10 Msps capture in 0.13 s, against 2.03 s one by one (15x). Memory: a capture is 2.3 s of 8-byte samples, 0.4 GB at 20 Msps (two buffers while one is decoded).

At 20 Msps the default search (the middle 72 %) covers 14.4 MHz; the whole 380 - 400 MHz range is then two steps (the Scan button does them), or raise "searching the middle N %" if the radio's passband is flatter. How flat a real HackRF is at 20 Msps was not measured here.

Needs a user test: a real wide capture. The test signals are synthetic.
