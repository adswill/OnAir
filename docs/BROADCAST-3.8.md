# Broadcast additions in 3.8

Branch `feat38-broadcast`. Every item has a unit test on generated signals: nothing here has been run on a real recording, so treat all of it as simulated only.

## Done

| Item | What it does | Test | Status |
|---|---|---|---|
| T2-Lite in FEF | The T2 receiver follows T2-Base or T2-Lite in a mixed signal and steps over the other's frames (`T2Receiver::selectLayer`, a combo in the TV panel) | `t2_fef_layers`: base and lite frames interleaved at 1, 2, 3 frames per FEF, offsets, noise; identical BB frames | simulated |
| ATSC-M/H | Detects MHE groups in the transport stream (118 packets in a 156-packet slot), PID and share of slots, status bar line | `atsc_mh`: every period, loss, false-positive streams | presence only: the M/H services are not decoded (A/153 Part 2 SCCC, RS frames, FIC) |
| BTSC | NTSC stereo and SAP on the 4.5 MHz sound carrier, pilot-locked; dbx-TV compander per OET Bulletin 60 | `atv_btsc`: 37 to 40 dB separation, SAP, CFO and noise sweeps | simulated |
| RDS2, SCA | RDS2 groups from 66.5, 71.25, 76 kHz; SCA detection and audio at 67 and 92 kHz, in the FM panel | `fm_subcarrier`: 3 streams, clock offset, noise; SCA tones; negatives | simulated; RFT and the C-group layer are in IEC 62106-2, not public, not decoded |
| J.83 annex B | 64 and 256-QAM clear QAM: precoder, 16-state punctured Viterbi, RS(128,122), convolutional interleaver, randomizer, trailer sync; behind the DVB-C demodulator (Annex B checkbox, roll-off and rates per Table B.3) | `dvbc_j83b` (symbol level, all interleaver modes, rotations, swapped streams) and `dvbc_j83b_rf` (full demodulator with CFO and clock error) | simulated. A/B stream assignment comes from reading the figures; both orders are tried. The MPEG parity checksum is not used to find packets (the continuity counters are) |
| DOCSIS structure | MAC frames on PID 0x1FFE: SYNC, UCD, MAP, MDD (upstream and downstream channel table) in the DVB-C panel; data frames counted only | `dvbc_docsis` | simulated; TLV identifiers from Wireshark's dissector; no subscriber data |
| UMTS cells | Family 480: cell search by primary scrambling code (P-SCH, S-SCH table, CPICH), Ec/Io, level, CFO; P-CCPCH to the transport block and the MIB (MCC, MNC) and SIB 3 (cell identity) | `umts_cell`, `umts_rx`: groups, several cells, CFO, noise, BCH at 5 dB | simulated; only the MIB and SIB 3 layouts are checked against TS 25.331 |
| GSM census | The GSM-R mode's existing band scan and CSV export now cover DCS 1800 (a third band checkbox and the ARFCN table) | `gsmr_rx` adds the DCS raster checks | the standalone GSM test build here fails 6 cell checks both with and without this change, so check CI |
| PSIP | MGT, VCT source ids, EIT titles, ETT descriptions, STT into the same guide as DVB; Huffman text with the Annex C tables | `psip`, with the worked example of A/65 annex F | simulated |
| CEA-608/708 | Captions from video user data; 608 CC1 pop-on, roll-up, paint-on; 708 service 1 as text; shown in the TV panel | `cc_decode` | simulated |
| ARIB text | STD-B24 8-unit text with designations and shifts, for ISDB-T and ISDB-S | `arib_text` | test strings; Gaiji not mapped |
| TR 101 290 | Priority 1, 2, 3 checks with counts and PCR jitter, in the Multiplex view | `tr101290` | simulated |
| T2-MI ingest | Finds a T2-MI PID by its CRCs and rebuilds each PLP's transport stream (TV panel, selectable PLP) | `t2mi_in` | simulated |
| EMWIN reader | WMO heading, AWIPS id, VTEC, UGC, polygons (`goes/emwin_parse`) | `goes_emwin` | sample text |
| DCF77 phase | Correlates the 512-chip code, reads the bit of each second and finds the second by itself; shown in the time panel | `time_dcf_phase` | simulated |
| T2 common PLP, sub-slicing | The generator makes several PLPs; type 2 cells gathered from their sub-slices, the group's common PLP decoded and merged (annex D) | `t2_plps`: common + type 1, 8 and 1 sub-slices, 16 sub-slices, another group; merge unit test | simulated; the merge assumes the two PLPs line up per T2 frame |
| T2 MISO | Generator for both transmitter groups; sum and difference pilots, Alamouti in P2 and data, MISO pilot and closing-symbol rules | `t2_miso`: equal paths, group 2 at -6 dB, delayed, 64-QAM, T2-Lite MISO, group 2 off | simulated |
| DRM SDC | Types 2, 3, 4, 6, 7, 14 in the DRM panel (schedules, regions, announcements, alternative frequencies, CA, packet FEC parameters) | `drm_sdc_extra` | packets written with the encoder |
| DAB packet FEC | EN 300 401 5.3.5, RS(204,188) over the 188 x 12 table, applied when FIG 0/14 signals it | `dab_pktfec`: error counts, chunk sizes, resync, bad header | simulated |
| HD Radio maps | TTN traffic tiles into a 600 x 600 picture; DWRO radar and DWRI corners; in the HD Radio panel | `hdr_maps` | names from nrsc5-gui, not seen on a capture; no base map |
| WWVB phase | NIST 2013 frame: sync word, minute counter, Hamming(31,26), table 4 | `time_wwvb_phase`: CNR, CFO, polarity, start | simulated |
| Wenet telemetry | GPS, system, power, orientation and text packets in the SSDV panel | `ssdv_wenet_tel` | layouts from WenetPackets.py; simulated packets |

## Not done

- **ATSC-M/H service decode**: presence only. A/153 Part 2 needs the group byte map (figures), block processor, SCCC with iterative decoding against the 8-VSB trellis, RS frames and a matching generator. That is a separate piece of work, several times the size of annex B.
- **5G Broadcast (#441)**: not attempted. FeMBMS needs the CAS with MIB-MBMS and the long-CP PMCH; the existing LTE receiver covers none of the PMCH side.
- **DAB TPEG**: not built. The TPEG application specifications are ISO/CEN documents (ISO 18234), not public, so no decoder can be checked against them.
- **DRM packet FEC**: the SDC parameters are read; the FEC itself is not applied.
- RDS2 RFT (IEC 62106-2, not public).

## Numbers

UMTS cells: family 480, engine standard 482, active standard 481, poll slot 440.

## Sources

Rec. ITU-T J.83 (12/2007) Annex B; ETSI EN 302 755 V1.4.1; ETSI ES 201 980 V4.3.1; ETSI EN 300 401 V2.1.1; ETSI TS 125 212, TS 125 331; ATSC A/65:2013; ATSC A/153 Part 2; NIST Enhanced WWVB Broadcast Format; cmnybo/nrsc5-gui (file names only); ETSI TS 125 213 (scrambling, SCH table 4); FCC OET Bulletin 60 (BTSC), reading the HamCall copy of the text; ITU-R BS.643-4 (RDS2 subcarriers); projecthorus/wenet WenetPackets.py (GPLv3 or later, used for layouts, no code copied); Wireshark packet-docsis-mgmt.c (GPLv2 or later, used for numeric identifiers).
