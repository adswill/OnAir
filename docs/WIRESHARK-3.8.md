# Wireshark output (OnAir 3.8)

View > Wireshark has one switch per mode. All switches are off by default. With a switch on, the mode's decoded packets go to:

- a pcapng file in `<data folder>/wireshark/` (`<mode>-<date>-<time>.pcapng`, one file per switch-on, 1 GiB limit per file);
- a live capture in Wireshark, through the extcap program `onair-extcap` ("OnAir: <mode>" in the interface list);
- for GSM-R, TETRA and LTE, GSMTAP over UDP to 127.0.0.1:4729.

"Add OnAir to Wireshark" copies `onair-extcap` to the personal extcap folder and `onair.lua` to the personal plugins folder (`~/.local/lib/wireshark/` on macOS and Linux, `%APPDATA%\Wireshark\` on Windows). Run it once, then restart Wireshark.

Advanced: turn the file, live or GSMTAP output off individually, set the folder, the local port, and the GSMTAP host and port. Timestamps are the time of decoding, so a replayed IQ file carries the replay time.

## Local only

The live capture socket is bound to 127.0.0.1 and exists only while a switch is on. It has no option for another address. GSMTAP goes to 127.0.0.1 unless a different host is typed under Advanced; the window then shows a warning.

## Encrypted packets

Packets are taken at the point where the decoder has the frame as received, before any decryption. Keys imported for decoding in OnAir are not applied to this output. Wi-Fi protected frames, Zigbee and Thread network security, Z-Wave S0 and S2, LoRaWAN and TETRA air interface encryption reach Wireshark as they were on the air. TETRA traffic channels (voice) are not sent; only signalling blocks are.

## Link types

| Mode | Link type | Read in Wireshark by |
| --- | --- | --- |
| Wi-Fi, Wi-Fi HaLow | 127 IEEE802_11_RADIO (radiotap; no FCS) | radiotap, wlan |
| Zigbee, Thread | 195 IEEE802_15_4_WITHFCS | wpan, zbee, 6lowpan |
| Wi-SUN | 230 IEEE802_15_4_NOFCS (FCS removed: it is 2 or 4 bytes) | wpan, wisun |
| Bluetooth LE | 251 BLUETOOTH_LE_LL | btle |
| LoRaWAN | 270 LORATAP, version 0, sync word 0x34 | loratap, lorawan |
| DVB-S/S2 IP (GSE, MPE, ULE) | 101 RAW | ip, ipv6 |
| APRS / AX.25 | 3 AX25 (no FCS) | ax25, aprs |
| ASTERIX | 101 RAW, IPv4 + UDP to port 8600 | asterix |
| AIS | 101 RAW, IPv4 + UDP to port 10110, NMEA sentences | nmea0183 |
| GSM-R, TETRA, LTE | 101 RAW, IPv4 + UDP to port 4729, GSMTAP | gsmtap, then gsm_a, tetra, lte-rrc |
| ADS-B / Mode S | 147 USER0 | onair.lua |
| Z-Wave | 148 USER1 | onair.lua |

The IPv4 and UDP headers around AIS, ASTERIX and GSMTAP in files and live captures are placeholders (127.0.0.1 to 127.0.0.1); those packets were not sent over a network.

### Private link types

DLT_USER is used for two modes only, because nothing else works in Wireshark today:

- **ADS-B / Mode S (USER0):** the 7 or 14 bytes of the frame, parity included. Wireshark has no Mode S dissector or link type.
- **Z-Wave (USER1):** one byte for the rate (0: 9.6, 1: 40, 2: 100 kbit/s), then the MPDU with its checksum or CRC. The registered link types 261 and 262 are not known to Wireshark's file reader, so such a file does not open; 297 (Z-Wave Tap) is known to the reader but has no dissector.

`onair.lua` reads both, and adds port 10110 for the NMEA 0183 dissector, whose UDP heuristic is off by default. Without `onair.lua`, these two link types appear as raw data, and AIS needs Decode As.

### Limits

- AIS sentences are shown as NMEA text; Wireshark does not decode the AIVDM payload.
- LTE: only the system information (BCCH-DL-SCH). The receiver keeps the MIB's fields but not its bits, so the MIB is not sent.
- TETRA: BSCH, BNCH, SCH/HD and SCH/F. AACH is not sent.
- GSM-R: BCCH blocks that passed the block code; the ARFCN field is 0.
- DVB: the recovered IP packets. Raw GSE and BBFRAMEs are not written.
- Modes with no dissector in Wireshark, such as ACARS, VDL2, HFDL, DMR and P25, have no switch. ADS-B and Z-Wave are the exceptions, because they were asked for.
- The older "Write a PCAP file" checkbox in the DVB-S window still works and now uses the shared writer. ASTERIX could only read pcap before this; its new switch is the first way to write it.

## extcap protocol

`onair-extcap` connects to 127.0.0.1 (default port 47290) and sends `ONAIR-WIRE 1 <mode id>\n`. OnAir answers `OK <link type>\n` and then pcap records, or `OFF\n` and closes. The program writes the pcap file header to Wireshark's pipe itself, so it can reconnect after OnAir restarts without breaking the stream. If OnAir cannot be reached within 20 seconds of the start (`--connect-wait`, 0 means never give up) it exits with a message that Wireshark shows.

## Verification

`tests/wirecap/test_wirecap.cpp` writes every channel through the hub and parses the files back with a strict pcapng reader (block lengths, padding, options, one section and interface, link type, timestamps), then checks each format's own fields: IPv4 header checksum and UDP lengths, GSMTAP offsets, radiotap alignment, LoRaTap offsets, and unchanged bytes for encrypted frames. It also covers the extcap answers, a live capture through a real FIFO, the refusal of a mode whose switch is off, the port closing when all switches are off, and the install into an empty home folder.

Zigbee, Bluetooth LE and Z-Wave are also tested from their generators through the real receivers into the file. The BLE CRC is checked the way Wireshark's dissector checks it. These are simulated signals only.

tshark was not installed where this was written, so no file has been opened by Wireshark or tshark yet. The test uses tshark when it is on the PATH. `onair.lua` was run only against stubs of the Wireshark Lua API. The hooks in the other receivers are compiled but not exercised by a decoder test. The Wireshark window was compiled but not opened.
