# HF utility modes (3.8)

Receive-only decoders for HF utility traffic, mode families 430 to 439. Each has a built-in test signal and is exercised by `tests/hfutil`. **Every decoder here has been validated on simulated signals only; no over-the-air recording was available.** Results below are the generators and receivers written from the same public documents agreeing with each other, plus the worked examples noted.

Encrypted content stays as bytes; nothing is decrypted.

## Modes added

| Family | Mode | Source | What is decoded |
|---|---|---|---|
| 430 | MIL-STD-188-110 serial | MIL-STD-188-110C with Change 1, 5.3.2 | 75 to 4800 bit/s, short and long interleaver; speed and interleaver from the preamble; 8-PSK DFE with probes; K=7 Viterbi; STANAG 5066 D_PDU framing |
| 431 | STANAG 4285 | STANAG 4285 Annexes A, E | Frames, scrambler, 75 to 3600 bit/s; coded rates and interleavers found automatically by the start-of-message pattern; uncoded rates chosen under Advanced; D_PDU framing |
| 432 / 433 | ARQ-E / ARQ-N | Hoka and Wavecom notes, sigidwiki | ITA2-P words, automatic speed, shift, alignment and inversion cycle; control characters counted, not named (their codes are not published) |
| 434 | AUTOSPEC | Wavecom note | 10-bit Bauer code with single-error correction, automatic speed |
| 435 | SPREAD 11/21/51 | sigidwiki, Wavecom | Bauer code with bit spreading; depth detected |
| 436 | G-TOR | DCC 1994 Kantronics paper | 100/200/300 Bd, interleaver, data and Golay forms, CRC; ASCII frames as text, Huffman frames as hex |
| 437 | EFR teleswitch | Wavecom note | DCF49 / DCF39 200 Bd, DIN 19244 frames, checksum; Semagyr-TOP and Versacom payload as hex |
| 438 | Piccolo Mk VI | Crypto Museum matrix | Tone-pair to ITA2 |
| 439 | Coquelet-8 | Wavecom tone table | Tone-pair to ITA2 |

STANAG 5066 is read inside modes 430 and 431 rather than as its own entry.

## Measured on simulated signals

- 110 (10 cases): no bit errors at 25 dB (4800), 18 dB (2400), 12 dB (1200), 8 dB (600), 5 dB (300), 3 dB (150), 1 dB (75), SNR in 3 kHz, 40 Hz offset; 1200 bit/s through a 2 ms -3 dB second path with 20 ppm clock error.
- 4285: no bit errors at 18 / 12 / 8 / 5 / 3 / 2 dB for 2400 ... 75 bit/s coded, short and long, 60 Hz offset; uncoded below 0.5 % errors at 28 / 22 / 16 dB.
- D_PDU header and data CRC checked over both modems at 12 dB.
- ARQ-E/N, AUTOSPEC, SPREAD: text correct at 18, 14 and 10 dB; G-TOR at 22 dB, with +25 Hz offset; EFR telegrams at 15 dB; Piccolo and Coquelet at 14-16 dB.
- All ten modes read back through the engine's channel front end at 1 Msps, 20 kHz off-tune.

## Limits and assumptions

- MIL-STD-188-110: the count symbols follow the rule and the worked example in 5.3.2.3.7.2.1 (a 1 above the two-bit value); Table XII of the same section reads as if the bits were listed the other way round. The 2400 bit/s digital voice setting (D1 D2 = 7 7) and the 0.0 s interleaver are not decoded.
- STANAG 4285: the first scrambler symbol is taken from the initial register state.
- ARQ-E / ARQ-N lock only on real traffic: an idle channel alone is ambiguous under rotation, so nothing is shown until varied traffic arrives. Control codes are not named.
- SPREAD: the bit layout (bit k of character n at position 10 n + k S) is inferred from "after each bit, S - 1 bits of other characters".
- AUTOSPEC: Wavecom's rule (the last five bits inverted when the first five have even parity) is implemented.
- G-TOR: the CRC is confirmed against the paper's connect-frame example (F5 E4); the paper's text-frame example did not match, most likely a misprint or scanning error.
- Coquelet-8: tones 1 and 8 are also the code of the letter M; runs of that pair are treated as idle, shorter ones as M.
- Piccolo Mk VI: the phase of the tone pairs comes from the pair D/D never being sent; the idle pattern assumed is SYNC and its barred form alternating.
- STANAG 5066 D_PDU layout and CRC polynomials come from Wireshark's dissector; the payload of C_PDU segments is not shown beyond the count.

## Skipped, and why

| Item | Reason |
|---|---|
| NAVDAT (ITU-R M.2010-3) | Revisions -2 and -3 could not be retrieved; the available M.2010-1 text gives neither the MIS/TIS cell positions nor the data cell mapping, shows the pilot grid only in a small figure and the rate 3/4 LDPC matrix as an image. Building a decoder would mean inventing the mapping. |
| 3G ALE (MIL-STD-188-141B Appendix C, STANAG 4538) | No public copy with the BW0 tables could be retrieved. The HF waveform ID mode still identifies the burst shape. |
| Link-11 structure | MIL-STD-188-203-1A frame format is not publicly available; HF waveform ID already identifies the tone plan. |
| CLOVER | HAL's waveform is proprietary: public pages give the tone pulses, not the block and coding format. |
| Mazielka | No technical description found. |
| Piccolo Mk XII, Coquelet-13/80 | The tone-to-character tables are not published (the Piccolo ITA5 matrix is listed as unavailable). |

`tests/hfutil` and `tests/app/test_hfutil_mode.cpp` cover the receivers and wiring.
