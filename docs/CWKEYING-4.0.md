# CW keying quality meter (family 769)

Receive only. `core/src/x40/x40_cwkeying.cpp`, test `tests/x40/test_x40_cwkeying.cpp`. **Checked on simulated signals only.**

What a keyed carrier's timing and shape look like, for the person who sent it or the person who wants to know why it sounds wrong. The carrier (within +-1.5 kHz of the tuned frequency) is found and followed; its envelope (2 kHz rate, 800 Hz bandwidth) is cut into marks and spaces at the 50 % points with hysteresis (on above 50 % of the recent peak, off under 35 %, judged on a 4 ms smoothing).
From the last 60 elements: dit and dah lengths by two-means clustering of the marks, the speed (PARIS: 1200 / T in ms, T = (dit + element gap) / 2, so weight does not change the speed), the dah to dit ratio (ideal 3), the spaces sorted into element gaps (under 2 dits), character gaps (2 - 5) and word gaps (over 5) as multiples of the dit (ideal 1, 3, 7),
the weight (dit over element gap, ideal 1.00; a heavy keying is above), the dit scatter (coefficient of variation: a keyer is well under 2 %, a hand sending over 10 %), the rise and fall times (10 - 90 % of the plateau; limited by the envelope filter: under about 1.5 ms reads 1.5 ms) and the chirp (the carrier frequency 5 - 25 ms after key-down against the second half of the element, for marks of 50 ms or more).
It does not copy the text (the CW mode does). Needs about 20 dB carrier to noise in 500 Hz (at 15 dB the edges chatter and nothing is reported).

## Tested (generated keyer, text of 40 letters, 48 ksps, carrier to noise in 500 Hz of 40 dB, raised-cosine edges; 10 - 90 % time of a ramp of length L is 0.59 L)
| sent | read |
|---|---|
| 20 wpm, dah 3.0, ramp 6 ms | 20.0 wpm, dah / dit 2.94, rise 3.6 ms, fall 3.6 ms (3.5), scatter 0.5 % |
| 12 wpm, ramp 10 ms, +400 Hz, 2 Hz/s | 12.0, 2.96, 5.9 / 6.0 ms (5.9) |
| 30 wpm, ramp 4 ms, -300 Hz | 30.0, 2.92, 2.4 / 2.4 ms (2.4) |
| 20 wpm, dah 3.3, weight 1.15 | 20.0, 3.24, weight read within 8 % of 1.35 |
| 20 wpm, 12 % uniform jitter of the elements | scatter 6.4 % (the generator's standard deviation is 6.9 %), 2.85 |
| 20 wpm, a chirp of +90 Hz decaying with 20 ms | chirp read 33.9 Hz (the definition averages 5 - 25 ms and the envelope filter smooths: about 44 Hz would be the ideal reading) |
Noise sweep (carrier to noise in 500 Hz -> wpm / scatter, true 20.0): 40: 20.0 / 0.4; 30: 20.0 / 0.4; 20: 20.0 / 1.0; 15: nothing reported. Noise alone and a steady carrier: no row.
