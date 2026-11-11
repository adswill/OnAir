# FM modulation monitor (family 889)

The readings of a broadcast modulation monitor, taken from the FM-demodulated multiplex (MPX) of one station. Receive only. **Simulated signals only**; the generator (`tests/x40/x40_gen2.h`) is not reachable from the application.
This is a reading of what arrives after the radio and the path. It is not a type-approval instrument and does not implement the 60 s window and weighting of ITU-R BS.412.

- Shown (once a second, from 1 s of MPX; Welch spectrum with 30 - 60 Hz bins): peak deviation (third-largest excursion in 5 ms blocks, so that a click of the noise is not read as deviation), RMS deviation, MPX power in dB against a sine of 19 kHz deviation (plain mean square of the multiplex),
  carrier offset, the 19 kHz pilot (kHz and % of 75 kHz, "usual range" 5.5 - 8.2 kHz), the stereo (L-R) band, the 57 kHz RDS injection, SCA subcarriers at 67 and 92 kHz, the audio (L+R) level.
- Logged: over-deviation (a second with a peak over 80 kHz), and changes of pilot, RDS and SCA (a change must hold for two readings).
- Input: any rate from 200 kHz up (decimated to about 250 kHz, 100 kHz channel filter). The 92 kHz SCA is only read at 250 kHz or more of working rate.
- Tested: deviation / pilot / RDS / L-R / audio / offset readings against the generator at 250 kHz, 500 kHz and 1 Msps, carrier offset 0, +12 and -35 kHz (and 400 Hz/s drift); mono station; over-deviation (95 kHz) and none at 60 kHz; pilot and RDS stopping and an SCA starting mid-run (the changes are logged); noise sweep.
  Peak deviation reads 4 to 13 % below the generator's largest instantaneous value (the channel filter and the third-largest rule take a little off the top); pilot within 0.4 kHz, RDS injection within 0.35 kHz.
- Noise (carrier to noise in 200 kHz): pilot read correctly to 15 dB; RDS reported present down to 25 dB, absent at 20 dB; SCA 67 reported at 40, 30 and 25 dB, absent at 20 dB; the peak reading rises with the noise (58.6 kHz at 30 dB, 60.8 at 25, 64.8 at 20, 72.2 at 15, true 58.4).
  A plain carrier in noise (35 and 22 dB), 10 minutes: no false RDS, SCA or over-deviation report.
- Not covered: BS.412 averaging and weighting, pre-emphasis-weighted audio level, left / right levels (stereo decoding is not done here), RDS content (the FM mode decodes it).
