# SSR interrogations on 1030 MHz (4.0, family 781)

Reads what the secondary surveillance radars around you ask the aircraft (ICAO Annex 10 Vol. IV): pulses of 0.8 us in groups. P1 - P3 spacing 3, 5, 8, 17, 21 and 25 us is Mode 1, 2, A, B, C and D; P2 2 us after P1 is the side-lobe suppression pulse; P4 2 us after P3 (0.8 or 1.6 us) is the intermode all-call; Mode S is P1, P2 and the long P6 (16.25 or 30.25 us, 4.75 us after P1).
Shares the x40 receiver (`core/src/x40/x40_ssrint.cpp`, test `tests/x40/test_x40_ssrint.cpp`, generator `tests/x40/x40_gen4.h`). Variant 44, family 781, active standard 782, engine standard 783. Needs 2 Msps or more (4 Msps or more for Mode 1).

How it works: the envelope is searched for pulses (4 times the adaptive floor, edges at half of the pulse's own peak, interpolated; width 0.45 - 2.2 us, or 12 - 33 us for P6). Pulses within 37 us of a first pulse form a group, read as an interrogation when the spacings fit a pattern within 0.35 us (0.6 us for P6) and P1 and P3 are within 6 dB.
P2 not 9 dB under P1 marks a side-lobe interrogation (the radar will not be answered). Every 2 s a row gives the counts per mode, the measured spacing of each and the rate. It cannot say which radar sent them.

Tested (simulated signals, envelope with 0.1 us edges): each of the six modes at 4, 10 and 2 Msps (300 per second, 5 s, carrier offsets of up to 3 MHz): counts within 10 % of the sent interrogations (the last second is not reported yet), spacing within 0.12 us, no other mode; Mode 1 at 2 Msps: 99 of 1200 (3 us is 6 samples: not reliable, use 4 Msps).
P2 classes, intermode P4 short and long, Mode S 56 and 112 bit all counted right. Two radars (Mode A and C interlaced at 250 per second and a second Mode C radar 14 dB weaker with 0.1 us jitter): A 1479 of 1500, C 2531 of 2580, 2 interrogations of other modes where pulses of the two radars overlapped by chance.
Pulse amplitude over the noise rms, Mode A 1200 sent: 40 to 16 dB 1199; 12 dB 1086; 9 dB 199; 6 dB 2; never a wrong mode. 20 s of noise, pairs 12 / 10 / 36 us apart (DME-like) and a plain carrier: nothing.
**No recording of 1030 MHz was available.** Real P6 pulses dip at the phase reversal (handled by a 0.3 us hold-off, not tested on a real signal); the generator's pulses have no overshoot or band-limiting ripple.
