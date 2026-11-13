# Golay paging, GSC (4.1, family 330)

Receive only. Golay Sequential Code (GSC) is Motorola's one-way paging format: 2-FSK, with tone, numeric, alphanumeric and tone-and-voice pages. It was asked for on Discord (sugusly) and approved by the owner.
It is its own mode-list entry under Pagers. It uses the x40 receiver as variant 52 (`Gsc`): family 330, active standard 331, engine standard 332 and poll slot 330, all from the free range 324 - 337 (`x40::kGscFamily`).
- Code: `core/include/dect2/x40/gsc_proto.h` (format: tables, Golay and BCH, pager code rules, block builders) and `core/src/x40/x40_gsc.cpp` (receiver).
- Tests: `tests/x40/test_x40_gsc.cpp`.
- Generator: `tests/x40/x40_gen_gsc.h`.

**Simulated signals only.** No GSC recording was available. test_x40_gsc passes on GitHub (quick.yml run 43386471840, Linux).

## Sources

| Source | What it gave |
|---|---|
| CCIR Report 900-2, Annex I "Golay sequential code" (Reports of the CCIR, Düsseldorf 1990, ITU history portal) | The main source. It gave: the format and timing; mark = positive shift = 1; preamble (28-bit comma + 18 words, Table V with 10 words and their parity bits, LSB first); the inverted preamble marking batch mode; start code 713 (Word 2 = complement of Word 1); the comma rules (first comma bit = first bit of Word 1, separator bit opposite to the first bit of Word 2); the Table VI Word 1 values (50 words + complements); the Golay (23,12) generator x^11+x^9+x^7+x^6+x^5+x+1 with the LSB written to the left; the data block of 8 BCH (15,7) words (generator x^8+x^7+x^6+x^4+1, sent by columns from the LSB, comma bit opposite to the first data bit); the continue bit and the 7-bit sum check; the Table VII character sets (4-bit numeric, 6-bit alphanumeric = ASCII with bit 6 dropped); NULL fill; the Table VIII illegal codes; the Table IX function suffixes; the worked example 954853 -> preamble 3, Word 1 number 4, Word 2 1753 with its parity bits. |
| Motorola "A Guide to the Golay Sequential Code (GSC)", R8-1-67 (scan on sigidwiki; read by OCR) | Pager code layout I G1 G0 A2 A1 A0; preamble = I + G0; the four addresses per code (W1/W2 and their inversions); functions (tone, tone and voice, data); the activation code that starts the alert and squelches the audio again after the voice; batch and extended batch; timings (preamble 1.427 s, address 0.202 s, 12 digits or 8 characters per 0.202 s). |
| sigidwiki "Golay Paging (GSC)" | 300 / 600 bit/s, NFM, frequencies. |
| ESPager `gsc_tx.h` (GPL-3.0) and gr-mixalot `gscencode_impl.cc` (GPL-3.0) | Format reference only; no code copied. They confirmed: the bit order (12 information bits then 11 parity bits, each LSB first, the 0xC75 form of the generator, which gives the parity bits of Table V); the Word 2 rule of the flow diagram (Fig. 6 of the report is an image the text dump does not hold); the packing of the 7-bit words of a data block. Credited in THIRD_PARTY.md. |

## Certain, inferred, guessed

- **Certain (published tables, checked in the test):** the 10 preamble words, start code 713, the 50 Word 1 values, both generators, the parity bits of Table V and of the 954853 example, the illegal codes, and the comma rules. The bit rates are certain too: words at 300 bit/s and every comma bit and data bit at 600 bit/s. So a 300 bit/s bit is two 600 bit/s bits, and the "300/600 baud switch" is part of the 600 bit/s grid. The receiver runs one 600 bit/s clock and adds bit pairs for the words.
- **Inferred:**
  - Pager code to Word 2: w2 = 100 x (A div 50) + A mod 50, + 50 when G1G0 >= 50. This matches the report's example and ESPager, but Fig. 6 itself could not be read.
  - Address function numbers 2 - 4 = W1 / not W2, not W1 / W2, not W1 / not W2. The order is from the Guide's text. The overbars of Table IX were lost in the text dump.
  - Bit order inside a data block word: information bit 1 first, then the parity bits from the highest power. This follows the Golay rule and Fig. 5.
  - The shifted numeric set is not used. Code 15 is shown as "E", as the table says for pagers without it.
- **Guessed:**
  - The activation code word. Neither source gives its value: the report leaves voice out, and the Guide gives no bits. The receiver therefore accepts any two-word block with Word 2 = complement of Word 1 that is not an address, preamble or start code word. It treats the first one after an address as "voice page" and the same word again as "voice ended". The generator uses 3061 as a stand-in.
  - Whether a data page is numeric or alphanumeric is not sent on the air (the pager knows its type). The receiver reads both and keeps the more plausible text. When they score close, it also shows the other reading in brackets.

## What the mode shows

| Row | Contents |
|---|---|
| Tone page | Pager code (`I G1 G0 A2 A1 A0`; `N` + 5 digits when no preamble was seen), address function, preamble group, single call or batch, bits corrected. |
| Numeric page / Alpha page | The text, the number of blocks, and a note when the last block said more would follow. |
| Voice page | Tone and voice. The analog voice follows on the channel and is flagged, not decoded. Also gives the activation word. |
| Voice end | The activation word came again, with the length of the voice message. |

Facts: page counts, block lock, preamble and mode, polarity, deviation, carrier offset, bit clock error.

## Receiver

1. **Front end:** channel at about 24 ksps (+-9 kHz), FM discriminator, and a boxcar of one 600 bit/s bit.
2. **Clock and levels:** a Gardner clock with an integral term (+-3 %) gives soft 600 bit/s bits. The levels are tracked as upper and lower means, which gives the offset and the deviation.
3. **Search:**
   - Preamble words: two in a row, both polarities.
   - Start code: two words within 6 bits in all, either polarity. This sets the polarity.
   - Address without a preamble: comma with 24 of 27 transitions, Word 1 within 2 bits of the 100 words, Word 2 soft-decoded (`voicefec::golaySoft`) within 2 bits, and both comma rules.
4. **Block lock:** after a start code or address, every 121-bit block is read as one of these, in this order: start code, data block (8 soft BCH words with at most 2 errors each, plus the sum check), address (Word 1 within 3 bits), activation code. Two unreadable blocks in a row end the lock.

## Tests (`test_x40_gsc`, passing: quick.yml run 43386471840)

1. **Format checks:** the Table V parity bits, the start code, and the 954853 example. The pager-code to word mapping is checked both ways. Also the illegal codes, the minimum distances of Golay (7) and BCH (5), block packing, and characters.
2. **Single-call transmission:** a tone page, a numeric page, an alpha page, and a voice page with 2 s of voice and an end word. It is run with:
   - offsets of +-2.5 kHz and drift of 150 Hz/s;
   - clock errors of +-800 ppm, and a clock drifting 120 ppm/s;
   - 3 kHz deviation;
   - an inverted spectrum;
   - 24 / 48 / 250 ksps, and 40 kHz off centre.
3. **Batch** of 4 pages after an inverted preamble.
4. **Non-battery-saver** pages after a 75 Hz preamble, with no start code.
5. **Noise sweep** (-2 to 16 dB in 12.5 kHz, 3 seeds): the 50 % and 90 % points for messages and for tone pages.
6. **No false pages:** noise only (3 x 40 s), a carrier, random 1200, 600 and 300 bit/s FSK, and FM voice audio must give 0 pages.

## Results (simulated)

The figures below are from run 43378578019. The final run, 43386471840, passed every check, but ctest does not print the output of passing tests. The changes between the two runs touch only the voice-end search and when the block lock is held.

**Noise sweep** (signal to noise in 12.5 kHz; offset +800 Hz, clock +300 ppm; 9 messages and 3 tone pages per step):

| Pages | 50 % point | 90 % point | Notes |
|---|---|---|---|
| Numeric and alphanumeric | -1.1 dB | +0.7 dB | 9 / 9 from 1 dB up |
| Tone | -1.8 dB | -1.2 dB | |

**Impairments:** every case reads all four pages, including the 250 ksps case 40 kHz off centre. The voice end was found in every case except "clock -800 ppm drifting +120 ppm/s". That miss was fixed in the final run.

**Batch and non-battery-saver:** a batch of 4 after an inverted preamble and the non-battery-saver pages were all read.

**False pages:** 0 in every check:
- noise only, 3 x 40 s;
- a carrier, 20 s;
- random 1200, 600 and 300 bit/s FSK, 30 s each;
- FM voice audio, 20 s.

**Bugs the runs found:**
- The bit clock wandered on an idle carrier and on voice audio. The integral now moves only on bit edges, leaks while unlocked and is held within +-0.4 %.
- The search could lock half a bit early. It now keeps the best of three bit positions.
- The voice end word was matched as a cyclic shift of itself. The search now uses the exact word.
- Voice audio could hold a stale lock. Blocks that belong to no page now count as misses.
