# Cellular broadcast information in 3.9 (feat39-cell)

What every base station broadcasts to all phones: identity, operator, neighbours, system parameters. Receive only. Nothing addressed
to a phone is decoded: no user traffic, no paging identities, no decryption, no tracking of devices.

| Item | Where | State |
| --- | --- | --- |
| GSM BCCH / CCCH | GSM-R mode (shared code, one entry for GSM) | extended |
| CDMA2000 / IS-95 sync | new mode CDMA2000 cells (family 660) | new |
| TD-SCDMA DwPTS | new mode TD-SCDMA cells (family 661) | new, BCH not decoded |
| PHS | - | skipped this round |
| LTE-M MIB / SIB1-BR | Cell towers (LTE) | extended |
| NB-IoT MIB-NB / SIB1-NB | NB-IoT cells | already there (3.x), unchanged |
| 5G NR SSB / PBCH / MIB | 5G NR cells | already there (3.x), unchanged |

All results below come from simulated signals only. No real recording of CDMA2000, TD-SCDMA or LTE-M was available. Neither
was a real GSM CCCH or a GSM 850 / PCS 1900 capture.

## GSM (GSM-R mode)

- Channel rasters for GSM 850 (ARFCN 128 - 251) and PCS 1900 (512 - 810, the same numbers as DCS 1800; the band decides), from
  TS 45.005 2. The mode's range is now 860 - 1990 MHz. The cell detail shows the band name.
- SI2bis neighbour list (the same format as SI2, TS 44.018 9.1.33), shown under Lists.
- The cell detail says when a cell sends SI13 (GPRS / EDGE) and SI2quater (3G / 4G neighbour information). The rest octets of these
  messages are not parsed.
- CCCH load. The paging and access grant blocks of the 51-multiframe (FN mod 51 = 6, 12, 16, 22, 26, 32, 36, 42, 46) are decoded and
  counted by message kind only: paging request type 1/2/3, immediate assignment (extended), reject, idle. The contents (identities,
  channel descriptions) are never parsed, kept or exported. CCCH frames are not sent to the GSMTAP output.
- Test: tests/gsmr/test_gsmr_bands.cpp checks the rasters, the kinds and the SI2bis list. It also runs the receiver on the test signal,
  which now carries paging and assignment frames with no identities: SNR 25 dB at +400 Hz and 12 dB at -1800 Hz, CCCH shares within 4 %.

## CDMA2000 cells (new)

Spec: 3GPP2 C.S0002-0 v3.0 (pilot PN 3.1.3.1.13, Walsh, the K = 9 rate 1/2 code, repetition, BRO interleaver), C.S0004-0 v3.0
(Sync Channel SAR: SOM bit, MSG_LENGTH, CRC-30) and C.S0005-0 v3.0 (Sync Channel Message 3.7.2.3.2.26). Code in core/src/cdma.

- The search uses a segmented FFT correlation against the complex pilot PN over all 32768 offsets, and spectrum shifts for carrier
  offsets up to +-5 kHz. Each pilot is then tracked over about 1 s: one correlation per PN period gives the timing and the sample
  clock, and the pilot phase gives the carrier. The Sync Channel (W32) is demodulated against the pilot, de-interleaved, Viterbi
  decoded and reassembled.
- Shown per base station: pilot PN, SID, NID, P_REV / MIN_P_REV, system time (UTC and local, with leap seconds, LTM_OFF and daylight
  saving), paging channel rate, CDMA_FREQ / EXT_CDMA_FREQ (with MHz for band classes 0, 1, 4), Ec/Io, carrier offset, and the message
  count. LC_STATE is read but not used: the paging channel is not decoded.
- Test (tests/cdma/test_cdma_phy.cpp) covers the PN sequences (balance, the 15-zero run, both recursions), Walsh orthogonality, the
  code's impulse response = 753 / 561, Viterbi BER, CRC-30, the interleaver and the message round trip. The chain test runs 1 to 3
  base stations at PN offsets 5 - 499, carrier offsets -4400 to +3100 Hz, clock errors -8 to +12 ppm, SNR -3 to 10 dB over
  2.4576 MHz, and starts in the middle of a frame. Every case decodes every base station, Ec/Io is within 2 dB of the transmitted
  value, and pure noise gives nothing. ONAIR_TEST_LONG=1 runs all 6 cases.
- Not verified against a real signal. The pulse shape is a root raised cosine (roll-off 0.2), not the C.S0002 FIR, and the matched
  filter uses the same.

## TD-SCDMA cells (new)

Spec: ETSI TS 125 221 V11.0.0 (frame 5A.1, burst 5A.2.2, DwPTS figure 18I: 32 chips guard then 64 chips SYNC-DL; midambles
Annex AA, complex rule 5A.2.3). ETSI TS 125 223 V11.0.0 (SYNC-DL codes Annex AA.1, (-j)^i rule 8.1, phase quadruples Table 8,
code groups Table 7E, parameter cycling Table 7F, scrambling codes Annex A). The tables were copied programmatically from the
specs into core/src/tdscdma/tdscdma_tables.inc.

- SYNC-DL search: an FFT correlation for the 32 codes, folded over the 16 sub-frames of an 80 ms block. This gives the code
  group, timing and level.
- Cell parameter: the 128-chip window of the time slot 0 midamble is correlated against the 4 basic midambles of the group at every
  cyclic shift. Frames alternate between parameters p and p xor 1, which gives the pair and the frame boundaries. The SYNC-DL phase
  against m(1), matched to quadruples S1 / S2, gives the even-SFN frames and so the initial cell parameter.
- Not done: the P-CCPCH / BCH (operator and system information). It needs the TS 25.222 rate matching and interleaving chain, which
  was not written in this round. The UMTS RRC parser already in OnAir would read the MIB once that chain exists.
- Test (tests/tdscdma/test_tdscdma_cell.cpp) checks the tables (the complex rules on the first chips, midamble shifts, SYNC-DL
  cross-correlation) and 6 chain cases: 1 to 3 cells, carrier offsets -6100 to +4000 Hz, clock -10 to +20 ppm, SNR -3 to 15 dB, any
  start in the sub-frame, and S1 only or alternating S1 / S2. It also checks that noise alone finds nothing, and runs the receiver
  thread.

## LTE-M (Cell towers)

The MIB's schedulingInfoSIB1-BR-r13 and systemInfoUnchanged-BR-r15 (TS 36.331 6.2.2) are read. The cell detail shows whether the
cell serves LTE-M, with the SIB1-BR repetitions and transport block size (TS 36.213 Tables 7.1.6-1, 7.1.7.2.7-1). The SIB1-BR
itself (sent in narrowbands with repetition) is not decoded. Test: tests/cell/test_cell_ltem.cpp, 6 cases over offsets, noise and
block lengths; the cell's normal SIB1 is still read.

## PHS: skipped

The standard is public (ARIB RCR STD-28, the English translation is a free download). The downlink control slot (UW, CI, CS-ID 42
bits, I 62 bits, CRC-16, PN(10,3) scramble) and the broadcasting messages are specified there. It was not built in this round:
the public PHS networks have mostly closed (China, Japan, Taiwan), so there is little left to receive. Several details read from the
figures (the scramble register taps, the CRC output, the bit order inside the 6-bit first octet) also could not be checked
without a recording. The mapping of the 8-octet broadcasting messages onto the 62-bit I field across slots needs a careful read first.

## Mode numbers

CDMA2000 cells: family 660, engine standard 662, poll slot 660. TD-SCDMA cells: family 661, engine standard 663, poll slot 661
(docs/MODE-NUMBERS.md).
