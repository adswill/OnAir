# Mode Builder signal plots (4.1)

Requested on Discord (chasyxx): modes made in the Mode Builder should show a histogram, and PSK a constellation.

Every custom mode now has two tabs in its main panel, **Frames** (as before) and **Signal**. The Signal tab reads `Receiver::signalView()`
(core/src/modebuilder/rx.cpp), a snapshot of the symbol-timed values of the last 2048 symbols seen while the squelch was open.

| Modulation | Plot | Marked |
|---|---|---|
| OOK, ASK | histogram of the envelope after the matched filter | slicer thresholds (orange), measured level centres (green) |
| FSK, GFSK, GMSK | histogram of the discriminator output, in Hz | slicer thresholds, measured tone centres |
| PSK | constellation after carrier recovery | ideal points (crosses) |

Numbers next to the plot:
- clusters found (of the mode's levels), separation (closest cluster distance / pooled spread; PSK: ideal neighbour distance / spread per axis);
- FSK/OOK/ASK: eye SNR = 20 log10(separation / 2); thresholds now and measured; FSK: measured deviation and carrier offset;
- PSK: EVM (it includes the amplitude scatter of the demodulator, which does not move a PSK decision, so it reads high: 25-70 % on clean simulated BPSK/QPSK/8PSK), Es/N0 = -20 log10(EVM), Eb/N0 = Es/N0 - 10 log10(bits per symbol), carrier offset.

A view counts the clusters only when separation >= 4. A single smooth blob split into any number of slices cannot reach 3.5 (a flat
distribution gives sqrt(12)), so noise does not produce clusters (symbols are taken only while the signal is above the squelch threshold, not in the hang time after a burst); below 4 the tab says the clusters are not separate yet.

**Advanced > Use measured deviation / offset** writes the measured values into the definition file (FSK: `deviation_hz` and
`offset_hz`; GFSK, GMSK, PSK: `offset_hz` only, because Gaussian filtering pulls the symbol-time frequency below the nominal deviation) and reloads the mode. There is no threshold field in the format: the slicer thresholds follow the
signal by themselves (OOK/ASK have nothing to write), so only deviation and offset are applied.

Tests: `test_mb_signal` (ctest `mb_signal`): generated signals of 2FSK, 4FSK, GFSK, GMSK, OOK, ASK4, BPSK, QPSK, 8PSK; cluster count,
centre positions, thresholds, deviation and offset (400 Hz carrier error), PSK points/EVM/Eb/N0, EVM follows the noise, low SNR is not
reported as resolved, noise only (default squelch and squelch off, two seeds) gives no clusters. Simulated signals only, not on air.
