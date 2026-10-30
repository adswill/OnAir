# Reception Doctor

View > Reception doctor (also Tools list and the command palette). Nothing decoding? Press **Diagnose**: the doctor takes a quarter of a second of the
running receiver's raw IQ (the same tap the frequency calibration uses, `Engine::setIqTap`), reads the radio's state and says, in plain sentences,
what is wrong, worst first, with a button for the fix where one is safe. **Check again** after a fix.

Code: `core/include/dect2/doctor/reception_doctor.h`, `core/src/doctor/reception_doctor.cpp` (pure analysis, no ImGui, no engine),
`app/panels/doctor_ui.cpp` (window, capture, fixes). Tests: `tests/doctor/test_reception_doctor.cpp` (ctest `reception_doctor`), synthetic IQ.

## What it finds

| Finding | How | Fix button |
|---|---|---|
| Overload (ADC clipping) | more than 0.1 % of the samples with I or Q at 98 % of full scale, or a peak at full scale | lower gain by 6 / 12 / 20 dB (by how much clips) |
| Too little signal | rms below -48 dBFS on a live radio; at maximum gain the sentence points at antenna, cable and an LNA | raise gain, when there is room |
| Nothing in the spectrum | the strongest smoothed bin is within 8 dB of the median bin: flat spectrum, no antenna / wrong frequency / dead band | none |
| Signal off-centre | a signal 0.33 to 3 times the mode's width sits more than 10 % of that width from where the mode listens | tune by the measured offset |
| Sample rate too narrow | the mode's channel is more than 85 % of the sample rate | set the sample rate to 1.25 times the width (never limited, only suggested) |
| Strong adjacent interferer | the strongest bin outside the channel and a skirt of half a width is 25 dB over the floor and 15 dB over the channel's strongest bin | lower gain by 10 dB |
| USB drops | the radio delivers under 97 % of its rate, or lost samples in the last 10 s | none (another port, no hub) |
| CPU drops | OnAir lost samples in the last 10 s | none (close programs, lower the rate) |
| DC spike | the DC bins are 15 dB over the bins 20 to 60 away; a warning only when the spike sits in the channel | none (offset tuning, Remove DC spike) |
| I/Q imbalance | a narrow tone 30 dB over the floor whose mirror is a peak less than 30 dB below it | none (IQ correction setting) |
| Wrong mode | nothing above explains it and the signal identifier (`core/src/engine/sig_ident.cpp`, docs/IDENTIFIER.md) gives a *Likely* verdict for another mode | switch to that mode |

When the decoder is locked the doctor stays quiet about everything except overload, drops and the like. The sentence is English from the core; the app
translates it by lookup and shows the English when there is no translation (the numbers are in the sentence).

## Limits

* One quarter-second capture at one moment: a burst mode (ADS-B, ACARS) may show a flat spectrum between bursts. Press the button again.
* The mode's width and offset come from the app's tune settings (`tune.bandwidthMhz`, the tuning offset); a mode that listens off to the side by its own
  design, not through the tuning offset, can be called off-centre.
* The IQ tap is shared with the frequency calibration: do not run both at once.
* Thresholds are rules of thumb, as in the station health report (docs/STATION_HEALTH.md), which looks at bands rather than the live signal.

## Calibrate frequency error

This already exists: Tools > Calibrate frequency (docs/self-calibration.md). It measures the radio's ppm against an FM stereo pilot, an ATSC pilot,
a carrier of known frequency (time signals, a CW beacon) or a typed-in carrier, shows ppm and its uncertainty, keeps a per-radio history and writes
the radio's `ppm` setting. The core also converts cell and GPS carrier errors (`fromCell`, `fromGpsCarrier`). Not wired into the dialog yet: a DVB-T / DAB
pilot as a reference and the GSM / LTE reference (they need those modes running on the same capture).
