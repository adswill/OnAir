# PSK demodulator and analyser (family 202, issue #282)

EXPERIMENTAL. Tested with generated signals only; no real signal has been run through it. This is not PSK31 (family 37, `core/src/psk`).

A generic BPSK / QPSK / OQPSK / 8PSK receiver for an unknown signal. It finds the strongest narrow signal near the dial (or the one you click
in the spectrum), measures the symbol rate, modulation order, pulse shape (root-raised-cosine roll-off or rectangular), carrier and Doppler
blindly, demodulates it and shows the constellation, the quality (EVM, MER, Es/N0, Eb/N0) and the bits.

Numbers (docs/MODE-NUMBERS.md): family 202, engine standard 204, active standard 203, poll slot 163, id `pskdemod`.

## Files
- `core/include/dect2/pskdsp/pskdsp.h`: the DSP API (channel finder, analyser, channeliser, demodulator, bit layer, test signals).
- `core/src/pskdsp/`: `pskdsp_channel.cpp` (findCarrier, zooms into narrow signals), `pskdsp_analyse.cpp` (analyse, snapRate, estimateRolloff),
  `pskdsp_dsp.cpp` (RRC / low-pass taps, channeliser, Gardner timing + Costas carrier demodulator), `pskdsp_bits.cpp` (Gray mapping, differential
  decoding, G3RUH and CCSDS descramblers, sync hunt that resolves the phase ambiguity), `pskdsp_gen.cpp` (test signals), `pskdsp_rx.cpp` (the receiver).
- `app/pskdsp/pskdsp_ui.cpp`: the panel. Tests: `tests/pskdsp/`.

## How the blind analysis works
1. `findCarrier`: Welch spectrum, noise floor from the 20th percentile, regions above 2.5x the floor; the region with the most power is zoomed
   (mix + decimate) until it spans 40 bins or more, so a 25 baud signal in an 8 Msps capture is measured as well as a wide one. A DC spike is ignored.
2. The signal is brought to 3-6 samples per symbol. The residual carrier comes from the lowest power-law line (x^2, x^4, x^8) that stands out from its pedestal.
3. Symbol rate candidates: lines in the spectrum of |x|^2 and of the squared real rail (OQPSK has no envelope line), their halves, and a 1 % grid around the
   -3 dB width (the -3 dB width of RRC pulses equals the symbol rate whatever the roll-off). A short QPSK / OQPSK trial per candidate ranks them by how
   constant the symbol level is (M2M4 noise estimate, which needs no decisions); the best are re-run at the rate the timing loop reached.
4. Order: BPSK, QPSK, OQPSK and 8PSK trial demodulations; the lowest order that uses all its points and fits as well as the measured noise says a correct one
   must. Then the roll-off (auto tries 0.2 ... 1.0 and rectangular), the rate (snapped to 75 x 2^k, 1200 x n, 9600 x n and the other usual rates within 0.6 %),
   Doppler (slope of the loop frequency) and the quality figures.

## The bits
Differential decoding (symbol DPSK, NRZ-M, NRZ-S / NRZI), G3RUH (1 + x^12 + x^17) or CCSDS (x^8 + x^7 + x^5 + x^3 + 1) descrambling, inversion.
A sync word (hex or bits) resolves the phase ambiguity (every rotation, spectral inversion and, for OQPSK, the one-bit offset of the swapped rails) and lists the
frames behind it. Symbol DPSK is not offered for OQPSK in the tests (the swapped-rail read loses the pairing).

## Measured limits (generated signals, Es/N0 for the order found correctly)
BPSK about 5 dB, QPSK about 8 dB, 8PSK and OQPSK about 14 dB. Doppler ramps up to 30 Hz/s at 1200 baud and 400 Hz/s at 9600 baud, +-300 ppm clock error,
carrier anywhere in the span, rates from 75 baud to 100 kilobaud.

## Not verified
Real signals of any kind; filtered or Gaussian pulse shapes (GMSK is not PSK); bursty signals (the analysis assumes a continuous carrier); more than one
signal at the same frequency; rates above about 1 Mbaud (not tested for speed).

## Tests (`ctest -R pskdsp`)
`pskdsp_rates` (75 baud to 100 kilobaud, 4 orders), `pskdsp_impair` (off-centre, ppm, Doppler, roll-off, DC spike, second signal), `pskdsp_noise` (near the limit,
forced settings), `pskdsp_bits` (every order x differential x scrambler x phase / inversion, block and streaming decoder), `pskdsp_channel` (carrier finder),
`pskdsp_rx` (the receiver through the engine's feed path, the registry and the numbers).
