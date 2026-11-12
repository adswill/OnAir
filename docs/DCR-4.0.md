# DCR, Japan's digital convenience radio (4.0, family 782)

Receive only. ARIB STD-T98 (digital convenience radio, "デジタル簡易無線"). The standard has three volumes, one per modulation: Volume 1 pi/4-shift QPSK (9.6 kbit/s), Volume 2 real-zero SSB, Volume 3 four-level FSK (4.8 kbit/s). **This mode is Volume 3.** Volumes 1 and 2 are not decoded.
Shares the x40 receiver: variant 45, family 782, active standard 783, engine standard 784. Code `core/src/x40/x40_dcr.cpp`, channel plan `core/include/dect2/x40/dcr_plan.h`, tests `tests/x40/test_x40_dcr.cpp`, generator `tests/x40/x40_gen_dcr.h`.
One mode-list entry, not a second NXDN entry: the framing differs above the physical layer (below), so NXDN receiver and DCR receiver cannot read each other's messages.

## What DCR shares with NXDN48 and what differs (read from the standard, checked against its test frames)

| | NXDN48 | DCR (STD-T98 vol. 3) |
|---|---|---|
| Modulation | 4FSK, 2400 symbols/s, 6.25 kHz | the same; root-Nyquist raised cosine, roll-off 0.2 |
| Deviation | +-350 / +-1050 Hz | **+-315 / +-945 Hz** (Table 3-1) |
| Frame | 384 bits, 80 ms: sync 20, LICH 16, SACCH 60, 4 x 72 | 384 bits: sync 20, RICH 16, SACCH 60, TCH1 144, TCH2 144 (each TCH = two 72-bit AMBE+2 frames) |
| Sync word | 0xCDF59 | the same, 0xCDF59 |
| Whitening | PN9, seed 0x0E4, over the 182 symbols after the sync | the same |
| Channel coding | K = 5 conv. code G1 = 1+D3+D4, G2 = 1+D+D2+D4, puncturing, block interleaving; CRC-6 (SACCH), CRC-12 (FACCH1) | **identical** (SACCH 26 bits + CRC-6, 12 x 5 interleaver; PICH and error-corrected data 80 bits + CRC-12, 16 x 9). The NXDN Rust layer decodes them. |
| Link information | LICH: RF channel type, functional channel type, option, direction, parity | **RICH**: F (0 sync burst, 1 service channel), 2 reserved bits, M (traffic layout), D (0 direct), even parity. Same 8 symbols, same bit to symbol mapping |
| SACCH | 2-bit structure + 6-bit RAN, 18 bits of a 72-bit message per superframe | **one unit per frame**: M byte (F lead flag, Wr units still to come, 5-bit message type) + 18 bits; first unit: 2-bit call type, **9-bit user code (0 - 511, 0 = any)**, **7-bit manufacturer number**; message types: idle, voice (1), manufacturer defined 1 - 18, end of communication (30); up to 4 units |
| Sync burst | none (CAC on a control channel) | SB0: preamble (>= 24 bits), sync, RICH (F = 0, M = 100), SACCH, **PICH = 36-bit call sign (9 BCD digits) + 44 reserved bits**, 144 undefined bits |
| Call type | RAN, talkgroup / unit IDs of 16 bits, call type in layer 3 | no talkgroups: user code only; call type 0 normal, 1 privacy scrambled |
| Privacy | 15-bit scrambler, key 1 - 32767 | the same 15-bit scrambler (PN 15, 14), applied to the 49 voice bits before FEC |
| Voice | AMBE+2 | AMBE+2 enhanced half rate (3.6 kbit/s, 72 bits per 20 ms) recommended |
| Trunking, site info | yes | no |

## Channel plan (Tables 3-4, 3-5, 3-6), 6.25 kHz
351 MHz registered stations: channels 1 - 70 from 351.20000 MHz, 71 - 82 from 351.03125 MHz (5 W; 351.2875 MHz, channel 15, is the calling channel, no privacy or selective call on it), S1 - S5 from 351.16875 MHz and S6 - S15 from 351.10625 MHz (1 W, usable in the air): 97 channels from 351.03125 to 351.63125 MHz.
467 / 465 MHz licensed: channels 1 - 65 from 467.00000 MHz, 66 - 75 from 465.096875 MHz. 154 MHz licensed: channels 1 - 28 from 154.44375 MHz (20 - 28 data only). Relay pairs D1 - D10: input 465.034375 MHz + n x 6.25 kHz, output 468.796875 MHz + n x 6.25 kHz.
(The assignment said "30 channels from 351.20 MHz"; the standard has 82 + 15.)

## What the mode shows
Rows: **Call** (user code, manufacturer number, normal or privacy scrambled, call sign when known), **Call end** (duration, frames, voice frames, bit errors, end-of-communication message or signal lost), **Call sign** (9 BCD digits of the synchronous burst), **Data** (80-bit blocks of the error-corrected traffic channels, CRC-12 good, as hex), **Manufacturer message** (format, manufacturer number, user code, the extra bits of 1 - 4 units), **Idle**. Facts: channel on the plan, deviation and polarity, carrier offset, bit errors, frame counts, voice frames.
A message counts when the same one comes in two frames in a row (the SACCH CRC-6 lets noise through once in 64); the end message is accepted for the call that is on.
**Voice is counted, not played** (each 80 ms frame with audio carries four 20 ms AMBE+2 frames). Audio through the user-installed ambe2 plug-in is not wired into this mode; the frames are not decoded to speech.
**Scan**: with a capture of 100 ksps or more tuned so that two or more plan channels are inside +-42 % of the sample rate, every channel is watched (4096-point spectrum, +-2.5 kHz around the channel, 10 dB over the quietest fifth of the channels) and a decoder starts on each active one (up to 8; one is dropped after 8 s without a frame, and not tried again for 20 s). In the 351 MHz band the default 250 ksps shows 35 channels; 500 ksps shows about 80. Rows then start with the channel label ("351 MHz ch 15 UC 7"). Tuned outside the plan, or below 100 ksps: one channel at the tuned frequency (carrier offset up to about +-2.5 kHz).

## Checked
- **The standard's own test frames** (chapter 7, whitened bits in hexadecimal): a script reproduced RICH, SACCH and PICH bits of all four test signals (user code 1 and 511, normal and privacy) from the field definitions with the same coding as the NXDN layer: exact match, including the undefined field. The receiver test feeds these frames (synchronous burst with call sign 100000001, 24 audio frames with the standard's TCH bits, the end frame) as 4FSK at 48 ksps, 250 ksps and 1 Msps: user code, call type, call sign, end of communication and 96 voice frames read right. The TCH voice bits and the privacy scrambling of the voice were not checked against an AMBE+2 encoder.
- Frames built from the fields, 8 transmissions per case (user codes 0 - 511, manufacturer 0 - 127, normal and scrambled), at 48 ksps, 250 ksps, 1 Msps: carrier +-2000 Hz, off-centre +40 / -60 kHz, symbol clock +-150 ppm, reversed polarity, deviation x 0.85 and x 1.15, 24 dB and 20 dB carrier to noise in 6.25 kHz. Manufacturer messages of 1, 2 and 4 units, data blocks, call sign. All 14 impairment cases found 8 of 8 calls with no wrong call (CI run 43159024184; a first version with a wide channel filter lost calls below 18 dB, a Blackman filter of 5 kHz and a carrier tracker in front of the discriminator fixed that: 30 dB down to 8 dB carrier to noise in 6.25 kHz all 16 of 16 calls, 0 wrong; the sweep now also runs 6 to 2 dB, numbers with `ctest -R x40_dcr -V`).
- Noise sweep 30 dB down to 8 dB, wrong calls counted next to the found ones. 150 s of noise, of noise with a carrier and of noise with a 1 kHz FM tone: no row, no good frame. Scan: three channels at different levels in one 500 ksps capture, off-plan tuning, scan false starts (noise, carrier, FM tone on a channel).
- **Not checked: a real DCR radio.** No recording with a licence that allows tests was found (searched for public IQ of Japanese digital simplified radio). Everything above is simulated signals and the standard's test frames. NXDN48 signals use the same physical layer: an NXDN48 call decodes as DCR nonsense and a DCR call as NXDN nonsense; the outer deviation in the facts (945 Hz DCR, 1050 Hz NXDN) tells them apart.
- Not done: Volumes 1 (pi/4 QPSK) and 2 (RZ-SSB), audio, privacy-key decryption of the voice, transmit.
