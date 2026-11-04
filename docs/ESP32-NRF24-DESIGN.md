# ESP32 and nRF24L01 modules as an OnAir radio (design, 3.7)

Status: written without hardware. The firmware and the OnAir driver are tested only against a simulated serial device (see Testing). **Untested on a real ESP32 or nRF24 module.**

## What the hardware can do

| Radio | Where | Receive | Transmit | Notes |
|---|---|---|---|---|
| nRF24L01+ module on the ESP32 SPI bus | any ESP32 | Enhanced ShockBurst on channel 0..125 (2400 + N MHz), 250 kbit/s / 1 / 2 Mbit/s, address 3..5 bytes, CRC off / 8 / 16, auto-ack, static (1..32) or dynamic payload; a promiscuous mode | the same, with the user's address, optional no-ack | The chip cannot sniff unknown addresses. The public trick is used: 2-byte "address" 0xAA55, CRC off, 32 raw bytes reported; OnAir finds frames in them with `nrf24::sniff` (same CRC-based search as the IQ mode). Much weaker than the IQ mode: only frames the chip's matcher lets through are seen (those whose first address byte fits the pattern); the normal mode with a known address is reliable. |
| BLE (ESP32, S3, C3, C6, H2) | on chip | advertising on 37/38/39: address, PDU type, data, RSSI | legacy advertising PDUs (ADV_IND, SCAN_IND, NONCONN_IND) | no connection sniffing |
| IEEE 802.15.4 / Zigbee / Thread | C6 and H2 only | frames on channels 11..26, RSSI (protocol and OnAir side done, **firmware hardware layer not written yet**) | raw frame (same) | ESP32, S2, S3, C3 have no 802.15.4 radio |
| WiFi 2.4 GHz (all but H2) | on chip | scan and passive listen: SSID, BSSID, channel, RSSI, security (open / WEP / WPA / WPA2 / WPA3 / enterprise), beacons, probe requests (clients), data frames (client to access point), deauthentication counted | **none, by design** | observation only: no deauthentication, no cracking, no password or handshake capture |

Not possible: 5 GHz WiFi (S3 and C3 are 2.4 GHz only; C5/C6 variants are not covered), BLE connection following, FM/ISM/sub-GHz.

## Link and protocol

Transport: USB serial (CP210x / CH340 / FTDI bridge, or the native USB of S3 / C3 / C6), 115200 baud 8N1 (ignored on native USB). WiFi transport (the device as a TCP server carrying the same frames) is planned, not built.

Frames, protocol version 1 (`firmware/esp32/onair_esp32/onair_wire.h` is the single definition; OnAir includes the same file):

`0xA5 | len u16 | type u8 | seq u8 | payload | CRC-16/CCITT-FALSE u16` (CRC over len..payload). A bad CRC drops one byte and resynchronises on the next 0xA5.

Host to device: HELLO (protocol version), CONFIG (radio + its parameters), START, STOP, TX, PING, TX_ENABLE. Device to host: HELLO_ACK (protocol version, firmware version, chip, radios present, name), ACK (request type + status), PONG, RX_NRF24 / RX_BLE / RX_IEEE802154 / RX_WIFI, TX_DONE, LOG, STATS. Field layouts are in the header. Versioning: HELLO carries the host's version; the device answers with its own and rejects (status BAD_VERSION) a major it does not know. New fields are only appended; new message types are ignored when unknown, so an old host can talk to a newer firmware.

The device boots with transmit **off**: it ignores TX until the host sends TX_ENABLE 1, which OnAir sends only when the user presses Arm. That is the only gate; there are no band, licence or frequency refusals in OnAir or the firmware, only a one-line warning in the panel (project policy). WiFi transmit does not exist at all.

## Firmware (firmware/esp32/)

`onair_esp32.ino` (Arduino-ESP32 core, also builds with PlatformIO, `platformio.ini` included) plus `onair_core.h`: the protocol dispatcher written against a small hardware abstraction (nRF24 register access, BLE, WiFi, 802.15.4, clock, serial). The same dispatcher is compiled into the OnAir test with a fake hardware layer. The nRF24 driver is written from the public Nordic nRF24L01+ datasheet (register map, SPI commands); no third-party library code is copied. Licence GPL-3.0-or-later. The Arduino-ESP32 core and ESP-IDF (Apache-2.0, GPL-3 compatible) are used as build dependencies, not copied; credited in THIRD_PARTY.md.

Wiring (default, editable at the top of the sketch): nRF24 SCK 18, MISO 19, MOSI 23, CSN 5, CE 4. Use a 10 uF capacitor across the module's supply (the usual nRF24 power problem).

## OnAir side (core/src/esp32, app/esp32)

* `esp32_proto`: frame builders and decoders over the shared header.
* `esp32_port`: serial port interface, a POSIX implementation (macOS, Linux; Windows port is not written), a list of candidate ports (`/dev/cu.usbserial*`, `cu.usbmodem*`, `ttyUSB*`, `ttyACM*`) and auto-detection by sending HELLO and waiting for HELLO_ACK.
* `Esp32Radio`: the device driver: connect, configure, start / stop, send, a reader thread, thread-safe telemetry. Packets become `Nrf24Frame` / `Nrf24Device` (nrf24_tel.h) so the nRF24 lists are reused; BLE, 802.15.4 and WiFi have their own lists. WiFi also keeps an access point table (BSSID, SSID, channel, RSSI history, security, vendor from a small built-in OUI list, last seen), a client list and a per-channel congestion count.
* Panel: Tools > "nRF24 / ESP32 radio...", also a button in the nRF24 mode. Up front, receive first: device and Connect, radio (nRF24 / BLE / WiFi / 802.15.4), channel, rate, Listen / Stop, the packet or network lists (WiFi: networks, clients, signal graph over time, channel congestion bars). Under Advanced: every parameter (address, address width, CRC, auto-ack, retries, payload length / dynamic, power, promiscuous, BLE channel mask, WiFi hop time, baud), and Transmit: hex or text payload, no-ack, an Arm checkbox and a Send button, with the one-line warning.

## Testing

`tests/esp32/test_esp32_loopback`: OnAir's driver talks over an in-memory serial pair to the firmware dispatcher with a fake nRF24 / BLE / WiFi layer: handshake and version check, every nRF24 parameter reaching the fake chip registers, RX packets and raw promiscuous bytes decoded by the host, TX refused until armed then delivered with the right bytes, damaged and split frames, garbage before a frame, WiFi table, deauth counting and the no-WiFi-TX rule. This proves the protocol and the logic, not the radio.

## Open items

Real-hardware test on ESP32 + nRF24L01+ (owner or community); WiFi transport; Windows COM port support; 5 GHz WiFi on ESP32-C5.
