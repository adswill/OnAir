# Broadcast additions in 3.8

Branch `feat38-broadcast`. Every item has a unit test on generated signals: nothing here has been run on a real recording unless stated, so treat all of it as simulated only.

## Done

| Item | What it does | Test | Status |
|---|---|---|---|
| T2-Lite in FEF | The T2 receiver follows T2-Base or T2-Lite in a mixed signal and steps over the other's frames (`T2Receiver::selectLayer`, a combo in the TV panel) | `t2_fef_layers`: base and lite frames interleaved at 1, 2, 3 frames per FEF, offsets, noise; identical BB frames | simulated |
| ATSC-M/H | Detects MHE groups in the transport stream (118 packets in a 156-packet slot), PID and share of slots, status bar line | `atsc_mh`: every period, loss, false-positive streams | presence only: the M/H services are not decoded (A/153 Part 2 SCCC, RS frames, FIC) |
| BTSC | NTSC stereo and SAP on the 4.5 MHz sound carrier, pilot-locked; dbx-TV compander per OET Bulletin 60 | `atv_btsc`: 37 to 40 dB separation, SAP, CFO and noise sweeps | simulated |
| RDS2, SCA | RDS2 groups from 66.5, 71.25, 76 kHz; SCA detection and audio at 67 and 92 kHz, in the FM panel | `fm_subcarrier`: 3 streams, clock offset, noise; SCA tones; negatives | simulated; RFT and the C-group layer are in IEC 62106-2, not public, not decoded |
| J.83 annex B | 64 and 256-QAM clear QAM: precoder, 16-state punctured Viterbi, RS(128,122), convolutional interleaver, randomizer, trailer sync; behind the DVB-C demodulator (Annex B checkbox, roll-off and rates per Table B.3) | `dvbc_j83b` (symbol level, all interleaver modes, rotations, swapped streams) and `dvbc_j83b_rf` (full demodulator with CFO and clock error) | simulated. A/B stream assignment comes from reading the figures; both orders are tried. The MPEG parity checksum is not used to find packets (the continuity counters are) |
| DOCSIS structure | MAC frames on PID 0x1FFE: SYNC, UCD, MAP, MDD (upstream and downstream channel table) in the DVB-C panel; data frames counted only | `dvbc_docsis` | simulated; TLV identifiers from Wireshark's dissector; no subscriber data |
| UMTS cells | Family 480: cell search by primary scrambling code (P-SCH, S-SCH table, CPICH), Ec/Io, level, CFO | `umts_cell`, `umts_rx`: groups, several cells, CFO, noise | simulated; the BCH (operator, cell id) is not decoded |
| GSM census | GSM-R mode tuning extended and the ARFCN table now covers DCS 1800 | existing GSM-R tests | the receiver finds any GSM cell; there is no separate census UI |
| PSIP | MGT, VCT source ids, EIT titles, ETT descriptions, STT into the same guide as DVB | `psip` | simulated; Huffman text (A/65 Annex C) left blank and counted |
| CEA-608/708 | Captions from video user data; 608 CC1 pop-on, roll-up, paint-on; 708 service 1 as text; shown in the TV panel | `cc_decode` | simulated |
| ARIB text | STD-B24 8-unit text with designations and shifts, for ISDB-T and ISDB-S | `arib_text` | test strings; Gaiji not mapped |
| TR 101 290 | Priority 1, 2, 3 checks with counts and PCR jitter, in the Multiplex view | `tr101290` | simulated |
| T2-MI ingest | Finds a T2-MI PID by its CRCs and rebuilds each PLP's transport stream (TV panel, selectable PLP) | `t2mi_in` | simulated |
| EMWIN reader | WMO heading, AWIPS id, VTEC, UGC, polygons (`goes/emwin_parse`) | `goes_emwin` | sample text |
| DCF77 phase | Correlates the 512-chip code, reads the bit of each second and finds the second by itself; shown in the time panel | `time_dcf_phase` | simulated |
| Wenet telemetry | GPS, system, power, orientation and text packets in the SSDV panel | `ssdv_wenet_tel` | layouts from WenetPackets.py; simulated packets |

## Not done

- 5G Broadcast (#441): not tried; the FeMBMS subframe structure needs a separate read of TS 36.211/36.331.
- T2 common PLP merge, sub-slicing, MISO: not tried in this branch.
- HD Radio traffic and weather maps, DRM SDC types, DAB TPEG and packet FEC: not tried.
- WWVB phase code: the NIST format is public; not implemented. DCF77 is the only phase code decoded.
- ATSC-M/H service decode (see above).
- UMTS BCH, RDS2 RFT.

## Numbers

UMTS cells: family 480, engine standard 482, active standard 481, poll slot 440.

## Sources

Rec. ITU-T J.83 (12/2007) Annex B; ETSI TS 125 213 (scrambling, SCH table 4); FCC OET Bulletin 60 (BTSC), reading the HamCall copy of the text; ITU-R BS.643-4 (RDS2 subcarriers); projecthorus/wenet WenetPackets.py (GPLv3 or later, used for layouts, no code copied); Wireshark packet-docsis-mgmt.c (GPLv2 or later, used for numeric identifiers).
