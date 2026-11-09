# Iridium decoder audit and changes (3.7, branch feat37-iridium2)

## Audit against iridium-toolkit, gr-iridium, iridium-extractor

Already matching (checked in code, tests pass): burst detector (FFT, per-bin noise floor, duty cycle), DQPSK mapping, unique-word rule
(gr-iridium check_sync_word), uplink preamble handling, decision-directed PLL, bit order (toolkit order, i^1 swap), BCH repair of LCW / IRA /
IBC / MSG, IDA de-interleave + CRC, ACARS/SBD assembly, voice byte order (first bit received = LSB, same as bits_to_dfs.py), wide capture
(one worker pool over 10 Msps), +-81 kHz offset + 37.5 kHz Doppler, +-100 ppm clock.

Gaps found:
1. **Voice slots carry more than speech.** The toolkit tests a voice slot in this order: CRC24 (IP data, was done), RS(52,10) over 6-bit symbols
   (VO6), RS(55,31)/8 over bytes (VOD), zero-sum (VOZ); only the rest is "VOC", the only thing the vocoder may see. OnAir sent VO6/VOD/VOZ
   frames to the user's codec plug-in. Plug-ins then output concealment (silence) for them and the call looked silent. Fixed (below).
2. **Lost bursts shortened the call.** A missed 90 ms frame left no hole in the WAV, so speech was time-compressed. Fixed: holes of up to
   11 frames (never more than 1 s) are filled with silence of the codec's frame length.
3. Voice FEC / vocoder layout itself is the plug-in's job (never bundled). Byte order and the 312-bit field verified against bits_to_dfs.py;
   the plug-in must expect the toolkit's 39-byte `.dfs` frames (first bit = LSB). If a plug-in expects the other order, WAVs are silent or noise:
   call warning now says so and the per-call counters show it (see below). No real recording was available to prove more.
4. Not done (needs real recordings or large work): soft-decision BCH, Doppler tracking across bursts to predict the next burst's frequency,
   multi-burst voice stitching across slots, satellite/beam labels from non-ring frames, Reed-Solomon error correction (the new VO6/VOD test is
   an exact code-word check; a damaged data slot still reaches the codec as VOC).

## Changes
- `iridium_frame.cpp`: GF(2^6) / GF(2^8) Reed-Solomon parity checks, `IridiumFrame::vtype` = VOC / VO6 / VOD / VOZ. Known answers from the
  reedsolo library with the toolkit's parameters (rs.py: prim 0x11d, fcr 0; rs6.py: prim 0x43, fcr 54).
- `iridium_voice.cpp`: data slots counted (`IridiumCall::dataSlots`) and never sent to the codec; call status "data" when only data;
  gap filling (`gapFrames`); refused frames counted (`emptyFrames`).
- Tests: `test_iridium_vslot` (classification, KATs, no codec contact), gap block in `test_iridium_voice`, `test_iridium_sens` (low-SNR sweep by threshold).

## Sensitivity (4 Msps, 340 bursts per point, Es/N0 of the burst, found = unique word found)
| Es/N0 | thr 13 dB (default) | thr 10 dB | thr 8 dB |
|---|---|---|---|
| 8 | 94 % | 98 % | 98 % |
| 6 | 51 % | 81 % | 83 % |
| 4 | 8 % | 40 % | 50 % |
Extra (false) bursts per 340: 0 / 9 / 170. Bit-exact bursts are limited by the hard decisions, not detection: 97 % at 12 dB, 65 % at 10 dB, 7 % at 8 dB,
so frames only decode through the BCH codes down to about 9-10 dB whatever the threshold. Result: lowering the threshold to 10 dB gains about
2 dB of *detection* but no decodable-frame gain; the default stays 13 dB. A real decoding gain needs soft-decision BCH (not done).
