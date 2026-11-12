# HF data modes from Rivet (4.0)

A Discord user asked for "more HF data modes", naming Rivet (https://github.com/priyom/Rivet), the decoder of the Enigma 2000 / priyom.org
community for number stations and HF utility signals. This branch (feat40-rivet) adds the Rivet modes that OnAir did not have.

## Licence

Rivet is GPL-3.0-or-later ("either version 3 of the License, or (at your option) any later version"; Rivet Copyright (C) 2011 Ian Wraith).
That is compatible with OnAir's GPL-3.0-or-later, so its logic was ported to C++. Every ported file keeps Rivet's copyright and warranty
lines in its header; Rivet has a row in `tools/credits_data.tsv`, so it appears in THIRD_PARTY.md, the in-app Credits window and
docs/credits-for-website.md (`python3 tools/gen_credits.py`).

What is ported: the tone tables, the character tables, the meaning of each tone, the async and block framing, the F06 block layout and its
CRC-16, the header / F06a file blocks, the GW packet formats, the MMSI digit codings, the shore station codes and the message assembly.
What is new: the signal processing (`core/src/x40/x40_rivet.h`). Rivet finds each symbol's frequency with one FFT at 8 or 11.025 kHz
and needs the audio at those rates. Here the channel (any sample rate) is searched for the whole tone set on a long averaged spectrum,
the last 16 seconds are replayed once the tones are found, every tone has its own sliding matched filter and the symbol clock follows the
peak of the filter outputs.

## Rivet's modes against OnAir

| Rivet mode | In OnAir before | Done here |
|---|---|---|
| XPA (10 and 20 Bd) | no | new mode, family 370 (both speeds in one entry: they are one protocol and the speed is found automatically) |
| XPA2 | no | new mode, family 371 |
| CROWD36 | no | new mode, family 372 |
| FSK200/500 (F01) | no | new mode, family 373 |
| FSK200/1000 (F06) and F06a | no | new mode, family 374 (one entry: one physical layer, F06 / F06a / ASCII told apart from block 1, as Rivet switches between them) |
| GW FSK (100 Bd) | no | new mode, family 375 |
| CIS 36-50 (50 Bd) | yes: CIS-36-50 / 50-50 (BEE), family 620 | nothing: OnAir already reads the same as Rivet (start word, session key twice, 7-bit characters as hex, end of message) and more (36 / 50 / 75 / 100 / 150 Bd) |
| CCIR 493-4 (HF selective calling) | yes: DSC, family 15 (MF / HF and VHF) | nothing |
| Baudot (various speeds) | yes: RTTY, family 25 (automatic speed and shift) | nothing |
| FSK (raw) | yes: FSK parameter analyser (767), FSK modems (780) | nothing |
| AT-3004D / AT-3104 (CIS-12) | CIS-12 physical layer, family 622 | nothing: Rivet's AT3x04 decoder is marked experimental and "doesn't work yet" in its own code, so there is no decoding to take over |

## The modes

Tune the dial frequency in upper sideband (the usual way to receive these signals); the decoders find the tones anywhere in the channel
(about +-3.4 kHz around the tuned frequency), so the dial need not be exact. All are receive only.

| Mode | Signal | Shows |
|---|---|---|
| XPA | 20-tone MFSK, 40 Hz apart from 520 Hz, 10 or 20 Bd | start tones, message start, five-figure groups ten to a line, block sync and break groups, end of message |
| XPA2 | 16-tone MFSK, 15.625 Hz apart from 978 Hz, 7.8125 Bd | five-figure groups ten to a line, end of message |
| CROWD36 | 34-tone MFSK, 40 Hz apart from 1410 Hz, 40 Bd | the sync tones, then the characters line by line (letters and figures, Cyrillic figure-case letters as in Rivet) |
| FSK200/500 | 200 Bd FSK, 500 Hz shift, async ITA2, 1.5 stop bits | figure groups line by line, framing errors |
| FSK200/1000 | 200 Bd FSK, 1000 Hz shift, 288-bit blocks, CRC-16 | header blocks (block count, messages, their start blocks), F06 figure blocks, ASCII blocks, F06a files (name, size, file CRC, contents), CRC per block |
| GW FSK | 100 Bd FSK, 200 Hz shift, bursts | shore free-channel markers with the station, packets with type and subtype, MMSI numbers and commands, position reports put together from fragments, ACKs |

Differences from Rivet, on purpose:
- XPA / XPA2: the readings for both speeds, both sidebands and a tone numbering off by up to three tones run side by side; the one that
  forms the most five-figure groups is shown. A tone outside the table is a '?' inside the line (Rivet ends the line and prints "UNID").
  Rivet's XPA only works in upper sideband; XPA2 tells the sideband itself, as here.
- CROWD36: the shift tones are not printed (Rivet prints "<fs>" and "<ls>": its test for them compares against a string without the
  brackets). The sync high tone is taken as tone 24, Rivet's default; Rivet lets the user change it because stations differ, OnAir has no
  setting for it yet. No text before the sync tones, as in Rivet. Upper sideband only (the sync rule cannot tell the sidebands apart).
- FSK200/500: each character is timed from its own start-bit edge, so the half stop bit needs no special case; both senses of the shift are
  tried (the one whose stop bits hold wins). Always the figures case, as Rivet prints it.
- FSK200/1000: a sync word with up to three wrong bits is accepted when the CRC then holds (Rivet: at most one); one wrong bit per block is
  corrected with the CRC (its Hamming distance is 4 here: one error corrected, two still detected). Rivet does not use bytes 24 - 35 of a
  block; neither does OnAir (the tests fill them with random bytes).
- GW FSK: Rivet's ship-name lookup (its own ship database) is not included; the MMSI is shown. Both senses of the shift are tried.
  A first bit lost before the marker pattern is padded.

## Numbers

Family, active standard, engine standard, poll slot: docs/FAMILY-NUMBERS-4.0.md ("Rivet HF modes"). Families 370 - 375, x40 variants 46 - 51.

## Tests

`tests/x40/test_x40_{xpa,xpa2,crowd36,f01,f06,gwfsk}.cpp`, generators in `tests/x40/x40_gen_rivet.h` (written from the formats as Rivet
reads them, not from the decoders; the CRC of the generator is a separate MSB-first implementation checked against the CRC-16/X-25 check
value). Every test runs the receiver with a dial error, off-centre tuning (several hundred Hz, and a receiver offset of tens of kHz at
250 ksps), a sender clock error, drift, and where the mode allows the other sideband or the reversed shift; a noise sweep with a dial error
and a clock error; and noise alone and a carrier (and for the FSK modes another mode's signal), which must give no decodes.

SNR is the signal power over the noise power in 3 kHz. A group / block / packet counts when it is read exactly right.

| Mode | 50 % point | 90 % point | Sweep conditions | False decodes (noise only, carrier, other) |
|---|---|---|---|---|
| XPA 10 Bd | -14.9 dB | -12.6 dB | 30 groups x 2, dial +23 Hz, clock +100 ppm | 0 (2 x 60 s noise, a carrier) |
| XPA 20 Bd | -12.3 dB | -11.0 dB | same | 0 |
| XPA2 | -16.5 dB | -14.5 dB | 24 groups x 2, dial +2.1 Hz, clock +100 ppm | 0 |
| CROWD36 | -7.1 dB | -5.4 dB | 40 groups x 2, dial -7 Hz, clock +100 ppm | 0 |
| FSK200/500 | -0.8 dB | +1.3 dB | 60 groups x 2, dial +12 Hz, clock +200 ppm | 0 |
| FSK200/1000 | -2.3 dB | -0.3 dB | 20 blocks x 2, dial +17 Hz, clock +200 ppm | 0 (and an FSK200/500 signal); 0 wrong blocks with CRC OK, 2 right blocks with a wrong block number |
| GW FSK | +2.8 dB | +5.0 dB | 10 packets x 2, dial -15 Hz, clock +300 ppm | 0 (and FSK200/500 text) |

Measured on GitHub (quick.yml runs 43195900765 for XPA / XPA2 / CROWD36 / FSK200/500 / GW, 43197842356 for FSK200/1000). There is no
3.9 number to compare with: these modes are new in OnAir. GW FSK is limited by its burst detector (a 32 ms spectrum), not by the bits.

Known limitations:
- GW FSK: the first bits of a burst that follows another closely are sometimes lost (in the position-report test one fragment of ten,
  so Rivet's rule of ten fragments gives "incomplete"); the test accepts that and prints a note.
- FSK200/1000: the 11-bit block number is outside the CRC. A block that comes a whole number of blocks after a good, confirmed one takes
  its number from that one; a misread number without such a reference stays (2 of 320 blocks in the sweep).
- CROWD36: sync high tone fixed at 24, upper sideband only (see above).

**Simulated signals only.** No recording of any of these modes was available to this branch; the formats are Rivet's, which its authors
worked out from off-air recordings (GW FSK in particular is reverse-engineered and unpublished).
