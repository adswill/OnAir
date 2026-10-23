# DVB-S, DVB-S2 and DVB-S2X in OnAir

The receiver takes the L-band IF of an LNB and finds the carrier, symbol rate, roll-off, modulation and code rate by itself (DVB-S: EN 300 421; DVB-S2: EN 302 307-1;
DVB-S2X: EN 302 307-2, including the VL-SNR frames). See `docs/dvbs-decsat-parity.md` for what was added from the study of DecSAT and what was left out.

## IP data (GSE)

When the BBFRAMEs of a DVB-S2 signal carry a generic stream instead of a transport stream (an IP data service, a VSAT outbound), the receiver de-encapsulates **GSE**
(ETSI TS 102 606-1): packets, fragments reassembled across BBFRAMEs, CRC-32 checked. Four readings of GSE are tried on every data field (the standard, GSE_LENGTH counting its
own header, a split 6-bit fragment id and 2-bit counter, and both) and the one that yields valid IP packets is used; a vendor's CRC-32 at the end of the BBFRAME is checked and
taken off. When no reading produces IP, the data fields are searched blind for IPv4 packets. IP packets are validated strictly (version, lengths, header checksum), IPv4
fragments are put together, and the packets are counted.

The receiver page shows, under IP DATA: the source, packets, bytes, IPv4 and IPv6, the protocols, the destination ports, the biggest flows (source, destination, protocol,
packets, bytes), and for an ACM outbound a table of **GSE labels** (a terminal's address): its PDUs and bytes, the IP addresses sent to it and the MODCODs it is sent in, which is
the terminal's link as the hub sees it. **Write a PCAP file** writes every IP packet to `dvbs_ip.pcap` in the data folder (libpcap, link type RAW), also with the window hidden.
From the command line, `dect2cli --standard dvbs --ip-pcap file.pcap`.

**Privacy and encryption.** Satellite IP links can carry other people's private traffic; the user is responsible for what local law allows them to receive and keep. Nothing
encrypted or scrambled is decoded, and payloads are never shown: the view holds addresses, ports, protocols and counts. A flow whose headers are good but whose UDP checksums
fail was changed after the sender made them (an inline encryptor) and is marked encrypted; GSE PDUs that are not IP and read as random bytes (8 bits per byte over 32 KiB) are
called ciphertext. The PCAP file holds the packets as received (what is encrypted stays encrypted).

## Input streams, GSE-HEM and super-frames

A DVB-S2 carrier can interleave several input streams (multistream, told apart by the ISI of the BBHEADER). The receiver page lists every stream under INPUT STREAMS: what it
carries (transport stream, GSE, GSE-HEM, GSE-Lite, transport packets in a generic stream such as a hub's signalling, other fixed-length packets), CCM or ACM/VCM, frames, the MODCODs
used and the useful bit rate. The chosen ISI is decoded as a transport stream as before; every other stream that carries GSE is de-encapsulated at the same time, each with its own reader
(a VSAT hub often sends the user IP as GSE on one ISI and its signalling on another). **GSE-HEM** (EN 302 307-2 clause 5.1.7, TS/GS = 10, the BBHEADER's CRC-8 XOR 1): the GSE packets are
sliced across BBFRAMEs and SYNCD points at the first one that starts in a data field; the slices are joined and the packets handled as GSE.

**S2X super-frames** (EN 302 307-2 Annex E, formats 0 and 1): the stream of symbols is searched for the 270-symbol start sequence (a differential correlation, blind to the carrier offset);
a super-frame has 612 540 symbols with a 720-symbol SOSF and SFFI head and, when switched on, a 36-symbol pilot field after every 16 capacity units. The format is read from the SFFI (a
simplex code), the pilots tell whether they are on, and the stage takes those symbols out (turning the symbols after each gap back by the carrier phase the gap carried away) so that the
receiver sees plain PLFRAMEs; with super-frame pilots the PLHEADER's pilot bit then means that the pilots are the super-frame's own and the frames have none. The receiver page notes the format,
the pilots and the number of super-frames read; formats 2 to 7 are named, not decoded. A carrier without super-frames is passed through unchanged after three super-frames' worth of symbols.
The code is adapted from DecSAT (`decsat-frame/superframe.rs`, `decsat-engine/superframe.rs`).

## IP in the transport stream (MPE and ULE)

The packets of the transport stream that the receiver delivers are also searched for IP: **MPE** (EN 301 192: sections with table_id 0x3E on any PID that carries one; a PES start
rules a PID out; the datagram is taken also from behind LLC/SNAP; sections whose payload is scrambled are skipped, never decoded) and **ULE** (RFC 4326: SNDUs in the TS payload, CRC-32
checked, bridged Ethernet frames and IPv6 as well; a PID is accepted after three valid SNDUs that carry IP, so video and audio PIDs are never taken for it). Their packets go to the same
statistics and PCAP file as GSE; the IP DATA source line says GSE, MPE, ULE or a mix. MPE is ported from DecSAT (`decsat-ts/mpe.rs`); ULE is written from the RFC.

## Tables, carousels and data broadcast (the TS tab)

The TS tab has views for the service information of the stream: **Networks** (the NIT: network names and every transponder with delivery system, frequency, polarisation, symbol rate,
modulation, FEC, orbital position and service count; other networks too), **Other services** (the SDT other: names and providers of services in other transport streams, and the
event counts and running events of the EIT other), **Bouquets** (the BAT), **Time** (the TDT, the TOT with local time offsets), **Conditional access** (the CAT: the CA systems and their
EMM PIDs by name, and which services arrive scrambled; signalling only, nothing is descrambled), **Carousels** (DSM-CC data and object carousels: modules, completeness, the file tree of
an object carousel; a progress bar per carousel, the file list with size and completeness, **Save all files** writes them to `carousels/pid_XXXX/...` in the data folder with cleaned paths and an `index.html`, and clicking a file previews it: text as text, PNG/JPEG as a picture, MHEG-5 files as the named object tree and, for a Scene, a static drawing of its Rectangles, Text and Bitmaps; experimental, scripts and interactive links are not run or followed) and **Data** (the AIT: HbbTV and MHP applications with name, control
code, transport (object carousel or HTTP URL base) and initial path; SCTE-35 cues; EBU data and every private PID labelled by what it carries). The sources are `core/src/tv/si_tables.cpp`,
`dsmcc.cpp`, `ts_data.cpp`, `mheg.cpp` and `mheg_render.cpp` (MHEG-5 tag names from the ISO13522-MHEG-5 ASN.1 module as published in the RedButton engine) and the view in `app/panels/carousel_view.cpp`. Hong Kong TVB and other DTMB services send weather and stock pages this way (issue 176).

**Multicast media** in the IP view: SAP announcements with their SDP (session names, media, codecs), RTP streams (payload type, codec, loss, rate) and MPEG-TS in UDP or RTP, each
analysed by a TS demultiplexer (services found, scrambling noted). Audio and video are not decoded here.

## Error correction near the threshold (3.0)

The DVB-S2/S2X LDPC decoder is layered min-sum with the two-minimum box-plus correction (`LdpcCode::decodeFast(..., boxCorr)`): every edge except the one holding the smallest input gets the box-plus of the two smallest inputs instead of the smallest alone. That brings min-sum closer to belief propagation and cuts the iterations. The normalisation factor (`s2LdpcAlpha(mod, rate, short)`) is measured per MODCOD, all 55 S2X ones included. Before, it was set per code rate, and that cost 0.1 to 0.5 dB.

The 8-bit decoder takes the same factor. 32APSK and up stay on the float decoder: the 8-bit range clips their weak bits. Metal and Direct3D 11 run the same rule. The Vulkan kernel still runs plain min-sum, and the CPU retry of the frames it fails gives the same answer. VL-SNR uses the corrected decoder too: BPSK 1/5 SF2 now decodes 0.3 dB below table 20.

Measured through the whole receiver at 1 Msym/s, 2 Msps, 8-bit samples, 5 s per point. The threshold is the lowest Es/N0 from which every higher point has FER ≤ 15%, given relative to table 13:

| MODCOD | before | after |
|---|---|---|
| QPSK 1/4 | +0.1 | −0.3 |
| QPSK 1/2 | +0.2 | −0.1 |
| QPSK 3/5 | +0.5 | ≤ 0.0 |
| 8PSK 3/5 pilots | +0.3 | ≤ 0.0 |
| 8PSK 2/3 pilots | −0.1 | −0.2 |
| 32APSK 4/5 pilots | +0.2 | ≤ 0.0 |
| QPSK 3/5 short | +0.3 | ≤ 0.0 |

Two acquisition changes go with it:

- The second header of the frequency fix is now looked for 8 symbols either side of where it is expected, because the symbol clock may slip while it settles.
- A lock whose headers decode but whose first 16 frames all fail searches again, up to 3 times. The cause is a carrier frequency a whole number of cycles per frame off, which the headers cannot see.

Before these changes, QPSK 1/4 at the table 13 value and 8PSK 2/3 0.2 dB below it lost the whole recording.

`tests/dvbs/test_dvbs_s2fec.cpp` checks QPSK 3/5, 8PSK 3/5, 32APSK 4/5, short QPSK 3/5 and S2X 8PSK 23/36 0.15 dB above the table on an ideal channel (12 frames each).

## Credits

The GSE, IP and MPE code is adapted from DecSAT by Arclamp (CasualArclamp, GPL-3.0-or-later; https://github.com/CasualArclamp/DecSAT), crates `decsat-gse` and `decsat-ip`, rewritten in
C++; its notes on what was seen on real links are kept in the comments. DecSAT credits dontlookup (MIT) for the two GSE variants. See THIRD_PARTY.md.

## Tests

`tests/dvbs/test_dvbs_ip.cpp`: GSE round trips for every variant with fragmentation, the variant picked from the data, a link without IP, ciphertext recognised, a vendor CRC trailer,
a lost fragment, bridged Ethernet, the blind search, IP fragments in any order, flow statistics, encrypted flows and the PCAP file. `tests/dvbs/test_dvbs_gse.cpp`: the whole chain
(DVB-S2 QPSK 3/5 with 8-bit samples and noise, UDP in GSE with fragmented PDUs and three terminals), for two GSE variants, ending in a PCAP file that holds exactly the packets that were sent.

`tests/dvbs/test_dvbs_streams.cpp`: the HEM deframer on its own, GSE-HEM and a multistream carrier (ISI 0 transport stream, ISI 1 GSE) through the whole chain. `tests/dvbs/test_dvbs_sf.cpp`: the sequences, the stripper (formats 0 and 1, with and without super-frame pilots, noise and a carrier offset, a carrier without super-frames, and the exact symbols it passes on) and the whole receiver on a super-framed carrier.

`tests/dvbs/test_dvbs_si.cpp`: the NIT with satellite, cable and terrestrial transponders, another network, a bouquet, SDT other, EIT other, the TOT and the CAT. `tests/dvbs/test_dvbs_dsmcc.cpp`: an object carousel with directories and files, plain and zlib modules, blocks before the DII, saving to a folder. `tests/dvbs/test_dvbs_tsdata.cpp`: the AIT, SCTE-35, EBU data and labelled PIDs.
