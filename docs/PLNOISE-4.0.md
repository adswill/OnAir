# Power-line noise locator (family 891)

Finds the impulses in a band, measures their rate and level, and looks for a repetition. Receive only. **Simulated signals only**; the generator (`tests/x40/x40_gen2.h`) is not reachable from the application.
The mode measures; it cannot tell a pole from a lamp. To locate a source, walk with an antenna and watch the level.

- Method: envelope in a 10 kHz analysis band (48 ksps working rate). The noise level and spread are learned for 0.3 s and then followed (0.5 s time constant); an impulse is a peak over level + 6.5 spreads, and counted once. Every 2 s the spacings of all impulse pairs (1.5 to 60 ms) of the last 10 s are histogrammed; the strongest peak, and the smallest lag
  with 60 % of it, gives the repetition rate, refined from the spacings near it. It must be 4 times the mean bin and 12 pairs to count, and hold for two reports before a row is logged. Classes: twice the 50 / 60 Hz mains (100 / 120 Hz, an arc at every half cycle), the mains rate (50 / 60 Hz, half-wave), any other steady rate (a switching supply or a clock). Impulses without a rate: static or lightning.
- Shown: impulse rate, median level over the noise in the analysis band and its spread, repetition rate, how much of the impulse train falls on it. A pattern that disappears for three reports is logged as gone.
- Tested: 100, 120, 50, 60 and 173 Hz at 250 ksps and 1 Msps (rate within 0.15 Hz at the lower rates); a mains drifting 49.8 to 50.2 Hz with 30 % of the impulses missing and 25 random impulses a second; Poisson impulses alone (100 per second, 25 s): no pattern; 15 minutes of noise alone: 0 false patterns and 16 impulses counted (0.018 per second, the expected false-peak rate of the threshold).
- Level sweep (100 Hz, 20 % missing, 70 us wide, peak over the mean envelope of the wideband noise, 4 runs each): the repetition was found at 25, 18, 14, 10, 6 and 3 dB, not at 0 and -3 dB. The level shown is against the noise in the 10 kHz analysis band, which is about 9 dB higher than that figure.
- Not covered: impulses wider than a few ms (they are not impulses to this detector), polarity, direction finding, a repetition below 15 Hz.
