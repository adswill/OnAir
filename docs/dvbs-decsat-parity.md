# DVB-S/S2 in OnAir against DecSAT

[DecSAT](https://github.com/CasualArclamp/DecSAT) by Arclamp (CasualArclamp, GPL-3.0-or-later) is a satellite signal analyser written in Rust. The owner of OnAir asked
for everything it does that fits OnAir. This table lists every capability of DecSAT (its README, `docs/DESIGN.md`, `docs/STATUS.md`, its crates and apps), what OnAir had
at the start of this work, what was done, and what was left out and why. Status words: **done** (OnAir already had it), **added** (written in this branch), **partial**,
**planned** (to be added in this branch, in the order of the Chunk column), **skipped** (with the reason). Code taken from DecSAT keeps its notice and has a credits row

Two kinds of skip: "descrambling, not done by policy" (BISS, CSA, any CA or key-based scheme: only detected and labelled) and "no verified spec/capture" (nothing public to implement or test against).
(THIRD_PARTY.md, CONTRIBUTORS.md); nothing encrypted is ever decoded, whatever DecSAT does.

Chunks: 1 GSE, IP, PCAP and the IP view; 2 MPE and ULE; 3 ACM, VCM, multistream, S2X superframes and GSE-HEM; 4 SI/PSI tables, DSM-CC carousels, data broadcast, TS payloads;
5 multicast media and the rest.

## Physical layer and demodulation

| DecSAT capability | OnAir | Chunk / reason |
|---|---|---|
| DVB-S2 PL demodulation, frame lock, MODCOD per frame | done (`core/src/dvbs`) | |
| Carrier recovery, Es/N0 (MER), Eb/N0 | done (Eb/N0 per information bit shown beside Es/N0) | |
| DVB-S (EN 300 421): code rate, rotation, inversion found blind; Viterbi, RS | done | |
| LDPC + BCH, all 21 S2 and 31 S2X codes, BBHEADER | done | |
| S2X PLS table, all 55 normal/short MODCODs, 2+4+2 8APSK to 256APSK | done | |
| S2X VL-SNR (medium FECFRAMEs, pi/2-BPSK, spread, 9 MODCODs) | done | |
| PL scrambling codes (Gold n found from pilots) | done (manual code + pilot hunt); S2X Table 19e preferred sequences | 3: check against Table 19e |
| Low roll-offs 0.15/0.10/0.05 signalled by RO alternating with 11 | done | |
| Blind symbol rate, roll-off, constellation, DVB-S2/S2X detection ("Identify") | done in the receiver's own hunt (spectrum, roll-off, pilots) | |
| Narrow carriers (down to about 10 kBd) | added: a fine second spectrum finds and measures carriers from about 6 kSym/s blind, in any capture with wide ones beside them; the front end and loops are per symbol (docs/dvbs.md, "Narrow carriers"); verified on synthetic signals only | 2026-10-23 |
| ACM/VCM: MODCOD per frame, VCM flag | done (telemetry), ACM terminal table added with GSE | 1, 3 |
| Multistream (ISI): one stream chosen; each ISI to its own stage | added: every ISI is listed (content, CCM/ACM, MODCODs, rate); the chosen one is decoded as a transport stream and every GSE, GSE-HEM or packet stream is read at the same time, each with its own reader | 3 |
| S2X superframing (Annex E): start sequence, format, formats 0 and 1 | added (ported; formats 2 to 7 are named only), with a phase correction over the removed symbols that DecSAT's stage does not have | 3 |
| Modulator (DVB-S/S2/S2X, HackRF TX / IQ file) | done (`dvbs_tx.cpp`, TX framework) | 3: GSE and IP test signals |
| Waterfall, zoom, pan; carrier detection over a whole span | done (the app's spectrum views; the DVB-S/S2 mode takes one carrier) | |
| VFOs: many carriers decoded at once from one wide capture, survey of every carrier | added: the Receiver tab lists every carrier found in the spectrum (offset, symbol rate, roll-off, level, C/N) and "Decode" makes the receiver work on that one; decoding several at once is not done (one receiver per input) | 5 |
| IQ file replay (cs8, cu8, cs16, cs24, cf32, WAV from SDR#/HDSDR/SDRuno/SDR++) | done (files with rate and format in the name or header) | |
| Live HackRF One 2-20 MS/s (pure Rust USB), LNB LO, input-level warning | done (HackRF over USB, LNB help, AGC) | |
| IQ recorder | done | |

## Transport stream and its tables

| DecSAT capability | OnAir | Chunk / reason |
|---|---|---|
| MPEG-TS out: file, UDP, TCP/HTTP | done (file, UDP, HLS, player) | |
| TS analyser: PIDs, services, now/next, network, tables, errors, bitrate per PID | done (`TsDemux`) | |
| PAT, PMT, SDT actual, NIT actual (name, LCN), EIT, TDT | done | |
| NIT transponder lists (frequency, polarisation, symbol rate, FEC, orbital position), other networks, BAT, SDT other, EIT other, TOT, CAT | added (`tv/si_tables`): satellite, cable, terrestrial and T2 delivery descriptors, other networks, bouquets, services and event counts of other transport streams, local time offsets, the CAT with CA system names (signalling only); shown in the TS tab | 4 |
| EPG for every service, schedule | done for present/following and the schedule of the actual TS | 4: other TS |
| Teletext, DVB subtitles | done | |
| In-app player (H.264, MPEG-2, HEVC; MP2/AAC/AC-3) | done | |
| BISS descrambling (DVB-CSA, user key) | skipped (descrambling, not done by policy): this project never descrambles; payloads scrambled with CSA or any CA are reported as scrambled, nothing more | owner's rule |
| DSM-CC data and object carousels, file extraction | added (`tv/dsmcc`): DSI, DII, DDB, zlib modules, BIOP service gateway, directories and files, saved to a folder with cleaned paths; carousel list and file list in the TS tab | 4 |
| Data broadcast signalling (AIT, MHEG-5, HbbTV) | added: the AIT with application names, control codes, transport protocols, base URLs and initial paths; MHEG-5 applications travel in the object carousel and are saved with it | 4 |
| SCTE-35, EBU data, other TS payloads named | added (`tv/ts_data`): SCTE-35 splice insert and time signal cues, EBU teletext/subtitles/VPS/WSS/closed captions in PES, DVB subtitles, DSM-CC, MPE, DIT, SIT, RST labelled; scrambled packets only labelled | 4 |
| Stuffing and null statistics | done | |

## IP data

| DecSAT capability | OnAir | Chunk / reason |
|---|---|---|
| GSE de-encapsulation (TS 102 606-1), fragments, CRC-32 | added | 1 (`dvbs_ip`) |
| GSE variants (length counting the header, split fragment id) chosen from the data; blind IPv4 search | added | 1 |
| Vendor CRC-32 trailer on the BBFRAME | added | 1 |
| IP validation, IPv4 fragment reassembly, flow statistics, protocols | added | 1 |
| PCAP export (link type RAW) | added | 1 |
| Flows whose UDP checksum fails marked as encrypted in transit; non-IP PDUs that read as random bytes marked as ciphertext | added; the data is never shown or decoded | 1 |
| ACM terminal table (GSE label, its IP addresses, the MODCODs it is sent in) | added | 1 |
| GSE-HEM, GSE-Lite (S2X) | added: HEM headers (CRC-8 XOR 1) read, the sliced packets joined by SYNCD; GSE-Lite is named (its packets are plain GSE) | 3 |
| MPE (table 0x3E) to IP | added (also behind LLC/SNAP; scrambled sections skipped, never decoded) | 2 |
| ULE (RFC 4326) to IP | added (not in DecSAT: written from the RFC; a PID is taken up after three valid SNDUs) | 2 |
| Multicast MPEG-TS in UDP/RTP found, analysed and played | added: found by the sync bytes, in UDP or behind an RTP header, analysed by a TS demultiplexer per flow (services, scrambling flag); playing in the app's player is not wired (the stream can be saved as a PCAP and replayed) | 5 |
| Multicast audio: SAP/SDP names, RTP AAC/Opus, now-playing | added: SAP announcements with their SDP (names, media, codecs), RTP streams found from their sequence numbers (payload type, codec from the announcement, loss, rate); audio is not decoded (codecs are user-installed add-ons here) | 5 |
| DVB-RCS signalling (EN 301 790 / 545-2: NCR, TIM, SCT/FCT/TCT/TBTP/CMT, RMT) | added: TS analysis tab "Interactive" (forward and return links, superframes, frame and timeslot types, NCR interval and clock error, TIM status, tables declared but not sent) | 5 |
| A hub's private burst plan / inroute signalling (Newtec D1h, worked out from a capture) | skipped (no verified spec/capture): no public specification, DecSAT derived it from one recording; we have none to check against |
| Return pools and TDMA/MF-TDMA burst demodulation | skipped (no verified spec/capture): needs the VFO front end and burst demodulators found from live recordings |

## Other protocols DecSAT decodes

| DecSAT capability | OnAir | Chunk / reason |
|---|---|---|
| Satellite beacons: carrier, drift, C/N0, PCM/PSK/PM telemetry, IRIG 106 and CCSDS frames, counter, major frame map, word plot, pointing tone | added earlier in this branch (TV sat beacon decoder: carrier, subcarrier, bi-phase, frames, counter, checksum, decoded view, export); CCSDS Reed-Solomon, randomiser, layout search and TM/AOS headers added; K=7 under the symbols, the word plot and the pointing tone still planned | 5 |
| Carrier ID (DVB-CID, TS 103 129) | added: framing, BCH, scrambler and fields after DecSAT; receiver of our own (FFT acquisition, early/late tracking, half-bit frequency, four copies combined); Receiver tab "Carrier ID" (off by default); tested on synthetic signals with offset and clock error, no live recording | 5 |
| TPC 2964 (IESS-315), Comtech TPC 7/8, Q-Flex FastLink, K=7 Viterbi SCPC, IBS/IESS-309 with apt-X, E1 and G.728 voice, Comtech CDM modem types, radar feeds (ASTERIX, RCMS), IS-19 DAMA | skipped (no verified spec/capture): professional VSAT and SCPC links with vendor framings, mostly worked out from live captures; they need DecSAT's blind carrier front end and recordings we do not have. Voice codecs would also be user-installed add-ons here |
| Generic PSK/APSK/QAM to symbols | added: "Write the symbols to a file" in the Receiver tab (float32 I/Q after timing and carrier recovery, up to 512 MB) | 5 |
| Survey of every carrier in a span | added with the carrier list above | |

## Apps, tools and project

| DecSAT | OnAir | Chunk / reason |
|---|---|---|
| GUI (waterfall, VFOs, cards) | done (own GUI) | |
| CLI `decsat decode` / `modcods` | done (`dect2cli`) | 1: `--ip-pcap` |
| Signal gallery HTML | skipped: documentation of its own signals; the viewer of extracted carousel files is added (Carousels > Save files writes index.html with previews) | |
| Opus and xaac (AAC SBR/PS) libraries | skipped: multicast audio needs patented codecs as user-installed add-ons, see docs/CODECS.md | |
| Tests (golden vectors from recordings, synthetic signals) | done in kind: synthetic and loopback tests per feature; golden vectors that do not need a recording | each chunk |
