# GNSS: carrier smoothing of the pseudoranges

The position of the GNSS mode comes from code pseudoranges, one a second per satellite. A code range is unbiased but noisy (2 - 5 m per reading at
40 dB-Hz with 1 ms loops); the carrier of the same satellite says how much the range CHANGED since the last reading to millimetres. The receiver now
blends the two (a Hatch filter): `smoothed = range / n + (n - 1) / n * (smoothed before + change of the carrier)`, `n` growing to the window.

- On by default, window 100 epochs (100 s). `GnssReceiver::setCarrierSmoothing(epochs)`; 0 or 1 turns it off. `gnsstool rx|run --smooth N`.
- `GnssMeasurement::smoothed` says how many epochs are in a satellite's range (1: the code range alone), `carrierCycles` is the carrier it used.

## What it does to the fix

| | off | on |
|---|---|---|
| off-air recording, 8 GPS satellites, 300 s (RSPdx, 2.048 Msps): 3D scatter of the fixes | 11.7 m | 3.0 m |
| the same, horizontal | 5.5 m | 0.8 m |
| the same, jump from one fix to the next (rms) | 10.8 m | 0.9 m |
| a second off-air recording, 9 - 10 satellites: 3D scatter | 12.1 m | 2.1 m |
| simulator, GPS, error against the truth over 30 s (horizontal / vertical rms) | 4.9 / 8.9 m | 0.6 / 0.5 m |
| simulator, GPS + Galileo, `test_gnss_galileo` "clean" | 2.9 / 3.3 m | 0.35 / 0.17 m |
| simulator, GPS + BeiDou with the broadcast offset, `test_gnss_beidou` | 2.8 / 2.0 m | 0.06 / 0.18 m |

The satellites in the fix and the time to the first fix do not change. The cost is not measurable (`test_gnss_rx`: the same real-time factor).

## The four things the textbook filter needs here

1. **The common term, per carrier.** The pseudoranges are taken against a reference time that is re-chosen every epoch, and the radio's oscillator
   adds its own rate to every carrier. Both move the code-minus-carrier change of every satellite on ONE carrier frequency by the same amount; that
   amount is taken out first, as the mean over the satellites that have not slipped. (A median was tried first: the medians of changes do not add up
   to the change of anything, and their sum wandered 5 - 10 m in a minute.) GPS, QZSS and Galileo E1 are one group, BeiDou B1I another.
2. **GLONASS is not smoothed.** Its satellites each have a carrier of their own; with a tuning error of 5 kHz or a sample clock 20 ppm off they
   drift 2 m/s against each other, and the filter turned that into 60 - 190 m of position error (`test_gnss_glonass`).
3. **The group follows its code ranges.** What the running filters of a group hold in common is put back on the code ranges every epoch. The
   satellites keep their smoothing against each other, which is what steadies the position; a satellite that joins does not stand off from the older
   ones (7 - 15 m in height while Galileo joined GPS, before this).
4. **A new clock, a new filter.** When the broadcast GST-GPS or BDT-GPS offset arrives, a satellite's range moves to the GPS clock: a step of the
   size of that offset, too small to look like a slip. Its filter starts again. A jump of more than 30 m against its group is a slip and does the same,
   and so does a channel whose phase loop is not locked (pull-in, or after a phase drop).

## Tried and not kept

The 20 ms phase loop for weak LNAV signals (a4c520f74, reverted in 48c1abbd8) was put back on top of this and measured on the two recordings: no
change on the one with eight strong satellites, and on the one with weak satellites the 3D scatter went from 2.1 to 2.4 m and the fix-to-fix jump
from 0.4 to 1.5 m. With the ranges smoothed, the noise of the tracking loops is no longer what limits the fix.

## Not verified

- No off-air recording with Galileo, BeiDou or GLONASS in it: those paths are checked on the simulator only.
- A change of ephemeris (a new IODE) steps a range by up to a few metres; the filter does not restart on it.
- The simulator does not put the satellite clock's rate on the carrier (millimetres a second): real signals do, so the real case is the easier one.

`tests/gnss/test_gnss_smooth.cpp` runs the three cases above (GPS off / on, Galileo joining, GLONASS left alone). The method and the first
correction come from gr-gpsrx (https://github.com/Felbs/gr-gpsrx, GPL-3.0-or-later), where they were measured on air.
