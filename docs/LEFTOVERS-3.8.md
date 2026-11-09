# Leftover modes, 3.8 (branch feat38-leftovers)

Family 600, engine standards 602 (20 MHz) and 603 (narrow), telemetry 601 and 602, poll slots 600 and 601. One mode-list entry per protocol. Code: core/src/xlink, app/xlink, tests/xlink. Receive only.

## Added

| Protocol | What it decodes | Validated on |
|---|---|---|
| wfb-ng / OpenHD | New 802.11n HT receiver (core/src/xlink/ht_phy.cpp): HT-mixed format, 20 MHz, one stream, MCS 0-7, long and short GI, BCC, HT-SIG CRC checked. Frames recognised by the wfb-ng and OpenHD 802.11 layouts: link id and radio port, FEC block and fragment numbers, frames lost, session frames counted. Payload stays encrypted, structure only. | simulated only |
| New Packet Radio (NPR-70) | Si4463 4GFSK at 50, 100, 180, 300, 500 ksym/s: sync word (with the network ID), symbol rate, tuning offset. Frame contents not read. | simulated only |
| WINMOR | 1500 Hz alternating-phase pilot, tuning error, 4 raw frame-type tones. Frame contents not read. | simulated only |

Settings come from public sources: wfb-ng src/wifibroadcast.hpp (GPL-3), OpenHD/wifibroadcast Ieee80211Header.hpp (GPL-3), NPR-70 firmware Si4463 configuration arrays (GPL-3) and the EZRadioPRO API documentation, WINMOR specification Rev 1.0.6 (public domain). THIRD_PARTY.md and the Credits window are updated.

## Test results (tests/xlink, all simulated, no real recording exists for any of these)

- HT PHY: MCS 0-7 x long / short GI x offsets 0, +90 kHz, -210 kHz all decode; 15 ppm clock offset, 150 ns echo inside the guard interval, two frames with an interfered one between them (one damaged, nothing wrong reported), noise only gives nothing. MCS 1 packets of 300 bytes: 15/20 decoded at 8 dB SNR, 19/20 at 10 dB, 0/20 below 6 dB.
- wfb-ng / OpenHD through XlinkReceiver at 20 Msps with +-3.1 MHz channel offset and at 40 Msps: all 39 frames, 4 links, lost-frame count right (1 fragment + 1 whole block of 12 in the test).
- NPR: all 5 rates x networks 0, 5, 15 x 3 offsets; noise sweep (sigma 0.2: 10/10, 0.3: 5/10); 4 Msps noise only gives nothing.
- WINMOR: offsets 0, 42, -83, 150 Hz at two noise levels, tone readings right; from a 96 kHz capture; noise only and a plain 1500 Hz carrier give nothing.
- The Si4463 4FSK mapping and the WINMOR pilot format follow their documents. Real-signal behaviour of all three is untested.

## Limits of what is decoded (and why)

- wfb-ng / OpenHD: MCS 8 and up, STBC, LDPC, 40 MHz and greenfield format are counted as "not decodable here". OpenHD frames are recognised by the header pattern only; no check of the OpenHD payload.
- NPR-70: the whitening of the Si4463 packet handler (PN generator bit arrangement, PKT_WHT_BIT_NUM 0x20) is only partly in the published API documentation, and no recording exists to confirm it; decoding the length byte and frame contents on an untested guess would be invention, so frames are detected, not read.
- WINMOR: the specification leaves open the 4FSK symbol-to-bits mapping, the Reed-Solomon generator, and the callsign packing (FormatCallsToByte). Without them frames cannot be read; the VB.NET TNC source is not public. Winlink retired WINMOR in 2020.

## Not built

| Item | Reason |
|---|---|
| ExpressLRS 2.4 GHz FLRC | The ExpressLRS source only configures the SX1280 (FLRC, 650 kbit/s, CR 1/2, BT 1). The convolutional code and interleaver live inside the chip; the datasheet calls the packet proprietary and gives no polynomials. It cannot be documented from public source code, so no decoder. |
| IP400 (70 cm) | The radio side uses ST's MR-SUBG 802.15.4g packet handler: 4FSK 100 kbit/s (25 kHz deviation), FEC_15_4_G_RSC, 32-bit FCS. The FEC and PHR details are in IEEE 802.15.4g and ST's HAL behaviour, not in the IP400 sources; the IP400 frame header above that layer is public (adrcs/ip400) but not useful without the layer below. |
| Mioty (ETSI TS 103 357) | Specification is public, but a telegram is 24 hopped sub-packets of about 2.4 kHz with convolutional FEC and interleaving (TSMA patterns, Section 6.4.7); it needs a coherent multi-burst receiver and payloads are AES encrypted. Feasible but a large receiver; not started in this branch, no test signal. |
| DECT NR+ (ETSI TS 103 636) | Specification is public (OFDM, polar-coded control channel, HARQ data). Not started; no recording or transmitter to test against. |

## Not checked

Nothing was built with CMake and no full test run was made locally. xlink sources and tests compiled and ran standalone against the prebuilt libraries in /private/tmp/b37t; engine and app files were checked with clang++ -fsyntax-only only. The app build and full suite run in CI (dispatched once for this branch).

Other workers' mode numbers: family 600, standards 602 / 603, telemetry 601 / 602 and poll slots 600 / 601 are this branch's block. kNumFamilies 601, kNumSubModes 40 and kPollSeqSlots 602 were raised here; the integrator resolves merges.
