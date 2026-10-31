# ATIS (inland waterways)

ATIS, the **Automatic Transmitter Identification System**, is how ships on the European inland waterways (Rhine, Danube, Rhone, the Dutch,
Belgian and German canals ...) identify themselves: each time the operator lets go of the push-to-talk key, the VHF radio sends a short burst
of data on the same channel, after the voice. It is **not AIS** (a separate system on 161.975 / 162.025 MHz).

The mode is receive only. Tune an inland channel (156.3 to 162.0 MHz; channel 10 and 6 are the usual intership channels, default 156.500 MHz).
Any radio that covers VHF marine works (RTL-SDR, Airspy, SDRplay, HackRF ...). The voice plays as narrow FM (Listen, on by default); the burst
is decoded, checked and listed.

## What the screen shows

* **Messages**: every burst, newest first: UTC, the ten digit ATIS number, the country (from the three MID digits), the call sign the digits
  spell, the ship name if your name table knows it, and whether the check symbol matched ("ok, repaired" when a damaged symbol was rebuilt).
  A burst whose check fails is listed in orange and is not counted as a station.
* **Stations**: one row per ATIS number with the last time heard and how often. Click a row and type a name: it is saved in the ship name
  table (the same table the DSC and AIS screens use, `ship_names.tsv` in the data folder), under the nine digits after the 9.
* **Converter**: call sign to ATIS number and back, for checking a radio's programming.
* The list on the right shows the stations; the Receiver tab shows counters, carrier offset and the channel level.

## The format

ETSI EN 300 698-1 annex B (based on ITU-R M.493, described in ITU-R Report M.1159):

* 1200 baud FSK on a phase-modulated carrier (modulation index 1.0): **1300 Hz = Y (1), 2100 Hz = B (0)**.
* Ten bit symbols, as in DSC: seven information bits (least significant first) and three bits giving the number of B elements.
* The sequence: 20 bit dot pattern (may be left out), phasing (6 x symbol 125 in the DX places, symbols 111 to 104 in the RX places),
  format specifier **121** twice, five identity symbols (two decimal digits each) twice, end of sequence 127 and the check symbol (even
  parity over format specifier, identity and end of sequence). Every symbol is sent again five places later (time diversity), so a burst
  survives short fades; the second copy and the check symbol repair damaged symbols.
* The identity is `9 MID X1 X2 X3 X4 X5 X6`: 9, the country digits (MID, ITU Radio Regulations appendix 42), `X1 X2` = the second letter of the
  call sign (01 = A), `X3 .. X6` = the number of the call sign. The first letter of the call sign is the country: PC8075 in the Netherlands
  is `9 244 03 8075`. The first letter is shown for the countries where it is certain ("likely") and as `?` otherwise.
* Many radios are programmed with **9 + the MMSI** instead (`9 207 072225`); the nine digits after the 9 are shown as an MMSI too, and the name
  comes from the ship name table when that MMSI is in it.
* The DSC decoder of the marine receiver does the symbol work, so a repaired frame is exactly as reliable as a DSC call.

## How it copes with real radios

A radio's carrier is rarely exact and the tone decoder wants the tones within a few hundred hertz of 1300 / 2100. The receiver therefore runs
seven small carrier-offset branches (0, +-500, +-1000, +-1600 Hz) and both tone polarities, and keeps one message per burst (a checked copy beats
a failed one). The tests cover carrier offsets to +-1.5 kHz, the channel off the centre of the capture, noise down to 12 dB carrier to noise,
a wrong bit clock, phase modulation and flat FM, no dot pattern, swapped tones, 0.5 to 2 Msps input, and the negatives (noise only, voice only).

## Limits

* Needs a user test on a real signal: no real recording was available; the format is from the standard and the tests use a synthetic signal
  made from it.
* The first letter of a call sign is only known for common inland countries; the rest show `?`.
* Only the ATIS burst is decoded. Other DSC formats heard on the channel are counted ("other DSC calls") and not listed.
