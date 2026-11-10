# Leftover modes, 3.8 (branch feat38-leftovers)

Family 600, engine standards 602 (20 MHz) and 603 (narrow), telemetry 601 and 602, poll slots 600 and 601. One mode-list entry per protocol. Code: core/src/xlink, app/xlink, tests/xlink. Receive only.

## Added

| Protocol | What it decodes | Validated on |
|---|---|---|
| wfb-ng / OpenHD | New 802.11n HT receiver (core/src/xlink/ht_phy.cpp): HT-mixed format, 20 MHz, one stream, MCS 0-7, long and short GI, BCC, HT-SIG CRC checked. Frames recognised by the wfb-ng and OpenHD 802.11 layouts: link id and radio port, FEC block and fragment numbers, frames lost, session frames counted. Payload stays encrypted, structure only. | simulated only |
| New Packet Radio (NPR-70) | Si4463 4GFSK at 50, 100, 180, 300, 500 ksym/s: sync word (with the network ID), symbol rate, tuning offset. Frame contents not read. | simulated only |
| WINMOR | 1500 Hz alternating-phase pilot, tuning error, 4 raw frame-type tones. Frame contents not read. | simulated only |
| mioty (ETSI TS 103 357, TS-UNB uplink) | Pilot search over the 100 kHz channel, fit of the 17 uplink TSMA patterns (UPG1, UPG2, UPG3), coherent demodulation of the 24 core bursts, de-interleaving, rate 1/3 Viterbi, PN9, header and payload CRC-8, extension frames from the PHY header. MAC fields of the fixed MAC mode (address, counter, flags); payload stays encrypted. | simulated only |
| DECT NR+ (ETSI TS 103 636-3 / -4) | 1.728 MHz channel (mu 1, beta 1): STF with cover sequence, DRS channel estimate, physical control channel (QPSK, scrambling, LTE-design turbo code and rate matching, CRC-16 with the MIMO / beamforming masks), Type 1 and Type 2 control fields: network ID, transmitter and receiver IDs, length, MCS, HARQ. Data channel not decoded. | simulated only |

Settings come from public sources: wfb-ng src/wifibroadcast.hpp (GPL-3), OpenHD/wifibroadcast Ieee80211Header.hpp (GPL-3), NPR-70 firmware Si4463 configuration arrays (GPL-3) and the EZRadioPRO API documentation, WINMOR specification Rev 1.0.6 (public domain). THIRD_PARTY.md and the Credits window are updated.

## Test results (tests/xlink, all simulated, no real recording exists for any of these)

- HT PHY: MCS 0-7 x long / short GI x offsets 0, +90 kHz, -210 kHz all decode; 15 ppm clock offset, 150 ns echo inside the guard interval, two frames with an interfered one between them (one damaged, nothing wrong reported), noise only gives nothing. MCS 1 packets of 300 bytes: 15/20 decoded at 8 dB SNR, 19/20 at 10 dB, 0/20 below 6 dB.
- wfb-ng / OpenHD through XlinkReceiver at 20 Msps with +-3.1 MHz channel offset and at 40 Msps: all 39 frames, 4 links, lost-frame count right (1 fragment + 1 whole block of 12 in the test).
- NPR: all 5 rates x networks 0, 5, 15 x 3 offsets; noise sweep (sigma 0.2: 10/10, 0.3: 5/10); 4 Msps noise only gives nothing.
- WINMOR: offsets 0, 42, -83, 150 Hz at two noise levels, tone readings right; from a 96 kHz capture; noise only and a plain 1500 Hz carrier give nothing.
- mioty: all 17 uplink patterns with channel offsets up to +-9 kHz; extension frames (PSI 21 and 30); two frames overlapping in time; through XlinkReceiver from a 500 kHz capture. Noise sweep of 15-byte frames: 4/4 decoded down to Es/N0 4.3 dB, 2/4 at 1.8 dB. Noise only: no frame.
- DECT NR+: turbo and rate-matching round trip for both field sizes and all three CRC masks; packets with carrier offsets 0, 12 and -40 kHz; four packets back to back over a two-path channel; through XlinkReceiver from a 4 Msps capture 300 kHz off the middle. Noise sweep (SNR per sample): 6/20 at 0 dB, 20/20 from 3 dB. Noise only: nothing.
- The test signals come from generators written from the same reading of the specifications as the receivers, so a misreading would be shared by both; only a real recording can catch that.
- The Si4463 4FSK mapping and the WINMOR pilot format follow their documents. Real-signal behaviour of all three is untested.

## Limits of what is decoded (and why)

- wfb-ng / OpenHD: MCS 8 and up, STBC, LDPC, 40 MHz and greenfield format are counted as "not decodable here". OpenHD frames are recognised by the header pattern only; no check of the OpenHD payload.
- NPR-70: the whitening of the Si4463 packet handler (PN generator bit arrangement, PKT_WHT_BIT_NUM 0x20) is only partly in the published API documentation, and no recording exists to confirm it; decoding the length byte and frame contents on an untested guess would be invention, so frames are detected, not read.
- WINMOR: the specification leaves open the 4FSK symbol-to-bits mapping, the Reed-Solomon generator, and the callsign packing (FormatCallsToByte). Without them frames cannot be read; the VB.NET TNC source is not public. Winlink retired WINMOR in 2020.

## mioty and DECT NR+: limits and choices

- mioty: uplink UL-ULP, standard TSMA mode (2 380.371 Hz carriers) only. Not done: downlink, the optional sync burst, UL-ER mode, narrow and wide TSMA modes, coherent combining of repetitions. Points the specification leaves open, chosen and marked in core/src/xlink/mioty.cpp: the MAC header's bit numbering (bit 0 taken as least significant), the shift direction of the extension frame's Galois LFSR (right shift), the tap order of the convolutional code (as in the 802.11 code). The TSMA tables were read from the specification's PDF by position and checked: every pattern uses each of the 24 carriers once.
- DECT NR+: only mu 1 / beta 1 and one transmit stream; the data channel (MAC PDU) is not decoded. The control field bits are read most significant first in the order of TS 103 636-4 Table 6.2.1-1 / -2.

## Not built

| Item | Reason |
|---|---|
| ExpressLRS 2.4 GHz FLRC | The ExpressLRS source only configures the SX1280 (FLRC, 650 kbit/s, CR 1/2, BT 1). The convolutional code and interleaver live inside the chip; the datasheet calls the packet proprietary and gives no polynomials. It cannot be documented from public source code, so no decoder. |
| IP400 (70 cm) | The radio side uses ST's MR-SUBG 802.15.4g packet handler: 4FSK 100 kbit/s (25 kHz deviation), FEC_15_4_G_RSC, 32-bit FCS. The FEC and PHR details are in IEEE 802.15.4g and ST's HAL behaviour, not in the IP400 sources; the IP400 frame header above that layer is public (adrcs/ip400) but not useful without the layer below. |

## Not checked

Nothing was built with CMake and no full test run was made locally. xlink sources and tests compiled and ran standalone against the prebuilt libraries in /private/tmp/b37t; engine and app files were checked with clang++ -fsyntax-only only. The app build and full suite run in CI (dispatched once for this branch).

Other workers' mode numbers: family 600, standards 602 / 603, telemetry 601 / 602 and poll slots 600 / 601 are this branch's block. kNumFamilies 601, kNumSubModes 40 and kPollSeqSlots 602 were raised here; the integrator resolves merges.
