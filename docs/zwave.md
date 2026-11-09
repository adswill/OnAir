# Z-Wave (ITU-T G.9959)

A receive-only monitor for Z-Wave home automation networks on the sub-GHz bands. Nothing is sent, included or answered.

## What it decodes

- The FSK PHYs: R1 9.6 kbit/s (Manchester, 19.2 kchip/s), R2 40 kbit/s (NRZ), R3 100 kbit/s (GFSK). Z-Wave Long Range (O-QPSK) is not decoded.
- Frames: preamble (0x55), start-of-frame delimiter (0xF0 for R1 / R2, 0x5B for R3), then the MPDU: home ID, source node, frame control (routed, ack request, low power, speed
  modified, header type, beaming, sequence number), length, destination (a node, 255 for all, or a multicast node mask), payload and the check: an XOR checksum (R1, R2) or CRC-16
  (R3). Only frames whose check passes are shown; frames whose delimiter and length were found but whose check failed are counted.
- Payload: command class and command names (the public list of command classes and the common commands of the basic ones: BASIC, SWITCH_BINARY, SWITCH_MULTILEVEL, SENSOR,
  BATTERY, WAKE_UP, ASSOCIATION, VERSION, CONFIGURATION, NOTIFICATION, DOOR_LOCK, the protocol commands), a few values (switch state, battery, temperature).
- S0 (SECURITY) and S2 (SECURITY_2) encapsulated payloads are shown as encrypted with their length. The network key is not obtainable and nothing is decrypted.
  S0 / S2 handshake frames (nonce get / report, key exchange) are named only, no key material is shown.
- Routed frames: the route header (type, repeater list, hop index) is decoded and shown ("routed 1 > 2 > 3 > 4 (hop 1 of 3)") and drawn in the Topology view. The field layout follows the public descriptions and is checked by loopback only, not against a real capture.

## Regions

EU 868.42 (R1) / 868.40 (R2) / 869.85 (R3), US 908.42 / 908.40 / 916.00, ANZ 921.42 / 921.40 / 919.80, Hong Kong 919.80, India 865.20, Russia 869.00, China 868.40,
Israel 916.00, Japan 922.50 / 923.90 / 926.30 and Korea 920.90 / 921.70 / 923.10 (R2 only for the last two). The table is typed from the public regional frequency list and
has not been checked against a radio. Automatic picks the region with most channels inside the band taken in; "Centre on region" tunes to the region's centre.
A 2 Msps capture centred on 869.13 MHz holds all three European channels; the US R3 channel (916 MHz) is 7.6 MHz from R1 / R2 and needs a 10 Msps capture.

## How it works

Every channel of the region inside the band gets a narrowband front end (mixer, low-pass at 110 kHz, decimation to about 400 ksps). An energy detector with a tracked noise
floor marks bursts; each is FM demodulated, the carrier offset is taken from the preamble, bit timing from the preamble's alternation (best of the sample phases), and the
delimiter is searched at both polarities (and both Manchester alignments for R1). The generator (`zwave_gen.h`) makes a small test network (home ID C0FFEE01) at the three
rates, with an S0 and an S2 frame (random octets stand in for the ciphertext).

## Tests

`tests/zwave`: the frame layer (CRC-16 check value, round trips for all rates, multicast, acks, encrypted payload names), the receiver on the generator (clean, +-6 kHz offset,
+-80 ppm bit clock, 18 dB SNR, off-centre capture, the US region at 2 Msps, noise alone), and the engine with the synthetic source. Written clean-room from the public
specification; no third-party code is used, so there is no credits row.
