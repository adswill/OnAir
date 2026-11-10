# HF military and utility modes (3.9)

Receive-only decoders for HF military and utility waveforms that OnAir did not read before. They join the HF utility family (`core/src/hfutil`, `app/hfutil`) as kinds 10 to 13 in mode families 603 to 606. SITOR-A extends the existing SITOR mode (family 119), so it adds no mode entry. Each one has a built-in test signal and tests in `tests/hfutil` or `tests/sitor`.

**All of them are tested on simulated signals only.** No real recording of any of these modes was available.

Encrypted content stays as bytes. Nothing is decrypted.

## Checked before building

ALE (2G), STANAG 4285, MIL-STD-188-110 serial, ARQ-E and ARQ-N were already in OnAir (MODES.md). The HF waveform identifier (family 145) already named Link 11 CLEW and STANAG 4481 from their spectra. It identifies only and decodes nothing.

## Added

| Family | Mode | Source | What is read |
|---|---|---|---|
| 603 | STANAG 4539 (MIL-STD-188-110B/C Appendix C high rate) | MIL-STD-188-110B Appendix C. The same appendix in 110C Change 1 (both on sigidwiki). | The full chain at 3200, 4800, 6400, 8000 and 9600 bit/s, all six interleavers, and 12800 uncoded. The receiver reads the rate and interleaver from D0 D1 D2 in the preamble. It can join a transmission at a reinserted preamble. Mini-probes give the channel and frequency tracking, followed by the deinterleaver and a tail-biting Viterbi decoder. It finds the end-of-message pattern, frames STANAG 5066 D_PDUs, and shows the bytes as text or hex. |
| 604 | 3G-ALE | MIL-STD-188-141B Appendix C, C.5.1.2, C.5.1.3, C.5.1.8 and C.5.2.2 (hflink.com copy) | BW0 bursts, found by their 384-symbol preamble at any time. Decoding covers the Walsh symbols, the PN de-spreading, the 4 x 13 interleaver and the tail-biting K=7 code. The PDU CRC is checked (CRC-4 or CRC-8 of C.4.12). LE_Call, LE_Handshake, LE_Notification and LE_Broadcast are shown in words. The other PDUs are shown as bits with the CRC checked. |
| 605 | Link 11 CLEW (frame structure) | Public summaries: Wavecom Link 11 notes, sigidwiki. MIL-STD-188-203-1A is not public. | It finds each message from its 605 Hz Doppler tone and 2915 Hz sync tone. It reports the frame rate (13.33 or 22 ms), the Doppler offset, the preamble length, the number of data frames and the constellation offset. It also gives each frame's 15 phase changes as raw quarter turns. |
| 606 | STANAG 4481 FSK | STANAG 4481 Annex C | Single-channel RATT (+-425 Hz) or up to 16 sub-carriers at 255 + 170 n Hz (+-42.5 Hz), detected automatically. Speed is measured per channel (45.45, 50, 75 or 100 Bd). ITA2 start-stop text is shown when the channel is plain. Synchronous (encrypted) channels are shown only by speed and bit count. |
| 119 | SITOR (now with SITOR-A / AMTOR) | ITU-R M.476-5 Annex 1, 3.1 | Both stations of an ARQ link: blocks every 450 ms, the CS1, CS2 and CS3 control signals, and the repeats dropped by following them (or by comparing blocks when the receiving station is not heard). It also reads the selective call from the call blocks, the "+?" change of direction, the beta-alpha-beta acknowledgement, the other station's text and the end of communication. |

## Skipped

- **STANAG 4529**: no public specification. Sigidwiki and Wavecom give only summaries (1200 Bd, 1240 Hz, a 4285 derivative). Its scrambler, frame and probe counts, interleaver and code could not be confirmed, so writing a decoder would mean inventing those details.
- **CLOVER-II**: only the concept is public (four tones 125 Hz apart, 31.25 Bd pulses, BPSM to 16P4A, Reed-Solomon). The block sizes, framing and sync are not published.
- **PAX / PAX2**: the DL4DSS description gives 8-FSK, Walsh-coded 6-bit characters and AX.25-like framing. It leaves out the scrambler sequence and the order of the Walsh rows. The mode is closed (MultiPSK), and no open decoder exists to check against.
- **Link 11 SLEW**: the single-tone variant has no public frame description.
- **3G-ALE BW1 to BW4** (traffic management, HDL and LDL data): not done in this round. BW0 carries all link-setup PDUs.
- **STANAG 4481 PSK**: this is the STANAG 4285 waveform, which OnAir already reads in its own mode.

## Measured on simulated signals

The SNR figures are measured in 3 kHz. All results below come from the tests named at the end of this page.

- **STANAG 4539**:
  - Every rate decodes byte-exact with a +37 Hz offset, and 12800 bit/s decodes at 35 dB.
  - With the short interleaver there were no errors at 11 / 15 / 19 / 22 / 24 dB for 3200 / 4800 / 6400 / 8000 / 9600 bit/s. These levels are 2 to 3 dB above the AWGN figures in table C-XV.
  - Other byte-exact cases:
    - 9600 bit/s very long interleaver at 28 dB.
    - 9600 bit/s short at -75 Hz and 30 dB.
    - 4800 bit/s very short at +75 Hz and 20 dB.
    - A drift of 3.5 Hz/s.
    - A sample clock off by +-40 ppm.
    - Three AGC blocks.
    - A 1 ms echo at -12 dB (3200 bit/s).
    - Chunks of 7 to 100000 samples.
  - Two messages in a row are read, each with its own rate.
  - When the first 3 s are missed, the receiver joins at the reinserted preamble 8.64 s in.
  - Noise alone never locks. The receiver also works through the engine's front end at 250 ksps.
  - With ONAIR_TEST_LONG=1, all 30 rate and interleaver combinations decode byte-exact at 30 dB and +15 Hz.
- **3G-ALE BW0**:
  - 4 of 4 PDUs were read in each of these cases: clean, +60 Hz, -75 Hz, a drift of 3 Hz/s, +-60 ppm, a 1 ms echo at -6 dB, 6 dB and 0 dB.
  - At -4 dB, 8 of 8 were read. At -8 dB, 6 of 8 were read.
  - Bursts back to back are all read, and noise alone gives no PDU with a good CRC.
  - The worked LinkID examples of C.5.2.2.2 match.
- **Link 11 CLEW**:
  - Both frame rates and both constellation offsets (0 and 45 degrees) are read.
  - The frame count and all 600 phase changes are exact in these cases: clean, Doppler +40 and -55 Hz (measured within 2 Hz), 15 dB (fast) and 10 dB (slow), and a 60 ppm clock over 150 frames (2250 of 2250 phase changes).
  - A 1 ms echo at -10 dB and chunks of 77 samples also give exact results.
  - Two messages (one fast, one slow) are read, and noise alone gives none.
- **STANAG 4481**:
  - A single channel at 45.45, 50, 75 and 100 Bd, with +23 Hz offset at 15 dB, gives the exact text, and the tones are found within 10 Hz.
  - In a multi-channel signal at 20 dB, the two plain channels are read and the synchronous channel is reported at 75 Bd.
  - In a full band of 16 channels at 50 Bd and 25 dB, all 16 texts are read.
  - Noise alone gives nothing.
- **SITOR-A**:
  - The text is exact, without repeats, in these cases: offsets from -60 to +150 Hz, swapped tones, 15, 8 and 4 dB, a drift of 1 Hz/s, the receiving station 20 dB weaker or 25 Hz off, round trips of 3 to 15.6 bits, repeats asked every 3rd block, chunk sizes, and the receiving station not heard at all.
  - The selective call is read. A change of direction gives both texts.
  - At 0 dB the text is partly read.

## Limits and assumptions

- **STANAG 4539 scrambler**: figure C-5 is a drawing, not text. The text gives the polynomial x^9 + x^4 + 1, an initial value of 1, the value as "the rightmost bits", and a shift after use. The shift direction used here is a left shift with feedback from bits 8 and 3 into bit 0. The opposite direction would give a different sequence. Without a real recording, this choice cannot be confirmed.
- **STANAG 4539 equaliser**: the receiver interpolates the channel between mini-probes. There is no adaptive equaliser, so strong multipath (the ITU-R Poor channel) is not handled. A 1 ms echo at -12 dB is.
- **STANAG 4539 acquisition**: it uses the 103-symbol known block (preamble end or reinserted preamble). Acquiring from the mini-probes alone (C.5.1: "deduce the data rate from the data portion") is not done.
- **3G-ALE encoder**: the encoder cells are read from C.5.1.3.3 the same way as the MIL-STD-188-110 code (x6 the newest bit, the "1" term the oldest). The prefixes of LE_Call (00), LE_Handshake (10), LE_Notification (00 111111) and LE_Broadcast (01110) come from figure C-17 and the C.5.2.2 example. The other "01" PDUs (time offset, group time broadcast, scanning call) are not split into fields, because the extracted figure does not show their prefixes clearly.
- **Link 11**: the tone plan and frame rates come from summaries. Some details are assumptions and are marked as such in hfu_link11.h: the sync tone turning over every preamble frame, the Doppler and data tone levels, and keying at frame boundaries. The receiver does not depend on the turn-over. No bits, words or addresses are shown, because their mapping is not public.
- **STANAG 4481**: the standard gives no speed or alphabet for the FSK broadcasts. ITA2 start-stop at 45.45 to 100 Bd is tried because plain RATT uses it. The space tone is taken as the higher tone in multi-channel mode too, which the standard states for the single channel only.
- **SITOR-A**: only a monitor. Nothing is transmitted. The control signal is looked for in the 24 bits after each block, and its position is learned from the traffic.

## Tests

`tests/hfutil/test_hfutil_hdr.cpp`, `test_hfutil_ale3g.cpp`, `test_hfutil_link11.cpp`, `test_hfutil_4481.cpp`, `tests/sitor/test_sitor_arq.cpp`, `tests/app/test_hfutil_mode.cpp` (wiring). Long sweeps of STANAG 4539 (all 30 combinations through the waveform) run with `ONAIR_TEST_LONG=1`.
