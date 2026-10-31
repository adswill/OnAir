# ADS-B / Mode S (1090 MHz)

Status: receiver, test signal, tool and tests are done and delivered; the interface (`app/adsb_ui.cpp`) is a later phase. Everything here was checked on
synthetic signals and on published example messages. **No real recording has been decoded yet**: what the receiver does with real traffic, real radios
and real garble is not known beyond what the generator imitates.

## What it does

Decodes Mode S replies and ADS-B extended squitters from complex baseband around 1090 MHz at any sample rate from 2 Msps up (2, 2.4, 3.2, 4, 5, 6, 8, 10,
12.5, 16 and 20 Msps are tested, odd chunk sizes too). The engine standard code is 13, the class is `AdsbReceiver`, the report is `AdsbTelemetry`
(`adsb_tel.h`): aircraft table, message monitor (last 64 messages), statistics.

| layer | file | what |
|---|---|---|
| message | `adsb_msg` | CRC-24, bit fields, altitude and identity codes, CPR, message decoding, Comm-B inference |
| frame / table | `adsb_track` | DF and CRC and address checks, error correction, aircraft table, expiry, statistics |
| pulses | `adsb_demod` | magnitude, noise floor, preamble search, bit slicing at any rate |
| receiver | `adsb_rx` | the class the engine calls, 4 reports a second, setters |
| test signal | `adsb_encode`, `adsb_gen` | message encoders; a simulated airspace and the radio channel |
| harness | `adsb_sim` | generator -> impairments -> receiver, with every transmission checked off |
| tool | `tools/adsbtool.cpp` | `gen`, `rx` (a recording), `sim`, `sweep`, `decode` (one message taken apart) |

### Supported

- Formats: DF0, 4, 5, 11, 16, 17, 18 (TIS-B / ADS-R, control fields 0, 1, 2, 3, 5, 6), 19 (application 0), 20, 21. DF24 (ELM) frames of known addresses are
  accepted and counted, their content is not decoded.
- Extended squitter: TC 0 (altitude, no position), TC 1-4 identification (callsign, category), 5-8 surface position (movement, track), 9-18 airborne position (barometric altitude, Q bit or
  Gillham), 20-22 position with GNSS height, 19 velocity (subtypes 1-4: ground speed with track, airspeed with heading, supersonic variants, vertical rate,
  GNSS minus barometric difference), 28 aircraft status (emergency state, squawk), 29 target state and status (subtype 1), 31 operational status
  (version, NACp, SIL).
- DF4 / 5 / 20 / 21: altitude code (13 bit, M and Q bits, Gillham), identity code (squawk), flight status (alert, SPI, on ground). Comm-B in DF20 / 21: BDS
  2,0 (callsign), 4,0 (selected altitude, baro setting), 5,0 (roll, track, ground speed, TAS), 6,0 (heading, IAS, Mach, vertical rates), by the plausibility
  checks of the Riddle's BDS inference chapter; a field that fits two formats is left alone.
- CPR: global decoding of an even / odd pair (both within 10 s), local decoding from the aircraft's last position (within 30 s) or, for a first frame, from
  the reference position (`setReference`, marked "not confirmed" until a pair confirms it). Surface positions need the reference or an earlier fix. A position
  that jumps farther than 1800 kt allows is dropped; the third in a row is believed.
- Checks: CRC-24 of DF11 / 17 / 18 / 19 must be 0 (DF11 also accepts a remainder below 128, an interrogator code, from a known address). Address / parity
  formats (DF0, 4, 5, 16, 20, 21, 24) are accepted only when the address recovered from the parity belongs to an aircraft heard in a DF17 or DF11 within the
  expiry time, so noise does not make aircraft. Such a reply must also fit the aircraft: flight status 0 - 5, a level within 25 dB of the aircraft's mean,
  an altitude within 5000 ft of its last ADS-B altitude when that is under 30 s old; a reply that changes the squawk has to come twice within a minute.
  DF24 - 27 only (bit 3 of an ELM is a spare 0). Error correction: one flipped bit of a DF11 / 17 / 18 from a known address (default), or two among the
  eight least certain bits (`setCorrection(2)`).
- Aircraft table (keyed by 24 bit address, at most 2000 kept, 128 listed per report): callsign, category, squawk, barometric altitude, GNSS height, speed
  (ground / IAS / TAS), heading or track, vertical rate, GNSS-baro difference, position with how it was decoded, distance and bearing from the reference,
  message count, age, mean level (dBFS), emergency (state or squawk 7500 / 7600 / 7700), on ground, alert, SPI, TIS-B flag, selected altitude / heading,
  baro setting, roll, IAS, Mach, ADS-B version, NACp, a track of the last 30 positions (one per 5 s). Expiry after 60 s of silence (`setExpiry`).
- Statistics: messages per second (last second), good and failed counts, repaired count, histogram by DF, noise floor, preamble candidates, maximum range,
  SNR and level of the good messages, the last 64 messages (hex, address, DF, type code, correction, level, one line of text).

- Coverage plot (Statistics view): the farthest position per 10 degrees of bearing, for all heights, each altitude band (under 10000, 10000-20000, 20000-30000, 30000-40000, 40000 ft and above) or all bands overlaid in the map's height colours. A low band that stops short in one direction is a horizon problem (building, hills), a high band that does is the antenna or gain. "Save results" exports every band in the chosen units; "Copy coverage CSV" puts nautical miles and km on the clipboard. Needs the antenna position.
- Automatic gain on the message rate and the share of loud messages (docs/AUTOGAIN.md, "ADS-B: the message-rate loop").

### Not supported

- Mode A / C replies (20.3 us frames), MLAT, TCAS resolution advisories (DF0 / DF16 RI fields are not decoded), Comm-B formats other than the four above,
  the content of DF24, ADS-B version 0 / 1 / 2 differences in the NIC / NACv fields beyond what the table shows, TC 23-27 and 30.
- The two-bit correction is only tried on frames of known addresses and only on the eight least certain bits; there is no longer search.
- No notch against a strong carrier in the band (see limits).

## Sources and what is not from the standard

- ICAO Annex 10 Vol. IV (formats, CRC), RTCA DO-260B (extended squitter), as described in "The 1090 Megahertz Riddle" (Junzi Sun, https://mode-s.org/1090mhz),
  which also gives the worked examples used in `tests/test_adsb_msg.cpp` and `test_adsb_track.cpp`. The Gillham code: en.wikipedia.org/wiki/Gillham_code.
- NL transition latitudes: the formula, checked against the table in readsb `cpr.c` (first and last six values).
- **Target state (TC 29)** layout: readsb `mode_s.c`, read again during this work (selected altitude type bit 9, altitude 10-20, baro 21-29, heading status 30,
  heading 31-39, NACp 40-43, mode bits 47-54 with approach at 52 and LNAV at 54 of the ME field). Not read from the standard.
- TC 31 (operational status) layout: checked against the Riddle's operation status chapter (version bits 73-75, NIC supplement 76, NACp 77-80, SIL 83-84).
- **TC 20-22 altitude** (GNSS height): decoded like the barometric altitude (25 ft steps with the Q bit), as readsb does. The Riddle says the 12 bits are a
  height in metres. The two cannot both be right and the standard was not read; the value is shown separately (`geoAltFt`) and is **unverified**.
- Emitter category names: DO-260B as listed at adsbexchange.com.
- The BDS 5,0 consistency check (coordinated turn) is my own, not from the Riddle.

## How it works

The search works on the magnitude (on-off keying); a frame that fails is sliced again coherently (step 6), which measures the carrier of that frame.

1. Below 4.5 Msps the samples are first interpolated to twice the rate (12 tap half-sample filter): at 2 Msps a pulse that falls between two samples would
   otherwise have half its power in each. Power per sample, with the radio's DC offset followed in blocks that hold no signal.
2. Power per 0.25 us bin from a running sum with linear interpolation at the edges, so any rate (2.4 Msps is 2.4 samples per us) goes through the same code.
   Noise floor: lower quartile of the means of the last 16 blocks of 64 us (follows gain steps in about a millisecond).
3. Preamble score at every bin (since 3.0): the four pulses at 0, 1, 3.5, 4.5 us together must exceed the quiet windows (in between and the 3 us before the
   data) by 4 standard deviations of that difference, each pulse must be above the noise, pulse power at least 2x the gap power; then the first 56 bits must
   show a mean contrast between their two halves of 1.8 standard deviations of a noise window (noise alone gives about 1.13). A peak must be the highest
   within 1.5 us (else a strong frame is also found 1 us early). Before 3.0 every pulse had to exceed 3x the noise floor, which lost many weak frames.
4. Timing refined in 0.05 us steps, 112 bits sliced by the energy of the first against the second half of each microsecond. A frame that fails the CRC is
   tried again with the timing from the data, with narrower windows (against smeared pulses), and with "ties" (below), then dropped.
5. Frames that pass skip their own length, so a message is not found again inside itself.
6. Coherent pass (since 3.0, `AdsbDemod::sliceCoherent`), for a frame the first slice did not get right: the carrier offset from lag-1 products inside the
   pulses that slice found, refined by one FFT over the phases of all pulses (+-190 kHz); complex integrals of the half bits turned back by it; a phase
   reference from the 32 pulses around each bit; the bit is the difference of its halves along the reference (two passes). Then sequence estimation against
   the smearing of the radio's filter: three taps by least squares from those decisions, a two-state Viterbi over the bits, twice. Then the older retries.
   Retries are made for frames above 5 dB in 2 MHz (before: 6 dB per sample, which left out weak frames at high rates).
7. CPU guard: when `feed()` takes more than half of the signal's duration the receiver steps down (2: every pass; 1: coherent pass and retries only for
   preambles that pass the older strict test; 0: only those preambles), back up below a quarter, with a log line. `AdsbReceiver::setEffort` fixes it.

**Ties.** At 2 Msps and one sampling phase in about four, the samples fall on the edges of the pulses and a run of equal bits looks like a flat line. The
two halves of such a bit are then equal, and the bit is the same as the one before it. The last attempts take a bit whose half energies differ by less than 30,
40 or 50 % of the pulse contrast to be equal to the previous bit. Measured at 2 Msps, 28 dB, 8 bit: 84 % of the frames without this, 100 % with it; every
sampling phase decodes at 30 dB (`test_adsb_rx`, "2 Msps phases").

## Test signal (`makeAdsbSynth`, `adsb_gen.h`)

A simulated airspace: 12 aircraft by default (0 to 300) flying great circles over a disc of 12 to 190 NM around the reference point (default 25.25 N 55.36 E),
one taxiing on the ground when there are four or more, replaced by a new one entering from the edge when one leaves. Each has an address, an airline-style
callsign, squawk, category, altitude profile (level, climb, descent), speed, track, ADS-B version, and a carrier offset of its own (about +-100 kHz).
Message mix at the DO-260B rates: airborne position 2 per second per aircraft in total (even and odd alternate, 0.4 to 0.6 s apart), velocity 2 per second,
identification every 5 s, operational status every 2.5 s, target state every 1.25 s on half of them, DF11 about once per 2 s, and (option) radar replies:
DF4, 5, 20 (BDS 2,0 / 4,0 / 5,0 / 6,0 in turn), 21, 0, 16 at 3 to 12 s intervals. 12 % of the aircraft send Gillham altitudes, one in eleven sends
airspeed instead of ground speed. Levels fall with distance (1/d) and fade slowly (+2 / -2.5 dB); overlapping bursts appear at the natural rate (about 1 % of
the transmissions at 12 aircraft). Pulses are 0.5 us with a 25 ns Gaussian edge through a third-order Butterworth filter of the radio's width, sampled
without further filtering as the HackRF does (so a 2 Msps stream is aliased a little, as on the real radio). Noise goes through the same filter.
The CPR encodings, Gillham / Q altitudes, identity codes and CRC are real (the receiver decodes them with code that is checked against published messages).

Options (`SynthConfig`): `snrDb` pulse-to-noise ratio of an aircraft at 50 NM, noise measured in 2 MHz (default 30; 14 dB more at 10 NM, 12 dB less at 200 NM),
`cfoHz`, `sroPpm`, `modeOpt[0]` aircraft count (0 = 12, -1 = none), `modeOpt[1]` 1 = ADS-B and DF11 only, `modeOpt[2]` seed, `modeOpt[3]` 1 = one aircraft
squawks 7700 and sends the emergency status, `modeVal[0]` message rate multiplier (0.1 to 20), `modeVal[1]` radio filter width in MHz (0 = automatic),
`modeVal[2..3]` reference latitude and longitude. The message rate and noise can change while it plays; the rest starts a new generator.

## Tuning table

1085 to 1095 MHz, default 1090. Asks the radio for **4 Msps** with the 3.5 MHz baseband filter and 2 MHz channel width; minimum 2 Msps. 4 Msps because
that is the lowest rate at which the 0.5 us pulses are not smeared into each other by the filter: at 2 Msps (HackRF minimum, 1.75 MHz filter) the receiver
works but needs about 5 dB more signal (table below). HackRF Pro: 4 Msps is a rate with an exact centre.

## Verification

Tests (`ctest -R adsb_`): `adsb_msg` (known answers), `adsb_track` (checks, correction, CPR, table), `adsb_gen` (generator), `adsb_rx` (receiver, about 25 s),
`adsb_engine` (through the engine, about 30 s).

**Known answers** (`test_adsb_msg`, `test_adsb_track`), all from the Riddle unless noted: CRC remainders of eight published messages (0, 0, 0, 0, 0, 16, 22),
the generator polynomial 0xFFF409, the parity AA4BDA; identification 8D4840D6202CC371C32CE0576098 (KLM1023, ICAO 4840D6); the CPR pair
8D40621D58C382D690C8AC2863A7 / 8D40621D58C386435CC412692AD6 (52.25720214843750 / 3.91937255859375 with the even frame latest, 52.26578017412606 with the odd),
the surface pair and local decode (52.320607 / 4.734735, then 52.320561 / 4.735735); velocity 8D485020994409940838175B284F (159 kt, 182.88 deg, -832 ft/min,
GNSS +550 ft) and 8DA05F219B06B6AF189400CBC33F (airspeed 375 kt, heading 243.98, -2304 ft/min); DF4 2000171806A983 (36000 ft); DF5 2A00516D492B80 (squawk 0356);
DF11 5D484FDEA248F5; Comm-B 2,0 / 4,0 / 5,0 / 6,0 examples; Gillham 0 / 100 / 700 ft from Wikipedia and all 1280 codes (unique, one bit apart, round trip);
NL table thresholds from readsb `cpr.c`; every single-bit syndrome unique. The CRC and the CPR global decode were also re-derived in a separate
short Python script for this work (same results). The **encoders** of the generator reproduce published messages byte for byte (`test_adsb_gen`): the
identification, both position frames, the all-call reply and the altitude reply; the identity reply and the velocity message match in every field (the
published ones have a spare bit set that the encoder leaves at 0). Not covered by any published message: TC 28, 29 and 31, DF18, DF0 / 16, TC 19 subtypes 2 and 4,
TC 0 and TC 20-22: their encoders and decoders agree with each other and with the layouts named under Sources, which is weaker evidence.

**Round trip**: generator -> 8 bit samples -> receiver, every transmission checked off by its bytes and time (400 frames of every kind at 30 dB, 4 Msps: all
400, no phantoms; airspace of 12 aircraft for 20 s at 4, 2.4 and 10 Msps: 99.8 % of the transmissions that are not overlapped, every aircraft in the table with
the right callsign, squawk, altitude (within 250 ft), speed, position (within 0.03 deg) and range).

**Rates** (300 frames, 28 dB, 8 bit): 2, 2.4, 3.2, 4, 5, 6, 8, 10, 12.5, 16, 20 Msps all 99 % or better, no phantom messages.

**Sensitivity** (pulse-to-noise in 2 MHz; `test_adsb_rx sweep 2000`: 2000 frames of every kind, random sampling phase, random +-200 kHz carrier offset,
float samples; percent decoded, 3.0 against before 3.0 in brackets; 0 phantoms in every cell):

| SNR dB | 2 Msps | 2.4 | 4 | 6 | 8 | 10 |
|---|---|---|---|---|---|---|
| 8 | 3.5 (0.1) | 9.1 (0.2) | 40.0 (0.1) | 39.2 (0) | 38.8 (0) | 39.1 (0) |
| 10 | 33.6 (10.9) | 66.8 (22.8) | 91.5 (31.6) | 91.6 (9.6) | 90.7 (13.1) | 92.3 (12.2) |
| 11 | 49.4 (27.4) | 88.8 (58.1) | 98.2 (69.1) | 98.0 (43.4) | 98.2 (45.5) | 98.5 (45.9) |
| 12 | 63.9 (43.5) | 97.0 (85.4) | 99.9 (90.2) | 99.8 (77.7) | 99.8 (79.2) | 99.8 (78.5) |
| 13 | 76.2 (58.5) | 99.3 (96.2) | 100 (98.2) | 100 (94.7) | 100 (94.8) | 100 (95.0) |
| 14 | 84.8 (68.0) | 100 (99.2) | 100 (100) | 100 (99.3) | 100 (99.5) | 100 (99.2) |
| 16 | 96.2 (82.3) | 100 | 100 | 100 | 100 | 100 |
| 18 | 99.2 (92.3) | 100 | 100 | 100 | 100 | 100 |

90 % points: 4 Msps 12.0 -> 9.8 dB, 10 Msps 12.6 -> 9.9, 2.4 Msps 12.3 -> 10.9, 2 Msps 17.8 -> 15.3. About 36 % of the frames in this mix are address /
parity replies, which cannot be repaired; with the one-bit repair of the others, 10 dB at 4 Msps is close to the coherent bound (Q(sqrt(SNR)) per bit).
Two-bit repair (`setCorrection(2)`) adds about 0.2 dB. Noise alone: 0 candidates per second at every rate (the data check), 0 messages.

**Head-to-head** (generated 2.4 Msps cu8 airspaces, 60 aircraft, 30 s, `adsbtool gen --snr 10 / 16 / 22`; messages decoded; readsb and dump1090-fa
built from their GitHub sources in a scratch folder): weak 878 (before 3.0: 724) against readsb 705 and dump1090-fa `--fix` 679 (`--fix-2bit` 691);
medium 2992 (2207) against 1925 and 1850 (1959); strong 10237 (8579) against 7587 and 6965 (7413).

**Time stamps** (Beast output, 12 MHz): error against the true frame time, good frames at 20 dB (`test_adsb_rx ber`): 17 to 20 ns rms at 2.4 to 10 Msps,
68 ns at 2 Msps (a constant offset of 0 to 25 ns). Good enough for multilateration with an external mlat-client reading the Beast port: the clock is the
radio's sample clock, not GPS time; the MLAT server synchronises receivers with ADS-B position messages, which works as long as no samples are dropped.

**Crowded weak airspace** (`test_adsb_rx stress 2.4 14 30 100`: 100 aircraft, 30 s): 3747 messages, 39 aircraft (before 3.0: 2743, 26), 0 phantoms; CPU in
`feed()` 2.85 s against 1.66 s before. With the CPU guard at 1: 3263 messages, 2.16 s; at 0: 2795, about 1.9 s.

**Impairments** (4 Msps, 300 frames at 24 dB, 8 bit, `test_adsb_rx`): clean, DC offset 5 % and 15 % of full scale, IQ imbalance 1.5 dB / 8 deg, carrier
offset +-300 kHz, clock +-100 ppm, all together, carrier 14 dB below the pulses at 50 kHz and 700 kHz offset, radio filter 1.75 MHz at 4 Msps: 100 % each, no
phantoms. Gain step +-20 dB in the middle: no frame lost before it, the first ones after it decode within about a millisecond (4 of 4 in the next 4 ms).
Input 5x and 40x too strong (clipped at full scale): 100 %. Dropout of 5 ms: frames before 100 / 100, in the gap 0 / 5 (nothing is there), first 2 ms after
2 / 2, later 293 / 293. `reset()` in the middle, also inside a frame: later frames decode, the table is empty, report numbers keep growing. Chunks of 1, 7,
4096, 65536 and mixed sizes: the same set of frames decoded. 1.5 Msps: not ready.

**False messages**: none in the test runs. In a deliberately heavy case (100 aircraft, 2 Msps, 8 bit, 350 failed candidates a second, 3 minutes) two replies of
the address / parity kind from known aircraft matched by chance before the checks above were added, and one after. They cannot make an aircraft, but a false DF21
or DF5 is shown in the message monitor (a squawk change from it is held back). Expect that rate to grow with the number of aircraft and with the failed candidates.
Noise only, 10 s: 0 decoded at 2, 2.4, 4, 10, 20 Msps, float or 8 bit; false preamble candidates 0 to 0.4 per second at 4 Msps and above, 26 per second at 2 Msps with noise of less than one 8 bit step (the 30 dB setting; a real radio has several).

**Other signals on the channel** (4 Msps and 2.4 / 10 Msps, ADS-B frames at 24 dB, `test_adsb_rx` "Mode A/C interference"): Mode A/C replies (20.3 us, 13 code pulses on the
1.45 us grid, random codes, 500 and 2000 a second): every ADS-B frame that no reply touches decodes (384 of 384, 310 of 310), 70 to 80 % of the touched ones too, no false
message. Impulse noise (single pulses of 0.1 to 3 us, 3000 a second, up to 20 dB above the signals): 289 of 289 untouched frames, 60 to 65 % of the touched ones, no false message,
the noise floor figure unchanged.

**Bad samples**: NaN, infinity and 1e30 in the stream (half a millisecond of NaN, shorter stretches of the others, a NaN every 997th sample): 197 of 200 frames
decoded, all 41 after the last bad stretch, the noise floor figure stays finite and about 1 dB low.

**Garble**: a burst 20 dB weaker on top of a strong one: the strong one decoded 40 of 40 at 4 and 10 Msps; a strong burst that starts inside a weak one: 40 of 40.
In the airspace test the overlapped transmissions (1.3 %) decode 35 % of the time; two bursts of equal power destroy each other, as in every receiver of this kind.

**Strong carrier**: 14 dB below the pulses: no effect. 12 dB above the pulses: nothing decodes and nothing false comes out (the receiver comes back when
the carrier goes). There is no notch.

**Through the engine** (`test_adsb_engine`, synthetic source, 12 aircraft, 8 bit, paced like a radio): 12 s at 4 Msps, 8 s at 2 and 20 Msps: locked after 0.1 to
0.3 s, 11 or 12 aircraft with a position after 8 to 12 s (the taxiing one needs a reference), 0 dropped samples, report numbers grow, a retune empties the table.

**Speed** (CPU time in `feed()`; the machine was busy with other builds, so these are conservative): 40 aircraft (260 messages per second): 2 Msps about 22x, 4 Msps
17x, 10 Msps 19x, 20 Msps 13x real time on one core in the test run; no aircraft at all gives about the same figure, because the per-sample work (power, running
sums, bins) dominates and the four-pulse test that follows rejects nearly every position at once. `dect2cli --standard adsb --synthetic` shows 6 % of real
time at 4 Msps.

**Sanitisers**: AddressSanitizer + UndefinedBehaviorSanitizer (`-fsanitize=address,undefined -g -O1`): `adsb_msg`, `adsb_track`, `adsb_gen`, `adsb_rx` (6 minutes) and `adsb_engine`
all pass with no report. ThreadSanitizer: `test_adsb_rx threads` (telemetry, setters, `reset()` and `clearAircraft()` from a second thread while `feed()` runs) is clean; the engine test reports
three data races, **all in shared code** (`Atsc3Rx::pump()` reading its packet callback while `Engine::start()` assigns it, `atsc3_rx.cpp:160` against `Engine::start`); none in the ADS-B
files. Under a sanitiser the engine test skips its 20 Msps run (the sanitised build is slower than real time there).

## Controls and telemetry for the interface

Setters (thread safe): `setReference(lat, lon)` / `clearReference()` (local decoding, range and bearing, surface positions; without it only global decoding),
`setCorrection(0 | 1 | 2)`, `setExpiry(seconds)`, `clearAircraft()`, `setThresholds` (tests). Log lines: first lock, loss of messages (5 s), new aircraft (at most one
per 2 s).

`AdsbTelemetry`: `state` (0 searching, 1 pulses but no messages, 2 receiving), `msgsPerSec`, `noiseDbfs`, `levelDbfs`, `snrDb`, `preambles`, `blocksOk` /
`blocksBad` (a candidate that passed the preamble test and failed the CRC), `corrected`, `dfCount[32]`, `aircraftCount`, `withPosition`, `maxRangeNm`,
`aircraft[]` (most recently heard first, up to 128), `frames[]` (last 64). `cfoHz` is always 0 (the receiver works on the pulse envelope).
Worth drawing: a radar plot with the reference at the centre (positions, tracks, rings by range), the table, the message monitor, a DF histogram, messages
per second against time, an emergency banner.

## Limits

- Not tried on a real radio or recording. The generator is the author's model of one.
- 2 Msps needs about 5 dB more signal than 4 Msps and above; the app asks for 4 Msps.
- No native MLAT client: the Beast output carries sample-clock time stamps for an external mlat-client.
- A carrier in the band that is as strong as the pulses stops reception; weak ones do not matter.
- Garbled frames (overlapping bursts of similar power) are lost.
- GNSS height of TC 20-22 unverified (above). TC 29 and the Comm-B inference are from sources other than the standard (above).
- Two-bit correction and `fixUnknown` raise the chance of a false aircraft; both are off by default (one bit, known addresses only).
- Each failed frame is sliced up to eleven times (coherent, sequence estimation, timing, windows, ties), so a very busy and very noisy band costs more than
  the figures above (the CPU guard steps down when it gets tight).

## Work log

1. First pass (an earlier agent, cut off before it could report): the whole message layer, tracker, demodulator, receiver, generator, harness, tool and the five tests.
2. Second pass (re-checked the first against the sources, then): the tie rule for 2 Msps (84 % -> 100 % at 28 dB); plausibility checks and squawk confirmation for address / parity
   replies, DF 28 - 31 no longer taken for ELM; TC 0; `ready()` and the thresholds made thread safe; CPU time instead of wall time in the speed figures; tests for every sampling
   phase at 2 Msps, Mode A/C replies, impulse noise, NaN / infinity input, the encoders against published messages; `adsbtool decode`; this file written out.
3. Not done: the interface (`app/adsb_ui.cpp` is still the placeholder), real recordings.
4. 3.0 (dec-data-030): the looser preamble test with the data check, the coherent pass, sequence estimation, the CPU guard, the sweep / ber / stress parts
   of `test_adsb_rx`, `adsbtool rx --older-demod` for comparisons, the head-to-head against readsb and dump1090-fa.

## Last check

Fresh `cmake -S . -B build-fresh -DDECT2_BUILD_APP=OFF` and a full build in the private copy, then `ctest -R 'adsb_|isdbt_engine|atsc3_engine|fm_rx'`: all pass; no compiler warning in an ADS-B file.
