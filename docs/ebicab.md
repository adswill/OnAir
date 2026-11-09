# EBICab / Eurobalise up-link (needs a user test)

Mode list: **CELLULAR & RAIL > EBICab / Eurobalise (needs a user test)**. Receive only.

**Needs a user test.** This was written from public documents only and no real recording was available; it is checked against its own test signal.
If you can record a balise (IQ file), please send a short one.

## What the air gap is

A train's antenna sends a **27.095 MHz** tele-powering field (amplitude modulated; it is *not* the data). A balise that is over it wakes up and
sends its telegram as a magnetic **up-link around 4.234 MHz: FSK, 3.951 MHz for 0 and 4.516 MHz for 1, 564.48 kbit/s (+-2.5 %)** (UNISIG
SUBSET-036). That is the signal this mode receives; the 27 MHz carrier is only mentioned so nobody tunes there. EBICab 700 (Sweden, Norway,
Portugal, Bulgaria) and EBICab 900 (Finland, Spain's Mediterranean corridor) are balise systems with the same kind of tele-powered balises.

## What is decoded

- **Eurobalise telegrams** (SUBSET-036 issue 4.0.0, clause 4.3 and Annex A / B2, all public): the long (1023 bit, 830 user bits) and short (341 bit,
  210 user bits) format, with the receiver of 4.3.4.1: parity check against g(x), the repeat of the first bits, the start position from f(x), the
  1024-word substitution table, the control bits, the descrambler. Inverted telegrams (frequencies swapped) are recognised. The telegram can start anywhere.
- The **header fields** NID_C, NID_BG, N_PIG, N_TOTAL, M_VERSION, Q_UPDOWN and so on, from the first 50 user bits (the layout of SUBSET-026), the
  balises seen (NID_C / NID_BG / position in the group), and the number of copies of each telegram.
- **Bursts that give no telegram** (damaged ones, other systems, EBICab) are listed with their length in bits, duration and first bits. A length
  near 32 or 255 bits is *hinted* as the length of an EBICab 700 / 900 telegram, nothing more.

- **Packets** of SUBSET-026 chapter 7 are walked by their length; linking (5), gradient (21), speed (27), level transition (41), temporary speed (65), track condition (68), plain text (72) and geographical position (79) are shown as text. Field layouts come from the openETCS XML transcription of the chapter (a Baseline 3 text); simulated bits only.

## What is not decoded

The EBICab 700 (32 bit) and EBICab 900 (255 bit) telegram formats and their coding are not published, and OnAir does not reverse engineer closed
binaries or equipment, so those telegrams are **not published** to decode: they appear only as bursts. If Trafikverket, Väylävirasto or the manufacturer
publish them, a decoder can be added on top of this mode.

Two readings of SUBSET-036 could not be confirmed with a published test telegram and are the first thing a real recording would test: the output of the
scrambler (the scrambled bit is the input bit added to the register's top bit, which is also fed back) and the SUBSET-026 header layout.
Both give consistent round trips with the encoder in the tests, which proves nothing about the real balises.

## Using it

A HackRF (or another receiver that reaches 4.234 MHz at 4 Msps or more; an RTL-SDR in direct-sampling mode is too slow) and a small loop antenna close to
the track-side balise. The radio is set 1 MHz above 4.234 MHz so the signal stays clear of the DC spike (the program does that by itself); sample rate 10 Msps.
Balises only transmit while a train's field powers them, so a test needs a powered balise (a test track or an on-board antenna), not a bare balise by the track.

## How it was checked

With the generator (bursts of repeated long and short telegrams starting at a random place, the data rate 564.48 kbit/s): clean; carrier offsets of +-40 kHz;
swapped frequencies; a data rate off by +-2 %; timing jitter of 100 ns; a carrier drift of 20 kHz/s; noise (Eb/N0 14 dB); sample rates of 4, 6, 8 and 10 Msps; all
together; random-bit bursts give no telegram and are listed; noise gives nothing; 2 million random bits give no false telegram. The coding itself is tested
for both formats with many scrambling values: all cyclic shifts, inversion, any single bit error, a bit slip, a short telegram repeated three times is not read as a long one.

Tests: `ctest -R ebicab` (`tests/ebicab`), wiring in `tests/app/test_ebicab_mode.cpp`.
