# Radar pulse analyser (family 892)

Measures the pulses of the strongest pulsed emitter in the capture (marine, air-traffic or weather radar, or any pulsed source). Receive only. **Simulated signals only**; the generator (`tests/x40/x40_gen2.h`, rendered block by block) is not reachable from the application.
It measures the emitter and says nothing of what the radar is. Radar bands (S, C, X) need a down-converter or a suitable radio and a 2 Msps (or more) capture.

- Method: envelope of two samples' power (a microsecond pulse is two samples wide), noise level and spread followed, a pulse is a peak over level + 6.5 spreads of at least two samples. Width at the half-power point (interpolated), time of arrival, level, and the frequency of the carrier inside the pulse (from five samples up).
  Spacings (PRI) of the last 8 s (the gaps between two passes of a scanning beam are dropped) are classified: constant (85 % of the spacings within 0.6 %), staggered (the spacing repeats after 2 to 8 steps), jittered (spread below 12 %), irregular. The pulse level over 30 s gives the antenna turn time (autocorrelation of the level, 0.8 - 12 s, needs a 4 dB swing).
  A class or a PRI change must hold for two reports before it is logged; an emitter silent for 3 s is logged as gone. One emitter at a time (the dominant one).
- Tested at 2 Msps: constant PRI 400 us, 1 ms, 2.5 ms with pulse widths 0.5, 1 and 2 us and 180 kHz inside the pulse (PRI within 1 %, width within 35 % + 0.15 us for pulses from 1 us, the frequency within 25 kHz from 1.5 us); stagger with 2, 3 and 4 positions; 10 % jitter; a scanning antenna (2.4 s per turn, a beam of 12 ms sigma, 40 pulses a turn, 24 s) read as constant PRI with a turn time within 0.1 s.
- Level sweep (1 us pulses, PRI 1 ms, peak amplitude over the wideband noise sigma): constant PRI found at 20 and 17 dB, not at 14 dB and below. Noise alone: 5 minutes at 1 Msps, no emitter.
- Not covered: several emitters at once, intra-pulse modulation (chirp, Barker), pulse widths under one sample (0.5 us at 2 Msps reads 1 us), direction and range.
