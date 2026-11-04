# nRF24L01 (Enhanced ShockBurst), receive only

OnAir listens to one 2.4 GHz channel and decodes the Enhanced ShockBurst (ESB) frames of nRF24L01 and nRF24L01+ chips: wireless mice and keyboards,
sensors, remote controls, toys, RC links and many hobby projects. Receive only; nothing is transmitted.

## Radios that work

Tuning the radio to 2.4 GHz is the whole requirement. These work: HackRF One and HackRF Pro (use 8 or 10 Msps), PlutoSDR with the extended
frequency range, bladeRF, LimeSDR, USRP B200 / B210 / B205mini. These do not reach 2.4 GHz: RTL-SDR (up to about 1.7 GHz), Airspy R2 and Mini,
SDRplay RSP1A / RSPduo / RSPdx, sound cards and the Flipper Zero.

## Channel

Channel N (0 to 125) is 2400 + N MHz. Enter the frequency in MHz (for example 2402 for channel 2, 2440 for channel 40). The radio is tuned 1.5 MHz
above it so the signal stays clear of the DC spike; the receiver mixes that out. The channels are 1 MHz apart, so at 2 Mbit/s the neighbouring channel
overlaps a little: tune to the channel of the transmitter.

## What is decoded

* GFSK at 250 kbit/s, 1 Mbit/s and 2 Mbit/s. The three rates are demodulated at the same time; the frame list says which one carried each frame.
* Frame: preamble (0xAA or 0x55), address of 3 to 5 bytes, 9-bit packet control field (payload length 6 bits, PID 2 bits, NO_ACK 1 bit), payload of up to
  32 bytes, CRC of 1 byte (x^8 + x^2 + x + 1, start 0xFF) or 2 bytes (CCITT x^16 + x^12 + x^5 + 1, start 0xFFFF) over address, control field and payload.
* The address is not sent as an address: it is the bytes after the preamble. The sniffer tries every position that follows an alternating preamble,
  every address length from 3 to 5 bytes and both CRC sizes on the demodulated bits, and accepts a frame whose CRC is valid (the public technique
  of the nRF24 sniffers). A CRC-16 match is accepted at once. A CRC-8 match is right by chance 1 time in 256 per candidate, so it is shown only when its
  address is already known or the same frame is heard a third time (ESB retransmits until it is acknowledged, so a real CRC-8 frame comes again). Even so a CRC-8 address is less certain than a CRC-16 one: an address known to use CRC-16 never takes a CRC-8 frame.
* The frame list shows the time, the data rate, the address, PID (NA = NO_ACK), CRC size, payload length, copies in a row, payload as hex and text. The address list
  keeps one row per address. Other bursts shows what was heard on the channel that was no ESB frame (Bluetooth LE, Wi-Fi, Zigbee, other chips).

## Limits

* One channel at a time (about 2.5 MHz wide). A frame sent on a channel you are not tuned to is not seen.
* The nRF24 sends the address on the air without any marker. If the transmitter uses an address of 2 bytes (not possible on the chip) or the
  preamble is not alternating, nothing is found. Frames with a dynamic payload length longer than 32 bytes are refused (not in the chip).
* The old nRF2401 and the Chinese clones with a different frame layout (for example Si24R1 in non-ESB mode, XN297) are not decoded.
* Tested only with a synthetic generator (all rates, CRC-8 and CRC-16, carrier offsets of +-100 kHz, noise); no real recording was available. Needs a user test.

## Parameter coverage (checked 2026-11-04, synthetic signal)

`test_nrf24_matrix` runs every combination the chip allows: 250 k / 1 M / 2 M x address 3 / 4 / 5 bytes x CRC-8 / CRC-16 x payload 0 / 1 / 32 bytes x carrier offset +-100 kHz, with PID and NO_ACK varied: 108 of 108 decoded at 22 dB Eb/N0. Gaps that remain (not fixed, not cheap):
* frames without the 9-bit control field (nRF2401, nRF24L01 non-plus in static mode, XN297 and other clones) are not decoded;
* a 2 Mbit/s signal on the neighbouring channel overlaps (channels are 1 MHz apart, a 2 Mbit/s signal is 2 MHz wide): not tested with two transmitters;
* ESB acknowledge packets with a payload (ACK payload) are decoded like any frame but not linked to the data frame they answer;
* no real recording yet.

An ESP32 board with an nRF24 module can be used instead of an SDR: Tools > nRF24 / ESP32 radio (docs/ESP32-NRF24-DESIGN.md).

## Test signal

The signal simulator makes ESB frames of four devices with addresses of 3, 4 and 5 bytes, in noise, at 250 kbit/s, 1 Mbit/s, 2 Mbit/s or all three in turn.
Tests: `test_nrf24_proto`, `test_nrf24_rx`, `test_nrf24_engine`, `test_nrf24_mode`.
