# Miscellaneous modes 3.9 (feat39-misc)

Branch `feat39-misc`, family block 750 - 769 (one family used: 750, JT6M).
Every result here comes from simulated signals only. No recording of a real on-air signal was available for any of these modes.

## Added

### JT6M (family 750, engine standard 752, poll slot 751)

- JT6M is the WSJT 6 m scatter mode. It uses 44 tones, 11025 / 512 = 21.533 Hz apart.
  - The sync tone is at 1076.66 Hz.
  - The data tones run from 1098.2 to 2002.6 Hz.
  - The symbol rate is 21.533 baud.
  - Every third symbol is the sync tone, and the two symbols after it carry one character each.
  - The character set, in tone order, is `0-9 . , space / # ? $ A-Z`.
  - The message gets one trailing space and is padded to an even length, then repeats.
- Facts were read from WSJT 5 (`gen6m.f`, `gentone.f`) as carried in MSHV (GPL-3.0). The receiver and generator are new code (`core/src/hamsci/hamsci_jt6m.cpp`). JT6M is part of the ham science receiver (kind 9).
- The receiver searches symbol phase, tuning error (+-200 Hz) and frame phase.
  - It lists the pings and their characters.
  - It folds a long weak signal over its message period: the "averaged message", for ionospheric scatter.
- The test generator plays three pings, 0.6 to 1.6 s long, in 24 s of noise.
- `tests/hamsci/test_hamsci_jt6m.cpp` covers:
  - the tone plan and character table
  - a round trip with tuning errors of -120 to +180 Hz and drift of up to 6 Hz/s
  - a noise sweep
  - a 0.6 s ping inside 20 s of noise at window edges (streaming)
  - a -6 dB continuous signal recovered by folding
  - noise alone and carriers (no decode)
  - 8, 11.025, 24 and 48 kHz audio
  - two overlapping signals
- `tests/hamsci/test_hamsci_rx.cpp` runs JT6M through the engine front end at 600 ksps.
- Simulated only.

### 8PSK125 and 8PSK250 (PSK mode, modulation 7 and 8)

- fldigi's 8PSK without FEC:
  - fldigi's symbol k steps the baseband phase by 180 + 45 k degrees. Its modulator sends I cos + Q sin, so on the air the step is 4 - k eighth turns.
  - 3 bits per symbol, least significant first.
  - IZ8BLY MFSK varicode.
  - Preamble: reversals and a NUL. Postamble: three NULs and a steady carrier.
- Read from fldigi `src/psk/psk.cxx` (GPL-3). The code is new: `core/src/psk/psk_varicode.cpp`, the 8PSK branch in `psk_rx.cpp`, `psk_gen.cpp` and `ham_kbd.cpp`.
- Both appear in the PSK modulation list, Auto included. They also appear in the shortwave route options and the keyboard transmit renderer.
- `tests/psk/test_psk_8psk.cpp` covers:
  - the phase map and the varicode round trip
  - tuning errors of +-60 Hz, drift of 3 Hz/min and clock errors of 40 ppm
  - a noise sweep
  - two signals at once
  - a BPSK31 signal set to 8PSK (no false text)
  - noise alone
- Simulated only.

## Already in OnAir (not rebuilt)

- **G3RUH 9600 packet:** the APRS / Packet mode already decodes 9600 baud G3RUH (`core/src/packet/packet_rx.cpp`: two slicers, the x^17 + x^12 + 1 descrambler and NRZI). Its generator also sends 9600 baud frames (`packet_gen.cpp`).
- **PSK63, PSK125, PSK250, QPSK31, QPSK63:** already modulations of the PSK mode (BPSK31/63/125/250, QPSK31/63/125).
- **EDACS control channel:** the EDACS trunking mode (family 130).
- **ETCS Eurobalise up-link (SUBSET-036):** the EBICab / Eurobalise mode (family 207, `core/src/ebicab`) already decodes SUBSET-036 telegrams.
- **Chayka:** it sends the same 100 kHz pulse groups as Loran-C, so the eLoran / Loran-C mode's blind GRI search already finds it. This branch adds the Chayka GRIs 4970, 5960, 5970 and 8000 to the chain table and the sweep test (commit "feat(eloran): name the Chayka chains").

## Skipped, and why

- **8PSK500, 8PSK1000, and the FEC variants (8PSK125F/FL, 250F/FL, 500F, 1000F, 1200F):** the PSK receiver brings every signal to a 1 kHz sample rate, which is too slow for 500 and 1000 baud. The FEC variants need fldigi's K = 13 / K = 16 codes, interleaver and puncturing. Neither was built in this run.
- **FreeDV 2020 / 2020B:** OnAir's FreeDV runs inside the codec2 library loaded at run time. The codec2 1.2.0 that OnAir ships is built without LPCNet. Its `freedv_open(FREEDV_MODE_2020)` then returns a decoder that was never set up for 2020, so it cannot be called safely. A native 2020 modem was not written in this run. That would be the OFDM 2020 / 2020B configuration with the HRA_504_396 LDPC code and the user-installed LPCNet speech. It remains open.
- **TETRA TEDS signalling and identity:** TEDS is a separate QAM multicarrier air interface (25 to 150 kHz channels) in ETSI EN 300 392-2. OnAir's TETRA receiver handles only the pi/4-DQPSK phase-modulation channel, and its PDU layer is in the Rust parser. A TEDS receiver was not started in this run, and no recording was available.
- **GE-Star:** no public description of the format was found (bit rate, frame, ID fields): not on sigidwiki, and in no open-source decoder. Only service manuals say that it exists. It was not built; no protocol was invented.
