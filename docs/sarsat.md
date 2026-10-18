# COSPAS-SARSAT 406 MHz distress beacons

Receive only. Mode "COSPAS-SARSAT 406 MHz", 406.028 MHz (the channels run 406.025 - 406.076 MHz). Code: `core/src/sarsat/`, panel: `app/sarsat/`, tests: `tests/sarsat/`.

A 406 MHz beacon (EPIRB on a boat, ELT in an aircraft, PLB carried by a person) sends a burst of about half a second roughly every 50 seconds. OnAir decodes the first-generation message format of the public specification C/S T.001. A real beacon means somebody may be in danger: report it to the rescue coordination centre of your country. The program shows test beacons and self-test bursts as such, and is not an alerting system.

## What is decoded

| Item | How |
|---|---|
| Burst | 160 ms unmodulated carrier, then 24 sync bits (15 ones, 9 frame-sync bits `000101111`, or `011010000` for a self-test) and a 112 bit (short) or 144 bit (long) message, 400 bit/s, Manchester coded, phase modulated +-1.1 rad |
| 15 hex ID | bits 26 - 85, with the position bits of a standard location protocol at their default value, so the ID stays the same when the beacon moves |
| Country | bits 27 - 36, the maritime identification digits (MID); about 270 are named |
| Protocol | bit 26 (user / location) and bits 37 - 40: EPIRB, ELT, PLB, ship security, test, national |
| Identification | MMSI, serial number and certificate, 24-bit aircraft address, aircraft operator, aircraft marking and call sign (Baudot), as far as the protocol has them |
| Position | location protocols: 15 minute cells in bits 65 - 85, refined to 4 seconds by the long message's bits 113 - 132 |
| Homing flag | bit 111 (121.5 MHz) |
| Checks | BCH(82,61) over bits 25 - 106 (up to 3 bits repaired), BCH(38,26) over bits 107 - 144 (up to 2 repaired) |

Screens: the decoded beacons with a detail panel, a map of the ones with a position, and the raw bits.

## How the receiver works

The input is mixed to the dial offset and averaged to 32 kHz. Every 32 ms a windowed FFT looks for one narrow line standing 15 dB over the median bin that was not there a moment ago (radio DC spikes do not count); that is the preamble. The burst is cut from a ring buffer, its carrier frequency is the median of the first lines (any offset within +-14 kHz), and each bit is the angle between the vector sums of its two halves, which does not depend on the carrier phase. Bit timing, bit rate (+-1 %) and polarity are found by sliding the 24 sync bits over the burst; the best candidates are decoded and the one the BCH check repairs with the fewest changes wins.

## Test signal

`makeSarsatSynth`: five made-up beacons (EPIRB, PLB, ELT, a maritime user beacon, an EPIRB in the southern hemisphere), one burst every 3 seconds. Options: `modeOpt[0]` one beacon only, `modeOpt[1]` reversed polarity, `modeVal[0]` bit rate error in percent, `modeVal[1]` seconds between bursts. Nothing is transmitted and there is no transmit entry for this mode: sending on 406 MHz is forbidden.

## Not verified against real beacons

No real recording was available. The message layout, the protocol code table, the position and offset fields, the Baudot table and the homing bit were written from public descriptions of T.001 and need checking against a real beacon or the current T.001 before the details are relied on. The protocol code tables (A2-A, A2-B) and the serial user beacon types were checked against T.001 (rev. March 2021). The two BCH generator polynomials (`0x26D9E3`, `0x1539`) were checked to divide x^127+1 and x^63+1, as a BCH generator must. User protocols other than the aviation, maritime and serial ones show the raw identification field.
