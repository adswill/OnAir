# FengYun geostationary: FY-4A / FY-4B LRIT, FY-4A HRIT-II/III, FY-2 S-VISSR

Receive only. Four modes in "Satellite and navigation", one receiver (`FygeoReceiver`) behind them. Code: `core/src/fygeo/`, screens: `app/fygeo/`, tests: `tests/fygeo/`, `tests/app/test_fygeo_mode.cpp`. Numbers: docs/MODE-NUMBERS.md.

| Mode | Family | Frequency | Signal | Needs |
|---|---|---|---|---|
| FY-4 LRIT, FY-4A | 232 (satellite choice) | 1697 MHz | DVB-S2, QPSK 2/5, 90 ksym/s, roll-off 0.25 | dish about 1 m, LNA, 1.7 GHz filter |
| FY-4 LRIT, FY-4B | 232 (satellite choice) | 1697 MHz | DVB-S2, QPSK 8/9, 120 ksym/s | dish about 1 m, LNA, filter |
| FY-4A HRIT-II/III | 233 | 1679 MHz | DVB-S2, QPSK 5/6, 1 Msym/s | dish 1.2 m or more, LNA, filter |
| FY-2 S-VISSR | 234 | 1687.5 MHz | BPSK (NRZ-M, no error correction), 660 ksym/s, roll-off 0.5 | dish about 1.2 m, LNA, filter |

FY-4A sits at 104.7 °E, FY-2G at 105 °E and FY-2H at 79 °E; the screens do not claim more (FY-4B drifts; check where it is). Everything is received, nothing is sent.

## What is decoded

**FY-4 (LRIT, HRIT-II/III).** The DVB-S2 receiver of the DVB-S/S2 mode does the demodulation, LDPC and BCH with the link's symbol rate (found by itself: it only needs the rate, set from the link or typed in the tuner). The base band frames carry MPEG transport stream packets; on one PID (3004, 3000 and 3002 in the receivers SatDump documents, any PID here) a payload of about 1064 bytes holds a 40 byte IP/UDP style header and then a 1024 byte CADU (`1ACFFC1D`, a CCSDS AOS VCDU of 1020 bytes: 6 byte header, 2 byte M_PDU header, 1012 bytes of packet zone, no Reed-Solomon). The CADUs go through the xRIT layers of the GOES receiver (`goes_proto.h`, packet zone 1012, CRC not checked as the DVB-S2 code has done the checking): CP_PDUs, files, in FY-4's header dialect (`fygeo_proto.h`): primary header, image information (type 1: satellite and instrument name, bits a pixel, columns, lines, channel, segment numbers and first line), navigation (type 2), annotation (the product's file name), time stamp, key (non-zero: encrypted, kept as received and counted, never decoded). Pictures are JPEG 2000 code streams (OpenJPEG): every segment is decoded and put into its picture (per satellite, channel, region and time), 8 bit for the screen (12 and 14 bit data are shifted down), with the rows received and the segments done. Text and admin files are listed. The lists show the products as they arrive; Save writes PNG files into `<data folder>/fengyun/`.

**FY-2 S-VISSR.** Matched filter (root raised cosine 0.5), squared-signal carrier search, Gardner timing, Costas loop, NRZ-M decoding (a 1 is a change of level, so the 180 degree ambiguity does not matter). A frame is a 10000 bit PN preamble (15 bit LFSR, x^15 + x^14 + 1, ending in a known 64 bit window) and 354848 bits (44356 bytes) scrambled with the continuation of the same sequence. The receiver follows the PN sequence with a free-running copy of the generator (which only joins the stream when the stream stops agreeing with it), finds the end of the preamble from that clean copy, so a bit error ratio of 25 % in the channel still gives frame sync, and checks the 10000 preamble bits against the known ones: that is the bit error ratio shown, measured on 10000 bits a frame. Frames are descrambled and the scan line is cut into IR1 10.8 um, IR2 12 um, IR3 6.75 um (water vapour), IR4 3.75 um (8 bit; IR4 is 10 bit on the air) and the visible channel (6 bit, four lines a frame, kept at half resolution so a picture stays about 23 MB). A picture is 2500 lines and takes about 25 minutes to scan; the five channels grow line by line in the list. The line counter starts a new picture when it wraps. The bit error ratio of the picture data is also estimated from the all-zero spare field of the frame.

## Easy to use

One click: pick the mode, press Start. The receiver finds the carrier anywhere in the capture (FY-4: the DVB-S2 spectrum analysis; S-VISSR: the squared-signal line), the symbol rate follows the link (type another in the tuner if a satellite changes it, there is no limit), and sample rates from 0.25 Msps (FY-4 LRIT is interpolated up to the 2 Msps the DVB-S2 receiver wants) or 1.0 Msps (S-VISSR needs about 1.4 samples a symbol) up to 20 Msps and beyond work. The Receiver tab shows the lock stages (spectrum, timing, carrier, frames, LDPC/BCH, transport stream) and "Why nothing decodes yet" in plain words: no carrier (dish, LNA, band), carrier but no frames (wrong link or too weak), frames but no data units (wrong link, encrypted or other PID layout), or a bit error ratio that is high. The screens say what dish and LNA the link needs.

## Sensitivity

Measured with `tools/sens` on the built-in test signals (AWGN, Es/N0 at the symbol rate; the threshold is the lowest Es/N0 at which at least 90 % of the trials gave at least half as many valid units as a clean run). The ideal figures are the quasi error free thresholds of ETSI EN 302 307-1 table 13 (normal frames, no pilots).

| Link | Threshold Es/N0 (dB) | Ideal (dB) | Lock time at threshold |
|---|---|---|---|
| FY-4A LRIT (QPSK 2/5, 90 ksym/s) | 0 | -0.3 | 2.3 s |
| FY-4B LRIT (QPSK 8/9, 120 ksym/s) | 7 | 6.2 | 1.8 s |
| FY-4A HRIT-II/III (QPSK 5/6, 1 Msym/s) | 6 | 5.2 | 0.8 s |
| FY-2 S-VISSR, frame sync | -2 | (sync only) | 1.0 s |
| FY-2 S-VISSR, bit error ratio under 2 % | 5 | 4.3 | 1.0 s |

No false lock on noise only in any of the five (12 trials, 1 dB steps).

S-VISSR has no code: the bit error ratio against Es/N0 matches coherent differential BPSK, `2 Q(sqrt(2 Es/N0)) (1 - Q(...))`, to within the noise of the measurement (6 dB: 4.7e-3 measured, 4.8e-3 theory; `tests/fygeo/test_fygeo_rx.cpp`).

## Faults of real radios and paths

`tests/fygeo/test_fygeo_impair.cpp` runs both receivers through: carrier offsets off the centre of the capture (up to -2 MHz at 20 Msps), 20 ppm radios and clocks, a drifting carrier (300 Hz/s S-VISSR, 100 Hz/s FY-4), DC offset (30 % of the rms), I/Q gain and phase imbalance (1.5 dB, 8 degrees), phase noise (200 Hz linewidth for S-VISSR, 15 Hz for FY-4 LRIT-A: the DVB-S2 receiver loses frames beyond that at 90 ksym/s), slow flat fading and all of them together, and sample rates from 0.5 to 20 Msps. The `tools/sens` scenarios (cfo2ppm, cfo20ppm, offcentre, sro50ppm, echo, tu6, rician, phasenoise, combined) are in the baseline.

## Test signals

`makeFy4LritSynth` (SynthConfig::modeOpt[0]: 0 FY-4A, 1 FY-4B), `makeFy4aHritSynth`, `makeFy2SvissrSynth` (`fygeo_gen.h`), SynthConfig: snrDb (Es/N0), cfoHz, sroPpm, modeVal[0] (symbol rate). FY-4: two AGRI channels (C02, C12) of a full disk picture in two 12 bit JPEG 2000 segments each, a text message, a new picture every round, through the real layers into the DVB-S2 transmitter chain of the DVB-S/S2 mode. FY-2: a test picture on eight scan lines, each behind its PN preamble, scrambled, NRZ-M coded, BPSK. `--mode fy4alrit` (also `fy4blrit`, `fy4ahrit`, `fy2svissr`) with the synthetic source plays them.

## Not verified

No recording of a FengYun signal was available (the IQ search was limited): only the generated signals. What is taken from public descriptions and not checked on air: the FY-4 header record layout and the 40 byte header in front of the CADU in the transport stream (the extractor looks for the sync word in the first 128 bytes of the payload, so another header length still works), which PIDs carry the CADUs, the FY-4B HRIT link (not listed in any public decoder; type its symbol rate in the tuner and the DVB-S2 receiver finds the rest), the FY-2 frame details beyond the PN preamble, the line counter, the picture blocks and the spare field (all from SatDump's reader), the sense of the IR counts (shown inverted as SatDump does) and the channel order. The orbit, attitude and calibration blocks of S-VISSR are not decoded: the pictures are raw counts. FY-4 pictures are not calibrated or projected. Encrypted FY-4 files (some LRIT products) are kept but not decoded.

## Credits

The file layouts and constants of FY-4 xRIT and FY-2 S-VISSR were read from SatDump (GPL-3.0, Aang23 and contributors); the receiver, the framing code, the generators and the tests are new. OpenJPEG 2.5.3 (BSD-2-Clause) is bundled in `third_party/openjpeg`. Both are in THIRD_PARTY.md and the Credits window.
