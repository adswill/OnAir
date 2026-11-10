# Satellite data collection and xRIT links (3.9, branch feat39-sat)

This package adds the Meteosat high rate data collection platforms as a new mode. It also extends GOES DCS with the Meteosat and Himawari channel plans and gives GOES HRIT the GOES-N/O/P LRIT link. Elektro-L LRIT/HRIT gets tests for every link.

**Simulated only.** No recording of any of these signals was found, so every result below comes from the generators.

## Meteosat HRDCP (new mode, family 700)

Mode list entry **Meteosat HRDCP** (engine standard 701, test signal 702). It is a separate protocol from the standard-rate DCP, so it gets its own entry. It shares the DCS screens, report types and CSV log style with GOES DCS (`app/goesdcs/goesdcs_ui.cpp`, `core/src/goesdcs`).

Written from EUMETSAT TD 16, "Meteosat Data Collection and Distribution Service" (EUM/OPS/DOC/08/0325 v2B), sections 3.2 to 3.4 and 5.3.

- **On the air**
  - 2 s of unmodulated carrier.
  - 128-bit preamble: 0xA05050A0 four times.
  - 64-bit marker 0x034776C7272895B0, sent as BPSK at 1200 bit/s with 0 at 45 degrees and 1 at 225 degrees.
  - Then the code symbols as offset QPSK at 2 x 1200 sym/s: G1 on I, the inverted G2 on Q half a symbol later.
  - Root raised cosine pulses, roll-off 0.5.
  - Assignments on the 1.5 kHz grid of TD 16 table 11, 402.2005 to 402.4345 MHz.
- **Coding**
  - Message: a 12-byte header (BCH address, reserved bit 1, length, sequence counter, version 1, type, compression, 10 health bits, spare), the platform data, and a CRC-32 (polynomial 0x741B8CD7, initial value 0, MSB first).
  - Zero filled to whole 669-byte frames.
  - Each frame: Reed-Solomon (255,223) with interleave 3, CCSDS dual basis, giving 765 bytes, then the CCSDS randomiser.
  - Tail byte 0x80, then the CCSDS r = 1/2, K = 7 convolutional code.
- **Receiver** (`core/src/goesdcs/hrdcp_live.cpp`)
  - Front end: a CIC filter to about 800 kHz, and spectra with 50 Hz bins. The 2 s carrier is a line over the noise.
  - Each carrier gets its own mixer and CIC filter to 9.6 kHz.
  - The carrier frequency is measured on the first 0.45 s of carrier, then the matched filter is applied.
  - The preamble and marker are found by correlating in six pieces whose magnitudes are summed. This tolerates a few hertz of error. The phases of the pieces give the phase and the residual frequency.
  - Offset QPSK is demodulated with a decision-directed carrier loop (about 5 Hz) and a Gardner timing loop on both rails.
  - Then a soft Viterbi decoder, the randomiser, Reed-Solomon and the CRC.
  - The header is read from the first frame, and its length field says how many frames to wait for.
  - Only messages whose CRC is good are reported.
- **Shown**: time, address (written as EUMETSAT writes it), Meteosat channel, sequence counter, alert or self-timed, compression, health bits, frames, Reed-Solomon corrections, and the data. The data appears as ASCII, pseudo-binary or hex. The CSV log goes to `hrdcp.csv`.
- **Not implemented**: gzip-compressed platform data. EUMETSAT does not use it, and the flag is shown. Meteosat Third Generation's extended band (402.435 to 402.85 MHz) is not described in TD 16.

## GOES DCS: Meteosat and Himawari standard-rate platforms

Meteosat SRDCP, Himawari DCP and GOES platforms all use the same CGMS international platform format, and the existing GOES DCS receiver already decoded it. This package adds:

- A **Channel plan** setting: Auto, GOES, Meteosat or Himawari. Auto uses GOES below 402.0 MHz and Meteosat from there up.
  - Meteosat channel numbers follow TD 16 table 11: n = 1 to 267 at 402.0340 + 0.0015 n MHz, and 268 to 289 at 402.0025 + 0.0015 (n - 268) MHz.
  - JMA publishes no channel numbers for Himawari, only the bands: 402.0685 to 402.4 MHz at 100 bps and 402.1 to 402.4 MHz at 300 bps. Himawari reports therefore show the frequency only.
- A band preset **Meteosat / Himawari 402.22 MHz**.
- Addresses are now shown as the operators write them: the 31 bits left-aligned in 32. Before, they were right-aligned. The EUMETSAT reference platform in TD 16 section 6 is "162096C4". Its 31-bit word passes the BCH check and decodes to authority 1 and type 6 (regional self-timed land), with bit 21 = 0, which confirms this layout.
- The generator takes an absolute frequency, so Meteosat and Himawari channels can be generated.

## GOES-N/O/P LRIT (GOES HRIT link choice)

The GOES-N/O/P LRIT broadcast (1691.0 MHz, BPSK 293.883 ksym/s) uses the same CCSDS frames and LRIT files as the GOES-R HRIT. Only the symbol rate and the frequency differ, so it is a **Link** choice in the GOES HRIT panel, not a new mode. The receiver already had `setSymbolRate`. The series is retired or stored, so this is for recordings and spare satellites.

## Elektro-L LRIT/HRIT

The Elektro-L mode already had seven links: HRIT 665.4 k and 1 M, LRIT 128 k and 64 k, 294 k, GGAK, and HRIT 1.15 Msym/s OQPSK. It had no tests of its own. This package adds:

- The xRIT generator can now send offset QPSK.
- The Elektro test signal follows the selected link (SynthConfig modeOpt[0]).
- `tests/goes/test_goes_lrit_links.cpp` runs every HRIT/LRIT link through `ElektroReceiver::setLink`, with offsets.

Arktika-M has only RDAS and GGAK modes. No public description of an Arktika-M xRIT broadcast was found, so none was added.

## Skipped: INSAT DCP

The INSAT-3D/3DR/3DS data relay transponder uplink is documented only to this level:

- 402.75 MHz, 180 kHz wide.
- BPSK at 4.8 kbps, NRZ-L.
- TDMA or random access (WMO OSCAR; the IMD TECO-2010 paper).

No public source gives the burst format: preamble, sync, address, framing or coding. Without it a decoder would have to be invented, so INSAT is not implemented.

## Tests

| Test | What |
| --- | --- |
| `test_goesdcs_hrdcp` | CRC check value from TD 16 ("CatMouse987654321" gives 0x1FC0DFEC); header bits; frames per message against table 4; coding round trip with Viterbi bit errors and Reed-Solomon byte errors; a broken frame fails the CRC; receiver: two platforms 3 kHz apart (one frame and two frames), +-125 Hz with +-1 Hz/s drift, +-50 ppm and 2 degrees phase noise at Eb/N0 8 dB, Eb/N0 6.5 dB, repeats with the counter wrapping, a standard-rate platform 6 kHz away, noise only; Eb/N0 sweep with ONAIR_TEST_LONG=1 |
| `test_goesdcs_regional` | Meteosat table 11 numbers, the reference address, 100 bps on regional and international channels, adjacent channels, +-600 Hz (the 1.5 ppm limit) with clock errors, Himawari 300 bps without the flag word, 24 dB noise point (sweep with ONAIR_TEST_LONG=1) |
| `test_goes_lrit_links` | GOES LRIT at 1 Msps, Es/N0 6 dB, +9 kHz, +40 ppm; wrong-rate settings decode nothing; every Elektro link except 64 k (that one only with ONAIR_TEST_LONG=1); Es/N0 sweep with ONAIR_TEST_LONG=1 |

Results: see the end of this file.

## Results
