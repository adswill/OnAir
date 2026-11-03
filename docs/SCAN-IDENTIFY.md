# One-button Scan and Identify (3.6)

For a beginner who does not know the modes: two buttons at the top of every Simple view (`scanIdentifyBar`, app/specx/scan_identify_bar.cpp, drawn by `modepanel::commandView`).

- **Scan** finds every signal in what the radio sees (`sigid::findRegions`), identifies each one and lists them in the "Signals in the view" card: frequency, width, best guess, a confidence bar, whether the mode's own detector confirmed it, and Open / Add / Details. One click on Open tunes and starts the right mode with the measured width.
- **Identify** ("I don't know what this is") measures the strongest signal around the tuned frequency, or the region dragged on the spectrum, and shows ranked guesses with confidence and the "Why?" evidence.

No detector is new. Both run `sigid::identify` (bandwidth, symbol rate, modulation class, frame syncs, mode fingerprints, verification by the mode's own detectors; docs/IDENTIFIER.md). The TV and voice families also have their own one-click rows (docs/AUTODETECT.md). Hand-set choices stay in Advanced.

`specx::proposeAll` / `proposeOne` (core/include/dect2/specx/scan_propose.h) is the same chain as a pure function: one ranked `Proposal` per signal (sure before guess), the runners-up, and the Memory that opens it. `proposalLine` gives the beginner's words ("Sure: DMR", "Could be: P25 Phase 1 or NXDN96", "Unknown signal, 12.5 kHz wide").

Test: `test_specx_scanpropose` puts DMR, NXDN96 and P25 into one 1 Msps capture at three sets of offsets (up to +-420 kHz), carrier errors, 30/25/20 dB; each must come back at its frequency, named right and openable, nothing "sure" may be wrong, and noise alone gives nothing likely.

Not done: the Scan works on the current capture span; it does not retune across a wider band (the Fast scan window does that: docs/fast-scan.md).
