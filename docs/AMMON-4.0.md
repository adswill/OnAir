# AM modulation monitor (family 890)

Reads one AM broadcast station (LW, MW, SW) the way a modulation monitor does, once a second. Receive only. **Simulated signals only**; the generator (`tests/x40/x40_gen2.h`) is not reachable from the application.
It reports what arrives after the radio and the path (fading and noise are in it). It is not a type-approval instrument and does not judge whether a station meets a rule.

- Shown: carrier offset (from the phase step of the carrier, Hz), carrier level, modulation depth (RMS, as the depth of a sine), positive and negative peaks (third-largest excursion in 20 ms blocks, against the mean level of the carrier), asymmetry, mains hum (50 / 60 Hz and 100 / 120 Hz, in dB below the carrier),
  audio bandwidth (99 % of the audio power below f), share of the second in which negative peaks reach zero carrier.
- Logged: over-modulation (a second with at least 0.5 % of the time at zero carrier), carrier lost / returned (more than 12 dB under the best level of the last minute, held for two seconds).
- Input: 24 ksps and up (decimated to about 48 ksps, 12 kHz audio band).
- Tested: depth, peaks, bandwidth and offset against the generator at 96 ksps, 250 ksps and 1 Msps with carrier offset 0 / +350 / -2200 Hz and drift; mains hum of 3 % at 100 Hz (read -30 dB) and 0.6 % at 60 Hz (read -44 dB); asymmetric modulation; 130 % modulation (over-modulation rows) and 70 % (none); a 6 s carrier outage (three carrier rows); noise sweep.
- Noise (carrier to noise in 12 kHz): the depth reads 54.0 % at 50, 40 and 30 dB (true 53.9 %), 55.0 % at 20 dB, 63 % at 10 dB (the noise adds to it); the offset is within 1.0 Hz down to 20 dB; 10 dB gives one false over-modulation row. A plain carrier in noise (40 and 25 dB), 10 minutes: no false row, no false hum.
- Not covered: IBOC / DRM side-bands, the carrier shift of a modulated carrier, AM stereo (C-QUAM has its own mode), RDS-like data.
