# RC and mesh link modes (3.8)

Family 360, engine standard 362, telemetry 361, poll slot 360 (block 360-379). One mode-list entry per protocol (sub-mode mask): Reticulum (RNode), ExpressLRS, MAVLink (SiK radio), FrSky RC (D8 / D16), FlySky RC (AFHDS / 2A). Code: core/src/rclink, app/rclink, tests/rclink.

Receive only. Nothing in the app can transmit these protocols. Encrypted or bound payload stays structure only.

## Done

| Mode | What is decoded | Validated |
|---|---|---|
| Reticulum / RNode | LoRa explicit header frames (BW 62.5 / 125 / 250 kHz, SF7-11, or a fixed plan under Advanced), RNode split header, packet header (type, destination, hops, transport), announce with Ed25519 signature and destination hash checked. Data stays encrypted. | simulated only |
| ExpressLRS 900 MHz LoRa | Implicit header LoRa, BW 500, SF6-9 and the 8 channel rate, both IQ polarities, every hop channel of the domain (9 domains, 16 channels at most) inside the capture. SYNC frames: link id (UID4/UID5), hop index, nonce, rate. RC channels only with a binding phrase the user types (Advanced; kept in memory only, never saved). | simulated only |
| MAVLink on SiK | GFSK bursts on any hop channel in the capture, every air speed, SiK packets with and without Golay ECC, net id, trailer, MAVLink v1 / v2 frames (CRC_EXTRA table of 308 messages), position, battery, attitude, position on the Map view. | simulated only |
| FrSky D8 / D16 | CC2500 GFSK frames, D8 (31.0 kbaud) and D16 (70 / 100 kbaud), transmitter id, D8 channels. | simulated only |
| FlySky AFHDS / AFHDS 2A | A7105 frames, transmitter id, channels. There is no CRC on the air: acceptance is the ID code plus a strict format and value check. | simulated only |

Test results (tests/rclink, simulated signals only, no real recording exists for any of these): every air speed from 16 kbit/s up with a second link overlapping in time, carrier offsets up to 30 kHz, hop channels across the capture, 2.4 and 8 Msps, noise sweep (FrSky D8 decodes to sigma 0.2, none at 0.3, no false frames), noise only gives no frame, tampered announces and damaged CRCs are rejected. Reticulum: 3 settings x 2 offsets. ExpressLRS: SF6-8, IQ normal and inverted, with and without a phrase.

Layouts checked against upstream sources: Reticulum Packet.py and Identity.py, RNode firmware, ExpressLRS OTA.h / OTA.cpp / FHSS.cpp / build_flags.py, SiK radio_443x.c / golay.c / crc.c / tdm.c, MAVLink message XML, DIY-Multiprotocol-TX-Module. A real capture of each link is still needed. Assumed and not confirmed on real signals: A7105 bit order (both are tried), CRC coverage of the SiK hardware CRC (over header + length + data), ExpressLRS SF6 on SX127x (the LoRa demodulator's SF5/6 timing was written for SX126x).

## Skipped, no public protocol

- TBS Crossfire: only the CRSF serial link to the flight controller is published. The air protocol is closed.
- Spektrum DSMX: the frame layout is public through reverse engineering, but the chip-level mapping of the CYRF6936 8DR mode (how data bytes become PN code symbols) is not documented, so a demodulator cannot be written or checked.
- Autel and other non-DJI drones: no published air protocol. Drone Remote ID (family 56) already covers the protocol those aircraft publish.

## Partly covered

- ExpressLRS 2.4 GHz: FLRC and the SX128x long-interleaver LoRa rates are not documented, so those rates are not decoded (no frames shown).

## Public but not built in this round

- OpenHD / wfb-ng: the frame format is public (802.11 frames with the "WB" MAC prefix, ChaCha20-Poly1305 encrypted payload), but the receiver needs an 802.11n HT PHY; the existing Wi-Fi receiver decodes non-HT OFDM only.
- New Packet Radio (NPR-70): firmware is public (F4HDK); the radio parameters are only in raw SI4463 property dumps and the framing would need a real NPR signal to verify.
- IP400 (adrcs/ip400): frame header is public, the radio settings of the STM32WL33 / OFDM variants were not extracted.
