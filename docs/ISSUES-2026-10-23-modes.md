# Issues round 2026-10-23: new modes, decoders and decoder requests

Branch `issue-modes-030` (from `integ-30`; merges `issue-modes-pactor-030`, `issue-modes-atv-030`, `issue-modes-mheg-030`). Reply drafts in
`/private/tmp/claude-501/-Users-kristijonasadomaitis-dvb-t2-decoder/6c46c5ab-dc6f-4964-bf96-43f9e5f83544/scratchpad/replies-3.0/` (nothing
posted). Mode numbers: PSK demodulator family 202 (std 204, active 203, slot 163), PACTOR-I family 203 (std 205, active 204, slot 164);
`kNumFamilies` 204 (docs/MODE-NUMBERS.md).

| # | title | request | status | commit / branch | not verified | reply draft |
|---|---|---|---|---|---|---|
| 282 | BPSK mode and analyser | BPSK (later QPSK) with a rate analyser and constellation | added now (experimental): PSK demodulator mode, BPSK/QPSK/OQPSK/8PSK, blind rate/order/shape/Doppler, constellation, eye, bits, sync, save | 1543dfc39, c8809bace | no real satellite signal; FEC/frames of particular satellites not decoded | 282-bpsk-analyser.md |
| 281 | Iridium extra | voice with external codec, ACARS on the map, LAP dump, pagers | done before | 1be08946c (iridium-extra-030), iridium-voice-030 | real Iridium capture | 281-iridium.md |
| 273 | Tetrapol polarity | downlink INV / uplink NOR, automatic | done before | 966a7231a (tetrapol-voice-030) | real network | 273-tetrapol-polarity.md |
| 241 | Tetrapol codec audio | RP-CELP voice | done before (plug-in path only, codec never shipped, clear calls only) | 966a7231a, 23fc3289b | real clear call, codec | 241-tetrapol-voice.md |
| 229 | LoRa satellite decoder | TinyGS-like LoRa sats 400/900 MHz | done before + added now: 41.7 kHz bandwidth, FOSSASAT 401.703 and 137.055 MHz presets | 05c7aad94; this branch (lorasat commit) | real pass; presets are the reporter's settings | 229-lora-satellites.md |
| 256 | Argos | decoder at 465.9875 MHz | done before (uplink + downlink) | 39389c9b8 (mode-argos-sigfox-030) | real recording; downlink content format not public | 256-argos.md |
| 255 | RWM | Russian time signal | done before | 5d5a7aace | on air | 255-rwm.md |
| 214 | GTH6/CF-06, SARSAT 1544, TX baseband | sonde decoder, downlink + auto search, baseband out | partly: SARSAT whole-capture search added now (on by itself 1543.5-1546 MHz); TX to IQ file done before; GTH6/CF-06 needs a format description + recordings | ef25bc669; 2a7f240b7 | real 1544 MHz downlink (reporter's Dropbox file); GTH6 | 214-gth6-sarsat.md |
| 216 | PACTOR 1-4 | PACTOR support | partly: PACTOR-I monitor added (experimental); II-IV not possible (proprietary SCS, no public spec, no reverse engineering) | issue-modes-pactor-030 | real recording; status-byte layout, CRC init/bit order and the full Huffman table are assumptions (the table is user-loaded) | 216-pactor.md |
| 211 | SELCAL | SELCAL on 8.879 USB | done before | 91cd1fc53 | real HF SELCAL | 211-selcal.md |
| 192 | Morse Micro HaLow | decode bonded-channel networks | partly (6 Msps for 1-4 MHz, 2 MHz sub-channel scan) | fd3e6c020, c17b8770f | real Morse Micro capture | 192-halow.md |
| 167 | HaLow 8/16 MHz | wider channels | done before | 2a62aa881, 758f6484d | real capture | 167-halow-widths.md |
| 165 | HD Radio decode/encode | names, logos; encoding | decode done before; encoding not possible (patented HDC encoder, broadcast licence; only a user plug-in behind the Arm gate: owner decision) | in tree | KTLK/KSLZ long names on air | 165-hd-radio.md |
| 164 | LTE + NR scanning | band filters, MFBI, multi-PLMN, alerts, NR multi-carrier | partly: filters/PLMN/warning flag done before; SIB1 v8h0/v9e0/v920 (bands above 64, MFBI list) added now; alert text and NR multi-carrier open | 60ac2f4fd, 935914656; 58ae98fe8 | real SIB1 with MFBI / an alert | 164-lte-nr.md |
| 189 | Globalstar, GNSS, Iridium, CDR | labels, sync-loss log, voice/SMS, DRA+ bitrate | done before (BeiDou report open: needs capture) | 3322defff, b5d9145c4, d45836f42 | BeiDou | 189-globalstar-gnss.md |
| 176 | MHEG-5 for DTMB | read TVB MHEG-5 | added now (experimental): carousel viewer, save files, previews, named object tree (tags from the RedButton ASN.1), static scene renderer | issue-modes-mheg-030 | TVB stream (link expired); UK-profile colour/font details from memory; Big5 text | 176-mheg5.md |
| 121 | DRA+ for CDR | DRA+ audio | done before | 0.2.6, aede16848, dtmb-dra-030 | (#275 is the bugs group) | 121-dra.md |
| 73 | more analogue TV systems | MAC/MUSE/FM-sat/audio/EDTV/mechanical | partly: NICAM-728 and A2 added now; list in docs/ANALOGUE-TV.md; MUSE/HD-MAC/ACTV etc. not now (reasons there); BTSC stereo not possible (licensed dbx-TV) | issue-modes-atv-030 | real NICAM/A2 | 73-analogue-systems.md |
| 239 | analogue TV snow | keep showing untuned picture | added now (checkbox, off by default); centred-carrier lock fix; Airspy 2.5 Msps below minimum | issue-modes-atv-030 | HackRF to Airspy case | 239-analogue-tv.md |
| 271 | NFC | NFC decoding | done before (mode 148) | 808989280 | real capture | 271-nfc.md |
| 223 | Cospas wider bandwidth | 100 kHz, fewer false decodes | done before | ec1bf9292, 94e47accf | - | 223-cospas.md |
| 95 | FLEX Next (found in the scan) | multimon-ng flex_next | done before: it is ordinary FLEX; fixes from the sent recordings | 881de146d (flexnext-030) | other bursts | 95-flex-next.md |
| 213 | FM audio, C-QUAM, VHF-DSC, GK-2A, TETRA, BPC/LORAN/DGPS (scan) | several | handled by the features group (draft 213-fm-audio-cquam.md); C-QUAM, VHF-DSC, TETRA voice done before; GK-2A needs a sample | f53dfea06, 81b3fab5d, e32b4dde8 | GK-2A | (existing) |
| 122 | Android (listed sibling) | Android app | done before (0.2.6), not this group | - | - | (existing 122-android.md) |

Counts: done before 14, added now 4 (282, 176, 239 and the LoRa / SARSAT / MFBI additions inside 229 / 214 / 164), partly 6 (214, 216, 192,
164, 73, 189), not possible 2 parts (PACTOR-II-IV, HD Radio encoding), needs owner 2 (HD Radio TX as a plug-in, PACTOR Huffman table).
