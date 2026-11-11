# DVB-C2 (3.9)

Second generation digital cable television, ETSI EN 302 769 V1.3.1. Mode row **DVB-C2** (family 741, engine standard 743, active standard
742), branch `feat39-dvbc2`. Everything below was checked against the generator in the same code only: no DVB-C2 recording and no cable
network were available. **Simulated signals only.**

## What is implemented

Shared (`core/include/dect2/dvbc2/dvbc2.h`, `core/src/dvbc2/dvbc2_frame.cpp`, `dvbc2_bicm.cpp`, `dvbc2_l1.cpp`):

- OFDM numbers of the 8 MHz raster (clause 10, table 38): 4K FFT, 64/7 Msample/s, carrier spacing 2 232 Hz, guard intervals 1/64 and
  1/128, the C2 frame of L_P preamble symbols and 448 data symbols, the L1 block of 3 408 carriers.
- Reference sequences (clause 9.2): the data scrambling PRBS w_k (X^11 + X^2 + 1), the pilot PRBS w'_i (X^10 + X^3 + 1) and the
  differential pilot reference r_k; the carrier indices are absolute (multiples of the carrier spacing from 0 Hz), as the standard
  defines them, so the pilots, the scrambling and the L1 block boundaries depend on where the signal sits in the cable spectrum.
- Pilots (clauses 9.3.3 and 9.6): preamble pilots on every 6th carrier with the amplitude of table 38 (4/3 and 6/5), scattered pilots
  (D_X 12 or 24, D_Y 4), the 30 continual pilots of table 35, edge pilots at the system edges and either side of a notch, amplitude 7/3.
- Reed-Muller (32,16) coding of the FECFrame header (clause 7.2.2) and of the preamble header (clause 8.2): generator matrix, the cyclic
  delay and scrambling of the lower branch, the robust QPSK form (32 cells) and the high efficiency 16QAM form (16 cells), a soft decoder
  over the 1 024 second-order words with a fast Hadamard transform for the first-order part.
- L1 signalling part 2 (clause 8.3, table 18): all fields of the data slice, PLP and notch loops with the guard interval dependent
  widths (13 / 14 and 8 / 9 bits), the block padding, CRC-32, the segmentation into 16K LDPC blocks (8.4.2), their shortening,
  puncturing and 16QAM mapping (8.4.3 - 8.4.4; this is DVB-T2's L1-post code, now exposed by `t2l1.h` as `encodeL1Block` /
  `decodeL1Block` with the C2 rounding rule), the cyclic repetition over the 2 840 cells of the L1 block and the L1 time interleaving
  modes (8.5: none, best fit, 4 and 8 symbols).
- Bit-interleaved coded modulation of the PLPs (clause 6): the FEC parameters of tables 3(a) / 3(b), BCH (t = 8, 10, 12), the LDPC
  codes (DVB-T2's tables for 2/3 .. 5/6, DVB-S2's for 8/9 and 9/10: annexes A and B of EN 302 769 list the same addresses), the
  parity and column-twist bit interleaver with the demultiplexer (tables 6, 7, 10) and 16 / 64 / 256 / 1024 / 4096-QAM, the valid
  ModCods of table 11 (including 4096-QAM 4/5 of V1.3.1).
- Data Slice Packets (clause 7): Type 1 (one PLP, the PLP_START pointer in the L1) and Type 2 (FECFrame headers, one or two FECFrames
  per header, several PLPs, stuffing packets skipped).
- The data slice time interleaver (clause 9.4.4: twisted row-column over D_SLICE_TI_DEPTH symbols, pilot positions reserved) and the
  4K frequency interleaver (clause 9.4.5: DVB-T2's 4K address generator, which table 32 matches).
- BB frames in normal mode with the CRC-8 chaining of transport stream packets (clause 5, DVB-T2's `BbUnpacker`).

Generator (`dvbc2_gen.h/.cpp`): a C2 system of up to one L1 block width (3 409 carriers, 7.61 MHz) centred on a chosen absolute carrier,
with its data slices, PLPs (each with its own packet source) and notches, L1 part 2 in every frame, the absolute-frequency phase
convention of clause 10.1, then an echo, the sample clock error (exact resampler), the carrier offset and noise, at any output rate.
It is the mode's test signal (`makeDvbc2Synth`, options in `dvbc2_gen.h`) and plays the demo transport stream in the TV player.

Receiver (`dvbc2_rx.h/.cpp`): guard interval correlation (symbol timing, the guard interval, the fractional carrier offset);
the preamble found by its pilot structure (no boosted scattered pilots), the absolute carrier index of the window from the differential
pilot reference (129 candidates around the tuned frequency, or a search over the whole (k mod 3408, k mod 2047) space when the centre
frequency is not known); the FFT window re-centred in the guard interval from the preamble pilot slope; preamble channel estimate,
preamble header, L1 part 2 (the copies of the L1 block combined by inverse variance); on the data symbols the scattered, continual and
edge pilots give the channel (averaged over symbols, windowed-sinc interpolation across the D_X grid centred on the window delay), the
common phase and the timing slope (first-order loops: the phase and slope go into the oscillator and the window in full), the sample
clock error (a 24-tap corrector at the input rate when the input is at least 1.07 times the native rate); the chosen data slice is
equalised, frequency and time de-interleaved, split into Data Slice Packets, LDPC / BCH decoded, and its BB frames become transport
stream packets for the TV player. The L1 is decoded again every frame. Telemetry: state, Es/N0 from the pilots, MER of the decoded
cells, the L1, the PLP table, block and packet counts, the window carrier index, the carrier and clock offsets.

Engine wiring: active standard 742 in `engine.cpp` (dispatch, reset), `engine_place.cpp`, `engine_start.cpp`, `engine_tune.cpp`,
`engine_telemetry.cpp` (`RxTelemetry::dvbc2`), `modes.cpp` (tuning, test signal, summary), mode row 741 in `app/ui/mode_table.cpp`,
`dvbc2` in the web policy's broadcast list, TV family in `app/autodet`. Tuning: 47 - 1002 MHz, default 346 MHz, 10 Msample/s with an
8 MHz baseband filter, at least 8 Msample/s.

## Added in branch feat39-bcast2

- **1024-QAM and 4096-QAM** (`dvbc2_bicm.cpp`). The bit interleaver of tables 6 and 7 has 20 columns for 1024-QAM (3 240 / 810 rows),
  and 12 (64K) or 24 (16K) columns for 4096-QAM. The column twists are those of tables 7(a) and 7(b), and the demultiplexer is table 10.
  The 1024-QAM words of 20 bits make two cells, as do the 16K 4096-QAM words of 24 bits; the 64K 4096-QAM words of 12 bits make one.
  The constellations follow tables 12(j) - 12(m). On each axis the first bit is the sign, and the others are the reflected Gray code
  of the distance from the outermost point. The cells are normalised by sqrt(682) and sqrt(2730) (table 13). The same rule
  reproduces DVB-T2's 16 / 64 / 256-QAM tables exactly, and the test checks that. The demapper is max-log per axis over the 32 / 64
  levels. DVB-T2's shared code is not changed. The test signal has two more options: 6 is 1024-QAM 3/4 and 7 is 4096-QAM 5/6.
  The tables were read from the ETSI PDF text: the twist values row by row, the demultiplexer from table 10(a), which is the same
  for every code rate. The 1024-QAM labels were checked against the first 16 levels of table 12(j) and the 4096-QAM ones against
  table 12(l). A round trip cannot show a wrong interleaver table (both ends would use the same one), so these values rest on that
  reading only.
- **GSE** (payload type 2, BB header TS/GS = 10). The receiver cuts the data field of each BB frame out and gives it to the GSE decoder
  that DVB-S2 already uses (TS 102 606: whole, first, middle and last packets, fragment reassembly with the CRC-32, labels). The IP
  packets come out of `Dvbc2Receiver::setIpCallback`, and the telemetry counts `ipPackets` and `gseFields`. A GSE PLP gives no
  transport stream. The generator takes IP packets per PLP (`PlpConfig::ip`) and encapsulates them, cutting them across BB frames.
  GSE PLPs feed the engine's IP output (pcap file and UDP forward, see docs/BCAST-3.9.md) like DVB-H datagrams do.
- **Reserved tones** (clause 9.7). When the L1 sets RESERVED_TONES, the carriers of table 36 (288 per 8 L1 blocks), shifted by
  D_X (l_d mod D_Y), carry no data in the data symbols. They are kept out of the data cells and the time interleaver memory, and out
  of notches. A pilot stays a pilot; none of them falls on a scattered pilot, which the test checks. The generator fills them with a
  random phase at 0.8 of the pilot amplitude.
- **Fixed: the noise estimate after every preamble.** The preamble's noise estimate (from neighbouring pilot differences) replaced
  the data symbols' estimate at every frame. It also contains the channel's curvature and the phase step of the window delay, so at
  good SNR it was some 16 dB too high (0.46 against 0.012 in one case). For about 20 symbols after each preamble, the LLRs of the data
  cells were then far too small. 256-QAM did not notice. A 4096-QAM 9/10 block that straddled the frame start failed at every frame,
  even without noise. The preamble's estimate is now used only until the data symbols have one of their own (and again after a
  re-sync or a window move).

## Tests (`tests/dvbc2/test_dvbc2.cpp`, target `test_dvbc2`)

- The PRBS sequences (first values and periods of clause 9.2, the differential property of r_k), continual and scattered pilot
  positions, the preamble cell / carrier mapping, the time interleaver write order.
- Reed-Muller (32,16): minimum distance 8, decoding with up to 3 hard errors, QPSK and 16QAM headers through noise (8 and 14 dB),
  the header pre-test on real headers and on random cells.
- L1 part 2: the bit count of table 18 for both guard intervals, pack / unpack, signed offsets, the carrier arithmetic of the slices and
  notches; coding with one and two FEC blocks and all four L1 time interleaving modes through noise at 9 dB; a damaged L1 is refused.
- FEC blocks: twelve ModCods of tables 11(a) and 11(b) (64K and 16K, 16 / 64 / 256-QAM, 2/3 .. 9/10) at about 2.5 dB above the
  thresholds of TS 102 991; the bit interleaver map is a permutation; cells have unit power.
- Receiver against the generator (0.55 - 0.7 s of signal each; packet contents and order are checked against the sources, zero bad or
  missing packets allowed, plus the carrier offset, the clock estimate and the window carrier index):
  - GI 1/64, Type 1, 64-QAM 2/3, 18 dB, +18.3 kHz, +25 ppm, 10 Msample/s.
  - GI 1/128, Type 2 robust header, 256-QAM 5/6, time interleaving 4, two PLPs (the second decoded), 31 dB, -7 kHz, -20 ppm, an echo
    at -15 dB / 1.8 us.
  - GI 1/64, Type 2 high efficiency header, 16K 16-QAM 4/5, time interleaving 16, a 35-carrier notch, 16 dB, +3.1 kHz, +12 ppm, at the
    native rate with the centre frequency unknown (the full carrier index search).
  - Type 1 1024-QAM 3/4 with reserved tones and time interleaving 4, 30 dB, +9 kHz, +15 ppm, echo at -20 dB.
  - Type 2 16K 4096-QAM 5/6 carrying GSE, 37 dB, -4 kHz, -10 ppm. The IP packets (UDP, 40 - 3 000 bytes, many fragmented across BB
    frames) are compared byte for byte and in order.
  - Type 1 64-QAM 2/3 GSE with a notch, reserved tones and time interleaving 8, GI 1/128, 19 dB, +12.5 kHz, +20 ppm.
  - 1024 / 4096-QAM: the rule against DVB-T2's tables, unit power, Gray neighbours, the bit maps are permutations, the first demultiplexed
    word of both, and FEC blocks through noise: 64K 1024-QAM 3/4 at 27 dB, 16K 1024-QAM 5/6 at 30 dB, 64K 4096-QAM 5/6 at 33.5 dB and
    16K 4096-QAM 4/5 at 32.5 dB decode; 1024-QAM 3/4 at 21 dB and 4096-QAM 5/6 at 27 dB must fail.
  - Reserved tones: 288 per 8 L1 blocks, the table 36 values and the shift, none on a scattered pilot, the data cell count of a slice.
  - A loss in the first 400 packets is allowed in every round trip (100 for IP). That is the receiver settling: 4096-QAM loses a few FEC
    blocks while the clock loop pulls in, about the first 10 ms at 10 ppm. A wrong packet is never allowed.
  - `ONAIR_TEST_LONG=1` adds 256-QAM 9/10 with time interleaving 8 (-40 kHz, +40 ppm, echo), 256-QAM 3/4 Type 2 with a notch,
    16K 64-QAM 8/9 Type 2 HEM with time interleaving 16 and two PLPs, 64-QAM 9/10 at GI 1/128 with +25 kHz, and 16K 256-QAM 5/6 at
    12 Msample/s; 64K 4096-QAM 9/10 Type 2 HEM (40 dB), 16K 1024-QAM 5/6 GI 1/128 with reserved tones and an echo, 64K 4096-QAM 4/5
    with two PLPs, and 64K 256-QAM 3/4 GSE at 12 Msample/s. All pass, also 64K 1024-QAM 9/10 at 33.5 dB, 16K 4096-QAM 8/9 at 37.5 dB,
    64K 1024-QAM 5/6 at 29.5 dB and 16K 1024-QAM 3/4 at 27.5 dB as FEC blocks.

Measured on the generator (clean signal, 10 Msample/s): MER 48 dB; with a 25 ppm clock error 45 dB (corrector active), at the native
rate without the corrector 26 dB (the inter-carrier interference of the uncorrected clock error, about 20 dB at 50 ppm); an echo at
-15 dB / 1.8 us costs nothing visible at 28 dB. Decoding at 18 dB Es/N0 gives MER 18.6 dB (64-QAM 2/3), at 31 dB 27 dB (256-QAM 5/6
with the echo).

## Not done, and interpretations

- PLP bundling and the common PLP are not decoded. GFPS and GCS payloads (types 0 and 1) are not interpreted.
- 4096-QAM at the native 64/7 Msample/s input rate (no time-domain clock corrector) keeps losing blocks with a 10 ppm clock error: the
  ICI of the uncorrected clock error limits the MER to about 37 dB. At 10 Msample/s and above it is fine.
  The L1 time interleaving modes 2 and 3 are implemented as the twisted interleaver over the signalled depth with the FEC blocks in a
  row; with more FEC blocks than the depth the standard says nothing and the code refuses.
- The data slice width for the time interleaver is taken as the number of carriers of the slice (K_DS,max - K_DS,min + 1, one column per
  carrier). The standard's text reads K_DS,max - K_DS,min; the memory has to hold every pilot and data position of the slice, which needs
  the +1. PLP_START is the index in the slice's cell stream before time interleaving, counted from the first data cell of the frame.
- The receiver decodes one data slice (the first, or the one chosen with `selectPlp`) that lies entirely in its 7.61 MHz window; a wider
  system is received by tuning to the slice's tuning position. Broadband notches wider than the preamble FEC can bridge are not handled
  specially.
- At the native 64/7 Msample/s input rate the sample clock error is not corrected in the time domain (the 24-tap corrector is not flat
  up to the 0.42 f_s band edge), so the ICI of the clock error remains: fine to about 25 ppm for 256-QAM 5/6. At 10 Msample/s and above
  the corrector runs and the error does not matter.
- The amplitudes, the phase convention of clause 10.1 (carriers on the absolute grid, phase reference at each symbol's useful part) and
  the Reed-Muller tables were read from the ETSI PDF; the scrambling sequence w^RM was checked against the glyph positions of the PDF
  because the text layer overlaps there. Nothing has been checked against a real transmitter.
