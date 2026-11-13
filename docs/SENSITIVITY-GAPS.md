# Sensitivity gaps, 4.1

Measured threshold of each mode (the QA sweep, `tools/sens/qa_sens.py`: the lowest SNR in the test signal's 99 % band at which every trial
counts at least half the clean run's checked units, 2 dB steps) against an ideal receiver of the same modulation, rate and unit length
(`tools/sens/gap_table.py`, which writes the table below). "measured" is `tests/sens/qa41`, "after" the sens2 and sens3 sweeps in
`tests/sens/gaps41`. Regenerate with:

    python3 tools/sens/gap_table.py --after tests/sens/gaps41/after-feat41-sens2.jsonl tests/sens/gaps41/after-feat41-sens3.jsonl

## sens3 (4.1): what changed

Before / after on the same runner and tool, 4 trials a point (before: integ-41 at 50f14876d, run 43511253443; e-Czas before: 60 s trials on
the receiver of integ-41, run 43511467794; after: run 43519850093). dB in the 99 % band; false = units counted on noise alone / noise trials.

| mode | awgn | +2 kHz, +50 ppm | off-centre, -50 ppm | false | CPU |
|---|---|---|---|---|---|
| NGHam | 12 -> 10 | 12 -> 10 | 12 -> 12 | 0/4 -> 0/4 | +9..14 % (cubesat receiver) |
| BEESAT (Mobitex) | 12 -> 12 (at 10 dB 11 of 12 trials pass, 1 of 12 before) | 12 -> 10 | 12 -> 10 | 0/4 -> 0/4 | same receiver |
| AX100 Mode 5 | 10 -> 8 | 10 -> 8 | 10 -> 8 | 0/4 -> 0/8 | same receiver |
| AX100 Mode 6 | 14 -> 12 | 14 -> 12 | 14 -> 12 | 0/4 -> 0/4 | same receiver |
| USP | 10 -> 8 | 10 -> 8 | 10 -> 8 | 0/4 -> 0/8 | same receiver |
| CubeSat (all framings) | 12 -> 10 | 12 -> 10 | 12 -> 10 | 0/4 -> 0/4 | same receiver |
| U482C | 14 -> 14 | 14 -> 14 | 14 -> 14 | 0/4 -> 0/4 | same receiver |
| e-Czas | 30 (8 s trials) / 18 (60 s) -> 16 | - -> - | - -> - | 0/4 -> 0/4 | a few operations per 500 Hz tick |
| VDL Mode 4 | 16 -> 14 | 18 -> 14 | - -> - | 0/4 -> 0/4 | QA wall time +6 %, vdl4_rx CPU check passes |

- CubeSat (NGHam, BEESAT, AX100, USP): `core/src/cubesat/cubesat_rx.cpp`. A 1-bit differential detector (arg of y[n] y*[n-T], the products
  averaged over half a bit) on the 9600 and 4800 baud narrow channels beside the discriminator paths: the phase over a bit without FM clicks,
  so no FM threshold. The AFC's residual comes from the discriminator path's slicer midpoint. AX100 Mode 6 is taken on these paths only with
  at most 8 byte errors put right. One Mobitex frame that several demodulators report with different lost blocks now counts once (the copy
  with more data); it was counted two or three times (the clean BEESAT trial counted 9 units for 3 frames). `DECT2_CUBESAT_NODIFF=1` turns
  the detectors off. U482C (audio FSK) is unchanged.
- e-Czas: the QA sweep runs 60 s trials for it (the time packet comes every 12 s; an 8 s trial held one or none, so the old 30 dB was the
  latency). `core/src/eczas/eczas_live.cpp`, `eczas_rx.cpp`: a coherent detector beside the phase-step one: the filtered carrier against a
  decision-directed reference at the midpoint of the two keying phases gives the keying state, so each bit comes from one phase estimate
  instead of the difference of two. Its own slicer; a time packet both give counts once. `DECT2_ECZAS_NOCOH=1` turns it off. The +2 kHz and
  off-centre scenarios are out of the e-Czas frequency search (+-250 Hz) before and after.
- VDL Mode 4: `core/src/vdl4/vdl4_rx.cpp`. The four one-pole low-passes (-14 dB at 25 kHz, -32 dB at 50 kHz) are a linear-phase Gaussian FIR
  (-3 dB at 11 kHz, -19 dB at 25 kHz, -75 dB at 50 kHz): the clean test signal decoded every burst on its neighbours too, so the old
  reference counted 18 frames for 11 bursts. The bits are a 1-bit differential detector with the carrier error taken out in the phase
  domain, and the burst is aligned to the sample by fitting the 16 ramp bits and the 24 training bits (a fit that took in ramp bits moved the
  threshold). A frame two channels decode counts once, on the stronger. The test's 25 kHz threshold: 18 / 20 / 18 dB at 0 / +-2.5 kHz (20 to
  22 before). The off-centre scenario moves the six fixed channels by 100 kHz: no channel is listened to there, before or after.
- Tried and not kept: nothing reverted in sens3; every change above gained at least 1 dB somewhere and lost nothing.

Still open: VDL4 (8 dB over the orthogonal-FSK bound; that bound is optimistic for h 0.5 BT 0.28, a coherent or multi-bit detector is the
next step), BEESAT (about 2 to 4 dB), G-TOR (energy detector on a 170 Hz shift at 300 Bd, h 0.57: not orthogonal; a sequence or
differential detector is the next step), TETRA and HF packet 300 (latency-limited rows: longer trials first).

## The table

| mode | measured (dB in B99) | after | theory | gap (dB) | gap after | basis | units per trial | false (units / trials) | model / class |
|---|---|---|---|---|---|---|---|---|---|
| eczas | 30.0 | 16.0 | 11.7 | +18.3 | +4.3 | U | 1 | 0 / 2 | two phase states 36 degrees apart, 50 b/s, on a constant carrier (the carrier counts in the SNR); coherent bound (the step detector: +2.9 dB; 4.1 sens3 adds a coherent detector; the 'after' figure is from 60 s trials, the 30 dB before was the latency of 8 s ones: 18 dB with 60 s); 1-2 units a trial: coarse; first unit at the end of the trial: latency-limited |
| vdl4 | 20.0 | 14.0 | 6.0 | +14.0 | +8.0 | U | 12 | 0 / 2 | GFSK 19.2 kb/s (h 0.5, BT 0.28), HDLC burst; the orthogonal-FSK bound is optimistic for h 0.5 BT 0.28 (a 1-bit differential detector needs several dB more); the 'before' 20 dB counted neighbour-channel copies (18 frames for 11 bursts) |
| tetra | 26.0 | - | 12.1 | +13.9 | - | U | 131 | 0 / 2 | pi/4-DQPSK 36 kb/s, coded blocks (RCPC 2/3 + interleaving): the true limit is lower; first unit at the end of the trial: latency-limited |
| gtor | 14.0 | - | 5.1 | +8.9 | - | U | 150 | 0 / 2 | FSK 300 Bd, 72-byte frame, CRC only (Golay parity frames are separate); 170 Hz shift is not orthogonal at 300 Bd (about 2 dB more); first unit at the end of the trial: latency-limited |
| ngham | 12.0 | 10.0 | 7.3 | +4.7 | +2.7 | C | 8 | 0 / 2 | FSK 4800, RS hard |
| hfpacket300 | 8.0 | - | 3.5 | +4.5 | - | U | 4 | 0 / 4 | AFSK 300 Bd HDLC; first unit at the end of the trial: latency-limited |
| beesat | 12.0 | 12.0 | 8.2 | +3.8 | +3.8 | C | 7 | 0 / 2 | GMSK 4800, Mobitex (12,8) Hamming + interleave: channel BER about 1e-2 |
| ax100m6 | 14.0 | 12.0 | 11.6 | +2.4 | +0.4 | U | 9 | 0 / 2 | G3RUH 9600, HDLC around the RS block: one bit error breaks the HDLC frame |
| cubesat | 12.0 | 10.0 | 9.6 | +2.4 | +0.4 | U | 15 | 0 / 2 | GMSK / G3RUH 9600 (mix of framings) |
| usp | 10.0 | 8.0 | 7.8 | +2.2 | +0.2 | C | 6 | 0 / 2 | FSK 9600, RS(255,223) hard |
| ax100m5 | 10.0 | 8.0 | 8.2 | +1.8 | -0.2 | C | 12 | 0 / 2 | GMSK 9600, RS(255,223) hard: channel BER 1.5e-2 |
| dsc | -2.0 | - | -2.8 | +0.8 | - | U | 1 | 0 / 8 | FSK 100 Bd, 10-bit symbols; 1-2 units a trial: coarse; first unit at the end of the trial: latency-limited |
| tetrapol | 10.0 | - | 9.4 | +0.6 | - | U | 398 | 0 / 2 | GMSK 8000; first unit at the end of the trial: latency-limited |
| uat | 8.0 | - | 7.8 | +0.2 | - | C | 32 | 0 / 4 | CPFSK 1.04 Mb/s, RS(48,34) t=7: channel BER about 2e-2; first unit at the end of the trial: latency-limited |
| dstar | 8.0 | - | 7.9 | +0.1 | - | U | 329 | 0 / 4 | GMSK 4800, 96-bit voice frame |
| rtty | -8.0 | - | -7.7 | -0.3 | - | U | 43 | 0 / 12 | FSK 45.45 Bd, characters |
| sigfox | -2.0 | - | -1.6 | -0.4 | - | U | 3 | 0 / 8 | DBPSK 100 b/s |
| ais | 8.0 | - | 8.7 | -0.7 | - | U | 20 | 0 / 4 | GMSK 9600, HDLC frame |
| sitor | -4.0 | - | -3.2 | -0.8 | - | U | 92 | 0 / 10 | FSK 100 Bd, 7-bit characters |
| nrf24 | 10.0 | - | 11.8 | -1.8 | - | U | 2303 | 0 / 2 | GFSK 1 Mb/s |
| sarsat | -4.0 | - | -2.2 | -1.8 | - | U | 2 | 0 / 10 | biphase PSK +-1.1 rad, 400 b/s, BCH (the carrier takes 21 % of the power: +1 dB); 1-2 units a trial: coarse |
| pocsag | 0.0 | - | 2.3 | -2.3 | - | U | 80 | 0 / 10 | FSK 1200, BCH(31,21) t=2 per word: the true limit is lower; negative gap: the model does not fit the test signal (shorter unit, code, 50 % pass) |
| argos | -4.0 | - | -1.3 | -2.7 | - | U | 3 | 0 / 10 | biphase PSK +-1.1 rad, 400 b/s (carrier +1 dB); negative gap: the model does not fit the test signal (shorter unit, code, 50 % pass) |
| vdl3 | 14.0 | - | 17.6 | -3.6 | - | U | 507 | 0 / 2 | D8PSK 10.5 kBd; negative gap: the model does not fit the test signal (shorter unit, code, 50 % pass) |
| ant | 8.0 | - | 11.7 | -3.7 | - | U | 98 | 0 / 4 | GFSK 1 Mb/s; negative gap: the model does not fit the test signal (shorter unit, code, 50 % pass) |
| dect | 2.0 | - | 7.1 | -5.1 | - | U | 3197 | 0 / 6 | GFSK 1.152 Mb/s, A-field + CRC; negative gap: the model does not fit the test signal (shorter unit, code, 50 % pass) |
| ble | 4.0 | - | 11.4 | -7.4 | - | U | 2286 | 0 / 6 | GFSK 1 Mb/s (h 0.5) on a discriminator; negative gap: the model does not fit the test signal (shorter unit, code, 50 % pass) |
| acars | -6.0 | - | - | - | - | - | 20 | 0 / 10 | data on an FM or AM audio channel: limited by the FM threshold, not a bit-error formula |
| adsb | 14.0 | - | - | - | - | - | 620 | 0 / 2 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| adsl | -6.0 | - | - | - | - | - | 34 | 0 / 10 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| aero | -12.0 | - | - | - | - | - | 252 | 0 / 14 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| aeromacs | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| afsk | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| airband | -10.0 | - | - | - | - | - | 5 | 0 / 12 | analog or squelch count (calls, tones, audio lines): no bit error theory |
| ale | -6.0 | - | - | - | - | - | 15 | 0 / 10 | no closed-form model here (coded, multi-carrier or PHY not modelled); first unit at the end of the trial: latency-limited |
| ale3g | -10.0 | - | - | - | - | - | 6 | 0 / 12 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| am | 8.0 | - | - | - | - | - | 799 | 0 / 4 | analog or squelch count (calls, tones, audio lines): no bit error theory |
| ammon | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| ardop | -22.0 | - | - | - | - | - | 4 | 0 / 18 | no closed-form model here (coded, multi-carrier or PHY not modelled); first unit at the end of the trial: latency-limited |
| arktikaggak | 8.0 | - | - | - | - | - | 11 | 0 / 4 | no closed-form model here (coded, multi-carrier or PHY not modelled); first unit at the end of the trial: latency-limited |
| arktikardas | -2.0 | - | - | - | - | - | 3017 | 0 / 8 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| arqe | -2.0 | - | - | - | - | - | 252 | 0 / 8 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| arqn | -2.0 | - | - | - | - | - | 266 | 0 / 8 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| asterix | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| astro | - | - | - | - | - | - | 1 | 16 / 16 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| atis | 12.0 | - | - | - | - | - | 3 | 0 / 2 | data on an FM or AM audio channel: limited by the FM threshold, not a bit-error formula |
| autospec | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| awsl | 4.0 | - | - | - | - | - | 1162 | 0 / 6 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| beacon | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| beaconevents | - | - | - | - | - | - | 1 | 16 / 16 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| bgan | 2.0 | - | - | - | - | - | 880 | 0 / 8 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| birdie | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| bletracker | 6.0 | - | - | - | - | - | 1312 | 0 / 4 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| btc | -4.0 | - | - | - | - | - | 9 | 0 / 10 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| burst | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| callisto | - | - | - | - | - | - | 1 | 16 / 16 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| cb | 16.0 | - | - | - | - | - | 4 | 0 / 2 | analog or squelch count (calls, tones, audio lines): no bit error theory |
| cdma2000 | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| cdr | 6.0 | - | - | - | - | - | 120 | 0 / 4 | OFDM / concatenated broadcast and satellite links: see SENSITIVITY-4.1-TV.md and the X-band rows of SENSITIVITY-4.1-ALL.md |
| cis12 | 6.0 | - | - | - | - | - | 838 | 0 / 4 | no closed-form model here (coded, multi-carrier or PHY not modelled); first unit at the end of the trial: latency-limited |
| cis128 | - | - | - | - | - | - | 1 | 0 / 4 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| cis14 | -4.0 | - | - | - | - | - | 24 | 0 / 12 | no closed-form model here (coded, multi-carrier or PHY not modelled); first unit at the end of the trial: latency-limited |
| cis3000 | - | - | - | - | - | - | 1 | 0 / 2 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| cis36 | - | - | - | - | - | - | 1 | 0 / 10 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| cisbee | -4.0 | - | - | - | - | - | 8 | 0 / 10 | no closed-form model here (coded, multi-carrier or PHY not modelled); first unit at the end of the trial: latency-limited |
| cishdr | - | - | - | - | - | - | 1 | 0 / 8 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| codar | -6.0 | - | - | - | - | - | 268 | 0 / 10 | analog or squelch count (calls, tones, audio lines): no bit error theory |
| contestia | -10.0 | - | - | - | - | - | 19 | 0 / 12 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| coquelet | -6.0 | - | - | - | - | - | 16 | 0 / 10 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| cp16 | -12.0 | - | - | - | - | - | 1 | 0 / 16 | no closed-form model here (coded, multi-carrier or PHY not modelled); 1-2 units a trial: coarse |
| cquam | 10.0 | - | - | - | - | - | 14 | 0 / 2 | analog or squelch count (calls, tones, audio lines): no bit error theory |
| crowd36 | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| css | - | - | - | - | - | - | 1 | 0 / 16 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| cv2x | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| cw | 4.0 | - | - | - | - | - | 334 | 0 / 6 | analog or squelch count (calls, tones, audio lines): no bit error theory |
| cwkeying | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| datawell | - | - | - | - | - | - | 1 | 0 / 12 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| dcr | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| dectnrp | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| dfcw | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| dgps | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| dme | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| dmr | 6.0 | - | - | - | - | - | 407 | 0 / 4 | 4FSK on a discriminator with voice FEC (no closed form here; DMR at 6 dB is the reference); first unit at the end of the trial: latency-limited |
| dominoex | -8.0 | - | - | - | - | - | 42 | 0 / 12 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| dopplerlog | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| dpmr | 8.0 | - | - | - | - | - | 60 | 0 / 4 | 4FSK on a discriminator with voice FEC (no closed form here; DMR at 6 dB is the reference) |
| drm | 16.0 | - | - | - | - | - | 110 | 0 / 2 | OFDM / concatenated broadcast and satellite links: see SENSITIVITY-4.1-TV.md and the X-band rows of SENSITIVITY-4.1-ALL.md |
| dsrc | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| ebicab | 10.0 | - | - | - | - | - | 462 | 0 / 2 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| edacs | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| efr | 2.0 | - | - | - | - | - | 102 | 0 / 6 | data on an FM or AM audio channel: limited by the FM threshold, not a bit-error formula; first unit at the end of the trial: latency-limited |
| elektro | -2.0 | - | - | - | - | - | 321 | 0 / 8 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| elektrordas | -2.0 | - | - | - | - | - | 3017 | 0 / 8 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| eloran | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| elrs | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| elt121 | 6.0 | - | - | - | - | - | 22 | 0 / 4 | analog or squelch count (calls, tones, audio lines): no bit error theory |
| em4100 | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| emeecho | - | - | - | - | - | - | 1 | 16 / 16 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| eot | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| ermes | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| erminaz | - | - | - | - | - | - | 1 | 16 / 16 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| esubghz | 12.0 | - | - | - | - | - | 11 | 0 / 2 | analog or squelch count (calls, tones, audio lines): no bit error theory |
| evm | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| f01 | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| f06 | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| fading | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| fanet | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| fbr | 2.0 | - | - | - | - | - | 384 | 0 / 6 | no closed-form model here (coded, multi-carrier or PHY not modelled); first unit at the end of the trial: latency-limited |
| flarm | 2.0 | - | - | - | - | - | 24 | 0 / 8 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| flex | 2.0 | - | - | - | - | - | 440 | 0 / 8 | data on an FM or AM audio channel: limited by the FM threshold, not a bit-error formula |
| flysky | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| fm | 18.0 | - | - | - | - | - | 89 | 0 / 2 | analog or squelch count (calls, tones, audio lines): no bit error theory |
| fmmon | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| freedv | 8.0 | - | - | - | - | - | 39 | 0 / 4 | OFDM / concatenated broadcast and satellite links: see SENSITIVITY-4.1-TV.md and the X-band rows of SENSITIVITY-4.1-ALL.md |
| freqacc | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| frs | 24.0 | - | - | - | - | - | 3 | 0 / 2 | analog or squelch count (calls, tones, audio lines): no bit error theory |
| frsky | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| fsk441 | -24.0 | - | - | - | - | - | 1 | 0 / 16 | no closed-form model here (coded, multi-carrier or PHY not modelled); 1-2 units a trial: coarse; first unit at the end of the trial: latency-limited |
| fskan | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| fskmodem | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| fsq | -24.0 | - | - | - | - | - | 8 | 0 / 18 | no closed-form model here (coded, multi-carrier or PHY not modelled); first unit at the end of the trial: latency-limited |
| fst4 | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| ft2 | 46.0 | - | - | - | - | - | 9 | 0 / 2 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| ft4 | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| ft8 | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| fusion | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| fy2svissr | -4.0 | - | - | - | - | - | 14 | 0 / 12 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| fy3ahrpt | 2.0 | - | - | - | - | - | 1996 | 0 / 6 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| fy3mpt | 6.0 | - | - | - | - | - | 3986 | 0 / 4 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| fy4ahrit | 6.0 | - | - | - | - | - | 235 | 0 / 4 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| fy4lrit | 0.0 | - | - | - | - | - | 20 | 0 / 8 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| gbas | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| general | 8.0 | - | - | - | - | - | 799 | 0 / 4 | analog or squelch count (calls, tones, audio lines): no bit error theory |
| geonetcast | 10.0 | - | - | - | - | - | 224 | 0 / 2 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| geoscan | - | - | - | - | - | - | 1 | 16 / 16 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| gk2acdas | 8.0 | - | - | - | - | - | 3001 | 0 / 4 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| gk2ahrit | 2.0 | - | - | - | - | - | 1460 | 0 / 6 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| gk2alrit | -2.0 | - | - | - | - | - | 59 | 0 / 8 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| gk2auhrit | 14.0 | - | - | - | - | - | 1545 | 0 / 2 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| glitch | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| globalstar | -6.0 | - | - | - | - | - | 5 | 0 / 12 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| gmr1 | -12.0 | - | - | - | - | - | 5 | 0 / 16 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| gnss | -6.0 | - | - | - | - | - | 1 | 0 / 10 | no closed-form model here (coded, multi-carrier or PHY not modelled); 1-2 units a trial: coarse |
| gnssjam | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| goes | -2.0 | - | - | - | - | - | 450 | 0 / 8 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| goescda | 4.0 | - | - | - | - | - | 150 | 0 / 6 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| goesdcs | -24.0 | - | - | - | - | - | 3 | 0 / 16 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| goesgrb | 6.0 | - | - | - | - | - | 1357 | 0 / 4 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| goesgvar | 0.0 | - | - | - | - | - | 52 | 0 / 8 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| goesmdl | 4.0 | - | - | - | - | - | 857 | 647 / 6 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| gonets | 20.0 | - | - | - | - | - | 153 | 0 / 2 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| graves | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| gsmr | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| gwfsk | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| halow | 0.0 | - | - | - | - | - | 1127 | 0 / 10 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| hamdrm | - | - | - | - | - | - | 1551 | 0 / 2 | OFDM / concatenated broadcast and satellite links: see SENSITIVITY-4.1-TV.md and the X-band rows of SENSITIVITY-4.1-ALL.md |
| hamfm | 22.0 | - | - | - | - | - | 3 | 0 / 2 | analog or squelch count (calls, tones, audio lines): no bit error theory |
| hdr | 22.0 | - | - | - | - | - | 4 | 0 / 2 | OFDM / concatenated broadcast and satellite links: see SENSITIVITY-4.1-TV.md and the X-band rows of SENSITIVITY-4.1-ALL.md |
| hell | -4.0 | - | - | - | - | - | 137 | 0 / 10 | analog or squelch count (calls, tones, audio lines): no bit error theory |
| hfbeacon | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| hfdig | -8.0 | - | - | - | - | - | 43 | 0 / 12 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| hfdl | -10.0 | - | - | - | - | - | 9 | 0 / 12 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| hfvoice | 12.0 | - | - | - | - | - | 1 | 0 / 2 | analog or squelch count (calls, tones, audio lines): no bit error theory; 1-2 units a trial: coarse; first unit at the end of the trial: latency-limited |
| himawaricast | 4.0 | - | - | - | - | - | 473 | 0 / 6 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| hop | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| horus | -4.0 | - | - | - | - | - | 1 | 0 / 12 | no closed-form model here (coded, multi-carrier or PHY not modelled); 1-2 units a trial: coarse; first unit at the end of the trial: latency-limited |
| hrdcp | 4.0 | - | - | - | - | - | 1 | 0 / 6 | no closed-form model here (coded, multi-carrier or PHY not modelled); 1-2 units a trial: coarse; first unit at the end of the trial: latency-limited |
| hrpt | 2.0 | - | - | - | - | - | 1845 | 0 / 6 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| ifkp | -24.0 | - | - | - | - | - | 19 | 0 / 16 | no closed-form model here (coded, multi-carrier or PHY not modelled); first unit at the end of the trial: latency-limited |
| ils | 16.0 | - | - | - | - | - | 30 | 0 / 2 | analog or squelch count (calls, tones, audio lines): no bit error theory |
| inmc | -24.0 | - | - | - | - | - | 2 | 0 / 16 | no closed-form model here (coded, multi-carrier or PHY not modelled); 1-2 units a trial: coarse |
| inmsat | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| inputhealth | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| intermod | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| iono | - | - | - | - | - | - | 1 | 0 / 16 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| iridium | -14.0 | - | - | - | - | - | 95 | 0 / 14 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| iscat | -10.0 | - | - | - | - | - | 1 | 0 / 12 | no closed-form model here (coded, multi-carrier or PHY not modelled); 1-2 units a trial: coarse |
| isdbtsb | 18.0 | - | - | - | - | - | 13884 | 0 / 2 | OFDM / concatenated broadcast and satellite links: see SENSITIVITY-4.1-TV.md and the X-band rows of SENSITIVITY-4.1-ALL.md |
| ism | 10.0 | - | - | - | - | - | 14 | 0 / 2 | analog or squelch count (calls, tones, audio lines): no bit error theory |
| issdown | - | - | - | - | - | - | 1 | 0 / 16 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| its | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| jove | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| js8 | -18.0 | - | - | - | - | - | 3 | 0 / 16 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| jt4 | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| jt65 | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| jt6m | -24.0 | - | - | - | - | - | 1 | 0 / 16 | no closed-form model here (coded, multi-carrier or PHY not modelled); 1-2 units a trial: coarse; first unit at the end of the trial: latency-limited |
| jt9 | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| kaal | 14.0 | - | - | - | - | - | 1264 | 0 / 2 | OFDM / concatenated broadcast and satellite links: see SENSITIVITY-4.1-TV.md and the X-band rows of SENSITIVITY-4.1-ALL.md |
| kaft | 2.0 | - | - | - | - | - | 5 | 0 / 6 | OFDM / concatenated broadcast and satellite links: see SENSITIVITY-4.1-TV.md and the X-band rows of SENSITIVITY-4.1-ALL.md |
| kait | 12.0 | - | - | - | - | - | 381 | 0 / 2 | OFDM / concatenated broadcast and satellite links: see SENSITIVITY-4.1-TV.md and the X-band rows of SENSITIVITY-4.1-ALL.md |
| kapp | 2.0 | - | - | - | - | - | 6 | 0 / 6 | OFDM / concatenated broadcast and satellite links: see SENSITIVITY-4.1-TV.md and the X-band rows of SENSITIVITY-4.1-ALL.md |
| kavb | 12.0 | - | - | - | - | - | 6896 | 0 / 2 | OFDM / concatenated broadcast and satellite links: see SENSITIVITY-4.1-TV.md and the X-band rows of SENSITIVITY-4.1-ALL.md |
| kaws | - | - | - | - | - | - | - | - | OFDM / concatenated broadcast and satellite links: see SENSITIVITY-4.1-TV.md and the X-band rows of SENSITIVITY-4.1-ALL.md |
| klw | 12.0 | 8.0 | - | - | - | - | 11 | 0 / 2 | data on an FM or AM audio channel: limited by the FM threshold, not a bit-error formula; first unit at the end of the trial: latency-limited |
| lbj | 4.0 | - | - | - | - | - | 5 | 0 / 8 | data on an FM or AM audio channel: limited by the FM threshold, not a bit-error formula; first unit at the end of the trial: latency-limited |
| ldacs | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| link11 | 6.0 | - | - | - | - | - | 7 | 0 / 4 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| loraaprs | -22.0 | - | - | - | - | - | 1 | 0 / 18 | no closed-form model here (coded, multi-carrier or PHY not modelled); 1-2 units a trial: coarse |
| loraraw | -16.0 | - | - | - | - | - | 19 | 0 / 18 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| lorasat | -20.0 | - | - | - | - | - | 2 | 0 / 20 | no closed-form model here (coded, multi-carrier or PHY not modelled); 1-2 units a trial: coarse |
| lorawan | -8.0 | - | - | - | - | - | 4 | 0 / 14 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| lorawanwide | -16.0 | - | - | - | - | - | 18 | 0 / 18 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| ltpos | - | - | - | - | - | - | 1 | 0 / 6 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| ltr | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| lucky7 | - | - | - | - | - | - | 1 | 16 / 16 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| m17 | 4.0 | - | - | - | - | - | 140 | 0 / 8 | 4FSK on a discriminator with voice FEC (no closed form here; DMR at 6 dB is the reference) |
| mac | 6.0 | - | - | - | - | - | 4018 | 0 / 4 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| marine | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| marinevhf | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| marker | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| mavlink | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| mdc1200 | 22.0 | - | - | - | - | - | 2 | 0 / 2 | analog or squelch count (calls, tones, audio lines): no bit error theory; 1-2 units a trial: coarse |
| mesh | -16.0 | - | - | - | - | - | 8 | 0 / 16 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| meshcom | -24.0 | - | - | - | - | - | 2 | 0 / 16 | no closed-form model here (coded, multi-carrier or PHY not modelled); 1-2 units a trial: coarse |
| meshcore | -12.0 | - | - | - | - | - | 3 | 0 / 14 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| meshtastic | -20.0 | - | - | - | - | - | 5 | 0 / 20 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| meteor | 2.0 | - | - | - | - | - | 68 | 0 / 6 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| meteorhrpt | -8.0 | - | - | - | - | - | 540 | 1170 / 12 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| mfsk | 14.0 | - | - | - | - | - | 13 | 0 / 2 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| milhf | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| milstd110 | 2.0 | - | - | - | - | - | 9 | 0 / 6 | no closed-form model here (coded, multi-carrier or PHY not modelled); first unit at the end of the trial: latency-limited |
| mioty | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| modeac | 18.0 | - | - | - | - | - | 188 | 0 / 2 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| mpt1327 | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| mradar | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| mscatter | - | - | - | - | - | - | 1 | 0 / 16 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| msgraw | 4.0 | - | - | - | - | - | 2440 | 0 / 6 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| msk144 | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| mt63 | -20.0 | - | - | - | - | - | 3 | 0 / 18 | no closed-form model here (coded, multi-carrier or PHY not modelled); first unit at the end of the trial: latency-limited |
| multitone | 24.0 | - | - | - | - | - | 11 | 0 / 2 | analog or squelch count (calls, tones, audio lines): no bit error theory; first unit at the end of the trial: latency-limited |
| navic | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| navtex | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| nbiot | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| ndb | 6.0 | - | - | - | - | - | 17 | 0 / 4 | analog or squelch count (calls, tones, audio lines): no bit error theory |
| nfc | 18.0 | - | - | - | - | - | 10990 | 798 / 2 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| nfmdev | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| noaaapt | -24.0 | - | - | - | - | - | 14 | 221 / 16 | analog or squelch count (calls, tones, audio lines): no bit error theory; first unit at the end of the trial: latency-limited |
| noaadsb | 6.0 | - | - | - | - | - | 12 | 77 / 6 | no closed-form model here (coded, multi-carrier or PHY not modelled); first unit at the end of the trial: latency-limited |
| noaagac | -4.0 | - | - | - | - | - | 391 | 1021 / 10 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| noaahrpt | -8.0 | - | - | - | - | - | 39 | 0 / 12 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| noisefloor | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| npr | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| nxdn | 8.0 | - | - | - | - | - | 98 | 0 / 4 | 4FSK on a discriminator with voice FEC (no closed form here; DMR at 6 dB is the reference) |
| occbw | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| occupancy | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| ogn | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| olivia | -12.0 | - | - | - | - | - | 4 | 0 / 14 | no closed-form model here (coded, multi-carrier or PHY not modelled); first unit at the end of the trial: latency-limited |
| oneweb | -12.0 | - | - | - | - | - | 1 | 0 / 14 | no closed-form model here (coded, multi-carrier or PHY not modelled); 1-2 units a trial: coarse |
| ookpulse | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| opensky | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| orbcomm | 0.0 | - | - | - | - | - | 592 | 0 / 8 | no closed-form model here (coded, multi-carrier or PHY not modelled); first unit at the end of the trial: latency-limited |
| p25 | 8.0 | - | - | - | - | - | 131 | 0 / 4 | 4FSK on a discriminator with voice FEC (no closed form here; DMR at 6 dB is the reference) |
| p25p2 | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| packet | 10.0 | - | - | - | - | - | 8 | 0 / 2 | data on an FM or AM audio channel: limited by the FM threshold, not a bit-error formula |
| pactor | -18.0 | - | - | - | - | - | 6 | 0 / 16 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| pactor2 | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| pactor3 | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| pactor4 | - | - | - | - | - | - | 1 | 0 / 8 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| pager | 2.0 | - | - | - | - | - | 168 | 0 / 8 | data on an FM or AM audio channel: limited by the FM threshold, not a bit-error formula |
| papr | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| passband | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| phasenoise | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| pi4 | -18.0 | - | - | - | - | - | 1 | 0 / 16 | no closed-form model here (coded, multi-carrier or PHY not modelled); 1-2 units a trial: coarse |
| piccolo | -10.0 | - | - | - | - | - | 18 | 0 / 12 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| plnoise | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| pmr446 | 24.0 | - | - | - | - | - | 3 | 0 / 2 | analog or squelch count (calls, tones, audio lines): no bit error theory |
| psk | -24.0 | - | - | - | - | - | 85 | 0 / 16 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| pskd | - | - | - | - | - | - | 1 | 0 / 8 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| pttlog | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| pulsar | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| q65 | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| qi | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| qo100 | - | - | - | - | - | - | 1 | 0 / 16 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| qrss | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| radar | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| radiojove | - | - | - | - | - | - | 1 | 16 / 16 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| rclink | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| rcppm | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| reticulum | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| rid | -2.0 | - | - | - | - | - | 2000 | 0 / 8 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| riometer | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| roshf | -8.0 | - | - | - | - | - | 5 | 0 / 12 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| same | 6.0 | - | - | - | - | - | 1 | 0 / 4 | data on an FM or AM audio channel: limited by the FM threshold, not a bit-error formula; 1-2 units a trial: coarse |
| satbeacon | -6.0 | - | - | - | - | - | 20 | 0 / 10 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| sband | 6.0 | - | - | - | - | - | 1281 | 0 / 4 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| sbas | -2.0 | - | - | - | - | - | 14 | 0 / 8 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| schumann | - | - | - | - | - | - | 1 | 16 / 16 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| selcal | 12.0 | - | - | - | - | - | 1 | 0 / 2 | analog or squelch count (calls, tones, audio lines): no bit error theory; 1-2 units a trial: coarse; first unit at the end of the trial: latency-limited |
| sensors | 10.0 | - | - | - | - | - | 6 | 0 / 2 | analog or squelch count (calls, tones, audio lines): no bit error theory; first unit at the end of the trial: latency-limited |
| sferics | -6.0 | - | - | - | - | - | 23 | 0 / 10 | analog or squelch count (calls, tones, audio lines): no bit error theory; first unit at the end of the trial: latency-limited |
| sfhss | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| shortwave | - | - | - | - | - | - | 1 | 16 / 16 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| sinad | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| smartnet | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| sonde | 14.0 | - | - | - | - | - | 1 | 0 / 2 | no closed-form model here (coded, multi-carrier or PHY not modelled); 1-2 units a trial: coarse; first unit at the end of the trial: latency-limited |
| specan | -24.0 | - | - | - | - | - | 39062 | 624992 / 16 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| speckurt | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| specx | - | - | - | - | - | - | 1 | 0 / 12 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| spektrum | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| spino | - | - | - | - | - | - | 1 | 16 / 16 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| spreadhf | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| spur | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| ssbtest | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| ssdv | 12.0 | - | - | - | - | - | 4 | 0 / 2 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| ssrint | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| sstv | 4.0 | - | - | - | - | - | 44 | 0 / 6 | analog or squelch count (calls, tones, audio lines): no bit error theory |
| stanag4285 | - | - | - | - | - | - | 1 | 0 / 2 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| stanag4481 | -6.0 | - | - | - | - | - | 3 | 0 / 12 | no closed-form model here (coded, multi-carrier or PHY not modelled); first unit at the end of the trial: latency-limited |
| stanag4539 | 0.0 | - | - | - | - | - | 13 | 0 / 8 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| starlink | -4.0 | - | - | - | - | - | 1 | 0 / 12 | no closed-form model here (coded, multi-carrier or PHY not modelled); 1-2 units a trial: coarse; first unit at the end of the trial: latency-limited |
| supersid | - | - | - | - | - | - | 1 | 16 / 16 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| tdcs | 12.0 | 8.0 | - | - | - | - | 8 | 0 / 2 | data on an FM or AM audio channel: limited by the FM threshold, not a bit-error formula |
| tdrs | - | - | - | - | - | - | 1 | 0 / 12 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| tetradmo | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| thor | -8.0 | - | - | - | - | - | 11 | 0 / 12 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| thread | -2.0 | - | - | - | - | - | 2382 | 0 / 8 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| throb | -8.0 | - | - | - | - | - | 16 | 0 / 12 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| tiantong | -10.0 | - | - | - | - | - | 4 | 0 / 12 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| time | - | - | - | - | - | - | 1 | 0 / 8 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| tones | 10.0 | - | - | - | - | - | 1 | 0 / 2 | analog or squelch count (calls, tones, audio lines): no bit error theory; 1-2 units a trial: coarse; first unit at the end of the trial: latency-limited |
| tsikada | - | - | - | - | - | - | 1 | 0 / 16 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| txstability | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| u482c | 14.0 | 14.0 | - | - | - | - | 7 | 0 / 2 | data on an FM or AM audio channel: limited by the FM threshold, not a bit-error formula |
| uhfrfid | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| uhfsatcom | -6.0 | - | - | - | - | - | 7 | 0 / 10 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| uplink | 4.0 | - | - | - | - | - | 72 | 0 / 8 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| uvb76 | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| vdes | -6.0 | - | - | - | - | - | 16 | 0 / 10 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| vdl2 | 2.0 | - | - | - | - | - | 19 | 0 / 6 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| vlfmsk | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| voiceinv | 10.0 | - | - | - | - | - | 12 | 0 / 2 | analog or squelch count (calls, tones, audio lines): no bit error theory |
| vor | 4.0 | - | - | - | - | - | 30 | 0 / 6 | analog or squelch count (calls, tones, audio lines): no bit error theory |
| vorils | 4.0 | - | - | - | - | - | 30 | 0 / 6 | analog or squelch count (calls, tones, audio lines): no bit error theory |
| wb | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| wefax | - | - | - | - | - | - | 1 | 0 / 6 | analog or squelch count (calls, tones, audio lines): no bit error theory; 1-2 units a trial: coarse |
| wfbng | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| whatsat | - | - | - | - | - | - | 1 | 0 / 16 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| whistler | -4.0 | - | - | - | - | - | 1 | 0 / 10 | analog or squelch count (calls, tones, audio lines): no bit error theory; 1-2 units a trial: coarse; first unit at the end of the trial: latency-limited |
| winmor | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| wisun | -10.0 | - | - | - | - | - | 51 | 0 / 12 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| wmbus | 2.0 | - | - | - | - | - | 17 | 0 / 6 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| wmic | - | - | - | - | - | - | 1 | 0 / 6 | analog or squelch count (calls, tones, audio lines): no bit error theory; 1-2 units a trial: coarse |
| wradar | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| wspr | - | - | - | - | - | - | - | - | not measured: no valid output on the clean test signal (8 and 32 s) |
| xb_aqua | 8.0 | - | - | - | - | - | 4663 | 0 / 4 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| xb_dmsp | -12.0 | - | - | - | - | - | 48317 | 0 / 16 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| xb_gcomc | 2.0 | - | - | - | - | - | 5508 | 0 / 6 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| xb_gcomw | 4.0 | - | - | - | - | - | 1859 | 0 / 6 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| xb_jpss2 | 4.0 | - | - | - | - | - | 2308 | 0 / 8 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| xb_kanopus | 8.0 | - | - | - | - | - | 3233 | 0 / 4 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| xb_landsats | 4.0 | - | - | - | - | - | 653 | 0 / 8 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| xb_landsatx | 2.0 | - | - | - | - | - | 2154 | 0 / 6 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| xb_metopd | 10.0 | - | - | - | - | - | 7267 | 0 / 2 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| xb_metopsg | 4.0 | - | - | - | - | - | 2708 | 0 / 6 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| xb_npp | 4.0 | - | - | - | - | - | 2903 | 0 / 6 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| xb_oceansat2 | 4.0 | - | - | - | - | - | 31 | 0 / 6 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| xb_oceansat3 | 2.0 | - | - | - | - | - | 852 | 0 / 6 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| xb_oceansat3x | 10.0 | - | - | - | - | - | 3040 | 0 / 2 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| xb_probas | -2.0 | - | - | - | - | - | 732 | 0 / 8 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| xb_probav | 6.0 | - | - | - | - | - | 2899 | 0 / 4 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| xb_saral | 4.0 | - | - | - | - | - | 376 | 0 / 6 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| xb_saralx | 4.0 | - | - | - | - | - | 2470 | 0 / 6 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| xb_stereo | -2.0 | - | - | - | - | - | 1127 | 0 / 8 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| xb_terra | 0.0 | - | - | - | - | - | 682 | 0 / 8 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| xlinknb | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| xpa | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| xpa2 | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| yfactor | - | - | - | - | - | - | - | - | not measured: no test signal or generator failed |
| zigbee | -2.0 | - | - | - | - | - | 2382 | 0 / 8 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
| zombie | - | - | - | - | - | - | 1 | 0 / 16 | lock flag only: no checked unit; 1-2 units a trial: coarse |
| zwave | -6.0 | - | - | - | - | - | 162 | 0 / 12 | no closed-form model here (coded, multi-carrier or PHY not modelled) |
