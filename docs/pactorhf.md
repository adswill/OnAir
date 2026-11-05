# PACTOR-II, PACTOR-III and PACTOR-IV (physical layer only)

Three mode-list entries (families 237, 238, 239; engine standards 239 to 241; poll slots 197 to 199), receive only, upper sideband on HF
(starting point 14.072 MHz dial; the buttons jump to the digital part of each band, PACTOR has no calling frequency). They are one receiver
(`core/src/pactorhf`) that is asked for one of the three; the **PACTOR-II** entry tells you when the signal on the channel is really PACTOR-III
and the other way round.

## What it does

PACTOR-II, -III and -IV are proprietary protocols of SCS (Special Communications Systems). The channel coding of all three (convolutional code,
puncturing, interleaver, scrambler, frame header, compression) is not published, and OnAir does not reverse engineer proprietary protocols, so
these modes do not produce text (the coding is not decoded). They measure what the signal itself shows:

| | PACTOR-II | PACTOR-III | PACTOR-IV |
|---|---|---|---|
| carriers | 2, 200 Hz apart | 2 to 18 on the grid 480 + 120 n Hz (n = 0 .. 17) | 1 at 1500 Hz (levels 5 to 10) |
| symbol rate | 100 baud | 100 baud | 1800 baud |
| modulation | D-BPSK, D-QPSK, D-8PSK, D-16PSK | D-BPSK (levels 1 to 3), D-QPSK (4 to 6) | BPSK, QPSK, 8-PSK, 16 / 32-QAM |
| found by | the 100 baud clock line of each carrier | the grid scan (tight 4th power of the phase steps) | the 1800 baud clock line |
| reported | carrier frequencies, spacing, baud rate and clock error (ppm), steps per symbol of each carrier, the symbols, the frame period and the constant part of a repeating frame | the same, and the speed level from the carrier count (2, 6, 14, 14, 16, 18) | centre, clock line, an estimate of the constellation |

Levels 1 to 4 of PACTOR-IV (a chirp and a spread-spectrum modulation at 66.7 to 225 baud) are not recognised; an equaliser is not implemented, so the
PACTOR-IV constellation is an estimate that needs about 15 dB of SNR and a clean path (a second path 1.5 ms later turns QPSK into "QAM").

Chain: the channel is mixed to 0 Hz and taken to 24 kHz (marine front end), the upper sideband becomes 8 kHz audio (as in the other HF modes), the last
40 s are analysed every 4 s on a worker thread. Carrier search: for every frequency from 300 to 3300 Hz the envelope of the mixed signal has a line at
the symbol rate when a carrier is there (leakage lines without power are ignored); PACTOR-III is looked for on the 120 Hz grid with the tuning error of the
radio searched; each carrier is then mixed down, sampled at the symbol clock (common to all carriers, line phase fitted over blocks, so a clock off by
hundreds of ppm is followed), and the differential phase of successive symbols gives the symbol (a constant phase step is a frequency error and is
followed block by block). PACTOR-II uses a raised-cosine window per symbol, PACTOR-III a rectangular window of 1/120 s, which nulls the neighbouring carriers.
Frames: a lag that makes a window of 24 symbols agree far beyond chance (a z-score of 9 or more) is the frame period; the positions that agree in
more than half of the copies are the constant part (a sync pattern), shown in hex.

## What was measured on a real recording

The owner's recording of 1 November 2026, 13:52, 8532.8 kHz USB: PACTOR-II, the two carriers 200 Hz apart (771 / 971 Hz in this analysis; the
ambiguity of a 4th power is baud / 4 = 25 Hz, so the true pair may be 25 Hz either way), 100.000 baud, and a frame that repeats every **362 symbols = 3.62 s** (22 copies in 79 s), with a constant
part of a few symbols in the middle of the frame. Neighbouring copies agree in about 60 % of the symbols outside that part (chance 25 %), the others
only at chance: the structure behind that (a changing scrambler, or pairs of frames) is not known and not guessed at. The channel is poor (a 4th-power
concentration of 0.4 per carrier), the symbols are right in perhaps 80 % of the places. A 26 s excerpt (8 kHz, 8 bit) is in `tests/data/pactorhf/`
and `test_pactorhf_real` checks the structure on it.

## What is not available

Not decoded:

* The channel coding, hence no text and no CRC check. What is missing to read the data: the convolutional code (generator polynomials; the public SCS
  descriptions only give "constraint length 9 or 7, rate 1/2, punctured to 2/3, 3/4, 7/8 and 8/9"), the puncturing patterns, the full-frame
  interleaver, the scrambler or whitening (if any) and the frame header and status byte layout. With those the symbols this mode already extracts
  could go through a Viterbi decoder (plan B: the 22 copies of a repeating frame can be combined to improve the symbols, and a decoder can be
  written the day the description is published or the owner supplies it).
* Memory ARQ, speed changes and the control signals (40-bit D-BPSK words) of the ARQ link: the mode shows a continuous stream, not a link.
* PACTOR-III level 1 is the same two-carrier signal as PACTOR-II and is reported as PACTOR-II.
* The PACTOR-III carrier sets of levels 3 to 6 are taken as the centred ones; the receiver only needs the grid and the count.

## Tests

`tests/pactorhf`: the physical layer against the generator with the exact symbols known (every order of PACTOR-II, all PACTOR-III levels, PACTOR-IV
constellations), tuning errors of +-20 Hz, clocks off by up to 300 ppm, noise down to 12 dB in 2.5 kHz, slow fading, noise and a tone alone (nothing may be
found), the receiver through the front end at 250 ksps, the engine with the built-in test signal, and the owner's recording. The generator's frames are not
real PACTOR frames: a constant 40-symbol pattern plus random symbols, repeated every 362 symbols.

## Sources

Facts only, no code: the SCS technical descriptions "PACTOR-3 Protocol" (2004) and "PACTOR-4 Protocol" (2011), the ARRL pages "PACTOR-II" and "PACTOR-III",
the Wavecom decoder notes for PACTOR-II / -III / -IV, "Pactor Series Overview" (Korean Institute of Navigation and Port Research). See THIRD_PARTY.md
and Help > Credits. No open implementation or reverse-engineering paper of the PACTOR-II / -III / -IV coding was found.
