# PACTOR-I to PACTOR-IV: what is publicly known (research, 15 November 2026)

Purpose: decide what OnAir can honestly decode (issue #216). Pages were read as data only. Nothing here is code taken from another project.
Short answer: the **signal** of PACTOR-II, -III and -IV is documented well enough to detect and measure; the **channel coding** is not published, so no
text can be extracted from PACTOR-II / -III / -IV. PACTOR-I is open (1994 QEX / TAPR specification) and OnAir decodes it (experimental).

## Known per version

| | PACTOR-I | PACTOR-II | PACTOR-III | PACTOR-IV |
|---|---|---|---|---|
| modulation | FSK, 200 Hz shift, 100 / 200 baud | 2 carriers 200 Hz apart, 100 baud, D-BPSK, D-QPSK, D-8PSK, D-16PSK, raised-cosine pulses, about 450 Hz at -50 dB | 2 to 18 carriers on a 120 Hz grid (480 to 2520 Hz), 100 baud, D-BPSK / D-QPSK, speed levels 1 to 6 with 2, 6, 14, 14, 16, 18 carriers | 10 speed levels: SL1 two-tone chirp, SL2 to 4 DSSS D-QPSK (spread 16 / 8, 66.7 to 225 baud), SL5 to 10 single carrier 1500 Hz, 1800 baud, BPSK, QPSK, 8-PSK, 16-QAM, 32-QAM with an adaptive equaliser; 2400 Hz wide |
| cycle | 1.25 s | 1.25 s short, 3.75 s long (3.28 s packet), 1.4 s long path | 1.25 s / 3.75 s (1.4 / 4.2 s long path) | same ARQ structure as PACTOR-III (SCS states the source coding is identical) |
| frame | header, data, status byte, CRC-16 | header, data, status byte, CRC; six 40-bit D-BPSK control signals (Hamming distance 24) | one status byte and two CRC bytes per frame | not described publicly |
| FEC | none (repeat / memory ARQ) | rate 1/2, constraint length 9, punctured to 2/3, 7/8 (and 3/4, 8/9 in some descriptions), soft Viterbi, full-frame interleaving, memory ARQ | the same family; levels differ by modulation and puncturing | rates 1/3, 1/2, 5/6 only |
| compression | ASCII, Huffman (table in the ARRL description) | Huffman, run-length, pseudo-Markov (PMC, about 1.9x) | the same source coding | the same; a "reserved" mode for a new P4 compression |
| net rate | 20 to 40 bit/s | 100 to 700 bit/s | up to about 2700 bit/s | up to about 5500 bit/s (10500 with PMC, SCS figure) |

## Not published (found in no public source)

* Generator polynomials of the convolutional codes, puncturing patterns, interleaver dimensions, scrambler or whitening, CRC polynomial (PACTOR-II / -III / -IV), the
  status byte layout, the payload size per frame and level, the tone sets of PACTOR-III levels 3 and 4, preamble and sync words.
* PACTOR-IV: chip rate and spreading codes of levels 2 to 4, SL1 carrier schedule, equaliser training sequences, any frame layout.
* The pseudo-Markov table-switching rules and tables (only the idea is described).
* No open implementation, reverse-engineering paper or public repository decoding PACTOR-II / -III / -IV was found (searches for decoders on GitHub, Viterbi
  polynomials and PACTOR returned nothing). Known decoders are commercial: Wavecom, Hoka Code 300-32, go2DECODE / go2MONITOR (SHOC), R&S CA100, Sorcerer, SCS PMON (with a
  PACTOR modem). The Wikipedia article states PACTOR-II/-III/-IV are "publicly documented" but not open source. The SCS "PACTOR-3 Protocol" (2004) and "PACTOR-4
  Protocol" (2011) PDFs used to be hosted at p4dragon.com; the links now redirect to scs-pactor.com and could not be fetched, so their content was not verified here.
* No public sample IQ of PACTOR-II/-III/-IV was found (SigIDWiki has short audio clips and pictures only, no download links).

## Consequence

Without the code parameters a Viterbi decoder would be guesswork: a wrong polynomial gives garbage that cannot be told from noise, and with no CRC polynomial a good
decode cannot be proven. OnAir therefore does not guess. What works is the signal level (carriers, baud, speed level, frame period, symbols), implemented in the
PACTOR-II / -III / -IV modes (docs/pactorhf.md) and, since this change, reported in the PACTOR-I screen too.

## Sources

* ARRL: https://www.arrl.org/pactor, https://www.arrl.org/pactor-ii, https://www.arrl.org/pactor-iii
* Wikipedia: https://en.wikipedia.org/wiki/PACTOR
* SigIDWiki: https://www.sigidwiki.com/wiki/PACTOR_III, https://sigidwiki.com/wiki/PACTOR_IV
* Wavecom decoder help: https://www.wavecom.ch/content/ext/DecoderOnlineHelp/worddocuments/pactorii.htm, https://cartoonman.github.io/WAVECOM/wavecomhtm/pactoriii.htm, https://www.wavecom.ch/content/ext/DecoderOnlineHelp/worddocuments/pactor4.htm
* Structure and timing of PACTOR-II frames (NB6Z): https://myplace.frontier.com/~nb6z/pactor2.htm
* SCS: https://www.scs-pactor.com/ and the PACTOR-4 page (speed levels, equaliser, PMC rates)
