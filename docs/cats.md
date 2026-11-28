# CATS (balloon packet radio)

CATS (Communication And Telemetry System, cats.radio) is a packet radio format that high-altitude balloons and small trackers use in place of APRS.
OnAir receives it; it does not transmit it. No calling frequency is fixed: set the frequency in the tuning bar (70 cm is typical).

## What OnAir decodes

* the physical layer: 2-FSK at 9600 baud with 4.8 kHz deviation,
* the identification: callsign, SSID and icon,
* the timestamp of the packet,
* the GPS position, altitude, speed and heading,
* the comment text.

Each decoded packet is a row in the packet list (newest first), with an event log and a level history beside it.

## Error correction

The packets are protected by a rate 1/2 LDPC code. LDPC correction is not done yet, so only packets that arrive clean (the CRC is good without any
correction) are decoded. A packet with bit errors is counted as bad and not shown.

## Test signal

The synthetic source plays CATS packets with noise, so the mode can be tried without a radio. Simulated signals only: no real CATS signal has been
tested.

## Tests

`cats_loop` (generator and receiver), `cats_mode` (wiring), `cats_engine` (engine standard 902, active standard 901, family 900).
