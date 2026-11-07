# Sferics (lightning), 3.7

Family 243, engine standard 245. Receive only. Own mode-list entry under Weather & science.

## What it does

Lightning discharges radiate short broadband pulses (sferics, atmospherics) that can be heard as crackles on every band below about 30 MHz, strongest at VLF / LF. The mode counts them from the IQ stream of any radio (or a 48 kHz sound card as IQ):

- smoothed power (25 us, at least 16 samples), a noise floor from the median of 1 ms block means over 4 s, a threshold over it (default 10 dB, 6 to 30 dB);
- each crossing is one impulse: start time on the sample clock, strength over the floor, width; pulses closer than the merge time (default 0.5 ms) are one strike;
- strikes, flashes (strikes within 0.5 s of each other), strikes per minute (last 1 and 10 minutes), a 60 minute histogram, the last 100 strikes;
- **mains interference**: impulses that repeat at a multiple of the 100 / 120 Hz mains half period (lamp dimmers, chargers, power lines) are recognised after 24 of 32 impulses fit and left out of the count; the receiver waits 0.5 s before it counts, so a train is recognised before its first impulses are counted;
- impulses longer than 5 ms are not lightning (carriers, switching) and are only counted as such.

Simple view: the meters, the histogram and the list of strikes side by side. Advanced: the same with the Receiver tab (noise floor, rejected impulses). The threshold, the mains filter and the merge time are decoder options in the side panel; Auto setup and Reset this mode work as in every mode.

## What it does not do

- It shows when and how strong, **not where**. A position needs the arrival times from several stations at once (a network such as Blitzortung). There is no map and no distance.
- It cannot tell lightning from other impulsive noise that is irregular (a faulty insulator, a sparking contact). A noisy neighbourhood shows as strikes.
- The strike strength is relative to the receiver's own noise floor, not calibrated; do not compare numbers between radios or antennas.
- The wall clock is the start of the capture plus the sample clock. A recording shows the time of playback, not of recording.

## Tests (synthetic only)

`tests/sferics/test_sferics_rx.cpp` renders noise plus damped-oscillation sferics and mains impulse trains and checks, at 48 kHz, 250 kHz, 1, 2 and 2.4 Msps: exact strike and flash counts, strike times within 0.2 ms, carrier offsets of -300 and +150 kHz, noise levels from -54 to -26 dBFS (the floor reading follows the noise), the threshold, 50 and 60 Hz mains alone and under a storm, an impulse of 20 ms, noise only at the lowest threshold. No real storm recording has been tried; thresholds and the mains rule are set from the synthetic cases and need real recordings. There are no frames and so no reserved bits.
