# Channelised analog voice modes (3.8, branch feat38-channels)

Seven modes, one receiver (`core/src/chanvoice`), families 340 to 346 (engine standards 342 to 348, active standards 341 to 347):

| Family | Mode | What it does |
|---|---|---|
| 340 | CB radio | The 40 channel plan of the region (CEPT, the German / Czech / Hungarian 80, the UK CEPT + 27/81, the USA / Canada), every channel of the span at once. AM, FM, USB and LSB are decided from the signal on each transmission (or forced). |
| 341 | FRS / GMRS | Channels 1 to 22 with the CTCSS tone or DCS code read on each transmission; GMRS repeater outputs on 15 to 22 with their 467.550 - 467.725 MHz inputs listed. |
| 342 | PMR446 analog | Channels 1 to 16 (446.00625 + 12.5 kHz n), CTCSS / DCS per transmission. |
| 343 | Ham FM repeaters | The repeater outputs of the IARU region of the station (2 m and 70 cm) with their input offsets, the calling channels, the tone heard on each. |
| 344 | HFGCS / HF voice | USB on the four HFGCS primary frequencies, the MARS and Canadian Forces relays and the VOLMET weather broadcasts with their minute schedules. A scan steps the radio through the list. |
| 345 | MDC-1200 | Every FM carrier of the span is followed; the 1200 bit/s MDC-1200 burst (unit ID, op, argument, double packets) is decoded. |
| 346 | Wireless mic finder | Wide FM carriers in the UHF microphone bands of the region, with width, measured deviation and FM / not FM, and audio of the one picked. |

All of them have a busy-channel list (state, level, mode, tone, calls, total airtime, last heard), a call log, one WAV per transmission (optional), a squelch, a multi-channel scan (whole span: the first channel that opens plays and is held until quiet; HF: the scan asks the app to retune, one channel at a time) and the plan of the region. The region follows the setup country, else the receiver position, else the first plan; a manual choice is always there. Receive only.

## Where the facts come from

* CB: FCC 47 CFR 95 (40 channels 26.965 - 27.405 MHz, channel 9 emergency, 4 W AM / 12 W PEP SSB). Checked against the Wikipedia table: CEPT numbers 23, 24 and 25 as 27.235 / 27.245 / 27.255, FCC order 27.255 / 27.235 / 27.245; Germany, Czechia and Hungary add 40 channels at 26.565 - 26.955 MHz; the UK adds 27.60125 + 10 kHz (n - 1) FM.
* FRS / GMRS: the Wikipedia table (FCC rules of 2017): checked channel by channel, including the 467.5500 - 467.7250 MHz repeater inputs.
* PMR446: ECC Decision (15)05 (16 analogue channels from 2016), channel table from Wikipedia.
* HFGCS: 4724, 8992, 11175 and 15016 kHz and the MARS 13927 / Canadian Forces 11232 kHz relays, from the Wikipedia page. VOLMET: the public station list of dxinfocentre.com (2025-07): Shannon continuous on 3413 / 5505 / 8957 / 13264 kHz, RAF Military One 5450 / 11253 kHz, Gander minutes 20 and 50, New York minutes 0 and 30 on 3485 / 6604 / 10051 / 13270 kHz (5 minute slots; New York is listed out of service). The MWARA / ICAO HF ground frequencies are not included: the ICAO Doc 7030 tables were not available.
* Amateur: the IARU Region 1 / 2 / 3 band plans as implemented in `ham_voice.h`. The repeater lists are **band-plan grids** (Region 1: 2 m RV48 - RV63 at 145.600 - 145.7875 MHz, -0.6 MHz; 70 cm 438.650 - 439.425 MHz, -7.6 MHz. Region 2: 2 m 146.610 - 146.970 MHz -0.6 and 147.000 - 147.390 MHz +0.6 at 15 kHz; 70 cm 442 - 445 MHz +5 MHz. Region 3: Australia style, 146.625 - 146.975 MHz -0.6 and 438.025 - 439.975 MHz -5 MHz). **Not checked against the published band plans** (no network copy of them was used): the Region 3 grids, the UK 70 cm repeater block (the UK does not use the -7.6 MHz continental split and is not specially handled) and the exact edges of the Region 1 70 cm outputs. They are not a repeater directory: a repeater that exists on a grid frequency is found and its tone is read from the signal; one that is off the grid shows as an off-plan carrier.
* UHF wireless microphone bands: Europe 470 - 694, 823 - 832, 863 - 865 MHz; USA 470 - 608, 614 - 698, 902 - 928, 944 - 952 MHz. From memory of the regulations, not checked against the national tables.
* MDC-1200: the format (sync 07 09 2A 44 6F, 7 bytes of 0x55 before it, 112 bits in 7 rows of 16, parity taps 0 / 2 / 5 / 6, CRC-16 poly 0x1021 reflected with start 0 and final xor 0xFFFF, double packets for ops 0x35 and 0x55) was checked against Matthew Kaufman's public decoder. That code is GPL-2.0 only, which GPL-3 cannot take: **none of it is used**, the encoder and decoder here are written from the format. Credited in THIRD_PARTY.md and the in-app credits as format knowledge.

## What is not done

* **FleetSync** (Kenwood) is not decoded. There is no public specification; the only open implementation found is GPL-2.0 reverse engineering, which is not used. The MDC-1200 mode says so on screen.
* MDC-1200: only the PTT ID (op 0x01) is named; other ops are shown as numbers. The decoder drops a burst with a CRC error (no single-bit correction by the parity bytes). A clock error of more than about 0.5 % loses the burst.
* Wireless microphones: no expander for compressed (companded) audio, no pilot-tone display, no digital system decoding. FM / not FM is the envelope (constant, with a deviation above 3 kHz) of the carrier; an unmodulated mic shows as not FM until it is modulated.
* CB: an unmodulated carrier looks the same in AM and FM and plays silence. SSB is demodulated against the nominal channel frequency (a transmitter more than about 1 kHz off gives a shifted pitch; the sideband comes from where the energy is).
* DCS: an inverted code is the same waveform as another normal code (023 inverted is 047). The tone column shows the first reading and the alias is kept in the telemetry.
* The UI text is wrapped in `tr()`; the translation catalogs have no entries for the new strings (they show in English).

## What was validated

All with **simulated signals only** (the generator in `chv_gen.cpp`): no recording of any of these services was available. The generator and the decoders were written by the same hand, so agreement proves the plumbing, not that a real radio sounds the same.

* `test_chanvoice_plan`: every plan number against the sources above, lookups, schedules, country and position to region.
* `test_chanvoice_subtone`: all 50 CTCSS tones (+0.2 % frequency error, speech at 0.35 over them), four deviations 150 - 700 Hz, 18 DCS codes in both polarities (either reading or its alias), no false tone from 20 speech / noise seeds.
* `test_chanvoice_mdc`: 120 bursts with noise 20 dB to 0 dB (116 decoded), 40 clean ones (all), 8 / 16 / 48 kHz audio, clock errors of +-0.1 % and +-0.5 %, double packets, two bursts around speech, no packet from 12 speech seeds.
* `test_chanvoice_rx` (receiver): CB with AM, FM, USB and LSB on four adjacent channels at once (two noise levels), FRS CTCSS and DCS and three calls on one channel, PMR446 CTCSS and inverted DCS on the 6.25 kHz grid, ham RV49 / RV56 and a Region 2 repeater with the right input offsets, MDC-1200 on two carriers (unit IDs and carrier frequencies), HFGCS USB with the HF single tuner scan, the whole-span scan holding the first channel, an adjacent channel 30 dB down, a sweep of 5 carrier offsets (-1.2 .. +1.2 kHz) x 3 levels (carrier to noise in 10 kHz about 33 to 17 dB) x AM and FM (30 of 30), the wireless mic finder (three wide FM carriers and a narrow AM one, deviation read), and the built-in demonstration scene of every service in every region.
* `test_chanvoice_mode` (app, built with the app tests): the registered tunings, mode rows, family count, screen mapping and engine cases agree.

Not tested: the GUI (no app build on this machine, only `-fsyntax-only` of the new sources and the touched engine files), the engine path end to end, a real radio.

## Integration notes

* Poll slot 218 is used for the shared receiver (`seqs[218]` in `engine_telemetry.cpp`); the integrator may need to move it if another branch took 218.
* `kNumFamilies` is 360 (the end of the block 340 - 359).
* `chv::Service` order is the family order; `ChanVoiceReceiver::setService()` switches the one receiver between them, as the mode changes.
* The Android generated files (`modes.xml`, `Modes.kt`, `results_generic_map.inc`, `mode_order.txt`) were regenerated with `android/tools/gen_modes.py` and `gen_results_map.py`.
