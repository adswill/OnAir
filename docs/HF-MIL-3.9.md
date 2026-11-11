# HF military and utility modes (3.9)

Receive-only decoders for HF military and utility waveforms that OnAir did not read before. They join the HF utility family (`core/src/hfutil`, `app/hfutil`) as kinds 10 to 13 in mode families 603 to 606. SITOR-A extends the existing SITOR mode (family 119), so it adds no mode entry. Each one has a built-in test signal and tests in `tests/hfutil` or `tests/sitor`.

**All of them are tested on simulated signals only.** No real recording of any of these modes was available.

Encrypted content stays as bytes. Nothing is decrypted.

## Checked before building

ALE (2G), STANAG 4285, MIL-STD-188-110 serial, ARQ-E and ARQ-N were already in OnAir (MODES.md). The HF waveform identifier (family 145) already named Link 11 CLEW and STANAG 4481 from their spectra. It identifies only and decodes nothing.

## Added

| Family | Mode | Source | What is read |
|---|---|---|---|
| 603 | STANAG 4539 (MIL-STD-188-110B/C Appendix C high rate) | MIL-STD-188-110B Appendix C. The same appendix in 110C Change 1 (both on sigidwiki). | The full chain at 3200, 4800, 6400, 8000 and 9600 bit/s, all six interleavers, and 12800 uncoded. The receiver reads the rate and interleaver from D0 D1 D2 in the preamble. It can join a transmission at a reinserted preamble. A decision-feedback equaliser (3.9, see below) handles multipath channels. Then the deinterleaver and a tail-biting Viterbi decoder. It finds the end-of-message pattern, frames STANAG 5066 D_PDUs, and shows the bytes as text or hex. |
| 604 | 3G-ALE | MIL-STD-188-141B Appendix C: C.4.12, C.5.1.2 to C.5.1.8, C.5.2.2, C.5.3.4, C.5.4.4 and C.5.5.4 (hflink.com copy) | All five burst waveforms of 141B, BW0 to BW4, each found at any time (see "3G-ALE traffic bursts" below). BW0: the Walsh symbols, the PN de-spreading, the 4 x 13 interleaver and the tail-biting K=7 code, the PDU CRC (CRC-4 or CRC-8). LE_Call, LE_Handshake, LE_Notification and LE_Broadcast are shown in words. The other link-setup PDUs are shown as bits with the CRC checked. BW1 to BW4 carry the TM, HDL and LDL PDUs, shown in words, and the data packets are put back together into datagrams. |
| 605 | Link 11 CLEW (frame structure) | Public summaries: Wavecom Link 11 notes, sigidwiki. MIL-STD-188-203-1A is not public. | It finds each message from its 605 Hz Doppler tone and 2915 Hz sync tone. It reports the frame rate (13.33 or 22 ms), the Doppler offset, the preamble length, the number of data frames and the constellation offset. It also gives each frame's 15 phase changes as raw quarter turns. |
| 606 | STANAG 4481 FSK | STANAG 4481 Annex C | Single-channel RATT (+-425 Hz) or up to 16 sub-carriers at 255 + 170 n Hz (+-42.5 Hz), detected automatically. Speed is measured per channel (45.45, 50, 75 or 100 Bd). ITA2 start-stop text is shown when the channel is plain. Synchronous (encrypted) channels are shown only by speed and bit count. |
| 119 | SITOR (now with SITOR-A / AMTOR) | ITU-R M.476-5 Annex 1, 3.1 | Both stations of an ARQ link: blocks every 450 ms, the CS1, CS2 and CS3 control signals, and the repeats dropped by following them (or by comparing blocks when the receiving station is not heard). It also reads the selective call from the call blocks, the "+?" change of direction, the beta-alpha-beta acknowledgement, the other station's text and the end of communication. |

### STANAG 4539 equaliser

The mini-probe is a 16-symbol Frank sequence sent 31 symbols long, so 16 samples of each probe see only probe symbols and give the channel's 16 taps exactly (the sequence's periodic correlation is perfect). The receiver does this on every mini-probe and on both probes of each reinserted preamble. The 16-tap window is placed from the averaged delay profile, with its edge where the profile has least power, so that a path is never split; the window follows a sample clock error. The estimates are interpolated (cubic over the two probes before and the two after, with the common rotation taken out first). Every 16 symbols the MMSE feed-forward filter (21 taps) and the feedback filter are computed from the interpolated channel, with the first path as the reference. Past symbols are cancelled with the decisions, or with the known symbols of the probes. The soft values take the noise the filters expect, or the error the decisions show if that is larger (a fade the interpolation missed). The noise comes from the decision errors, with the probes' figure as the ceiling. The frequency is tracked from the turn between successive probes; samples already read keep their correction, so the probes and the data stay consistent. D0 D1 D2 are read through the same equaliser, at the start and at every reinserted preamble. `setEqualizer(false)` gives the one-tap receiver of the first round.

### 3G-ALE traffic bursts

| Burst | Found by | Read |
|---|---|---|
| BW1 (TM, HDL_ACK, HDL_EOM, 48 bits) | its 576-symbol preamble | 36 Walsh symbols of 64 chips, the 16 x 9 interleaver, the tail-biting K=9 rate 1/3 code, then the CRC-12 of a TM PDU (protocol 001), the CRC-16 of an HDL_ACK, or the check pattern of an HDL_EOM (protocol 000). TM PDUs show the addresses, priority, traffic type (table C-XXXVII) or the reason for a TM_TERM; HDL_ACK shows which packets were acknowledged. |
| BW2 (HDL_DATA, 3 to 24 packets) | the TLC/AGC sequence, then its 64-symbol PN preamble, which also gives FTcount (0 to 127) | Frames of 32 data and 16 known symbols, the 2^16 PN generator of figure C-12 started at 0xAB91 + FTcount, the gray code and the rotation by FTcount mod 3, the code quarter FTcount mod 4 of the K=8 rate 1/4 code, then the CRC-32 of each 1913-bit packet. The number of packets is found from where the known symbols stop. Each packet's sequence number is shown in words, and the packets are put together into the datagram. |
| BW3 (LDL_DATA, 64 or 128 bytes) | its 640-symbol preamble | 16-chip Walsh symbols, the interleaver of table C-XVI, the code half FTcount mod 2 of the K=7 rate 1/2 code, then the CRC-32. The size is tried from the smallest up. Both halves are tried, and a failed packet is kept so that its retransmission (the other half) is decoded together with it. The datagram is put together from the sequence numbers. |
| BW4 (LDL_ACK / LDL_EOM, 2 bits) | the TLC/AGC sequence, when no BW2 preamble follows | 1280 chips of a 4-ary Walsh word, added in power over 20 segments and every path. |

All of them come out of the 3G-ALE mode's PDU list with the BW0 PDUs. The built-in test signal of the mode now also sends TM_REQUEST, TM_CONFIRM, an LDL packet, an LDL_ACK and a TM_TERM after the link setup.

### Watterson channel simulator

`tests/common/watterson.h` (tests only): independent Rayleigh paths with fixed delays, each tap a complex Gaussian process with a Gaussian Doppler spectrum of the given two-sigma width (ITU-R F.520 / Rec. 520-2), the paths together keeping the average power, and noise for an SNR measured in 3 kHz. It has the ITU-R Good, Moderate and Poor presets. The test checks its statistics (about 63 % of the samples below the mean power; the tap autocorrelation at 0.32 s matching the Gaussian spectrum). OnAir's other Watterson channel lives in the KAWS Rust crate and is not reachable from these C++ tests.

## Skipped

- **STANAG 4529**: no public specification. Sigidwiki and Wavecom give only summaries (1200 Bd, 1240 Hz, a 4285 derivative). Its scrambler, frame and probe counts, interleaver and code could not be confirmed, so writing a decoder would mean inventing those details.
- **CLOVER-II**: only the concept is public (four tones 125 Hz apart, 31.25 Bd pulses, BPSM to 16P4A, Reed-Solomon). The block sizes, framing and sync are not published.
- **PAX / PAX2**: the DL4DSS description gives 8-FSK, Walsh-coded 6-bit characters and AX.25-like framing. It leaves out the scrambler sequence and the order of the Walsh rows. The mode is closed (MultiPSK), and no open decoder exists to check against.
- **Link 11 SLEW**: the single-tone variant has no public frame description.
- **3G-ALE BW5 and BW6**: MIL-STD-188-141B Appendix C defines BW0 to BW4 only (table C-III). No BW5 or BW6 is specified in this edition, so none is implemented.
- **3G-ALE BW3 with 256 and 512 bytes**: the fetch parameters of table C-XVI, as printed (irf 1, icf -7, delta icf -7), read some cells of the 44 x 48 and 64 x 65 matrices twice and others never. Only 24 x 24 and 32 x 34 (64 and 128 bytes) give a permutation. Decoding the larger packets would mean guessing a correction, so they are reported as a BW3 burst with a bad CRC, and the generator does not make them.
- **3G-ALE BW2 code combining**: each HDL forward transmission sends a quarter of the rate 1/4 code, and the sender repeats only the packets that were not acknowledged, in new positions. The receiver reads each forward transmission on its own. It does not combine a packet with its earlier copies, because a monitor cannot tell which earlier packet a repeat belongs to without the HDL state of both stations. BW3 retransmissions are combined (see below).
- **3G-ALE BW2 under multipath**: the BW2 data symbols use the channel taken from the 16 known symbols of each frame, one tap, without an equaliser. Fading multipath (ITU-R Poor) is not handled for BW2.
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
- **STANAG 4539 with the equaliser** (Watterson channels of tests/common/watterson.h: ITU-R Poor is two equal Rayleigh paths 2 ms apart with a 1 Hz two-sigma fading spread, ITU-R Moderate 1 ms and 0.5 Hz, as C.6.1 asks):
  - Table C-XV, very long interleaver, 10 fading realisations of 26 s (3 interleaver blocks) per rate, BER over the decoded bits:

    | bit/s | AWGN at the table level | ITU-R Poor at the table level | ITU-R Poor, 3 dB above |
    |---|---|---|---|
    | 3200 | 0 in 79744 bits at 9 dB | 0 in 769792 bits at 15 dB | 0 at 18 dB |
    | 4800 | 1.7e-5 (2 in 119616) at 13 dB | 3.0e-4 at 20 dB | 0 at 23 dB |
    | 6400 | 0 in 159488 bits at 16 dB | 8.1e-4 at 24 dB | 0 at 27 dB |
    | 8000 | 0 in 199360 bits at 19 dB | 3.0e-3 at 28 dB | 4.0e-5 at 31 dB |
    | 9600 | 0 in 239232 bits at 21 dB | 1.3e-3 at 33 dB | 1.3e-4 at 36 dB |

    The standard asks for 1e-5 or better over 5 hours. These runs are far shorter, so a figure of 0 means below about 4e-6 (3200 bit/s) to 1e-5. In ITU-R Poor the higher rates need about 3 dB more than table C-XV for 1e-4. The start of the transmission was missed in 1 of the 10 runs at 3200, 8000 and 9600 bit/s (a deep fade on the preamble). Those runs count from the reinserted preamble on.
  - Without the equaliser, 3200 bit/s in ITU-R Poor at 15 dB does not decode (BER 0.5). With it, no errors.
  - ITU-R Moderate, 6400 bit/s short interleaver at 26 dB: no errors. ITU-R Poor, 9600 bit/s short interleaver at 36 dB: no errors in 57600 bits.
  - C.6.3 Doppler shift test, shortened from 5 minutes to 20 s: 9600 bit/s very long interleaver, ITU-R Poor, 30 dB, +75 Hz and -75 Hz. Synchronisation was kept through two reinserted preambles (C.6.3 asks for no BER; measured 1e-1 and 9e-3, below the 33 dB of table C-XV).
  - C.6.4 Doppler sweep, shortened from 1 hour to 30 s: 9600 bit/s very long, AWGN 24 dB, a 3.5 Hz/s triangle between -75 and +75 Hz: no errors in 288000 bits.
  - ITU-R Poor with the sample clock 40 ppm fast, 6400 bit/s at 30 dB over 20 s: no errors.
  - All the AWGN, offset, drift, clock, echo, chunk, two-message, late-join and noise-only cases of the first round still pass with the equaliser on, and all 30 rate and interleaver combinations decode (ONAIR_TEST_LONG=1).
- **3G-ALE BW1** (TM_REQUEST, TM_CONFIRM, TM_TERM, HDL_ACK, HDL_EOM):
  - 5 of 5 PDUs in each of: clean, +60 Hz, -75 Hz, a drift of 3 Hz/s, +-60 ppm, a 1 ms echo at -6 dB, 6 dB, 0 dB and -4 dB, and chunks of 77 samples.
  - AWGN, 10 bursts per level: 10 of 10 down to -8 dB, 6 of 10 at -10 dB.
  - ITU-R Poor: 9 of 10 at 10 dB, 10 of 10 at 5 dB (different fading realisations); 4 of 5 in the test at 10 dB.
- **3G-ALE BW2** (HDL_DATA): a 600-byte datagram in 3 packets is put back together with FTcount 0, 1, 5 and 7 (all four code quarters, all three symbol rotations): clean, +35 Hz at 18 dB, and -50 Hz with a 1 Hz/s drift and a 30 ppm clock at 20 dB. A 1200-byte datagram in 6 packets at 18 dB. One forward transmission carries a quarter of the code and no redundancy, so it needs about 15 dB: 3 of 3 packets at 18 and 15 dB, 0 of 3 at 12 dB.
- **3G-ALE BW3** (LDL_DATA, 64 and 128 bytes, both code halves): read clean, at -40 Hz and 3 dB, at +70 Hz with a 2 Hz/s drift and -40 ppm at 0 dB, and with a 1 ms echo at -6 dB. In AWGN, 9 of 10 at -2 dB, none at -5 dB in one transmission. Two transmissions at -7 dB (the first and its retransmission, the two halves of the code) are combined: 4 of 6 pairs read, none alone. In ITU-R Poor at 10 dB, 5 of 10 read in one transmission (a fade inside the 1.2 s burst costs the packet; LDL repeats it).
- **3G-ALE BW4** (LDL_ACK / LDL_EOM): all four words right, clean, at -6 dB and +55 Hz, and in ITU-R Poor at 3 dB. AWGN: 20 of 20 right at -8 dB, 12 of 20 found at -10 dB, no wrong word in any run.
- **3G-ALE exchange**: a BW0 call, TM_REQUEST, a 3-packet HDL datagram, HDL_ACK, HDL_EOM, an LDL packet and an LDL_ACK in a row, +25 Hz, 20 ppm and 15 dB, through the 3G-ALE receiver: every burst and the datagram are read. Noise alone gives nothing.
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
- **STANAG 4539 acquisition in fading**: the start of a transmission is found from the 103-symbol block. In a deep fade of both paths it can be missed (1 run in 10 in the table C-XV runs below); the receiver then joins at the next reinserted preamble, 8.64 s later. C.6.2 (acquisition) is "not yet standardized".
- **STANAG 4539 decoding delay**: with the equaliser, a frame is decoded once the mini-probe after the next frame is in (about 0.24 s), except for the last frame of a message.
- **STANAG 4539 acquisition**: it uses the 103-symbol known block (preamble end or reinserted preamble). Acquiring from the mini-probes alone (C.5.1: "deduce the data rate from the data portion") is not done.
- **3G-ALE encoder**: the encoder cells are read from C.5.1.3.3 the same way as the MIL-STD-188-110 code (x6 the newest bit, the "1" term the oldest). The prefixes of LE_Call (00), LE_Handshake (10), LE_Notification (00 111111) and LE_Broadcast (01110) come from figure C-17 and the C.5.2.2 example. The other "01" PDUs (time offset, group time broadcast, scanning call) are not split into fields, because the extracted figure does not show their prefixes clearly.
- **Link 11**: the tone plan and frame rates come from summaries. Some details are assumptions and are marked as such in hfu_link11.h: the sync tone turning over every preamble frame, the Doppler and data tone levels, and keying at frame boundaries. The receiver does not depend on the turn-over. No bits, words or addresses are shown, because their mapping is not public.
- **STANAG 4481**: the standard gives no speed or alphabet for the FSK broadcasts. ITA2 start-stop at 45.45 to 100 Bd is tried because plain RATT uses it. The space tone is taken as the higher tone in multi-channel mode too, which the standard states for the single channel only.
- **3G-ALE BW2 PN generator**: figure C-12 is a drawing. Read from it: 16 cells, the input on the left, feedback from cells 1, 2, 11 and 16 (B0), and B2 B1 B0 the three rightmost cells. This register as drawn has a period of 21845, not the 65535 of a maximum-length sequence that the caption names. No other reading of the tap positions in the drawing gives 65535 either. The code follows the drawing, so the BW2 spreading may differ from real equipment.
- **3G-ALE HDL_ACK CRC**: table C-XLVII says the CRC-16 covers "the Type and Ack Bit-Mask fields". It is computed over those 25 bits only (not the protocol and reserved fields).
- **3G-ALE BW4**: the same two bits are an LDL_ACK in the reverse direction and an LDL_EOM in the forward direction (C.5.5.4). A monitor does not know the direction, so the value 11 is shown as both readings.
- **3G-ALE multipath**: BW1, BW3 and BW4 add the Walsh correlations of up to three paths found on the preamble or the TLC/AGC sequence (a RAKE receiver). BW0 (from the first 3.9 round) uses the strongest path only.
- **SITOR-A**: only a monitor. Nothing is transmitted. The control signal is looked for in the 24 bits after each block, and its position is learned from the traffic.

## Tests

`tests/hfutil/test_hfutil_hdr.cpp`, `test_hfutil_hdreq.cpp` (the equaliser through Watterson channels; table C-XV with `ONAIR_TEST_LONG=1`), `test_hfutil_ale3g.cpp`, `test_hfutil_ale3gbw.cpp` (BW1 to BW4), `test_hfutil_link11.cpp`, `test_hfutil_4481.cpp`, `tests/sitor/test_sitor_arq.cpp`, `tests/app/test_hfutil_mode.cpp` (wiring). Long sweeps of STANAG 4539 (all 30 combinations through the waveform) run with `ONAIR_TEST_LONG=1`.
