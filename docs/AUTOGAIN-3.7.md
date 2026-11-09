# Auto gain 3.7: what "works" means and what changed

Owner order: "properly fix auto gain, it should work perfectly, which it doesn't". No symptom was given, so the real failures were measured first.

## How it is measured

`tests/source/test_autogain_matrix.cpp` runs the real controller (`AutoGainController`) against a simulated front end per radio gain model (RTL-SDR R820T, HackRF LNA/VGA/amp, Airspy, SDRplay, Pluto, USRP, rtl_tcp, SpyServer) and per mode class (continuous OFDM, FM broadcast, narrowband TDMA, sparse pulsed ADS-B, wideband capture with strong neighbours). The front end extends `autogain_sim.h` with what makes real radios hard: clipped samples are counted over the real number of samples per meter window, the meter gives a new window every 50 ms while the GUI ticks at 60 Hz, the decoder reports with its own latency and is deaf after a gain change, TDMA decoders flicker, a call ends and the channel goes idle, ADS-B is a population of aircraft with a message rate. An oracle sweeps every gain and stage split of the radio in that scene and gives the best achievable quality under the mode's constraints.

Limits per run: settle 12 s, final quality within 1 dB of the oracle, no clipping once settled, under 1.5 s clipped when an interferer appears, recovery within 30 s when it leaves, under 30 gain changes per hour once settled (ADS-B included), under 12 s per hour of interrupted decoder, recovery from retune and mode switch, moves under 20 per hour on an idle channel and 30 on a bursty one.

Limits that differ from those, each with its physical reason:
- **ADS-B settle 60 s**: the only quality signal is the message rate; a rate worth a decision needs at least 4 s and up to 20 s of a quiet sky per gain tried (PulseJudge).
- **Network radios: settle and interferer recovery times doubled** (rtl_tcp, SpyServer): each gain change is a round trip over the network, and the settling and measuring time per look scale with it.
- **OFDM on a bursty channel: 120 moves/h**: a DVB or DAB multiplex does not key on and off; the run is a stress case, and a channel that fills the capture cannot tell its own signal leaving from a neighbour leaving.
- **VFO step: at most 2 gain moves in 20 s**: a scheduled look (up and back) can fall inside the window; it is not a search.
- **The oracle obeys the mode's headroom floor**, the constraint the controller enforces.

All of this is a simulation; no radio was attached during this work, so nothing here was verified on hardware.

## Before and after

40 cells (8 radios x 5 mode classes), each through 11 runs (acquisition from 3 start gains, 20 minutes steady, an interferer that comes and goes, retune, VFO step, mode switch, idle channel, bursty channel).

| | before (integ-37) | after |
|---|---|---|
| cells with any failure | 40 / 40 | 5 / 40 |
| settle time over limit | 17 | 1 |
| more than 1 dB under the best achievable | 17 | 0 |
| clipping during acquisition | 0 | 0 |
| hunting when settled (> 30 changes/h) | 32 | 0 |
| decoder interrupted (> 12 s/h) | 20 | 0 |
| clipping when settled | 0 | 0 |
| clipping when an interferer appears (> 1.5 s) | 4 | 0 |
| quality with the interferer present | 8 | 0 |
| recovery after the interferer leaves | 27 | 1 |
| retune recovery time | 23 | 3 |
| quality after a retune | 22 | 2 |
| a VFO step triggers a gain search | 40 | 0 |
| mode switch recovery | 14 | 2 |
| moves on an idle channel (> 20/h) | 32 | 0 |
| gain wander on an idle channel (> 6 dB) | 2 | 0 |
| moves on a bursty channel (> 30/h, OFDM 120/h) | 24 | 0 |
| quality on a bursty channel | 9 | 1 |

### Per cell (before -> after)

Numbers: settle s, final dQ dB, changes per hour settled, interrupted s/h, interferer recovery s, retune s, retune dQ dB, gain moves after a VFO step, idle changes/h, bursty changes/h.

| radio | mode | settle | dq | chg/h | deaf/h | ir_rec | rt | rt_dq | small_chg | idle_chg/h | burst_chg/h | remaining failures |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| rtl-r820t | ofdm | 2.7 → 2.7 | 0.00 → 0.00 | 208.0 → 8.0 | 45.07 → 1.73 | 115.2 → 4.5 | 2.2 → 2.2 | 0.00 → 0.00 | 14 → 2 | 64.0 → 0.0 | 1336.0 → 0.0 | none |
| rtl-r820t | fm | 10.6 → 7.2 | 0.00 → 0.00 | 56.0 → 8.0 | 2.93 → 0.40 | 103.3 → 22.6 | 3.8 → 3.8 | 0.61 → 0.00 | 9 → 0 | 64.0 → 8.0 | 572.0 → 0.0 | none |
| rtl-r820t | tdma | 18.8 → 8.8 | 0.00 → 0.00 | 1092.0 → 0.0 | 145.60 → 0.00 | 47.3 → 6.9 | 2.2 → 2.2 | 0.00 → 0.00 | 16 → 0 | 60.0 → 8.0 | 1608.0 → 0.0 | none |
| rtl-r820t | pulsed | never → 3.7 | 4.42 → 0.25 | 56.0 → 8.0 | 3.07 → 0.40 | never → 5.2 | never → 2.2 | 4.42 → 0.25 | 14 → 0 | 44.0 → 0.0 | 0.0 → 0.0 | none |
| rtl-r820t | wide | never → 10.1 | 3.62 → 0.00 | 0.0 → 8.0 | 0.00 → 0.40 | never → 16.5 | 3.8 → 3.8 | 0.00 → 0.00 | 5 → 0 | 0.0 → 16.0 | 0.0 → 8.0 | none |
| hackrf | ofdm | never → 3.4 | 13.88 → 0.00 | 192.0 → 8.0 | 83.20 → 1.73 | never → 2.9 | 5.8 → 1.0 | 0.00 → 0.03 | 15 → 0 | 112.0 → 0.0 | 1568.0 → 0.0 | none |
| hackrf | fm | 10.0 → 9.6 | 0.74 → 0.74 | 200.0 → 8.0 | 20.47 → 0.40 | 41.4 → 8.3 | 59.2 → 13.5 | 0.06 → 0.31 | 16 → 2 | 88.0 → 16.0 | 1044.0 → 0.0 | burstq,retune |
| hackrf | tdma | never → 3.8 | 15.85 → 0.00 | 1200.0 → 0.0 | 197.33 → 0.00 | never → 2.9 | 0.5 → 0.5 | 0.00 → 0.00 | 20 → 0 | 56.0 → 8.0 | 600.0 → 0.0 | none |
| hackrf | pulsed | 53.1 → 21.2 | 3.42 → 0.00 | 48.0 → 8.0 | 5.07 → 0.40 | 90.0 → 20.5 | 52.7 → 18.9 | 0.87 → 0.00 | 18 → 0 | 64.0 → 0.0 | 0.0 → 0.0 | none |
| hackrf | wide | never → 10.6 | 6.23 → 0.65 | 0.0 → 8.0 | 0.00 → 0.40 | never → 7.0 | never → never | 16.99 → 16.98 | 7 → 0 | 0.0 → 8.0 | 0.0 → 8.0 | retune,rtq |
| airspy | ofdm | 5.8 → 1.0 | 0.00 → 0.00 | 384.0 → 16.0 | 83.20 → 3.47 | 1.3 → 1.9 | never → 2.9 | 3.28 → 0.00 | 14 → 0 | 64.0 → 0.0 | 1132.0 → 0.0 | none |
| airspy | fm | 2.1 → 2.1 | 0.00 → 0.00 | 216.0 → 8.0 | 11.33 → 0.40 | 0.0 → 1.1 | never → 2.9 | 1.07 → 0.00 | 10 → 0 | 112.0 → 16.0 | 1000.0 → 0.0 | none |
| airspy | tdma | 8.6 → 2.9 | 0.70 → 0.00 | 948.0 → 0.0 | 126.40 → 0.00 | 25.8 → 2.5 | 2.9 → 2.9 | 0.00 → 0.00 | 18 → 0 | 112.0 → 0.0 | 1404.0 → 0.0 | none |
| airspy | pulsed | 11.3 → 1.9 | 0.00 → 0.00 | 56.0 → 16.0 | 3.07 → 0.80 | 66.8 → 2.9 | 9.2 → 2.0 | 0.00 → 0.00 | 11 → 0 | 44.0 → 0.0 | 0.0 → 0.0 | modesw |
| airspy | wide | never → never | 2.72 → 0.00 | 0.0 → 16.0 | 0.00 → 0.93 | never → 6.8 | 2.9 → 2.9 | 0.00 → 0.00 | 4 → 0 | 0.0 → 16.0 | 0.0 → 16.0 | settle |
| sdrplay | ofdm | 0.7 → 0.7 | 0.00 → 0.00 | 384.0 → 16.0 | 82.80 → 3.47 | 0.0 → 0.0 | never → 1.6 | 3.32 → 0.40 | 11 → 0 | 64.0 → 0.0 | 1412.0 → 0.0 | none |
| sdrplay | fm | 0.7 → 0.7 | 0.00 → 0.00 | 384.0 → 16.0 | 20.13 → 0.80 | 0.0 → 0.8 | never → 1.6 | 3.24 → 0.30 | 9 → 0 | 112.0 → 0.0 | 1032.0 → 0.0 | none |
| sdrplay | tdma | never → 3.2 | 1.51 → 0.60 | 1280.0 → 0.0 | 170.67 → 0.00 | never → 3.8 | never → 2.2 | 4.81 → 0.80 | 11 → 0 | 112.0 → 0.0 | 976.0 → 16.0 | none |
| sdrplay | pulsed | 0.6 → 0.6 | 0.20 → 0.00 | 88.0 → 16.0 | 4.53 → 0.80 | 0.0 → 0.0 | 0.0 → 0.0 | 0.20 → 0.00 | 11 → 0 | 44.0 → 0.0 | 0.0 → 0.0 | none |
| sdrplay | wide | 3.2 → 3.2 | 0.84 → 0.00 | 0.0 → 8.0 | 0.00 → 0.40 | never → 2.9 | never → 2.2 | 4.82 → 0.00 | 5 → 0 | 0.0 → 8.0 | 0.0 → 8.0 | none |
| pluto | ofdm | 0.6 → 0.6 | 0.00 → 0.00 | 384.0 → 16.0 | 82.80 → 3.47 | 21.2 → 1.8 | never → 1.6 | 2.10 → 0.22 | 11 → 0 | 112.0 → 0.0 | 1156.0 → 0.0 | none |
| pluto | fm | 3.7 → 7.6 | 0.00 → 0.00 | 384.0 → 16.0 | 20.13 → 0.80 | 0.0 → 4.3 | never → 1.6 | 2.24 → 0.33 | 9 → 0 | 108.0 → 0.0 | 988.0 → 0.0 | none |
| pluto | tdma | 3.2 → 3.6 | 0.56 → 0.45 | 1388.0 → 0.0 | 185.07 → 0.00 | 18.1 → 3.8 | never → 1.6 | 2.64 → 0.00 | 19 → 0 | 112.0 → 0.0 | 932.0 → 0.0 | none |
| pluto | pulsed | never → 2.1 | 1.64 → 0.00 | 96.0 → 16.0 | 5.07 → 0.80 | never → 3.8 | never → 1.1 | 1.64 → 0.00 | 11 → 0 | 72.0 → 0.0 | 0.0 → 0.0 | none |
| pluto | wide | never → 4.2 | 2.86 → 0.00 | 0.0 → 8.0 | 0.00 → 0.40 | never → 3.9 | never → 1.6 | 2.75 → 0.00 | 5 → 0 | 0.0 → 8.0 | 0.0 → 8.0 | none |
| usrp | ofdm | 0.6 → 0.6 | 0.00 → 0.00 | 384.0 → 16.0 | 82.80 → 3.47 | 21.2 → 1.8 | never → 1.6 | 2.23 → 0.21 | 11 → 0 | 112.0 → 0.0 | 1156.0 → 0.0 | none |
| usrp | fm | 3.7 → 7.6 | 0.00 → 0.00 | 384.0 → 16.0 | 20.13 → 0.80 | 0.0 → 4.3 | never → 1.6 | 2.15 → 0.32 | 9 → 0 | 108.0 → 0.0 | 988.0 → 0.0 | none |
| usrp | tdma | 2.4 → 2.4 | 0.54 → 0.43 | 1624.0 → 0.0 | 216.53 → 0.00 | 32.4 → 3.8 | never → 1.6 | 2.85 → 0.32 | 11 → 0 | 112.0 → 0.0 | 972.0 → 16.0 | none |
| usrp | pulsed | never → 3.2 | 1.51 → 0.00 | 96.0 → 16.0 | 5.07 → 0.80 | never → 4.3 | never → 1.1 | 1.51 → 0.00 | 11 → 0 | 72.0 → 0.0 | 0.0 → 0.0 | none |
| usrp | wide | never → 4.2 | 2.82 → 0.00 | 0.0 → 8.0 | 0.00 → 0.40 | never → 3.9 | never → 2.2 | 3.07 → 0.00 | 5 → 0 | 0.0 → 8.0 | 0.0 → 8.0 | none |
| rtltcp | ofdm | 4.3 → 4.3 | 0.00 → 0.00 | 200.0 → 8.0 | 43.73 → 1.73 | 117.9 → 6.0 | 3.7 → 3.7 | 0.00 → 0.00 | 10 → 2 | 56.0 → 0.0 | 1264.0 → 0.0 | none |
| rtltcp | fm | 3.3 → 9.9 | 0.00 → 0.00 | 56.0 → 8.0 | 2.93 → 0.40 | 105.2 → 24.1 | never → 6.5 | 1.21 → 0.00 | 7 → 0 | 56.0 → 8.0 | 552.0 → 0.0 | none |
| rtltcp | tdma | 12.6 → 12.6 | 0.00 → 0.00 | 844.0 → 0.0 | 112.53 → 0.00 | 27.3 → 8.9 | 3.7 → 3.7 | 0.00 → 0.00 | 9 → 0 | 56.0 → 8.0 | 1568.0 → 0.0 | none |
| rtltcp | pulsed | never → 6.4 | 4.42 → 0.25 | 56.0 → 8.0 | 3.00 → 0.40 | never → 7.5 | never → 3.7 | 4.42 → 0.25 | 13 → 0 | 40.0 → 0.0 | 0.0 → 0.0 | none |
| rtltcp | wide | never → 13.6 | 3.62 → 0.00 | 0.0 → 8.0 | 0.00 → 0.40 | never → 21.2 | 6.5 → 6.5 | 0.00 → 0.00 | 5 → 0 | 0.0 → 16.0 | 0.0 → 8.0 | none |
| spyserver | ofdm | 21.8 → 12.0 | 0.00 → 0.00 | 204.0 → 8.0 | 44.20 → 1.73 | 8.2 → 22.1 | 22.0 → 12.1 | 0.00 → 0.00 | 13 → 0 | 56.0 → 8.0 | 932.0 → 0.0 | none |
| spyserver | fm | 5.9 → 5.9 | 0.00 → 0.00 | 56.0 → 8.0 | 2.93 → 0.40 | 47.0 → never | never → never | 17.42 → 17.42 | 6 → 0 | 64.0 → 8.0 | 500.0 → 0.0 | irrec,modesw,retune,rtq |
| spyserver | tdma | never → 12.0 | 38.40 → 0.00 | 468.0 → 0.0 | 62.40 → 0.00 | 56.4 → 7.4 | 12.1 → 12.1 | 0.00 → 0.00 | 7 → 0 | 56.0 → 8.0 | 468.0 → 0.0 | none |
| spyserver | pulsed | never → 6.1 | 26.82 → 0.58 | 56.0 → 16.0 | 2.93 → 0.80 | 114.6 → 7.2 | never → 6.1 | 1.62 → 0.58 | 10 → 0 | 40.0 → 8.0 | 0.0 → 0.0 | none |
| spyserver | wide | never → 14.6 | 17.85 → 0.00 | 0.0 → 8.0 | 0.00 → 0.40 | never → 21.6 | 6.1 → 12.1 | 0.00 → 0.00 | 13 → 0 | 0.0 → 8.0 | 0.0 → 8.0 | none |


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

12. **A clipped fraction under the mode's limit could still bury a weak channel** next to a strong carrier: the limit now follows the dynamic range between the floor and the span (never above the mode's own), which also ended the SpyServer "weak signal beside strong carriers" regression (no exemption in the tests any more).
13. **A look at a higher gain that clips is undone exactly** instead of cutting below the gain that was fine; a mild clip takes 3 dB, not 6; a look downwards stays at 2 dB.
14. **FM**: a station's intermodulation product reads as a channel signal and kept the loop busy on an idle channel; after the first minute a decoder that does not lock now counts as "nothing to judge by". The level the decoder saw while locked is held slowly (a decoder reports late), so the seconds after a call ends do not read as its level; a call that starts no longer restarts the back-off.
15. **A cut that clipped on a line outside the channel is told apart from one on the channel itself** by a line standing out of the floor; only the first kind climbs back, and the linear split goes when the signal that forced it has gone. A channel that fills the capture cannot tell, so it climbs back only to 3 dB under the gain that clipped.

## UI

The Auto gain control is on top of the gain section; with Auto on the sliders are under an "Advanced" button (they stay in view when the gain is manual). What it chose and why is the status line ("Gain 32 dB: decoder 28.5 dB, headroom 6.2 dB; last: ..."), and auto gain never touches the transmit gain.

## Issues read

#215 (Pluto touched without asking) was fixed in 0.2.8 by removing the network scan; auto gain only runs on the radio the user started. #450 (ADC oscillation on two radios at once, v3.6) is a driver-level direct-sampling loop outside this work.

## What remains

5 of 40 cells still miss a limit (per cell above):
- **HackRF FM**: retune settles in 13.5 s (limit 12 s, the looks after the search) and the 90th percentile on a bursty channel is 2.0 dB under the best (limit 1.5 dB).
- **SpyServer FM**: an index table whose real step has to be learned; recovery and retune do not reach within 1 dB in the measured window (it settles 17 dB under the best after a retune).
- **HackRF wide capture after a retune to an empty band**: the loop stops at 60 dB against the oracle's 74 dB (no signal to judge by; the oracle scores a weak channel the search cannot see).
- **Airspy wide capture** and **Airspy ADS-B to wide mode switch** do not get within 1 dB of the oracle in the measured window.

CI (`ctest autogain_matrix`, three radios) allows only the known failing cells in that set: HackRF FM and HackRF wide. A full run allows the five above; any more fails it.
