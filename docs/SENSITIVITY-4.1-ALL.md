# Sensitivity of every engine mode (4.1, feat41-sensAll)

All figures are from simulated signals: each mode's own built-in test signal, white Gaussian noise, synthetic impairments. No recording
was used. They are measured on GitHub's Linux runners (`sens-quick.yml`, modes `qa:REGEX@k/N`).

## Why a second harness

`sens_sweep` (docs/SENSITIVITY.md) measures the modes that have a receiver block in `tools/sens/sens_modes.h`, about 100 of them. Most
modes added in 3.9 and 4.0 have none, but every mode with a built-in test signal already runs through the QA sweep
(`tests/qa/test_qa_sweep.cpp`): generator -> 16-bit file -> Engine -> the mode's decoder, with the receiver's own count of checked units
(CRC, FEC, parity; the text decoders by what they save). `tools/sens/qa_sens.py` drives that binary's two stages for a series of SNRs, so
every mode is measured the same way and through the same Engine path a user has.

## Method

- **SNR** = power of the active part of the clean signal / noise power in the signal's own 99 % band (`B99`, measured on the clean signal
  by the QA generator stage, the floor of the generator removed). This is the QA sweep's `snr` file. It is a C/N in the occupied band,
  so for a single carrier it is close to Es/N0 with the roll-off counted. Where the test signal holds several carriers, spurs or a wide
  scene, B99 is wider than one channel and the figure is not comparable (marked "SNR band too wide": a -24 in the table is the bottom of
  the search, not a sensitivity).
- **Scenarios**: `awgn`; `cfoclk` = +2 kHz carrier and a +50 ppm sample clock with the same noise (a cheap radio's crystal); `offcentre` =
  the channel moved by a quarter of the mode's channel width (at least 2 kHz) and a -50 ppm clock, with the same noise. On HF the 2 kHz is
  more than a crystal gives and is outside an SSB-audio decoder's window: a "-" there is expected (qa_known.inc says so too).
- **Pass**: a trial passes when the receiver counts at least half the units of the clean run (the sens_sweep rule). A lock-only mode
  (monitors, identification-only modes) passes on its lock flag; those rows say so and their false rate is a false-lock rate.
- **Search**: 2 trials (two noise seeds) a point, from 24 dB down in 4 dB steps while every trial passes, then one 2 dB step; the
  impairment scenarios are measured at the same points and refined by 2 dB too. The threshold is the lowest SNR reached from the top
  where every trial passed. Resolution 2 dB. 8 s of signal a trial (32 s when 8 s gives no unit on the clean signal).
- **False decodes**: the noise-only file of every point at or below 6 dB (the same noise, no signal) through the same receiver; any
  checked unit counted there is a false decode. The column gives units / noise-only trials.
- **Theory**: for the modes whose physical layer is simple and known, the SNR in B99 an ideal receiver needs for 90 % of its units,
  from the bit error rate of the modulation: (1 - BER)^L = 0.9 for an L-bit unit, SNR = Eb/N0 + 10 log10(Rb / B99). Uncoded bound unless the
  note says otherwise (a code makes the true limit lower, so the gap shown is then a lower bound of the real gap). Noncoherent binary
  FSK: BER = exp(-Eb/2N0)/2; GMSK BT 0.3: Q(sqrt(1.36 Eb/N0)); DBPSK: exp(-Eb/N0)/2. A negative gap of more than 2 dB means the
  assumption does not fit the test signal (shorter unit, code, several signals in B99) and is marked.

Limits: 2 trials a point and 2 dB steps, so a threshold is good to about +-2 dB; a few rows show a trial that failed above the threshold
(threaded decoders on a loaded 4-core runner running three modes at once: TETRA is one), marked. Units that arrive only at the end of the
8 s file (long interleavers, message decoders) make the threshold latency-limited: marked; their decoder-alone figures in
SENSITIVITY-3.9-D.md (MFSK16 -15 dB in 2500 Hz, THOR16 -11 dB) are the better numbers.

Modes with no built-in test signal (the generator lives in their unit test only, e.g. DCR, GW FSK, the FSK modems, SSR, the ILS markers,
the Rivet modes) cannot run here; their figures come from their unit tests' noise sweeps (docs/RIVET-MODES-4.0.md, DCR-4.0.md,
FSKMODEM-4.0.md, SSRINT-4.0.md, MARKER-4.0.md) and are listed after the table.

## The table (all 6 shards: 371 engine entries; sens-quick runs 43364063415, 43364067712, 43374519331, 43380233150, 43384879116 and 43393226477, artifact `sens-results`)

SNR in dB in B99. Raw results (every point and trial): tests/sens/qa41/qa-sens-*.jsonl. CI: sens-quick 43364063415 (shard 1), 43364067712 (2), 43374519331 (3), 43380233150 (4), 43384879116 (5), 43393226477 (6). "had a figure before" = the mode id appears in an earlier SENSITIVITY doc (a loose text match).

| mode | name | measure | B99 (Hz) | awgn | +2 kHz +50 ppm | off-centre -50 ppm | false (units / noise trials) | theory | gap (dB) | had a figure before | note |
|---|---|---|---|---|---|---|---|---|---|---|---|
| acars | ACARS | blocks | 305664 | -6 | -6 | -6 | 0 / 10 | - | - | yes |  |
| adsb | ADS-B | blocks | 2824219 | 14 | 14 | 14 | 0 / 2 | - | - | yes |  |
| adsl | ADS-L | blocks | 1466797 | -6 | -4 | - | 0 / 10 | - | - | yes |  |
| aero | Inmarsat Aero | blocks | 3954102 | -12 | -12 | -12 | 0 / 14 | - | - | yes |  |
| aeromacs | AeroMACS | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| afsk | AFSK (TNC) tone tuner | - | - | - | - | - | - | - | - | yes | no test signal or generator failed |
| airband | Airband | blocks | 306152 | -10 | -10 | - | 0 / 12 | - | - | yes |  |
| ais | AIS | blocks | 10742 | 8 | 8 | - | 0 / 4 | 8.7 | -0.7 | yes |  |
| ale | HF ALE | blocks | 2930 | -6 | - | - | 0 / 10 | - | - | yes | units only at the end of the trial: latency-limited |
| ale3g | 3G-ALE | blocks | 3906 | -10 | - | - | 0 / 12 | - | - | no |  |
| am | AM broadcast | blocks | 8301 | 8 | 8 | - | 0 / 4 | - | - | yes |  |
| ammon | AM modulation monitor | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| ant | ANT+ sensors | blocks | 878906 | 8 | 8 | 14 | 0 / 4 | - | - | yes |  |
| ardop | ARDOP | blocks | 148926 | -22 | - | - | 0 / 18 | - | - | yes | units only at the end of the trial: latency-limited |
| argos | Argos | blocks | 3812 | -4 | -4 | -4 | 0 / 10 | - | - | yes |  |
| arktikaggak | Arktika-M GGAK | blocks | 6250 | 8 | 8 | - | 0 / 4 | - | - | no | units only at the end of the trial: latency-limited |
| arktikardas | Arktika-M RDAS | blocks | 36507568 | -2 | 0 | 20 | 0 / 8 | - | - | no |  |
| arqe | ARQ-E | blocks | 1953 | -2 | 0 | - | 0 / 8 | - | - | no |  |
| arqn | ARQ-N | blocks | 1953 | -2 | 0 | - | 0 / 8 | - | - | no |  |
| asterix | ASTERIX | - | - | - | - | - | - | - | - | yes | no valid output on the clean test signal (8 and 32 s) |
| astro | Radio astronomy | lock | 2147827 | -24 | -24 | -24 | 16 / 16 | - | - | yes | SNR band too wide (test signal holds several carriers or spurs): not comparable; lock flag only, no checked unit |
| atis | ATIS (inland waterways) | blocks | 1709 | 12 | 12 | - | 0 / 2 | 9.6 | +2.4 | yes |  |
| autospec | AUTOSPEC | - | - | - | - | - | - | - | - | no | no valid output on the clean test signal (8 and 32 s) |
| awsl | AWS L-band | blocks | 2211914 | 4 | 4 | 4 | 0 / 6 | - | - | no |  |
| ax100m5 | AX100 Mode 5 (ASM + Golay) | blocks | 10254 | 10 | 8 | 10 | 0 / 2 | - | - | no |  |
| ax100m6 | AX100 Mode 6 (Reed-Solomon) | blocks | 11230 | 14 | 14 | 14 | 0 / 2 | - | - | no |  |
| beacon | Beacon propagation monitor | - | - | - | - | - | - | - | - | yes | no test signal or generator failed |
| beaconevents | Aurora / Es / TEP beacons | lock | 247559 | -24 | -24 | -24 | 16 / 16 | - | - | no | SNR band too wide (test signal holds several carriers or spurs): not comparable; lock flag only, no checked unit |
| beesat | BEESAT / Mobitex | blocks | 6348 | 12 | 12 | 12 | 0 / 2 | - | - | yes |  |
| bgan | Inmarsat BGAN | blocks | 745606 | 2 | 6 | 2 | 0 / 8 | - | - | no |  |
| birdie | Persistent carrier (birdie) finder | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| ble | Bluetooth LE | blocks | 1047852 | 4 | 4 | - | 0 / 6 | - | - | yes |  |
| bletracker | BLE tracker detector | blocks | 1035156 | 6 | 6 | - | 0 / 4 | - | - | yes |  |
| btc | Bluetooth Classic | blocks | 12709961 | -4 | -4 | -4 | 0 / 10 | - | - | no |  |
| burst | Burst finder | - | - | - | - | - | - | - | - | yes | no test signal or generator failed |
| callisto | e-CALLISTO bursts | lock | 1979004 | -24 | -24 | -24 | 16 / 16 | - | - | no | SNR band too wide (test signal holds several carriers or spurs): not comparable; lock flag only, no checked unit |
| cb | CB radio | blocks | 732 | 16 | 16 | - | 0 / 2 | - | - | no |  |
| cdma2000 | CDMA2000 cells | - | - | - | - | - | - | - | - | no | no valid output on the clean test signal (8 and 32 s) |
| cdr | CDR | blocks | 96191 | 6 | 6 | 6 | 0 / 4 | - | - | yes |  |
| cis12 | CIS-12 | blocks | 1465 | 6 | - | - | 0 / 4 | - | - | no | units only at the end of the trial: latency-limited |
| cis128 | CIS-128 | lock | 4394 | 6 | 6 | - | 0 / 4 | - | - | no | lock flag only, no checked unit |
| cis14 | CIS-14 | blocks | 1465 | -4 | -2 | - | 0 / 12 | - | - | no | units only at the end of the trial: latency-limited |
| cis3000 | CIS-3000 | lock | 4394 | 12 | 12 | - | 0 / 2 | - | - | no | lock flag only, no checked unit |
| cis36 | CIS-36 | lock | 2441 | -4 | -4 | - | 0 / 10 | - | - | no | lock flag only, no checked unit |
| cisbee | CIS-36-50 (BEE) | blocks | 1465 | -4 | -4 | - | 0 / 10 | - | - | no | units only at the end of the trial: latency-limited |
| cishdr | CIS OFDM HDR | lock | 4394 | 4 | 6 | 4 | 0 / 8 | - | - | no | lock flag only, no checked unit |
| codar | CODAR / HF ocean radar | blocks | 1797363 | -6 | -6 | -4 | 0 / 10 | - | - | yes |  |
| contestia | Contestia | blocks | 2441 | -10 | - | - | 0 / 12 | - | - | yes |  |
| coquelet | Coquelet-8 | blocks | 1953 | -6 | -4 | - | 0 / 10 | - | - | no |  |
| cp16 | CP16 | blocks | 77148 | -12 | -10 | - | 0 / 16 | - | - | no | units only at the end of the trial: latency-limited |
| cquam | AM stereo (C-QUAM) | blocks | 2441 | 10 | - | - | 0 / 2 | - | - | yes |  |
| crowd36 | CROWD36 | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| css | Tiangong / Shenzhou carriers | lock | 92529 | -24 | -24 | -24 | 0 / 16 | - | - | no | SNR band too wide (test signal holds several carriers or spurs): not comparable; lock flag only, no checked unit |
| cubesat | CubeSat telemetry | blocks | 9277 | 12 | 12 | 12 | 0 / 2 | 12.4 | -0.4 | yes |  |
| cv2x | C-V2X | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| cw | CW | blocks | 89356 | 4 | 4 | 16 | 0 / 6 | - | - | yes |  |
| cwkeying | CW keying quality meter | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| datawell | Datawell Buoy HF Link | lock | 12634 | -4 | -2 | - | 0 / 12 | - | - | yes | lock flag only, no checked unit |
| dcr | DCR (Japan digital convenience radio) | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| dect | DECT cordless phones | blocks | 1488037 | 2 | 2 | - | 0 / 6 | - | - | yes |  |
| dectnrp | DECT NR+ | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| dfcw | DFCW grabber | - | - | - | - | - | - | - | - | no | no valid output on the clean test signal (8 and 32 s) |
| dgps | DGPS beacon | - | - | - | - | - | - | - | - | yes | no test signal or generator failed |
| dme | DME | - | - | - | - | - | - | - | - | yes | no test signal or generator failed |
| dmr | DMR | blocks | 8789 | 6 | 6 | 6 | 0 / 4 | - | - | yes | units only at the end of the trial: latency-limited |
| dominoex | DominoEX | blocks | 1465 | -8 | 10 | - | 0 / 12 | - | - | yes |  |
| dopplerlog | Satellite Doppler curve logger | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| dpmr | dPMR | blocks | 4394 | 8 | 10 | 10 | 0 / 4 | - | - | yes |  |
| drm | DRM | blocks | 10254 | 16 | 16 | 16 | 0 / 2 | - | - | yes |  |
| dsc | DSC | blocks | 1465 | -2 | - | - | 0 / 8 | 0.4 | -2.4 (theory assumption off: unit shorter or coded) | yes | units only at the end of the trial: latency-limited |
| dsrc | DSRC toll | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| dstar | D-STAR | blocks | 5371 | 8 | 8 | 8 | 0 / 4 | - | - | yes |  |
| ebicab | EBICab / Eurobalise | blocks | 1213379 | 10 | 10 | 12 | 0 / 2 | - | - | no |  |
| eczas | e-Czas (225 kHz, Poland) | blocks | 188 | 30 | - | - | 0 / 2 | - | - | yes | units only at the end of the trial: latency-limited |
| edacs | EDACS trunking | - | - | - | - | - | - | - | - | yes | no test signal or generator failed |
| efr | EFR teleswitch | blocks | 1953 | 2 | 4 | - | 0 / 6 | - | - | no | units only at the end of the trial: latency-limited |
| elektro | Elektro-L | blocks | 843164 | -2 | -2 | -2 | 0 / 8 | - | - | yes |  |
| elektrordas | Elektro-L RDAS | blocks | 36507568 | -2 | 0 | 20 | 0 / 8 | - | - | no |  |
| eloran | eLoran / Loran-C | - | - | - | - | - | - | - | - | yes | no test signal or generator failed |
| elrs | ExpressLRS | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| elt121 | ELT 121.5 MHz | blocks | 3418 | 6 | 6 | 6 | 0 / 4 | - | - | no |  |
| em4100 | EM4100 LF RFID (125 kHz) | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| emeecho | Moon echo (EME) | lock | 247803 | -24 | -24 | -24 | 16 / 16 | - | - | no | SNR band too wide (test signal holds several carriers or spurs): not comparable; lock flag only, no checked unit |
| eot | End of train | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| ermes | ERMES | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| erminaz | Erminaz | lock | 10559 | -24 | -24 | -24 | 16 / 16 | - | - | no | SNR band too wide (test signal holds several carriers or spurs): not comparable; lock flag only, no checked unit |
| esubghz | Flipper Zero Sub-GHz chat | blocks | 45410 | 12 | 10 | 24 | 0 / 2 | - | - | no |  |
| evm | Modulation quality (MER) meter | - | - | - | - | - | - | - | - | yes | no test signal or generator failed |
| f01 | FSK200/500 (F01) | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| f06 | FSK200/1000 (F06, F06a) | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| fading | Fading analyser | - | - | - | - | - | - | - | - | yes | no test signal or generator failed |
| fanet | FANET | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| fbr | Fleet Broadcast (UHF SATCOM) | blocks | 3296 | 2 | 2 | - | 0 / 6 | - | - | no | units only at the end of the trial: latency-limited |
| flarm | FLARM | blocks | 360596 | 2 | 2 | 6 | 0 / 8 | - | - | yes |  |
| flex | FLEX | blocks | 12207 | 2 | 4 | 8 | 0 / 8 | - | - | yes |  |
| flysky | FlySky | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| fm | FM | blocks | 137695 | 18 | 18 | 18 | 0 / 2 | - | - | yes |  |
| fmmon | FM modulation monitor | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| freedv | FreeDV | blocks | 2441 | 8 | - | - | 0 / 4 | - | - | yes |  |
| freqacc | Carrier frequency accuracy meter | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| frs | FRS / GMRS | blocks | 2930 | 24 | 24 | 24 | 0 / 2 | - | - | no |  |
| frsky | FrSky | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| fsk441 | FSK441 | blocks | 594141 | -24 | - | - | 0 / 16 | - | - | no | SNR band too wide (test signal holds several carriers or spurs): not comparable; units only at the end of the trial: latency-limited |
| fskan | FSK parameter analyser | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| fskmodem | FSK modems (Bell 103, V.21, V.23, Bell 202) | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| fsq | FSQ | blocks | 1978516 | -24 | -24 | -14 | 0 / 18 | - | - | yes | SNR band too wide (test signal holds several carriers or spurs): not comparable; units only at the end of the trial: latency-limited |
| fst4 | FST4 | - | - | - | - | - | - | - | - | yes | no valid output on the clean test signal (8 and 32 s) |
| ft2 | FT2 | blocks | 2930 | 46 | - | - | 0 / 2 | - | - | yes |  |
| ft4 | FT4 | - | - | - | - | - | - | - | - | yes | no valid output on the clean test signal (8 and 32 s) |
| ft8 | FT8 | - | - | - | - | - | - | - | - | yes | no valid output on the clean test signal (8 and 32 s) |
| fusion | Yaesu Fusion | - | - | - | - | - | - | - | - | yes | no test signal or generator failed |
| fy2svissr | FY-2 S-VISSR | blocks | 836133 | -4 | -4 | -2 | 0 / 12 | - | - | no |  |
| fy3ahrpt | FY-3 AHRPT | blocks | 3307617 | 2 | 2 | 2 | 0 / 6 | - | - | yes |  |
| fy3mpt | FY-3 MPT | blocks | 19638672 | 6 | 12 | - | 0 / 4 | - | - | no |  |
| fy4ahrit | FY-4A HRIT-II/III | blocks | 1104492 | 6 | 6 | 6 | 0 / 4 | - | - | no |  |
| fy4lrit | FY-4 LRIT | blocks | 100098 | 0 | 0 | 0 | 0 / 8 | - | - | no |  |
| gbas | GBAS VDB | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| general | General receiver | blocks | 8301 | 8 | 8 | - | 0 / 4 | - | - | yes |  |
| geonetcast | GeoNetCast | blocks | 32182617 | 10 | 10 | 10 | 0 / 2 | - | - | no |  |
| geoscan | Geoscan | lock | 10559 | -24 | -24 | -24 | 16 / 16 | - | - | no | SNR band too wide (test signal holds several carriers or spurs): not comparable; lock flag only, no checked unit |
| gk2acdas | GK-2A CDAS | blocks | 3927734 | 8 | 8 | - | 0 / 4 | - | - | no |  |
| gk2ahrit | GK-2A HRIT | blocks | 3803711 | 2 | 2 | 2 | 0 / 6 | - | - | yes |  |
| gk2alrit | GK-2A LRIT | blocks | 162305 | -2 | -2 | -2 | 0 / 8 | - | - | yes |  |
| gk2auhrit | GK-2A UHRIT | blocks | 17211914 | 14 | 16 | 18 | 0 / 2 | - | - | no |  |
| glitch | SDR stream glitch detector | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| globalstar | Globalstar | blocks | 2331543 | -6 | -8 | -8 | 0 / 12 | - | - | yes |  |
| gmr1 | Thuraya GMR-1 channels | blocks | 322266 | -12 | -10 | -10 | 0 / 16 | - | - | no |  |
| gnss | GNSS | blocks | 3948242 | -6 | - | - | 0 / 10 | - | - | yes |  |
| gnssjam | GNSS interference detector | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| goes | GOES HRIT | blocks | 1174805 | -2 | -2 | -2 | 0 / 8 | - | - | yes |  |
| goescda | GOES CDA | blocks | 51025 | 4 | 4 | 4 | 0 / 6 | - | - | no |  |
| goesdcs | GOES DCS | blocks | 988770 | -24 | -24 | -24 | 0 / 16 | - | - | yes | SNR band too wide (test signal holds several carriers or spurs): not comparable |
| goesgrb | GOES GRB | blocks | 9550781 | 6 | 8 | 8 | 0 / 4 | - | - | no |  |
| goesgvar | GOES GVAR | blocks | 2673340 | 0 | 0 | 0 | 0 / 8 | - | - | no |  |
| goesmdl | GOES MDL | blocks | 254150 | 4 | 4 | 4 | 647 / 6 | - | - | no | FALSE DECODES on noise |
| gonets | Gonets | blocks | 750000 | 20 | 20 | 20 | 0 / 2 | - | - | no |  |
| graves | GRAVES meteor echoes | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| gsmr | GSM-R | - | - | - | - | - | - | - | - | yes | no valid output on the clean test signal (8 and 32 s) |
| gtor | G-TOR | blocks | 1465 | 14 | 14 | - | 0 / 2 | 4.6 | +9.4 | no | units only at the end of the trial: latency-limited |
| gwfsk | GW FSK (100 Bd) | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| halow | Wi-Fi HaLow (802.11ah) | blocks | 3016113 | 0 | 0 | 6 | 0 / 10 | - | - | yes |  |
| hamdrm | HamDRM | saved | 3418 | 14 | - | - | 0 / 2 | - | - | yes |  |
| hamfm | Ham FM repeaters | blocks | 3418 | 22 | 22 | 22 | 0 / 2 | - | - | no |  |
| hdr | HD Radio | blocks | 110840 | 22 | 24 | - | 0 / 2 | - | - | yes |  |
| hell | Hellschreiber | blocks | 1465 | -4 | - | - | 0 / 10 | - | - | no |  |
| hfbeacon | HF beacons | - | - | - | - | - | - | - | - | yes | no valid output on the clean test signal (8 and 32 s) |
| hfdig | HF digital | blocks | 1953 | -8 | - | - | 0 / 12 | - | - | yes |  |
| hfdl | HFDL | blocks | 52734 | -10 | - | - | 0 / 12 | - | - | yes |  |
| hfpacket300 | HF packet 300 Bd | blocks | 1953 | 8 | - | - | 0 / 4 | 3.3 | +4.7 | no | units only at the end of the trial: latency-limited |
| hfvoice | HFGCS / HF voice | blocks | 2686 | 12 | 12 | 12 | 0 / 2 | - | - | no | units only at the end of the trial: latency-limited |
| himawaricast | HimawariCast | blocks | 2774414 | 4 | 4 | 4 | 0 / 6 | - | - | no |  |
| hop | Frequency-hopper analyser | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| horus | Horus balloons | blocks | 1641 | -4 | -4 | 6 | 0 / 12 | - | - | yes | units only at the end of the trial: latency-limited |
| hrdcp | Meteosat HRDCP | blocks | 977 | 4 | 4 | 4 | 0 / 6 | - | - | no | units only at the end of the trial: latency-limited |
| hrpt | HRPT / AHRPT | blocks | 3127441 | 2 | 2 | - | 0 / 6 | - | - | yes |  |
| ifkp | IFKP | blocks | 1978516 | -24 | -24 | 10 | 0 / 16 | - | - | yes | SNR band too wide (test signal holds several carriers or spurs): not comparable; units only at the end of the trial: latency-limited |
| ils | ILS | blocks | 1465 | 16 | 16 | - | 0 / 2 | - | - | yes |  |
| inmc | Inmarsat-C | blocks | 1412598 | -24 | -24 | -24 | 0 / 16 | - | - | yes | SNR band too wide (test signal holds several carriers or spurs): not comparable |
| inmsat | Inmarsat (all carriers) | - | - | - | - | - | - | - | - | yes | no valid output on the clean test signal (8 and 32 s) |
| inputhealth | SDR input health meter | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| intermod | Intermodulation finder | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| iono | Ionospheric sounders | lock | 19345703 | -24 | -24 | -24 | 0 / 16 | - | - | yes | SNR band too wide (test signal holds several carriers or spurs): not comparable; lock flag only, no checked unit |
| iridium | Iridium | blocks | 8601074 | -14 | -12 | -12 | 0 / 14 | - | - | yes |  |
| iscat | ISCAT | blocks | 3955 | -10 | - | - | 0 / 12 | - | - | no |  |
| isdbtsb | ISDB-TSB | blocks | 1275000 | 18 | 18 | - | 0 / 2 | - | - | yes |  |
| ism | ISM sensors | blocks | 34668 | 10 | 10 | 8 | 0 / 2 | - | - | yes |  |
| issdown | Space station | lock | 92529 | -24 | -24 | -24 | 0 / 16 | - | - | no | SNR band too wide (test signal holds several carriers or spurs): not comparable; lock flag only, no checked unit |
| its | 802.11p V2X | - | - | - | - | - | - | - | - | yes | no test signal or generator failed |
| jove | Radio Jove bursts | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| js8 | JS8Call | blocks | 142090 | -18 | -18 | - | 0 / 16 | - | - | yes |  |
| jt4 | JT4 | - | - | - | - | - | - | - | - | yes | no valid output on the clean test signal (8 and 32 s) |
| jt65 | JT65 | - | - | - | - | - | - | - | - | yes | no valid output on the clean test signal (8 and 32 s) |
| jt6m | JT6M | blocks | 594727 | -24 | -24 | - | 0 / 16 | - | - | no | SNR band too wide (test signal holds several carriers or spurs): not comparable; units only at the end of the trial: latency-limited |
| jt9 | JT9 | - | - | - | - | - | - | - | - | yes | no valid output on the clean test signal (8 and 32 s) |
| kaal | KAAL | blocks | 9727 | 14 | 14 | 12 | 0 / 2 | - | - | yes |  |
| kaft | KAFT | blocks | 176367 | 2 | - | - | 0 / 6 | - | - | yes |  |
| kait | KAIT | blocks | 168701 | 12 | 12 | - | 0 / 2 | - | - | yes |  |
| kapp | KAPP | blocks | 88440 | 2 | 2 | - | 0 / 6 | - | - | yes |  |
| kavb | KAVB | blocks | 765820 | 12 | 12 | - | 0 / 2 | - | - | yes |  |
| kaws | KAWS | - | - | - | - | - | - | - | - | yes | no valid output on the clean test signal (8 and 32 s) |
| klw | KLW | blocks | 7734 | 12 | 12 | 14 | 0 / 2 | - | - | yes | units only at the end of the trial: latency-limited |
| lbj | LBJ | blocks | 10078 | 4 | 6 | - | 0 / 8 | 2.7 | +1.3 | yes | units only at the end of the trial: latency-limited |
| ldacs | LDACS | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| link11 | Link 11 CLEW | blocks | 3906 | 6 | - | - | 0 / 4 | - | - | no |  |
| loraaprs | LoRa APRS | blocks | 122559 | -22 | -22 | -22 | 0 / 18 | - | - | no |  |
| loraraw | LoRa (raw) | blocks | 484375 | -16 | -16 | -12 | 0 / 18 | - | - | yes |  |
| lorasat | LoRa satellites | blocks | 124512 | -20 | -18 | - | 0 / 20 | - | - | yes |  |
| lorawan | LoRaWAN | blocks | 130371 | -8 | -8 | -2 | 0 / 14 | - | - | yes |  |
| lorawanwide | LoRaWAN (all channels) | blocks | 791992 | -16 | -16 | -12 | 0 / 18 | - | - | yes |  |
| ltpos | Lightning position (network) | lock | 79346 | 4 | 4 | 4 | 0 / 6 | - | - | no | lock flag only, no checked unit |
| ltr | LTR trunking | - | - | - | - | - | - | - | - | yes | no test signal or generator failed |
| lucky7 | Lucky-7 | lock | 5310 | -24 | -24 | -24 | 16 / 16 | - | - | no | SNR band too wide (test signal holds several carriers or spurs): not comparable; lock flag only, no checked unit |
| m17 | M17 | blocks | 10254 | 4 | 6 | 8 | 0 / 8 | - | - | yes |  |
| mac | MAC | blocks | 13740234 | 6 | 6 | - | 0 / 4 | - | - | yes |  |
| marine | Marine | - | - | - | - | - | - | - | - | yes | no valid output on the clean test signal (8 and 32 s) |
| marinevhf | Marine VHF | - | - | - | - | - | - | - | - | yes | no test signal or generator failed |
| marker | ILS marker beacons (75 MHz) | - | - | - | - | - | - | - | - | yes | no test signal or generator failed |
| mavlink | MAVLink (SiK) | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| mdc1200 | MDC-1200 | blocks | 3906 | 22 | 22 | 22 | 0 / 2 | - | - | no |  |
| mesh | Mesh (LoRa) | blocks | 248047 | -16 | -16 | -16 | 0 / 16 | - | - | yes |  |
| meshcom | MeshCom | blocks | 2000000 | -24 | -24 | -24 | 0 / 16 | - | - | no | SNR band too wide (test signal holds several carriers or spurs): not comparable |
| meshcore | MeshCore | blocks | 62500 | -12 | -12 | - | 0 / 14 | - | - | no |  |
| meshtastic | Meshtastic | blocks | 248535 | -20 | -20 | -18 | 0 / 20 | - | - | yes |  |
| meteor | Meteor-M LRPT | blocks | 96924 | 2 | 2 | - | 0 / 6 | - | - | yes |  |
| meteorhrpt | Meteor HRPT | blocks | 3559570 | -8 | -8 | -8 | 1170 / 12 | - | - | no | FALSE DECODES on noise |
| mfsk | MFSK | blocks | 1465 | 14 | - | - | 0 / 2 | - | - | yes |  |
| milhf | HF waveform ID | - | - | - | - | - | - | - | - | yes | no test signal or generator failed |
| milstd110 | MIL-STD-188-110 serial | blocks | 3906 | 2 | - | - | 0 / 6 | - | - | no | units only at the end of the trial: latency-limited |
| mioty | mioty | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| modeac | Mode A/C | blocks | 2824219 | 18 | 18 | 20 | 0 / 2 | - | - | yes |  |
| mpt1327 | MPT1327 trunking | - | - | - | - | - | - | - | - | yes | no test signal or generator failed |
| mradar | Marine radar | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| mscatter | Meteor scatter | lock | 1465 | -14 | -10 | - | 0 / 16 | - | - | yes | lock flag only, no checked unit |
| msgraw | MSG Raw Data | blocks | 4397461 | 4 | 4 | - | 0 / 6 | - | - | no |  |
| msk144 | MSK144 | - | - | - | - | - | - | - | - | no | no valid output on the clean test signal (8 and 32 s) |
| mt63 | MT63 | blocks | 222168 | -20 | - | - | 0 / 18 | - | - | yes | units only at the end of the trial: latency-limited |
| multitone | Multitone Paging | blocks | 1465 | 24 | 24 | 24 | 0 / 2 | - | - | yes | units only at the end of the trial: latency-limited |
| navic | NavIC | - | - | - | - | - | - | - | - | no | no valid output on the clean test signal (8 and 32 s) |
| navtex | NAVTEX | - | - | - | - | - | - | - | - | yes | no valid output on the clean test signal (8 and 32 s) |
| nbiot | NB-IoT cells | - | - | - | - | - | - | - | - | yes | no valid output on the clean test signal (8 and 32 s) |
| ndb | NDB | blocks | 938 | 6 | - | - | 0 / 4 | - | - | yes |  |
| nfc | NFC / RFID 13.56 MHz | blocks | 1696289 | 18 | 20 | 20 | 798 / 2 | - | - | yes | FALSE DECODES on noise |
| nfmdev | Narrowband FM deviation and CTCSS meter | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| ngham | NGHam | blocks | 6348 | 12 | 12 | 12 | 0 / 2 | 10.7 | +1.3 | yes |  |
| noaaapt | NOAA APT | blocks | 24250 | -24 | -24 | -24 | 221 / 16 | - | - | no | SNR band too wide (test signal holds several carriers or spurs): not comparable; FALSE DECODES on noise; units only at the end of the trial: latency-limited |
| noaadsb | NOAA DSB | blocks | 41750 | 6 | 4 | 22 | 77 / 6 | - | - | no | FALSE DECODES on noise; units only at the end of the trial: latency-limited |
| noaagac | NOAA GAC | blocks | 3384766 | -4 | -4 | -4 | 1021 / 10 | - | - | no | FALSE DECODES on noise |
| noaahrpt | NOAA HRPT | blocks | 3594727 | -8 | -8 | -8 | 0 / 12 | - | - | no |  |
| noisefloor | Noise floor logger | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| npr | New Packet Radio | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| nrf24 | nRF24L01 | blocks | 908203 | 10 | 10 | - | 0 / 2 | - | - | no |  |
| nxdn | NXDN | blocks | 8789 | 8 | 8 | 8 | 0 / 4 | - | - | yes |  |
| occbw | Occupied bandwidth meter | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| occupancy | Spectrum occupancy monitor | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| ogn | OGN tracker | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| olivia | Olivia | blocks | 2441 | -12 | - | - | 0 / 14 | - | - | yes | units only at the end of the trial: latency-limited |
| oneweb | OneWeb detector | blocks | 19775391 | -12 | -12 | -12 | 0 / 14 | - | - | no |  |
| ookpulse | OOK / ASK pulse analyser | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| opensky | OpenSky identifier | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| orbcomm | Orbcomm | blocks | 52490 | 0 | 0 | 0 | 0 / 8 | - | - | yes | units only at the end of the trial: latency-limited |
| p25 | P25 | blocks | 7324 | 8 | 10 | 10 | 0 / 4 | - | - | yes |  |
| p25p2 | P25 Phase 2 | - | - | - | - | - | - | - | - | yes | no test signal or generator failed |
| packet | APRS / Packet | blocks | 9277 | 10 | 10 | 8 | 0 / 2 | - | - | yes |  |
| pactor | PACTOR-I | blocks | 148926 | -18 | -18 | - | 0 / 16 | - | - | yes |  |
| pactor2 | PACTOR-II | - | - | - | - | - | - | - | - | yes | no valid output on the clean test signal (8 and 32 s) |
| pactor3 | PACTOR-III | - | - | - | - | - | - | - | - | no | no valid output on the clean test signal (8 and 32 s) |
| pactor4 | PACTOR-IV | lock | 3418 | -2 | - | - | 0 / 8 | - | - | no | lock flag only, no checked unit |
| pager | Pagers | blocks | 11230 | 2 | - | 6 | 0 / 8 | - | - | yes |  |
| papr | Crest factor (PAPR / CCDF) meter | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| passband | Passband signal classifier | - | - | - | - | - | - | - | - | yes | no test signal or generator failed |
| phasenoise | Phase noise meter | - | - | - | - | - | - | - | - | yes | no test signal or generator failed |
| pi4 | PI4 | blocks | 2783 | -18 | - | - | 0 / 16 | - | - | no | units only at the end of the trial: latency-limited |
| piccolo | Piccolo Mk VI | blocks | 1465 | -10 | -8 | - | 0 / 12 | - | - | no |  |
| plnoise | Power-line noise locator | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| pmr446 | PMR446 analog | blocks | 2441 | 24 | 24 | 24 | 0 / 2 | - | - | no |  |
| pocsag | POCSAG | blocks | 11230 | 0 | 2 | 4 | 0 / 10 | - | - | yes |  |
| psk | PSK31 | blocks | 241211 | -24 | -24 | - | 0 / 16 | - | - | yes | SNR band too wide (test signal holds several carriers or spurs): not comparable |
| pskd | PSK demodulator | lock | 2441 | 4 | 6 | 4 | 0 / 8 | - | - | no | lock flag only, no checked unit |
| pttlog | PTT and duty-cycle logger | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| pulsar | Pulsar folding | - | - | - | - | - | - | - | - | no | no valid output on the clean test signal (8 and 32 s) |
| q65 | Q65 | - | - | - | - | - | - | - | - | yes | no valid output on the clean test signal (8 and 32 s) |
| qi | Qi wireless charging | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| qo100 | QO-100 | lock | 8012695 | -12 | -6 | - | 0 / 16 | - | - | yes | lock flag only, no checked unit |
| qrss | QRSS grabber | - | - | - | - | - | - | - | - | no | no valid output on the clean test signal (8 and 32 s) |
| radar | Radar pulse analyser | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| radiojove | Radio Jove | lock | 1975098 | -24 | -24 | -24 | 16 / 16 | - | - | no | SNR band too wide (test signal holds several carriers or spurs): not comparable; lock flag only, no checked unit |
| rclink | RC and mesh links | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| rcppm | RC model PPM (72 MHz) | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| reticulum | Reticulum | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| rid | Drone Remote ID | blocks | 15390625 | -2 | 0 | - | 0 / 8 | - | - | yes |  |
| riometer | Riometer | - | - | - | - | - | - | - | - | no | no valid output on the clean test signal (8 and 32 s) |
| roshf | ROS (HF) | blocks | 2930 | -8 | - | - | 0 / 12 | - | - | no |  |
| rtty | RTTY | blocks | 1953 | -8 | - | - | 0 / 12 | - | - | yes |  |
| same | NOAA Weather Radio SAME | blocks | 9277 | 6 | 6 | 6 | 0 / 4 | - | - | yes |  |
| sarsat | COSPAS-SARSAT 406 MHz | blocks | 4188 | -4 | -4 | -4 | 0 / 10 | - | - | yes |  |
| satbeacon | TV sat beacon decoder | blocks | 643066 | -6 | -6 | -6 | 0 / 10 | - | - | yes |  |
| sband | S-band CCSDS | blocks | 4064941 | 6 | 6 | 6 | 0 / 4 | - | - | no |  |
| sbas | SBAS | blocks | 3955078 | -2 | - | - | 0 / 8 | - | - | yes |  |
| schumann | Schumann resonances | lock | 2543 | -24 | -24 | -24 | 16 / 16 | - | - | no | SNR band too wide (test signal holds several carriers or spurs): not comparable; lock flag only, no checked unit |
| selcal | SELCAL (aircraft tone call) | blocks | 4394 | 12 | - | - | 0 / 2 | - | - | no | units only at the end of the trial: latency-limited |
| sensors | Sub-GHz sensors | blocks | 38879 | 10 | 10 | 10 | 0 / 2 | - | - | yes | units only at the end of the trial: latency-limited |
| sferics | Sferics (lightning) | blocks | 1921387 | -6 | -4 | -4 | 0 / 10 | - | - | no | units only at the end of the trial: latency-limited |
| sfhss | Futaba S-FHSS | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| shortwave | Shortwave | lock | 27832 | -24 | -24 | -24 | 16 / 16 | - | - | no | SNR band too wide (test signal holds several carriers or spurs): not comparable; lock flag only, no checked unit |
| sigfox | Sigfox | blocks | 1000 | -2 | 0 | 0 | 0 / 8 | -1.6 | -0.4 | yes |  |
| sinad | FM SINAD meter | - | - | - | - | - | - | - | - | yes | no test signal or generator failed |
| sitor | SITOR | blocks | 1465 | -4 | - | - | 0 / 10 | - | - | yes |  |
| smartnet | Motorola Type II trunking | - | - | - | - | - | - | - | - | yes | no test signal or generator failed |
| sonde | Radiosonde | blocks | 9766 | 14 | 22 | 22 | 0 / 2 | - | - | yes | units only at the end of the trial: latency-limited |
| specan | Spectrum analyser | blocks | 15034180 | -24 | -24 | -24 | 624992 / 16 | - | - | yes | SNR band too wide (test signal holds several carriers or spurs): not comparable; FALSE DECODES on noise |
| speckurt | Spectral kurtosis RFI finder | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| specx | Spectrum explorer | lock | 3864746 | -8 | -8 | -8 | 0 / 12 | - | - | yes | lock flag only, no checked unit |
| spektrum | Spektrum DSM | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| spino | Spino | lock | 2624 | -24 | -24 | -24 | 16 / 16 | - | - | no | SNR band too wide (test signal holds several carriers or spurs): not comparable; lock flag only, no checked unit |
| spreadhf | SPREAD | - | - | - | - | - | - | - | - | no | no valid output on the clean test signal (8 and 32 s) |
| spur | Spur and image finder | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| ssbtest | SSB transmitter test meter | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| ssdv | SSDV pictures | blocks | 3418 | 12 | 14 | - | 0 / 2 | - | - | yes |  |
| ssrint | SSR interrogations (1030 MHz) | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| sstv | SSTV | blocks | 1953 | 4 | - | - | 0 / 6 | - | - | yes |  |
| stanag4285 | STANAG 4285 | lock | 3906 | 14 | - | - | 0 / 2 | - | - | no | lock flag only, no checked unit |
| stanag4481 | STANAG 4481 FSK | blocks | 2930 | -6 | -2 | 20 | 0 / 12 | - | - | no | units only at the end of the trial: latency-limited |
| stanag4539 | STANAG 4539 / 110C high rate | blocks | 3906 | 0 | - | - | 0 / 8 | - | - | no |  |
| starlink | Starlink detector | blocks | 12949219 | -4 | -2 | -2 | 0 / 12 | - | - | no | units only at the end of the trial: latency-limited |
| supersid | SuperSID (VLF) | lock | 241333 | -24 | -24 | -24 | 16 / 16 | - | - | no | SNR band too wide (test signal holds several carriers or spurs): not comparable; lock flag only, no checked unit |
| tdcs | TDCS | blocks | 7734 | 12 | 12 | 14 | 0 / 2 | - | - | yes |  |
| tdrs | TDRS carriers | lock | 1465 | -8 | -8 | -8 | 0 / 12 | - | - | no | lock flag only, no checked unit |
| tetra | TETRA | blocks | 21484 | 26 | 30 | 22 | 0 / 2 | - | - | yes | units only at the end of the trial: latency-limited |
| tetradmo | TETRA DMO | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| tetrapol | TETRAPOL | blocks | 6897 | 10 | 12 | - | 0 / 2 | - | - | yes | units only at the end of the trial: latency-limited |
| thor | THOR | blocks | 1465 | -8 | - | - | 0 / 12 | - | - | yes |  |
| thread | Thread | blocks | 2237305 | -2 | 0 | - | 0 / 8 | - | - | yes |  |
| throb | Throb | blocks | 3369 | -8 | -8 | - | 0 / 12 | - | - | no |  |
| tiantong | Tiantong signalling | blocks | 258789 | -10 | -10 | -10 | 0 / 12 | - | - | no |  |
| time | Time signals | lock | 188 | -2 | - | - | 0 / 8 | - | - | yes | lock flag only, no checked unit |
| tones | Tone decoders (CTCSS, DCS, DTMF, selcall) | blocks | 2441 | 10 | 10 | 12 | 0 / 2 | - | - | yes | units only at the end of the trial: latency-limited |
| tsikada | Tsikada / Parus | lock | 92529 | -24 | -24 | -24 | 0 / 16 | - | - | no | SNR band too wide (test signal holds several carriers or spurs): not comparable; lock flag only, no checked unit |
| txstability | Transmitter frequency stability meter | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| u482c | U482C (GomX-1) | blocks | 11230 | 14 | 14 | 14 | 0 / 2 | - | - | yes |  |
| uat | UAT (978 MHz) | blocks | 1120606 | 8 | 12 | - | 0 / 4 | - | - | yes | units only at the end of the trial: latency-limited |
| uhfrfid | UHF RFID reader (EPC Gen2) | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| uhfsatcom | UHF SATCOM skimmer | blocks | 58594 | -6 | -6 | -6 | 0 / 10 | - | - | no |  |
| uplink | Mode S uplink | blocks | 4973144 | 4 | 6 | 8 | 0 / 8 | - | - | yes |  |
| usp | USP (Sputnix) | blocks | 11230 | 10 | 10 | 10 | 0 / 2 | 11.6 | -1.6 | yes |  |
| uvb76 | UVB-76 buzzer monitor | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| vdes | VDES | blocks | 250732 | -6 | -6 | - | 0 / 10 | - | - | yes |  |
| vdl2 | VDL Mode 2 | blocks | 258789 | 2 | 2 | 4 | 0 / 6 | - | - | yes |  |
| vdl3 | VDL Mode 3 | blocks | 12451 | 14 | 14 | - | 0 / 2 | - | - | no |  |
| vdl4 | VDL Mode 4 | blocks | 67871 | 20 | 24 | 20 | 0 / 2 | - | - | yes |  |
| vlfmsk | VLF MSK transmitter monitor | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| voiceinv | Voice inversion scrambler | blocks | 5371 | 10 | 14 | 10 | 0 / 2 | - | - | no |  |
| vor | VOR | blocks | 20996 | 4 | 4 | - | 0 / 6 | - | - | yes |  |
| vorils | VOR / ILS | blocks | 20996 | 4 | 4 | - | 0 / 6 | - | - | yes |  |
| wb | Signal workbench | - | - | - | - | - | - | - | - | yes | no test signal or generator failed |
| wefax | Weather fax | lock | 1953 | 4 | - | - | 0 / 6 | - | - | no | lock flag only, no checked unit |
| wfbng | wfb-ng / OpenHD | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| whatsat | What satellite is this? | lock | 92529 | -24 | -24 | -24 | 0 / 16 | - | - | no | SNR band too wide (test signal holds several carriers or spurs): not comparable; lock flag only, no checked unit |
| whistler | Whistlers and tweeks | blocks | 47367 | -4 | - | - | 0 / 10 | - | - | no | units only at the end of the trial: latency-limited |
| winmor | WINMOR | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| wisun | Wi-SUN FAN | blocks | 9711914 | -10 | -10 | - | 0 / 12 | - | - | yes |  |
| wmbus | Wireless M-Bus (meters) | blocks | 756836 | 2 | 2 | - | 0 / 6 | - | - | yes |  |
| wmic | Wireless mic finder | lock | 1494141 | 2 | 2 | 2 | 0 / 6 | - | - | no | lock flag only, no checked unit |
| wradar | Weather radar | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| wspr | WSPR | - | - | - | - | - | - | - | - | yes | no valid output on the clean test signal (8 and 32 s) |
| xb_aqua | EOS DB (Aqua, Aura) | blocks | 9543457 | 8 | 8 | - | 0 / 4 | - | - | no |  |
| xb_dmsp | DMSP RTD | blocks | 1284668 | -12 | -12 | -10 | 0 / 16 | - | - | no |  |
| xb_gcomc | GCOM-C1 | blocks | 73806152 | 2 | 4 | - | 0 / 6 | - | - | no |  |
| xb_gcomw | GCOM-W1 | blocks | 12724609 | 4 | 4 | - | 0 / 6 | - | - | no |  |
| xb_jpss2 | JPSS HRD (NOAA-21) | blocks | 31279297 | 4 | 6 | - | 0 / 8 | - | - | no |  |
| xb_kanopus | Kanopus-V | blocks | 65808106 | 8 | 10 | - | 0 / 4 | - | - | no |  |
| xb_landsats | Landsat 8/9 S-band | blocks | 1357910 | 4 | 4 | 6 | 0 / 8 | - | - | no |  |
| xb_landsatx | Landsat 8/9 X-band | blocks | 229101562 | 2 | 2 | - | 0 / 6 | - | - | no |  |
| xb_metopd | MetOp X-band dump | blocks | 36979980 | 10 | 12 | - | 0 / 2 | - | - | no |  |
| xb_metopsg | MetOp-SG DDB | blocks | 112587891 | 4 | 4 | - | 0 / 6 | - | - | no |  |
| xb_npp | JPSS HRD (NPP, NOAA-20) | blocks | 18988037 | 4 | 4 | - | 0 / 6 | - | - | no |  |
| xb_oceansat2 | Oceansat-2 DB | blocks | 26805420 | 4 | 4 | - | 0 / 6 | - | - | no |  |
| xb_oceansat3 | Oceansat-3 L-band | blocks | 1273682 | 2 | 4 | - | 0 / 6 | - | - | no |  |
| xb_oceansat3x | Oceansat-3 X-band dump | blocks | 93164062 | 10 | 10 | - | 0 / 2 | - | - | no |  |
| xb_probas | Proba S-band dump | blocks | 2543945 | -2 | 0 | 0 | 0 / 8 | - | - | no |  |
| xb_probav | Proba-V X-band dump | blocks | 37067871 | 6 | 10 | - | 0 / 4 | - | - | no |  |
| xb_saral | Saral L-band | blocks | 509033 | 4 | 4 | 12 | 0 / 6 | - | - | no |  |
| xb_saralx | Saral X-band dump | blocks | 19805908 | 4 | 4 | - | 0 / 6 | - | - | no |  |
| xb_stereo | STEREO high rate | blocks | 1710205 | -2 | -2 | -2 | 0 / 8 | - | - | no |  |
| xb_terra | EOS DB (Terra) | blocks | 16688232 | 0 | -2 | - | 0 / 8 | - | - | no |  |
| xlinknb | Extra links, narrow | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| xpa | XPA (10 / 20 Bd) | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| xpa2 | XPA2 | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| yfactor | Noise figure (Y-factor) meter | - | - | - | - | - | - | - | - | no | no test signal or generator failed |
| zigbee | Zigbee / 802.15.4 | blocks | 2237305 | -2 | 0 | - | 0 / 8 | - | - | yes |  |
| zombie | Zombie satellites | lock | 92529 | -24 | -24 | -24 | 0 / 16 | - | - | no | SNR band too wide (test signal holds several carriers or spurs): not comparable; lock flag only, no checked unit |
| zwave | Z-Wave | blocks | 1518555 | -6 | -2 | - | 0 / 12 | - | - | yes |  |


### Modes measured by their unit tests only (no engine test signal)

Noise sweeps printed by the unit tests on the GitHub runner (quick.yml runs 43376327883, 43383912499; SNR in the band each test names).

| mode | band | 90 % point | 50 % point | false | source |
|---|---|---|---|---|---|
| FSK modems, Bell 103 300 Bd (24 bytes) | 3 kHz | **6 dB** (4.0: 14 dB) | 3 to 6 dB (4.0: about 10 dB) | 0 (20 s + 4 x 120 s noise, one tone, a non-standard tone pair) | test_x40_fskmodem |
| FSK modems, Bell 202 1200 Bd | 3 kHz | 10 dB, 65 of 72 bytes (4.0: 14 dB) | 6 to 10 dB (4.0: about 10 dB) | 0 | test_x40_fskmodem |
| DCR (6.25 kHz) | 6.25 kHz | 6 dB, 16 of 16 calls | about 5 dB (9 of 16) | 0 wrong calls | test_x40_dcr |
| GW FSK 100 Bd | 3 kHz | 6 dB | 2 to 4 dB | 0 | test_x40_gwfsk |
| SSR interrogations (Mode A) | pulse amplitude / noise rms | 12 dB (1098 of 1200) | 9 to 12 dB | 0 wrong modes | test_x40_ssrint |
| ILS markers | 12 kHz C/N | 8 dB (4 of 4 named) | 0 to 4 dB | 0 wrong names | test_x40_marker |
| Rivet modes (XPA, XPA2, CROWD36, FSK200/500, FSK200/1000) | 3 kHz | see RIVET-MODES-4.0.md (XPA2 -14.5 dB ... FSK200/500 +1.3 dB) | | 0 | test_x40_xpa etc. |

Theory for these: Bell 103 noncoherent FSK, 300 b/s, 240 bits at 90 %: Eb/N0 11.5 dB, 1.5 dB in 3 kHz (gap now 4.5 dB, was
12.5 dB); Bell 202 1200 b/s: 7.5 dB in 3 kHz (gap 2.5 dB); DCR (NXDN physical layer, K = 5 code): the NXDN receiver's 9 dB in
6.25 kHz (sens_sweep) is the reference, DCR is at or under it.

## Fixes (item 3) with before / after

| mode | change | before | after | false decodes |
|---|---|---|---|---|
| FSK modems (Bell 103 / V.21 / V.23 / Bell 202) | the burst gate at 3 x the floor (was 8 x); the band, sideband and tone-share gates on the power over the noise (they used the total power, so the noise in the empty sideband rejected every burst under about 8 dB in 3 kHz) | Bell 103: 10 dB 2 of 3, 6 dB 0 of 3; Bell 202: 10 dB 2 of 3, 6 dB 0 | Bell 103: 6 dB 3 of 3 (all 72 bytes); Bell 202: 10 dB 3 of 3 (65 / 72 bytes) | 0 in 20 s + 4 x 120 s of noise (two levels), one tone, a non-standard tone pair, a 600 baud burst; Bell 202 at 6 dB gave one message with the bytes shifted (0 of 72 in place) |
| DCR | tried: the sync search taking one wrong sync symbol of ten | 6 dB 16 / 16, 5 dB 9 / 16, 0 wrong | 6 dB 14 / 16, 5 dB 12 / 16, 1 wrong call, a scan case lost | **reverted** |

The other modes furthest from theory, and why they were not changed in this pass:

- **G-TOR 300 Bd** (+9 dB to the uncoded bound): 200 Hz shift at 300 Bd is a modulation index of 0.67 and the noncoherent tone
  detector loses most of that (SENSITIVITY-3.9-D.md found the same). The gain needs a coherent / sequence detector of the CPFSK; not
  done in this pass.
- **MDC-1200** (22 dB in its 3.9 kHz band, about 17 dB C/N in 12.5 kHz): the MDC decoder is only fed while the voice squelch is open
  (chv_rx.cpp `emit`: `if (mdc_ && b.gate)`), and the default squelch opens at 5 dB of quieting. Feeding it always (its own sync and
  CRC decide) is the likely fix; not verified in this pass, so not committed.
- **HF packet 300 Bd** (+4.7 dB): the same non-orthogonal shift (200 Hz at 300 Bd) as G-TOR, partly physics.
- **ATIS** (+2.4 dB), **NGHam**, **LBJ** (+1.3 dB): within the 2 dB resolution of this table plus the FM and code effects the bound does not count.
- **TETRA** (26 dB here, about theory in its own sens test): the trials that fail are not SNR-driven (a 22 dB point passed both trials, a
  24 dB point failed one): the threaded receiver on a loaded runner. Not a sensitivity loss.
- **MFSK, Olivia/MT63/keyboard modes, ALE, HFDL, CIS, KAIT, multitone, Polish time code (eczas)**: units arrive at the end of the 8 s file
  (long interleavers, minute frames): the threshold is limited by the trial length; the decoder-alone sweeps (SENSITIVITY-3.9-D.md) are the figures.

False decodes found by the noise-only runs (blocks counted on noise alone, not lock flags):
- **nfc** (798 units in 2 noise trials): known, qa_known.inc says its counter has no check (picture lines / measurements).
- **noaaapt** (221 units in 16): APT counts picture lines, which carry no check (the sensData worker has APT).
The lock-only monitors (callisto, erminaz, radiojove, shortwave, beaconevents) lock on noise by design: their lock flag is "something is
there", not a decode.
