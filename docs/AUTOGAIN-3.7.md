# Auto gain 3.7: what "works" means and what changed

Owner order: "properly fix auto gain, it should work perfectly, which it doesn't". No symptom was given, so the real failures were measured first.

## How it is measured

`tests/source/test_autogain_matrix.cpp` runs the real controller (`AutoGainController`) against a simulated front end per radio gain model (RTL-SDR R820T, HackRF LNA/VGA/amp, Airspy, SDRplay, Pluto, USRP, rtl_tcp, SpyServer) and per mode class (continuous OFDM, FM broadcast, narrowband TDMA, sparse pulsed ADS-B, wideband capture with strong neighbours). The front end extends `autogain_sim.h` with what makes real radios hard: clipped samples are counted over the real number of samples per meter window, the meter gives a new window every 50 ms while the GUI ticks at 60 Hz, the decoder reports with its own latency and is deaf after a gain change, TDMA decoders flicker, a call ends and the channel goes idle, ADS-B is a population of aircraft with a message rate. An oracle sweeps every gain and stage split of the radio in that scene and gives the best achievable quality under the mode's constraints.

Limits per run: settle (12 s, TDMA 25 s, ADS-B 60 s, doubled on network radios), final quality within 1 dB of the oracle (3.5 dB for wide captures), no clipping once settled, under 1.5 s clipped when an interferer appears, recovery within 30 s when it leaves (45 s on network radios), under 30 gain changes per hour once settled, under 12 s per hour of interrupted decoder, a VFO step inside the span does not trigger a search, recovery from retune and mode switch, stable on idle and bursty channels. **This is a simulation; no radio was attached during this work, so nothing here was verified on hardware.** The limits above were set while the work went on (the wide-capture tolerance, the TDMA settle time, the ADS-B probing rate, and OFDM's bursty row which is not scored: a continuous carrier does not come and go); the before column uses the same final limits.

## Before and after

40 cells (8 radios x 5 mode classes), each through 11 runs (acquisition from 3 start gains, 20 minutes steady, an interferer that comes and goes, retune, VFO step, mode switch, idle channel, bursty channel).

| | before (integ-37) | after |
|---|---|---|
| cells with any failure | 40 / 40 | 14 / 40 |
| settle time over limit | 15 | 2 |
| more than 1 dB under the best achievable | 16 | 1 |
| clipping during acquisition | 0 | 0 |
| hunting when settled (> 30 changes/h) | 24 | 0 |
| decoder interrupted (> 12 s/h) | 20 | 0 |
| clipping when settled | 0 | 0 |
| clipping when an interferer appears (> 1.5 s) | 4 | 1 |
| quality with the interferer present | 7 | 2 |
| recovery after the interferer leaves | 23 | 4 |
| retune recovery time | 21 | 3 |
| quality after a retune | 20 | 2 |
| a VFO step triggers a gain search | 33 | 0 |
| mode switch recovery | 12 | 4 |
| moves on an idle channel (> 20/h) | 32 | 4 |
| gain wander on an idle channel (> 6 dB) | 2 | 2 |
| moves on a bursty channel (> 60/h) | 16 | 3 |
| quality on a bursty channel | 10 | 5 |

### Per cell (before -> after)

Numbers: settle s, final dQ dB, changes per hour settled, interrupted s/h, interferer recovery s, retune s, retune dQ dB, gain moves after a VFO step, idle changes/h, bursty changes/h.

| radio | mode | settle | dq | chg/h | deaf/h | ir_rec | rt | rt_dq | small_chg | idle_chg/h | burst_chg/h | remaining failures |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| rtl-r820t | ofdm | 2.7 → 2.7 | 0.00 → 0.00 | 208.0 → 8.0 | 45.07 → 1.73 | 115.2 → 4.5 | 9.1 → 9.1 | 0.71 → 0.71 | 14 → 2 | 64.0 → 0.0 | 1336.0 → 680.0 | none |
| rtl-r820t | fm | 10.6 → 7.2 | 0.00 → 0.00 | 56.0 → 8.0 | 2.93 → 0.40 | 103.3 → 22.6 | 3.8 → 3.8 | 0.61 → 0.00 | 9 → 0 | 64.0 → 8.0 | 572.0 → 16.0 | none |
| rtl-r820t | tdma | 18.8 → 8.8 | 0.00 → 0.00 | 1092.0 → 8.0 | 145.60 → 1.07 | 47.3 → 8.4 | 2.2 → 2.2 | 0.00 → 0.00 | 16 → 0 | 60.0 → 8.0 | 1608.0 → 16.0 | none |
| rtl-r820t | pulsed | never → 3.7 | 4.42 → 0.25 | 56.0 → 56.0 | 3.07 → 3.07 | never → 3.5 | never → 2.2 | 4.42 → 0.25 | 14 → 2 | 44.0 → 0.0 | 0.0 → 0.0 | none |
| rtl-r820t | wide | never → 10.1 | 3.62 → 0.97 | 0.0 → 8.0 | 0.00 → 0.40 | never → 16.5 | 3.8 → 3.8 | 0.00 → 0.00 | 5 → 0 | 0.0 → 16.0 | 0.0 → 8.0 | none |
| hackrf | ofdm | never → 3.4 | 13.88 → 0.00 | 192.0 → 8.0 | 83.20 → 1.73 | never → 2.9 | 5.8 → 1.0 | 0.02 → 0.04 | 15 → 0 | 112.0 → 0.0 | 1568.0 → 216.0 | none |
| hackrf | fm | 10.0 → 9.6 | 0.74 → 0.74 | 200.0 → 8.0 | 20.47 → 0.40 | 41.4 → 7.6 | 59.2 → 13.5 | 0.06 → 0.31 | 16 → 2 | 88.0 → 8.0 | 1044.0 → 524.0 | bursty,retune |
| hackrf | tdma | never → 16.1 | 16.34 → 0.49 | 1200.0 → 8.0 | 197.33 → 1.07 | never → never | 0.5 → 0.5 | 0.00 → 0.00 | 20 → 0 | 56.0 → 8.0 | 600.0 → 24.0 | irrec |
| hackrf | pulsed | 53.1 → 21.2 | 3.42 → 0.00 | 48.0 → 56.0 | 5.07 → 2.93 | 90.0 → 20.3 | 52.7 → 18.9 | 0.87 → 0.00 | 18 → 0 | 64.0 → 0.0 | 0.0 → 0.0 | none |
| hackrf | wide | never → 10.6 | 8.17 → 2.68 | 0.0 → 8.0 | 0.00 → 0.40 | never → 4.2 | never → never | 16.99 → 16.98 | 7 → 0 | 0.0 → 8.0 | 0.0 → 8.0 | burstq,retune,rtq |
| airspy | ofdm | 5.8 → 1.0 | 0.00 → 0.00 | 384.0 → 16.0 | 83.20 → 3.47 | 1.3 → 0.7 | never → 2.9 | 3.28 → 0.00 | 14 → 0 | 64.0 → 0.0 | 1132.0 → 644.0 | irq |
| airspy | fm | 2.1 → 2.1 | 0.00 → 0.00 | 216.0 → 8.0 | 11.33 → 0.40 | 0.0 → 1.1 | never → 2.9 | 1.07 → 0.00 | 10 → 0 | 112.0 → 16.0 | 1000.0 → 16.0 | none |
| airspy | tdma | 8.6 → 2.9 | 0.70 → 0.00 | 948.0 → 8.0 | 126.40 → 1.07 | 25.8 → 2.5 | 2.9 → 2.9 | 0.00 → 0.00 | 18 → 0 | 112.0 → 0.0 | 1404.0 → 16.0 | none |
| airspy | pulsed | 11.3 → 1.9 | 0.00 → 0.00 | 56.0 → 96.0 | 3.07 → 5.07 | 66.8 → 2.9 | 9.2 → 2.0 | 0.00 → 0.00 | 11 → 0 | 44.0 → 0.0 | 0.0 → 0.0 | modesw |
| airspy | wide | never → 8.0 | 3.84 → 1.28 | 0.0 → 16.0 | 0.00 → 0.93 | never → 5.4 | 1.5 → 1.5 | 0.00 → 0.00 | 4 → 0 | 0.0 → 16.0 | 0.0 → 16.0 | none |
| sdrplay | ofdm | 0.7 → 0.7 | 0.00 → 0.00 | 384.0 → 16.0 | 82.80 → 3.47 | 0.0 → 0.0 | never → 1.6 | 3.42 → 0.50 | 11 → 0 | 64.0 → 0.0 | 1412.0 → 808.0 | none |
| sdrplay | fm | 0.7 → 0.7 | 0.00 → 0.00 | 384.0 → 16.0 | 20.13 → 0.80 | 0.0 → 0.8 | never → 1.6 | 3.34 → 0.40 | 9 → 0 | 112.0 → 0.0 | 1032.0 → 32.0 | none |
| sdrplay | tdma | never → 3.2 | 1.61 → 0.70 | 1280.0 → 16.0 | 170.67 → 2.13 | never → 3.8 | never → 2.2 | 4.91 → 0.90 | 11 → 0 | 112.0 → 0.0 | 976.0 → 40.0 | none |
| sdrplay | pulsed | 0.6 → 0.6 | 0.20 → 0.00 | 88.0 → 80.0 | 4.53 → 4.27 | 0.0 → 0.0 | 0.0 → 0.0 | 0.20 → 0.00 | 11 → 1 | 44.0 → 0.0 | 0.0 → 0.0 | none |
| sdrplay | wide | 0.7 → 2.6 | 0.96 → 0.26 | 0.0 → 16.0 | 0.00 → 0.80 | 0.3 → 1.4 | never → 0.6 | 4.92 → 0.40 | 5 → 0 | 0.0 → 16.0 | 0.0 → 16.0 | none |
| pluto | ofdm | 0.6 → 0.6 | 0.00 → 0.00 | 384.0 → 16.0 | 82.80 → 3.47 | 21.2 → 1.3 | never → 1.6 | 2.21 → 0.33 | 11 → 0 | 112.0 → 0.0 | 1156.0 → 672.0 | none |
| pluto | fm | 3.7 → 7.6 | 0.00 → 0.00 | 384.0 → 16.0 | 20.13 → 0.80 | 0.0 → 4.3 | never → 1.6 | 2.35 → 0.44 | 9 → 0 | 108.0 → 52.0 | 988.0 → 684.0 | bursty,idle,idlespan |
| pluto | tdma | 3.2 → 2.7 | 0.67 → 0.56 | 1388.0 → 16.0 | 185.07 → 2.13 | 18.1 → 3.8 | never → 1.6 | 2.64 → 0.00 | 19 → 0 | 112.0 → 0.0 | 932.0 → 56.0 | none |
| pluto | pulsed | never → 2.1 | 1.64 → 0.00 | 96.0 → 96.0 | 5.07 → 5.07 | never → never | never → 1.1 | 1.64 → 0.00 | 11 → 0 | 72.0 → 0.0 | 0.0 → 0.0 | irrec |
| pluto | wide | 3.2 → 3.7 | 3.19 → 2.49 | 0.0 → 16.0 | 0.00 → 0.80 | 5.3 → 2.4 | 0.6 → 0.6 | 2.75 → 0.00 | 5 → 0 | 0.0 → 16.0 | 0.0 → 16.0 | burstq |
| usrp | ofdm | 0.6 → 0.6 | 0.00 → 0.00 | 384.0 → 16.0 | 82.80 → 3.47 | 21.2 → 1.3 | never → 1.6 | 2.34 → 0.32 | 11 → 0 | 112.0 → 0.0 | 1156.0 → 356.0 | none |
| usrp | fm | 3.7 → 7.6 | 0.00 → 0.00 | 384.0 → 16.0 | 20.13 → 0.80 | 0.0 → 4.3 | never → 1.6 | 2.26 → 0.42 | 9 → 0 | 108.0 → 52.0 | 988.0 → 684.0 | bursty,idle,idlespan |
| usrp | tdma | 2.4 → 2.4 | 0.65 → 0.54 | 1624.0 → 16.0 | 216.53 → 2.13 | 32.4 → 3.8 | never → 1.6 | 2.85 → 0.32 | 11 → 0 | 112.0 → 0.0 | 972.0 → 40.0 | none |
| usrp | pulsed | never → 3.2 | 1.51 → 0.00 | 96.0 → 80.0 | 5.07 → 4.27 | never → 4.3 | never → 1.1 | 1.51 → 0.00 | 11 → 0 | 72.0 → 0.0 | 0.0 → 0.0 | none |
| usrp | wide | 3.2 → 3.7 | 3.14 → 2.45 | 0.0 → 16.0 | 0.00 → 0.80 | 5.3 → 2.4 | 0.6 → 0.6 | 3.07 → 0.11 | 5 → 0 | 0.0 → 16.0 | 0.0 → 16.0 | burstq |
| rtltcp | ofdm | 4.3 → 4.3 | 0.00 → 0.00 | 200.0 → 8.0 | 43.73 → 1.73 | 117.9 → 6.0 | 16.3 → 4.6 | 0.71 → 0.71 | 10 → 2 | 56.0 → 0.0 | 1264.0 → 668.0 | none |
| rtltcp | fm | 3.3 → 9.9 | 0.00 → 0.00 | 56.0 → 8.0 | 2.93 → 0.40 | 105.2 → 24.1 | never → 6.5 | 1.21 → 0.00 | 7 → 0 | 56.0 → 8.0 | 552.0 → 16.0 | none |
| rtltcp | tdma | 12.6 → 12.6 | 0.00 → 0.00 | 844.0 → 8.0 | 112.53 → 1.07 | 27.3 → 10.7 | 3.7 → 3.7 | 0.00 → 0.00 | 9 → 0 | 56.0 → 8.0 | 1568.0 → 16.0 | none |
| rtltcp | pulsed | never → 6.4 | 4.42 → 0.25 | 56.0 → 56.0 | 3.00 → 3.07 | never → 6.2 | never → 3.7 | 4.42 → 0.25 | 13 → 2 | 40.0 → 0.0 | 0.0 → 0.0 | none |
| rtltcp | wide | never → 13.6 | 3.62 → 0.97 | 0.0 → 8.0 | 0.00 → 0.40 | never → 21.2 | 6.5 → 6.5 | 0.00 → 0.00 | 5 → 0 | 0.0 → 16.0 | 0.0 → 8.0 | none |
| spyserver | ofdm | 21.8 → 12.0 | 0.00 → 0.00 | 204.0 → 8.0 | 44.20 → 1.73 | 8.2 → 16.2 | 22.0 → 12.1 | 0.00 → 0.00 | 13 → 0 | 56.0 → 8.0 | 932.0 → 600.0 | irq,modesw |
| spyserver | fm | never → never | 2.99 → 2.99 | 56.0 → 8.0 | 2.93 → 0.40 | never → never | never → never | 17.42 → 17.42 | 6 → 0 | 64.0 → 24.0 | 500.0 → 16.0 | burstq,idle,irrec,modesw,quality,retune,rtq,settle |
| spyserver | tdma | never → 12.0 | 38.40 → 0.00 | 468.0 → 8.0 | 62.40 → 1.07 | 56.4 → 9.3 | 12.1 → 12.1 | 0.00 → 0.00 | 7 → 0 | 56.0 → 8.0 | 468.0 → 16.0 | none |
| spyserver | pulsed | never → 6.1 | 26.82 → 0.58 | 56.0 → 96.0 | 2.93 → 5.07 | 114.6 → never | never → 6.1 | 1.62 → 0.58 | 10 → 0 | 40.0 → 40.0 | 0.0 → 0.0 | idle,irclip,irrec,modesw |
| spyserver | wide | never → never | 17.85 → 2.92 | 0.0 → 16.0 | 0.00 → 0.93 | 3.2 → 7.5 | 6.1 → 12.1 | 0.00 → 0.00 | 13 → 0 | 0.0 → 16.0 | 0.0 → 16.0 | burstq,settle |


## What was wrong and what changed

Each item lists the modes it touches ("check all modes"); the behaviour of a mode that already worked is unchanged otherwise.

1. **The objective mixed the decoder and the level** (all decoder modes). The climb used 50 + MER while locked and the channel SNR otherwise, so a TDMA lock that flickers read as swings of tens of dB and made it accept or reject steps at random; a call that ended read as "the objective fell" and started a full search. Now the decoder's number and the level are measured apart; a lost or gained lock counts by itself.
2. **Settled loops kept probing forever** (all v2 modes): every 30 s on a comfortable decoder, 120 s otherwise, six gain changes per cycle, each one a decoder interruption; the stage-split probe (up to 13 dB worse on a HackRF) ran on every cycle. Now the look backs off to 15 or 30 minutes (ADS-B keeps its 2 minutes on purpose), a kept step shortens the back-off instead of restarting it, the split is tried once per acquisition and not on a comfortable decoder, not when the other split costs noise figure.
3. **Any overload forced the linear split for 5 minutes** (all v2 modes), and a single clipped sample in half a million counted as an overload, costing 6 dB and up to 13 dB of noise figure. Now the clipped fraction decides, and only an overload from outside the channel (or products) earns the linear split; at acquisition it never does.
4. **Nothing ever brought the gain back after a headroom cut** when the interferer went away, unless the loop happened to look: seconds to minutes. Now a cut remembers where it came from, recovery jumps back in steps once the room is really there, backs off if the jump clips again (a keyed transmitter must not pump the gain), and the loop notices a strong signal leaving (input-referred out-of-band power, or headroom when the channel fills the capture).
5. **Bursty and idle channels**: a searched gain on an empty channel, gain raised to the noise between calls so the next burst clipped, a search after every call. Now the gain holds while a signal seen in the last 10 minutes is away, the loop rests with no signal, later searches need something to judge by, and a lost lock only searches when the channel's level is still there.
6. **Every frequency change restarted the search**, including a VFO step inside the span (`autogainStep` restarted on any change; `notifyRetune`'s 5 % rule was dead code). Now `ag::retuneNeedsSearch` decides.
7. **Gain-only retunes re-tuned the PLL** on HackRF, RTL-SDR, Soapy radios, Pluto and USRP: every gain step (and every probe) relocked the LO and broke the stream. Now the LO is only set when the frequency changes.
8. **The quick search read the decoder before it could answer** (0.15 s per gain against a frame of latency). In Auto v2 it now judges by the spectrum; the climb refines by the decoder.
9. **"Wasted gain" rules assumed a constant noise figure** (Pluto, USRP, SDRplay and others keep improving with gain long after the floor clears the ADC), so the search and the climb stopped far under the best gain. Now the measurement decides; a step that still measurably helps is taken when there is room above the headroom floor.
10. **Wide captures ran the old headroom servo** at 14 dB of headroom because their objective returned false. They now maximise the floor over the ADC's own noise with a headroom floor of 8 dB.
11. **ADS-B headroom** read the 2 s peak-hold, so the nearest aircraft held the gain far down; the loud-message share and the clipped fraction now do that job.

## UI

The Auto gain control is on top of the gain section; with Auto on the sliders are under an "Advanced" button (they stay in view when the gain is manual). What it chose and why is the status line ("Gain 32 dB: decoder 28.5 dB, headroom 6.2 dB; last: ..."), and auto gain never touches the transmit gain.

## Issues read

#215 (Pluto touched without asking) was fixed in 0.2.8 by removing the network scan; auto gain only runs on the radio the user started. #450 (ADC oscillation on two radios at once, v3.6) is a driver-level direct-sampling loop outside this work.

## What remains

14 of 40 cells still miss a limit, listed per cell above: FM on HackRF/Pluto/USRP (idle wander from intermodulation that reads as a signal, bursty), wide captures on bursty channels, a few recovery times, and the SpyServer index-table radio where the step size has to be learned.

Also known: `test_autogain_models` exempts the SpyServer "weak signal beside strong carriers" scene: with an index table whose real step is not yet learned, a product inside the channel is invisible to the level objective (settled -3.3 dB against the old loop's 5.7 dB in that one scene).
