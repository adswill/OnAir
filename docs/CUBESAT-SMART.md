# Smart CubeSat decoder

One button, "Auto decode cubesats" (CubeSat telemetry mode, view "Smart"). It works out which satellites are about to pass, tunes a span that holds
their downlinks, follows the Doppler shift, finds every narrow carrier in the span, identifies its modulation and baud rate, and decodes it with every
framing that fits. A frame counts only when a check passed (FCS, Golay, Reed-Solomon, Viterbi + Reed-Solomon, Morse structure). Nothing is uploaded.

## What it does

1. **Satellite knowledge** (`core/src/cubesat/cubesat_db.*`). A snapshot of 572 transmitters of 430 satellites (VHF/UHF, 130 to 480 MHz, alive and
   active in SatNOGS DB, 2026-11-02) and their TLEs (CelesTrak). `tools/gen_cubesat_db.py` rebuilds `cubesat_db_data.inc`. "Refresh orbits" in the view
   downloads current TLEs (the pass planner's CelesTrak download); the user's location is the receiver position of the ADS-B / pass planner settings
   (Lithuania by default for this owner). Advanced: add own transmitters (stored in the settings).
2. **Passes** (`PassPlanner`, SGP4 of `geo/passes.h`). Next passes of satellites with a decodable transmitter; `adviseTune()` picks the centre and
   span that cover the most satellites about to pass (weighted by peak elevation), and the button tunes it before the receiver starts (the capture rate is
   raised to fit, up to 10 Msps). The radio is not retuned during a pass: the Doppler shift moves a channelizer inside the wide capture.
3. **Blind scan** (`cubesat_blind.cpp`). Averaged spectra (about 0.25 s), noise floor per segment, groups of bins above it, merged, tracked over time;
   a carrier needs 4 estimates in a row. Table satellites are searched for around the predicted carrier (default +-5 kHz, TLE and clock errors).
4. **Classifier**. 1.5 s of the carrier at 96 kHz: occupied width and tonality, envelope variation (keyed carrier), tones at 1200 / 2200 Hz in the
   discriminator (AFSK), a line at twice the carrier in the square of the signal (BPSK), two lines a baud apart (MSK / GMSK), and cyclic lines of the
   discriminator and envelope for the baud rate (300 to 19200 baud). It gives a ranked list; the table's own hypothesis runs from the first sample.
5. **Chains** (`cubesat_chain.cpp`), all running side by side on one carrier, each reporting how many frames it heard and how many validated:

   | modulation | baud | framings |
   |---|---|---|
   | FSK, GFSK, GMSK, MSK | 1200 to 19200 (any value under Advanced) | AX.25 behind the G3RUH descrambler, AX100 Mode 6 (Reed-Solomon in HDLC), plain NRZI AX.25, AX100 Mode 5 (ASM + Golay + RS), CCSDS ASM + RS(255,223), ASCII text |
   | AFSK Bell 202 | 1200 | AX.25 |
   | BPSK, differential | 1200 to 9600 (any value) | AX.25 (NRZI, and G3RUH), AX100 / CCSDS ASM, ASCII, **AO-40 FEC** (FUNcube-1 / AO-73, Nayif-1, FUNcube-2) |
   | CW / OOK | keying speed measured | Morse |

   LoRa satellites are listed in the table but decoded by the separate "LoRa satellites" mode; the Smart decoder does not run LoRa. CCSDS
   concatenated (convolutional + RS) framing and the USP, TUBiX10 and DOKA framings of some table entries are not decoded (the carrier is tracked and
   shown, and the table marks the entry as not decodable).
6. **Telemetry** (`cubesat_telem.*`): FUNcube-1 / AO-73, FUNcube-2 (UKube-1) and Nayif-1 realtime frames as named fields with the data warehouse
   conversions (temperatures, RF power, antenna state); CSP headers; AX.25 and ASCII text beacons; CW text. Satellites without a parser still show the
   validated frame, its header fields and a hex dump.
7. **Exports**: a SatNOGS-ready file (one JSON telemetry record per line: NORAD id, source, timestamp, hex frame, locator, version), CSV, and KISS
   (AX.25 frames without FCS, others as the data field) in the data folder `cubesat/`. The program never uploads.

## The view

Simple: the button, the state in words (searching / carrier found / decoding), the next passes, the carriers (frequency, satellite, modulation, SNR,
status, packets; open a carrier to see every trial and its counts), the packet log with telemetry table and hex, Export / Clear / Refresh orbits.
Advanced (collapsed): follow the table or not, blind scan on or off, minimum SNR, carriers followed at once, lowest elevation, search window,
modulation and baud rate override for unknown carriers (any value), channel width (any value), scan resolution, own transmitters. Defaults unchanged.

## Verification state

| what | how | state |
|---|---|---|
| AO-40 FEC | the real FUNcube recording of the gr-satellites tests (`tests/data/ao40`): symbols, frame window, frame; also encode/decode, noise, hunter in both polarities | passes |
| FUNcube telemetry | the frame decoded from that recording: AO-73, WO10, battery 8140 mV | passes |
| table and passes | 850 passes above 10 degrees in 12 h for Vilnius, Doppler +-10 kHz at 436 MHz | passes |
| chains | `test_cubesat_chain`, 24 cases: synthetic bursts at 96 kHz (FSK 1200 to 19200, AFSK, BPSK 1200 / 9600 with AX.25 / AO-40, CW 12 to 25 wpm; Mode 5, Mode 6, CCSDS, text) with offsets up to 1.5 kHz, Doppler ramps, 300 ppm clock error, noise | all pass (macOS arm64 local, 2026-11-03) |
| controller | `test_cubesat_smart`: FSK 9600 with a Doppler ramp, BPSK 1200 AO-40 with 100 ppm and CW in one 960 kHz capture, blind: scan, classifier, chains, telemetry | passes (local); the classifier names the BPSK 1200 carrier "BPSK 2400" first, the AO-40 chain that runs beside it still decodes |
| sensitivity | `test_cubesat_sens`, carrier to noise in 10 kHz where 2 of 3 trials decode 3 of 4 frames, 2 dB steps: AFSK 1200 16 dB, FSK 9600 G3RUH 20, FSK 4800 G3RUH 22, GMSK 9600 AX100 Mode 5 16, BPSK 1200 AX.25 0, BPSK 1200 AO-40 FEC -2 (lowest tested), CW 18 wpm 0 | measured on the synthetic generator only |
| real satellites | **never**: no pass has been recorded; every signal above is synthetic | not verified |

Known weaknesses: the CW keying detector needs about 0.5 s to warm up and loses the first characters of a message that starts right then; the classifier's baud estimate is coarse (the demodulators are run for several candidates, the framing check decides); the FSK chains have no AFC (a carrier offset above about 1.5 kHz is lost); no LoRa, no CCSDS concatenated framing.

The synthetic generator (`cubesat_smartgen.*`) shapes pulses with a Gaussian filter and mixes in offsets and noise; a real transmitter's pulse shape,
phase noise and Doppler rate will differ. Treat the sensitivity numbers as properties of this receiver on this generator.

## Credits and licences

* SatNOGS DB (db.satnogs.org), transmitter table, CC-BY-SA 4.0: the generated table `cubesat_db_data.inc` is derived from it and is credited here, in
  THIRD_PARTY.md and in the program's Credits window. The table file may be reused under CC-BY-SA 4.0.
* gr-satellites (Daniel Estevez EA4GPZ, GPL-3.0-or-later): the AO-40 FEC chain and the FUNcube telemetry were re-implemented in C++ from its
  `ao40_fec_deframer.py`, `matrix_deinterleaver_soft`, `distributed_syncframe_soft` and `funcube.py`; its real-recording test vector is in
  `tests/data/ao40` with its notice. GPL-3 to GPL-3, compatible.
* CelesTrak (T.S. Kelso): TLEs, downloaded at run time (a snapshot is built in).
* Reed-Solomon, Golay, AX.25 and G3RUH code are the project's own (`cubesat_proto`, `packet_ax25`).
