# CIS HF data modes (3.9)

Seven receive-only modes for the CIS (Russian) HF data signals listed on sigidwiki.com, families 620 - 626 (engine standards 622 - 628,
receivers 621 - 627). The code is in `core/src/cishf`, `app/cishf` and `tests/cishf`. Tune the dial frequency, upper sideband. The radio
sits 20 kHz above the dial, as it does for the other HF modes, and any frequency can be entered.

**Simulated only.** Every check ran on the generators in `cishf_gen.cpp`. No real recording of any of these signals has been through the
decoders, so every mode needs a test with a real recording.

| Family | Mode | What it does |
|---|---|---|
| 620 | CIS-36-50 / CIS-50-50 (BEE, T-600) | Decoder: 2-FSK demodulator (36 / 50 / 75 / 100 / 150 Bd run side by side, shift and centre found from the spectrum, both polarities). It finds the start of a message (a 44-bit start word with 21 ones, then the 70-bit session key sent twice), reads the 7-bit 3:4 characters, and ends the message at three 0x77 characters. A follow-on message sent without a start sequence is picked up too. Characters are shown as hex. The traffic is encrypted and nothing is decrypted. |
| 621 | CIS-14 (AMOR, AMOR96) | Decoder: 2-FSK at 96 Bd. The 14-bit frame is aligned from its period (the two state bits stay constant), then each frame's fields are shown raw: 2 state bits, two bit-interleaved 5-bit words, 2 parity bits. |
| 622 | CIS-12 (MS5, AT-3004D, AT-3104D, FIRE, T-230) | Physical layer: the 3300 Hz pilot and its offset, the 12 carriers 700 - 2900 Hz (200 Hz apart) at 120 Bd, the symbol clock, the PSK order (DBPSK or DQPSK, from the phase-step moments) and the raw phase step of every carrier for every symbol. |
| 623 | CIS-36 (MFSK-36 duplex ARQ) | Identification only: tone count, 40 Hz spacing, the 10 / 20 / 40 Bd tone rate and the three tone groups. |
| 624 | CIS OFDM HDR modem (CIS-45, CIS-60, CIS-93) | Identification only: the OFDM symbol (useful part and guard from the cyclic-prefix correlation, so the rate too), the carrier count, the spacing and the PSK order. The variant is named from these. |
| 625 | CIS-128 | Identification only: about 128 OFDM carriers at about 21 Bd in 3 kHz. The 6 kHz form is not covered, because the decoders read 6 kHz of upper sideband. |
| 626 | CIS-3000 | Identification only: one 8-PSK carrier at 3000 Bd, from the symbol-rate line and the u^2 / u^4 / u^8 carrier lines of the matched-filtered symbols. The matched filter assumes a root-raised-cosine pulse with a roll-off of 0.15, which is not published. |

Under Advanced, the two FSK modes take a fixed speed and shift (0 = automatic). Automatic is the default. In automatic mode the shift
is read from the spectrum. At non-integer modulation indices (100 Bd with 250 Hz, 150 Bd with 500 Hz) the spectral peaks sit inside the
tones, so the displayed shift can read up to 20 % low. Decoding still works there. A forced shift finds the centre from the pair of tones
that holds the most power.

## Sources

- sigidwiki.com pages for CIS-12, CIS-14, CIS-36-50, CIS-36, CIS OFDM HDR Modem, CIS-128 and CIS-3000, and the CIS modes table on those pages.
- Rivet (Ian Wraith, GPL-3.0-or-later), `CIS3650.java`. Its comments and behaviour were used as a description of the CIS 36-50 framing: the
  start word weight, the key sent twice, the 3:4 characters, 0x77 as the end of message and the tone polarity of real recordings. No code
  was copied.

## What is not published, and what the generators use instead

- BEE: the start word itself. Only its weight (21 ones in 44 bits) is known, and that weight is what the decoder checks. The generator uses
  a fixed word with 21 ones.
- CIS-14: the parity rule, the meaning of the state bits and the tone polarity. These are shown in the order sent (1 = the higher tone) and not checked.
- CIS-12: the pulse shape and any frame format. The generator uses rectangular symbols. The decoder stops at the phase steps.
- CIS-128: the carrier spacing and the guard. The generator uses 24 Hz and 1/21 s.
- CIS-36, OFDM HDR, CIS-3000: no frame format is published. These modes identify the signal and read nothing else.

## Skipped

- **CIS-81 (CIS-8181)**: sigidwiki gives only FSK in a 600 Hz band, with no speed, no shift and no frame format, so there is nothing to
  build a decoder or a specific detector from. The HF waveform ID mode (family 145) still measures such a signal's speed and shift.
- **CIS-45 and CIS-50** are not separate modes. CIS-45 is a variant of the CIS OFDM HDR modem (family 624), and CIS-50 (BEE) is the
  CIS-36-50 / CIS-50-50 mode (family 620).
- **The other CIS table entries** (CIS-11, CIS-16, CIS-20, CIS MFSK-20 XPA, CIS MFSK-16 XPB, CIS-40.5, CIS-48, CIS MFSK-68, the VFT systems,
  CIS-112, CIS-300, CIS-1280, Russian Diplo VFT) are not in this package. Most of them are inactive and documented by their physical layer
  only. They are left for the HF waveform ID mode.
- No overlap with feat39-hfmil (SITOR / military HF), which has no CIS modes.

## Tests (`tests/cishf/test_cishf_dsp.cpp`, one binary)

- BEE at 50 and 100 Bd traffic, both polarities, carrier offsets of -60 / 0 / +85 Hz, SNR 30 and 12 dB in 3 kHz, a 40 ppm clock error, and two
  messages where the second has no start sequence: the characters must match (99 % at 30 dB, 95 % at 12 dB). The default run keeps the hardest case of each sweep; `ONAIR_TEST_LONG=1` runs them all. A forced 150 Bd / 500 Hz case.
  Noise alone must not start a message.
- CIS-14 at offsets of -40 / 0 / +120 Hz, SNR 30 and 14 dB, 30 ppm: the frame is found and 400 random frames are read field by field.
- CIS-12 with DQPSK and DBPSK, offsets of -25 / 0 / +40 Hz, SNR 30 and 15 dB, 20 ppm: the pilot offset to 1.5 Hz, the PSK order, and the phase
  steps against the transmitted ones (step error under 1 % at 30 dB and under 5 % at 15 dB).
- CIS-36 at 10 / 20 / 40 Bd, the four OFDM HDR variants, the 3 kHz CIS-128 and CIS-3000 (with offsets, noise and clock error) are identified,
  with the right rate and order. A cross check runs every identifier on every other CIS signal, and none of them may claim it.
- BEE alignment: a window one or two bits off can also pass the start check, because the key repeats. The decoder holds a passing start
  for three more bits and keeps the best-scoring alignment.
- CIS-12: the symbol clock sits in the middle of the widest high stretch of the window energy, it moves in small steps, and the lock
  survives single noisy spectra. Symbols received before the lock (1.25 s) are read too.
- The whole receiver: the engine's test signal at 250 kHz, with the dial 20 kHz below the centre, through the front end, for BEE, CIS-12 and
  CIS-3000.

## Not done here

- Android rows (`android/`) for the seven modes. The integrator's mode-list sync handles these.
