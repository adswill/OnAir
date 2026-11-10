# GOES / Meteosat DCS

The mode **GOES DCS** (Satellite and navigation group) receives the reports of **data collection platforms** (DCPs): river and tide gauges, weather stations,
buoys and other remote sensors that send a short report through the GOES and Meteosat satellites. The platforms transmit on the **401.7 - 402.1 MHz** uplink,
each on its own **1.5 kHz channel**, at 100, 300 or 1200 bps. A radio on the ground hears the platform's own transmission (the satellite only relays it, on
another band). It is receive only and written clean-room from the public NESDIS and JMA descriptions of the DCP radio set standards.

## The message

On the air, in this order: an unmodulated carrier, a bit preamble of alternating 1 and 0 (250 bits at 100 bps, 450 at 300 and 1200 bps), a 15-bit frame sync
(a maximal length sequence), the **platform address** (31 bits, a BCH (31,21) codeword shown as 8 hex digits), for the 300 and 1200 bps format a spare bit and an
8-bit flag word, the data as 8-bit characters (7 bits LSB first and an odd parity bit) and the end-of-transmission character 0x04. The carrier is phase modulated
+-60 degrees; the bits are NRZ-L or Manchester coded. The data is **ASCII** text or **pseudo-binary B** (six bits per character, 0x40 added), in which three
characters make an 18-bit sensor value.

## Meteosat and Himawari

The same platform format (the CGMS international DCP standard) is used by Meteosat (EUMETSAT standard-rate DCPs, 100 bps, 402.0 - 402.435 MHz) and Himawari (JMA, 100 and 300 bps, 402.0685 - 402.4 MHz). The **Channel plan** setting picks how channels are numbered: Auto (GOES below 402.0 MHz, Meteosat from there up), GOES, Meteosat (EUMETSAT TD 16 table 11) or Himawari (frequency only: JMA publishes no channel numbers). The Band selector has a **Meteosat / Himawari 402.22 MHz** preset. Addresses are shown as the operators write them, the 31 bits left-aligned in 32 (the EUMETSAT reference platform is 162096C4). The high rate Meteosat platforms (HRDCP) are their own mode, **Meteosat HRDCP** (docs/SAT-DCS-XRIT-3.9.md). Simulated only.

## The relay downlink

The satellite retransmits the uplink carriers: the 401.85 MHz pilot appears at 1679.85 MHz on the GOES-R downlink (NOAA frequency plan). The Band selector tunes either the uplink or this relay. The same decoder runs on both; this assumes a bent-pipe relay that keeps the uplink modulation. A signal wiki and an old frequency plan label the downlink 8PSK, so this has not been confirmed on air. Simulated only.

## How it works

- The capture is brought to about 800 kHz (fourth-order CIC) and an averaged spectrum is made every 80 ms. A line that stands more than the **threshold**
  (default 11 dB) above the local noise for three frames in a row is a carrier. The unmodulated carrier and the residual carrier of the +-60 degree
  modulation both make such a line, so every platform is found from its first half second.
- Each carrier is followed by its own mixer, CIC filter (to about 9.6 kHz) and phase locked loop. The data is the quadrature part against the locked carrier.
- Five hypotheses run side by side on every carrier: 100 bps Manchester, 300 bps Manchester, 1200 bps Manchester, 1200 bps NRZ and 300 bps NRZ, each with eight
  bit phases, looking for the preamble followed by the frame sync (at most one wrong bit in the 30). The first to match, in either polarity, takes the carrier.
- The address is checked with the BCH code and up to two bit errors are corrected. A bit timing loop follows the clock to the end code, so clock errors of
  100 ppm and more are harmless; a carrier error of a few hundred hertz is also harmless.
- The data offset (right after the address, or after the spare bit and the flag word) is chosen by the character parity. A report counts when it ends with the end code,
  or when the carrier ends after at least four characters and few parity errors.
- The satellite channel is counted on the 1.5 kHz grid from channel 1 at 401.70175 MHz when the radio's centre frequency is known.

## What is shown

- **Reports**: time, platform address, channel, rate, level and data, with the sensor values of the selected one (ASCII fields as label and value, pseudo-binary
  as 18-bit numbers with the signed value), Clear, Copy (CSV), Log to file, threshold.
- **Platforms**: the table of platforms heard with the number of reports, the last time, channel, rate, level and the latest data.
- **Band**: a waterfall of the captured band with the carriers being followed.

## Unattended logging

With **Log to file** on (the default) every report is appended to `goes_dcs.csv` in the data folder. From the command line:
`dect2cli --standard goesdcs --freq 401.85 --dcs-log reports.csv --secs 86400`.

## Tuning

Tune the middle of the uplink band, **401.85 MHz**, with **0.5 Msps or more** (1 Msps default): one capture then holds all of the channels at once. A UHF antenna
is needed; platforms are low power and often far away, so a good antenna and a low-noise amplifier matter. Most platforms send once every few minutes to once an
hour at set times. The test signal (`--synthetic --standard goesdcs`) is three platforms (100, 300 and 1200 bps) on channels 60, 100 and 140; nothing is transmitted.

## Not verified

The sync word, the BCH polynomial (checked against the JMA annex example), the end code, the parity rule and the 100 bps format follow the JMA annex. The spare bit
and the flag word of the 300 / 1200 bps format and the NRZ coding at 300 / 1200 bps follow public NESDIS summaries and have not been tried against a real
platform; the data offset is therefore chosen by the parity of the characters. No decoded real signal has been tested.
