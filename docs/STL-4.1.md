# STL link (4.1, issue #497)

Studio-to-transmitter (STL) links carry a broadcast station's programme from the studio to the transmitter site. This mode (x40 variant `Stl`, family 785) receives the **unencrypted analogue** ones and plays their sound; digital links are recognised and measured, not decoded. Receive only. **Everything below was checked on simulated signals only**; no recording of a real STL was available.

## What was researched

| Link type | What is on the air | What the mode does |
|---|---|---|
| Analogue composite STL (Moseley PCL-6000 class, Marti class, other "composite" STLs) | Wideband FM carrying the station's whole stereo multiplex: mono sum, 19 kHz pilot, stereo difference around 38 kHz, RDS on 57 kHz, SCA on 67 / 92 kHz. About +-60 to +-75 kHz deviation for full modulation, in 250 or 500 kHz channels; US Part 74 band 944 - 952 MHz, with similar bands elsewhere | Decoded: FM demodulation, pilot PLL, stereo matrix, 75 / 50 us de-emphasis, RDS, SCA listening |
| Analogue mono / discrete STL | FM carrying programme audio (mono, or left and right as two links) with no pilot | Decoded as mono; the de-emphasis can be set flat |
| Digital STL (Moseley Starlink, Intraplex / GatesAir and other QAM or OFDM links; IP links over microwave) | Wide, noise-like, flat spectrum; linear PCM or AAC inside proprietary frames; encryption is an option | Not decoded: no public specification, often encrypted. The mode names them "digital-unknown" and shows the occupied width and the level over the floor |

## How it works

The sound comes from the broadcast FM receiver (`FmReceiver`: channel filter, discriminator, 19 kHz pilot PLL, L+R / L-R matrix, de-emphasis, RDS, SCA), run with link settings (`core/include/dect2/x40/stl_cfg.h`):

- deviation for 100 % modulation (default 60 kHz; sets the sound level, `FmReceiver::setReferenceDeviation`, added for this mode: sound output only, the measurements are unchanged);
- channel width (default 280 kHz, any width the capture carries);
- de-emphasis 75 us (default, US), 50 us or flat;
- sound: automatic (stereo when the pilot is locked), composite stereo, mono;
- listen to the programme or to SCA 67 / 92 kHz;
- RDS or US RBDS meanings (default RBDS).

One click needs none of them: tune the channel centre, 1 Msps. The settings are under "Link settings (advanced)" in the Decoder panel, with the sound meters and volume above them.

"What is this" (`facts`, headline, one `Link` row per change that holds for two half-second reports):

- **composite**: an FM carrier (constant envelope) with a locked 19 kHz pilot;
- **mono**: an FM carrier without a pilot (mono programme audio, or a mono multiplex: the two cannot be told apart);
- **digital-unknown**: no FM carrier, the channel 6 dB or more over the floor beside it, flat across the channel (10th percentile of the bins within 3 dB of the mean) and at least 60 kHz wide;
- **unknown, not FM**: over the floor but not flat or too narrow;
- **nothing**: noise.

The comparison with the floor needs a capture wider than about 1.75 times the channel width; otherwise the digital check is off and the readout says so. A weak FM link below the carrier detector's threshold can read "unknown, not FM".

## Tests (`tests/x40/test_x40_stl.cpp`, ctest `x40_stl`)

Generated with the broadcast FM test transmitter (`FmGenerator`): composite at +-60 and +-75 kHz deviation with RDS and 75 us pre-emphasis, carrier offsets 0, +25 and -40 kHz, 1 and 2 Msps, 280 and 500 kHz channel settings; mono at +-40 kHz with offsets 0 and +30 kHz; noise only at four levels and three seeds (no detection allowed, no `Link` row); band-limited Gaussian noise as a digital link at 10 and 20 dB over the floor; a C/N sweep (6 - 40 dB in 200 kHz, 4 seeds each) printing SINAD of the 1 kHz tone and the share of runs in which RDS decoded, from which the 50 % and 90 % points are read.

## Channel width and centre (automatic)

The channel filter follows the link (`autoWidth`, on by default; manual width under "Link settings (advanced)"). The carrier offset is taken from the occupied region of the capture's spectrum and removed first. The width is Carson's rule, 2 x (measured peak deviation + highest multiplex frequency), with 60 kHz for the multiplex (RDS) and 99 kHz when an SCA subcarrier is present, or the occupied width of the capture if that is more, times 1.35 plus 10 kHz. It starts at 400 kHz, grows at once, settles once at 1.5 s, and narrows later only after 30 s of asking: every change leaves the filter empty and puts a click in the sound.

Two things found by testing: a margin of 10 % left a distortion floor (the tails of the FM spectrum cut by the filter), and a width change inside the measurement window did the same. Neither was a measurement fault of the harness.

## Results (CI run 43393788824, linux, simulated signals only)

| Case | SINAD of the 1 kHz tone | Reading |
|---|---|---|
| composite +-60 kHz, offset 0 / +25 / -40 kHz | 54.2 / 54.1 / 54.1 dB | composite stereo, pilot 5.4 kHz, RDS PS read, width 270 - 288 kHz |
| composite +-75 kHz, offset 0 / +25 / -40 kHz | 55.9 / 56.0 / 56.0 dB | composite stereo, RDS PS read in all three (before the automatic width the +25 kHz case lost RDS), width 294 - 295 kHz |
| mono +-40 kHz, offset 0 / +30 kHz | 73.1 / 72.6 dB | mono (no pilot) |
| noise only, 4 levels x 3 seeds | | 0 false detections in 12 runs, no Link rows |
| digital-like noise, 10 / 20 dB over the floor | | digital-unknown, width read 228 / 238 kHz (true 220) |

Carrier offsets were read to within 10 Hz. The peak-deviation reading follows the deviation (36.3 kHz at the +-60 setting, 45.2 kHz at +-75: the test transmitter's composite does not reach the nominal figure).

**C/N sweep** (C/N in 200 kHz; 5 seeds per point, stereo as the receiver blends it):

| C/N | 12 | 14 | 16 | 18 | 20 | 22 | 24 | 26 |
|---|---|---|---|---|---|---|---|---|
| RDS decoded | 0 % | 0 % | 100 % | 100 % | 100 % | 100 % | 100 % | 100 % |
| SINAD median (dB) | 27.9 | 32.6 | 31.2 | 30.8 | 30.9 | 31.4 | 33.4 | 35.4 |

RDS 50 % point and 90 % point: both 16 dB C/N (2 dB steps; the step from 0 % to 100 % lies between 14 and 16 dB). The stereo SINAD is not a monotone function of C/N: below about 20 dB the receiver blends towards mono, above it the stereo difference channel's noise (about 20 dB worse than mono) dominates, so the figure falls to a minimum near 18 - 20 dB and then rises.

**Mono sound against FM theory** (the receiver forced to mono, same composite; theory: the frequency noise of a carrier at C/N, f^2 N0/C, through 75 us de-emphasis over 15 kHz, against the measured tone of 26.6 kHz peak):

| C/N (dB) | 10 | 14 | 18 | 22 | 26 | 32 |
|---|---|---|---|---|---|---|
| measured SINAD (dB) | 41.5 | 45.7 | 50.0 | 54.0 | 58.1 | 64.0 |
| FM theory (dB) | 40.3 | 44.3 | 48.3 | 52.3 | 56.3 | 62.3 |

The measured values rise 1 dB per dB of C/N and lie 1.2 - 1.8 dB above theory. The earlier plateau at 31 - 35 dB came from the stereo decoder and from the two faults above, not from the C/N scale.

Not validated: any real STL recording, real RF impairments (multipath, adjacent-channel interference), real digital STLs, SCA listening, the sound output itself (the tests read the audio tap, silent).
