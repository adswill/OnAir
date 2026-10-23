# PSK demodulator and analyser

Mode list: **Satellite & navigation > PSK demodulator (experimental)** (family 202, engine standard 204, `dect2cli --standard pskd`).
Asked for in issue #282: a BPSK mode with a data-rate analyser and a constellation, for the many satellite services that use PSK at
bandwidths from about 2.5 kHz and rates from 75 bit/s up, and QPSK later.

It is a general demodulator for one PSK signal: BPSK, QPSK, offset QPSK and 8PSK, any symbol rate the radio's sample rate holds (from
about 50 Bd; at 2 Msps up to a few hundred kBd). It does not know any particular satellite: it shows the constellation, measures the
signal and gives the bits. Receive only. **Experimental**: it has only been checked against generated signals (`tests/pskdsp`), not against
a real satellite. Reports and short IQ recordings of real signals are welcome.

## Using it

Tune the dial onto (or near) the signal. Nothing else needs setting:

1. The receiver looks at 200 kHz around the dial (Decoder options > Search span to change it) and takes the signal nearest the dial.
2. Twice a second, until it locks, the analyser measures it: bandwidth, symbol rate, order, pulse shape, carrier and Eb/N0. A narrow
   signal in the wide span is zoomed into and analysed over up to 30 s, so slow signals (75 - 300 Bd) are found too.
3. Once the constellation settles it locks, demodulates continuously (starting from the samples it analysed, so a burst is not lost) and
   follows the carrier through the Doppler of a pass. When the constellation stays bad for a few seconds it goes back to searching.
   **Analyse again** drops the lock at once (after tuning to another signal, for example).

The PSK tab has three views:

- **Analyser**: the symbol rate (snapped to a standard rate when it is within 0.6 % of one, with the measured value next to it), the rate
  candidates and where each came from, the order with the EVM of every order tried, the pulse shape, bit rate, EVM / MER / Eb/N0, the
  Doppler rate, and the two line spectra the estimates come from (|x|^2 with the symbol-rate line, x^2 / x^4 / x^8 with the carrier line).
- **Bits**: hex and text, or plain bits. With a sync word set, every line starts at a sync word.
- **Log**: what each analysis decided, in words.

The analysis row at the bottom shows the constellation, the eye diagram of the I rail and the |x|^2 spectrum. **Save bits** writes the
decoded bits, packed into bytes (first bit in the top bit), to a new file in the `psk-bits` folder of the OnAir data folder.

### Decoder options (all automatic unless set)

| option | what it does |
|---|---|
| Modulation | BPSK, QPSK, OQPSK or 8PSK instead of the analyser's choice |
| Symbol rate | a known rate (0 = found by the analyser) |
| Pulse shape | rectangular, or a root-raised cosine roll-off |
| Search span | 10 kHz to the whole capture around the dial |
| Differential | DPSK (symbol differential), NRZ-M (a 1 is a change), NRZ-S / NRZI (a 0 is a change) |
| Descrambler | G3RUH (1 + x^12 + x^17, self-synchronising, as 9600 bd packet) or the CCSDS randomiser (x^8+x^7+x^5+x^3+1, restarted after each sync word) |
| Sync word | bits or hex, for example `1ACFFC1D` (CCSDS). It resolves the phase ambiguity of the carrier (every rotation and the spectral sense are tried; OQPSK also the pairing of the rails), and starts each line of the bit view |
| Invert bits | inverts the output |
| Snap to standard rates | off: the measured rate is used as it is |
| Follow the Doppler | off: the channel stays where the analysis found the carrier |

Without a sync word, BPSK has a 180 degree and QPSK a 90 degree ambiguity: the bits may come out inverted or rotated. Use a differential
coding or a sync word when the signal has one.

## How it works

`core/src/pskdsp` (`pskdsp.h`):

- **Channel** (`pskdsp_channel.cpp`): Welch spectrum, noise floor from the 30th percentile inside the usable band, the region above it
  nearest the dial (or the strongest), zoomed in until it spans at least 40 bins; centroid carrier, -3 dB / -10 dB / 99 % widths, SNR.
- **Analysis** (`pskdsp_analyse.cpp`): the signal is cut out at about four times its width. Symbol-rate candidates: the line of |x|^2
  (strong for shaped pulses), the side lines of the x^2 and x^4 carrier lines, the x^2 line pair without a carrier line and the I^2 - Q^2
  line after a block-wise x^4 carrier (offset QPSK, whose envelope is nearly constant). The search range follows the -3 dB width. The
  order: a trial demodulation for each order; a higher order wins only if it is clearly better and its extra points are used (BPSK is a
  subset of QPSK, QPSK of 8PSK). Pulse shape: the trial EVM for rectangular and roll-offs 0.2 - 0.8, rectangular only when the spectrum
  agrees. Doppler rate: the slope of the carrier loop over the block.
- **Demodulator** (`pskdsp_dsp.cpp`): mixer, CIC and FIR channeliser, matched filter (RRC or integrate-and-dump), Gardner timing loop with
  cubic interpolation (OQPSK reads Q half a symbol later), decision-directed Costas loop for 2 / 4 / 8 points with an FLL assist.
- **Bits** (`pskdsp_bits.cpp`): Gray demapping, DPSK / NRZ-M / NRZ-S, G3RUH and CCSDS descramblers, the sync hunt over rotations, sense and
  OQPSK rail pairing (two agreeing sync words decide; a run of missing ones re-hunts after a cycle slip).
- **Receiver** (`pskdsp_rx.cpp`): the search span, the zoom, the lock and the Doppler following, bit saving, the test signal
  (`makePskdSynth`: frames with the sync word 1ACFFC1D, options for order, rate, roll-off, differential coding, scrambler, Doppler ramp).

## Tests

`tests/pskdsp/test_pskdsp_analyse.cpp` (blind analysis from 75 Bd at 8 kHz to 250 kBd at 2 Msps, every order, RRC and rectangular, offsets,
off-centre, Doppler ramp, clock error, 8 dB Es/N0, a clicked weaker signal next to a stronger one, a seeded sweep of 28 random cases, noise
and a plain carrier never lock), `test_pskdsp_bits.cpp` (every payload back for every order with no coding, DPSK, NRZ-M, NRZ-S, G3RUH,
CCSDS, a random carrier phase and a spectrally inverted input), `test_pskdsp_rx.cpp` (the receiver on its test signal: tuning error, 2 Msps
with the dial 250 kHz from the centre, a 48 kHz recording, 150 Bd in a 200 kHz span, 150 Hz/s Doppler, settings by hand, saving, noise
only), `test_pskdsp_engine.cpp` (through the engine).

## Not verified

No real satellite signal was available. Not checked: real Doppler curves and fading, phase noise of real radios, signals with
convolutional or Reed-Solomon coding (the bits are given as demodulated; nothing decodes the FEC or the frames of a particular satellite),
bursty signals shorter than about 200 symbols, and two signals closer together than their width.
