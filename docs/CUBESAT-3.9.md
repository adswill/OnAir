# CubeSat telemetry 3.9: more gr-satellites framings and the satellite table

The CubeSat telemetry mode (family 61) and the Smart CubeSat decoder's chains decode five more framings, and the receiver sets itself up
for the satellite of a pass from the gr-satellites satellite table. No new mode entry: these are framings of the existing mode.

## What was already there (not redone)

| Item asked for | State before 3.9 |
|---|---|
| GOMspace AX100 Mode 5 / Mode 6 | decoded since 0.2.x (`cubesat_proto.h`) |
| G3RUH 9k6 AX.25 | decoded (9600 and 4800 baud) |
| AO-40 FEC | decoded in the Smart CubeSat BPSK chain (`cubesat_ao40.h`), checked on gr-satellites' real FUNcube recording |
| FUNcube with fields | `parseFuncube` (`cubesat_telem.h`), FUNcube-1 / FUNcube-2 / Nayif-1 realtime frames |

## Added

| Framing | Layers | Source | Where it runs |
|---|---|---|---|
| GOMspace U482C | sync 0xC3AA6655, Golay (24,12) length + flags, optional K=7 convolutional code (0x6D / 0x4F, terminated), CCSDS scrambler, RS (255,223) | gr-satellites `u482c_decode_impl.cc`, `viterbi.c` | receiver: 9600 / 4800 FSK slicers and a new audio-FSK path (4800 baud on a 3600 Hz audio carrier, as GOMX-1, AISAT, AU02/03, CZ02, D-SAT, GALASSIA); Smart chains |
| CCSDS concatenated | K=7 r=1/2 code (CCSDS, NASA-DSN, uninverted variants), sync 0x1ACFFC1D, CCSDS randomiser, RS dual or conventional basis, interleave 1..n, optional NRZ-M | gr-satellites `ccsds_concatenated_deframer.py`, `ccsds_rs_deframer.py` | receiver (frame size and code from the table when tracked; default 223 / dual / CCSDS); Smart FSK and BPSK chains (BPSK also with NRZ-M + conventional basis) |
| USP (SPUTNIX) | 64 bit sync, PLS code (48 or 223 data bytes), CCSDS code, scrambler, dual-basis RS, AX.25 inside | gr-satellites `usp_deframer.py`, `usp_pls_crop.py`, `usp_ax25_crop.py` | receiver and Smart chains; the frame's AX.25 header is parsed |
| NGHam | sync 0x5DE62A7E, 7 size tags, CCSDS scrambler, RS 16 / 32 parity (shortened, conventional), CRC-16 X.25 | NGHam reference `ngham.c` (LA3JPA, LGPL-3), gr-satellites `ngham_deframer.py` | receiver and Smart chains; also the "no Reed-Solomon" variant (CRC only) |
| Mobitex, Mobitex-NX, BEESAT-1, BEESAT-9 | sync 0x5765 / 0x0EF0, control bytes with (12,8) FEC, callsign + CRC-16 (bit-flip repair up to 2 bits), 1..32 blocks: scrambler, 12x20 interleaver, (12,8) FEC, CRC-16 per block | gr-satellites `mobitex_*.py`, `tubix20_reframer.py` | receiver and Smart FSK chain; output frame = gr-satellites' tubix20_reframer layout |

## The satellite table and pass auto-setup

`core/src/cubesat/cubesat_grsat_data.inc` is generated from gr-satellites' `python/satyaml` (680 transmitters, commit 0ff8b614c3ed) by
`tools/cubesat/satyaml2inc.py`: NORAD, transmitter, frequency, modulation, baud, framing, CCSDS parameters, AFSK carrier, callsign and
gr-satellites' telemetry decoder name. `cubesat_grsat.h` reads it.

When the pass planner starts the CubeSat mode for a satellite (`setTracked`), the receiver picks that satellite's transmitter nearest the
dial (the first decodable one when the dial is unknown) and:

- sets the CCSDS concatenated parameters (frame size, interleave, basis, code, NRZ-M) and the Mobitex variant;
- starts an extra demodulator at the satellite's baud rate (or AFSK carrier) when the fixed 9600 / 4800 FSK and 1200 AFSK ones do not cover
  it (1200, 2400, 19200 baud FSK; USP alone has 60 transmitters at 1200 / 2400);
- shows what it set up (`CubeTelemetry::trackedSetup`, the log line "CubeSat: set up for ...").

Field decoders OnAir has for gr-satellites' telemetry names: FUNcube, GOMX-1, the CSP header and the AX.25 header. The other telemetry
formats in the table (about 40 satellite-specific ones) are named but not decoded into fields.

## Tests

| Test | What | Result |
|---|---|---|
| `test_cubesat_more` | conv code vs the AO-40 decoder; RS 16/32; U482C all 8 flag combinations, errors; CCSDS concatenated 7 parameter sets x 2 polarities; USP: gr-satellites' qa vectors (long and short frame) and round trips; NGHam all 7 sizes, with and without RS, byte errors; Mobitex: gr-satellites' real BEESAT-9 packet, all variants, 1/5/32 blocks, bit errors; hunter streams with noise, lead, both polarities, back to back frames; noise alone | see the branch's last commit |
| `test_cubesat_grsat` | table sanity; GOMX-1, FUNcube, BEESAT-9, LilacSat-2 rows; USP at 2400 baud and NGHam (FloripaSat-1) at 2400 decoded only when tracked, with offset and noise | see the branch's last commit |
| `test_cubesat_rx`, `test_cubesat_engine` | the generator now sends the five new framings; each framing alone and all together with Doppler and noise | see the branch's last commit |

Real recordings: the USP and Mobitex decoders are checked against gr-satellites' qa vectors (USP: coded frames from gr-satellites'
test; Mobitex: soft symbols of a real BEESAT-9 packet). U482C, CCSDS concatenated and NGHam are validated on simulated signals only.

## Not done

- U482C on the GOMspace radios' 1200 baud AFSK (HUMSAT-D, 1500 Hz carrier) works only when tracked (the extra path); not tested.
- BPSK and DBPSK transmitters (USP / CCSDS concatenated BPSK, AO-40) are not demodulated by the FM CubeSat receiver; the Smart CubeSat
  decoder's BPSK chain handles them.
- The ~45 satellite-specific gr-satellites framings (SMOG-P, Light-1, S-NET, ...) and the per-satellite telemetry formats are not added.
