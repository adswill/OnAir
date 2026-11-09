# Inmarsat BGAN (feat37-bgan)

Status: **signalling and carrier structure decoded from generated test signals only. Nothing here has been validated on a real BGAN recording;** no public IQ recording of a forward bearer exists that we could find (Signal Identification Wiki lists a BGAN uplink image only; AMSAT list 2015 said none known; web searches came up empty). The bit-level interpretations below that the spec leaves to figures (UW bit order, PLMN digit packing, the outer-point UW symbols) are taken from the figures and need a real capture to confirm. `Needs a user test`.

## Sources

- ETSI TS 102 744 Family SL, Release 1, V1.1.1 (2015-10), all free from etsi.org: 2-1 (physical layer), 2-2 (RF), 3-1 / 3-2 (bearer control), 1-1 .. 1-4, 3-3 .. 3-9, 4-1 / 4-2.
- Annex tables that accompany 2-1 (`ts_1027440201v010101p0.zip`): turbo interleavers (C.1), puncturing, channel interleaving and symbol mapping (C.2), bearer parameters (B). The forward-bearer C.1 / C.2 tables are compacted into `core/src/bgan/bgan_tables_data.inc` by `tools/bgan_tables.py` (raw deflate of delta coded indices; the 118 forward-bearer tables are 0.97 MB). ETSI specifications are published for free but are copyright; these tables are numeric data that any implementation must reproduce, as for the other ETSI standards in this tree.
- No open-source BGAN receiver or decoder exists (searches found commercial SDR waveforms and Inmarsat Aero / C decoders only).
- IsatPhone / IsatPhone Link: there is no published air-interface specification for them. They are generally described as using GMR-2 / GMR-2+ (ETSI TS 101 377, ACeS heritage): 200 kHz forward carrier spacing, filtered OQPSK at 270.833 kbit/s (135.4 ksym/s). This mode labels a carrier that matches that shape "GMR-2 class (IsatPhone type)". It cannot prove a carrier is an IsatPhone, and nothing is decoded.

## Forward bearers (what the standard defines)

TDM carriers in 200 kHz subbands, 1518 - 1559 MHz (channel centres on a 1.25 kHz grid, usually 1525 - 1559), RRC roll-off 0.25 (F80T*) or 0.13 (FR80T*), 80 ms frames.

| bearer | symbol rate | modulation | FEC blocks | channel | notes |
|---|---|---|---|---|---|
| F80T0.25Q-1B | 8.4 kBd | QPSK | 1 | 10.5 kHz | the global-beam bulletin-board bearer; lowest level L8 needs 40.1 dB-Hz |
| F80T1Q-1B | 33.6 kBd | QPSK | 1 | 42 kHz | **interleaver and mapping tables are not published** (structure and identification only) |
| F80T1Q-4B | 33.6 kBd | QPSK | 4 | 42 kHz | |
| F80T1X-4B | 33.6 kBd | 16-QAM | 4 | 42 kHz | |
| FR80T2.5X4/16-5B | 84 kBd | QPSK / 16-QAM per block | 5 | 95 kHz | outer interleaver, dynamic modulation |
| FR80T2.5X16-5B / X32-6B / X64-7B | 84 kBd | 16 / 32 / 64-QAM | 5 / 6 / 7 | 95 kHz | |
| F80T4.5X-8B | 151.2 kBd | 16-QAM | 8 | 189 kHz | |
| FR80T5X4/16-9B | 168 kBd | QPSK / 16-QAM | 9 | 190 kHz | |
| FR80T5X16-9B / X32-11B / X64-13B | 168 kBd | 16 / 32 / 64-QAM | 9 / 11 / 13 | 190 kHz | |

Frame: 40 symbol Unique Word (bipolar on the outer points, the word selects the coding level of the first block), pilot symbols every DS + 1 symbols, data. Each bearer has levels L8 .. L1, R, H1 .. H6. Each FEC block: 15-stage scrambler (1 + x + x^15, 0x6959), 16-state turbo code (octal 23 / 35, 4 flush bits, S-interleaver from the tables), puncturing and channel interleaving from the tables, then optionally the 80 ms outer interleaver (FR bearers).

Signalling (TS 102 744-3-1): BCt PDUs carry broadcast BulletinBoard (RNC, net version, frame number, forward bearer, BCt ID, spot beam), AVP lists (satellite location, beam type, PLMN, primary bearers, UTC, state vectors, sub-band offsets, forward code-rate changes, access control), ReturnSchedule (the return slots: reserved vs contention, listed connections), SpotBeamMap, GPSEphemeris, SystemInfoIndex. CRC-16 (X.25).

## What this mode does

- Finds carriers anywhere in the capture (the Inmarsat CarrierDetector) and classifies by width, tries the demodulator(s) of that symbol rate.
- Acquires by pilot comb correlation (timing and fine frequency) then the Unique Word (frame start, coding level, polarity). Pilot-aided channel estimate per frame, LLRs, turbo decoder (8 iterations, early stop on CRC), block parse.
- Tells F80T1Q-4B / F80T1X-4B / F80T1Q-1B apart by UW polarity and by which tables decode with CRC.
- Reads: bearer type and level, satellite (id and longitude), spot beam and beam type, RNC, BCt ID, network (PLMN), primary bearer list, GPS time, return schedule load, FEC block occupancy.
- Never reads: common-signalling PDUs addressed to one terminal (they carry IMSI / TMSI / IMEI), connection PDUs, ConnPDU, StatusAckList or SpecificAVPList. Those are counted, not parsed or stored. No user data or voice path exists. The mode has no decrypt step and no subscriber display.
- One click: **Scan 1525 - 1559 MHz** walks the band with the radio's own bandwidth (7 s per step); results stay in the table.
- A carrier 130 - 225 kHz wide that does not lock is tested for the spectrum lines of filtered OQPSK (GMR-2 class, IsatPhone type). No decoding.
- It is a separate mode entry (family 245), not folded into "Inmarsat (all carriers)": that manager's windows and its 40 kHz width limit fit the narrow Aero / C carriers, BGAN carriers are up to 190 kHz with 80 ms frames. The detector class is shared.

## What a typical L-band patch antenna receives

Rough link budget only (no field test): the BGAN terminal classes have G/T of about -3 dB/K (class 1) to -11 dB/K or lower for hand-portables (TS 102 744-2-2 Table 5.3). A hobby patch plus good LNA is roughly -16 to -18 dB/K, so about 7 to 15 dB below the class the bearers are budgeted for.

- Global beam bulletin bearer (F80T0.25Q-1B): the lowest levels need only about 40 dB-Hz: the most likely real target for a patch. Decodable where that margin exists.
- Regional beam bearers: likely usable if your location is well inside the regional beam.
- Narrow spot beams: only the one that serves your location has useful EIRP there; carriers for other spots reach you far below the required C/N0. Their energy may still be visible as carriers, unidentified.
- FR80T* high-order QAM needs 60 - 70 dB-Hz: usually out of reach with a patch.
- The return link (user terminal to satellite, 1626 - 1660 MHz) is not reachable from the ground and is not implemented.

## Measured (synthetic signals, real frame structure, AWGN)

See `tests/bgan/test_bgan_demod.cpp` `sweep`. Column: required C/N0 (Annex B, ideal modem, BLER 1e-3) vs the C/N0 at which this implementation decodes at least 90 % of frames over 6 s.

SWEEP_TABLE

Impairments at R subtype, C/N0 = required + 4 dB:

IMP_TABLE

Generated with: 3 ppm sample clock, phase noise none, RRC and a 16-bit-free float pipeline, 4 samples per symbol. Fading is amplitude and phase; deeper fades than the table are not expected on a healthy GEO link but happen with foliage or motion.

## Honest limits

- No real signal ever went through this code.
- F80T1Q-1B: structure only, its tables are not in the public annex.
- Bit-level details taken from figures (UW MSB-first order, PLMN nibbles, the 32QAM / 64QAM UW points, the X4/16 "Mean symbol power = 1" pilots) are unconfirmed against a real carrier.
- GMR-2 class is a spectrum-shape guess, not an identification; "IsatPhone" is a family label, not verified.
- The FR bearers' ±50 ppm clock tolerance is limited (see the table); typical TCXO radios are fine, cheap uncalibrated radios may need a ppm correction.
- SpecificAVPList, addressed common signalling and traffic content are deliberately not displayed.
