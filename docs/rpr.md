# Robust Packet (RPR), physical layer only

One mode-list entry (family 899, engine standard 900, tuning standard 901, poll slot 899), receive only, upper sideband on HF
(starting point 10.1473 MHz, the world-wide RPR frequency of the sigidwiki page; the buttons also offer 3.610, 7.0473 and 14.1033 MHz).
**Needs a user test** and **needs a recording**: no real RPR signal was available (the sigidwiki page lists no downloadable sample), so the
mode was only tested on synthetic signals.

## What is confirmed (public SCS / Wavecom descriptions)

| | |
|---|---|
| modulation | eight-carrier, pulse-shaped OFDM, every carrier D-PSK |
| audio centre | 1500 Hz |
| carrier spacing | 60 Hz (eight carriers: 1290, 1350 ... 1710 Hz), 500 Hz bandwidth |
| symbol rate | 50 baud per carrier, constant |
| speed level 1 | D-BPSK, convolutional code R = 1/2, 200 bit/s before the AX.25 overhead |
| speed level 2 | D-QPSK, convolutional code R = 3/4, 600 bit/s |
| coding | constraint length k = 9, data scrambled, full-frame pseudo-random interleaver, CRC |
| preamble | 160 ms of 64 DPSK symbols spread over the eight carriers (8 symbols per carrier) |
| payload | AX.25 (callsigns compressed by a special algorithm, multi-frames with one CRC and one callsign field), up to 256 bytes, 25 packet types |
| typical SNR | 7 to 10 dB in 3 kHz (AWGN) |

Sources: the Wavecom datasheet "Robust Packet Radio (RPR)" and the W-CODE manual page, the sigidwiki page "Robust PACKET" (see
tools/credits_data.tsv). The numbers 200 / 600 bit/s agree with 8 carriers x 50 baud x bits per symbol x code rate.

## What is NOT public, so not implemented (nothing was invented)

* the generator polynomials of the k = 9 code and the puncturing that makes R = 3/4,
* the scrambler, the interleaver permutation, the callsign compression, the 25 packet types, the CRC polynomial,
* the content of the preamble (so the burst start is found from the power and the symbol clock, not from a known sequence),
* the phase convention of the D-PSK (the symbols are reported as phase steps relative to the observed constellation axes).

With any of these missing the bits cannot be turned into AX.25 frames, so this mode **does not produce AX.25 frames, APRS positions or text**
and nothing reaches the APRS map from it. The hand-over to the AX.25 / APRS parser of `core/src/packet` is ready to be added the moment a
recording lets the coding be worked out; until then it would only be guesswork. SCS also sells the only implementation (Tracker / DSP TNC),
and amateurs report that no public description exists.

## What it does

The channel is mixed to 0 Hz and taken to 24 kHz (marine front end), the upper sideband becomes 8 kHz audio (as in the other HF modes), the last
30 s are analysed every 4 s on a worker thread (`core/src/rpr/rpr_dsp.cpp`):

1. **Comb**: the average spectrum of the strongest frames is correlated with the comb template (+1 within +-240 Hz, -1 in a guard band to +-360 Hz);
   the best position is the comb centre (good to a few Hz, tested +-50 Hz off). A contrast of at least 3 dB is needed.
2. **Bursts**: the eight carriers are mixed down and integrated over the active part of the symbol (1/60 s, which nulls the neighbours 60 Hz
   away); the summed power against the 20th percentile (and 1 % of the peak, for noise-free files) gates a burst (at least 0.28 s, gaps up to 60 ms closed).
3. **Each burst**: the symbol clock from the 50 baud line of the summed envelope (the clock error from the two halves of a burst longer than 3 s),
   the symbols of every carrier, the differential phase, a per-carrier frequency error from the mean turn of d^4. The concentration of d^2
   (near 1 for D-BPSK) and d^4 (near 1 for D-QPSK) decides the speed level; at least five of the eight carriers must be concentrated.
4. **Output**: a burst list (time, length, level, SNR, the first demodulated bits as hex, still coded and scrambled), the newest symbols of the first
   three carriers, an event log, the carrier frequencies and the comb centre.

## Assumptions (marked, to be checked on a recording)

* The OFDM symbol is taken as 20 ms = 1/60 s active part + 3.33 ms guard (this makes 60 Hz spacing orthogonal and 50 baud; the published text only says
  "pulse-shaped"). The generator uses a cross-fade over the guard; the receiver integrates over 133 samples at 8 kHz. A different real pulse shape
  would cost some SNR, not the principle.
* The 160 ms preamble is taken as 8 symbols per carrier; the receiver does not need its content.
* Sensitivity on synthetic signals: D-BPSK bursts found down to about 3 dB, D-QPSK down to about 5 dB SNR (in 2.5 kHz); the published typical
  figure for the real system is 7 to 10 dB.

## Test signal

`RprGenerator` (`core/include/dect2/rpr/rpr_gen.h`) sends bursts of an 8 symbol preamble (its own fixed pattern) plus 60 pseudo-random differential
symbols per carrier, 1.5 s apart, with noise, tuning error, clock error and fading. They are **not** RPR frames. The synthetic source plays it for
the mode (SynthConfig mode option 0: speed level 1 or 2).

## Tests

`rpr_dsp` (both levels clean, SNR 20 / 8 / 3 or 5 dB, tuning error -50 / 0 / +50 Hz, clock error, fading; exact symbols compared; noise, tones and eight
unmodulated carriers give nothing), `rpr_rx` (through the radio front end at 250 ksps, both levels, +-50 Hz), `rpr_engine` (setStandard(901) with the
synthetic source), `rpr_mode` (wiring and the honesty strings).
