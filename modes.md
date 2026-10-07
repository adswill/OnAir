# Modes

What OnAir decodes today, and what could still be added, **ordered by popularity** (1 = most popular). Popularity here is my judgement of how many people use the mode and how often SDR users ask for it; it is not measured data. Effort is a rough guess: S = days, M = about a week, L = several weeks. Nothing below has been checked against the specifications yet.

## Supported now

| Mode | Kind | Notes |
|---|---|---|
| DVB-T2 (incl. T2-Lite) | TV | Checked on a real mux |
| DVB-T | TV | |
| ATSC 1.0 | TV | 8-VSB |
| ATSC 3.0 | TV | Experimental |
| ISDB-T (incl. ISDB-Tb) | TV | Experimental |
| DAB / DAB+ | Radio | |
| FM (stereo, RDS) | Radio | Checked on live stations |
| DVB-S / S2 (S2X partly) | TV | Synthetic signals only; needs an LNB and a dish |
| DTMB | TV | Synthetic signals only |
| Analog TV (PAL, SECAM, NTSC) | TV | Synthetic signals only; no NICAM |
| DRM30 | Radio | Synthetic signals only; AAC core audio |
| ADS-B / Mode S | Data | Synthetic signals only |
| DMR | Data | Synthetic signals only; no voice audio (AMBE+2) |

## To add, most popular first

| # | Mode | Kind | Where it is used | Needs | Effort |
|---|---|---|---|---|---|
| 2 | AM broadcast (long, medium, shortwave) | Radio | Worldwide | Plain demodulator; HF needs an upconverter on a HackRF One | S |
| 4 | DVB-C / C2 | TV (cable) | Europe, Middle East | Cable connection; QAM, 6-8 MHz | M |
| 6 | Airband voice, ACARS, VDL2 | Utility | Aircraft, 118-137 MHz | AM demodulator (shared with 2), data decoders | S-M |
| 8 | AIS | Utility | Ships, 162 MHz | Narrow FM / GMSK decoder | S |
| 10 | Weather satellites (NOAA APT, Meteor LRPT) | Utility | 137 MHz | APT is simple, LRPT is QPSK with error correction | M |
| 11 | HD Radio (NRSC-5) | Radio | US FM and AM | OFDM; the audio codec is licensed | L |
| 13 | P25 | Digital voice | North America public safety | C4FM decoder; external vocoder | M |
| 14 | TETRA | Digital voice | Europe, Middle East public safety | pi/4-DQPSK TDMA; external codec | L |
| 15 | POCSAG / FLEX | Utility | Paging | FSK decoder | S |
| 16 | APRS | Amateur | 144 MHz | 1200 baud AFSK | S |
| 17 | FT8 / FT4 | Amateur | HF only | Needs HF coverage | M |
| 18 | J.83 Annex B | TV (cable) | US cable, 6 MHz QAM | Cable connection | M |
| 19 | T-DMB | TV (mobile) | South Korea | DAB with video streams; most of the DAB receiver | S-M |
| 20 | D-STAR | Digital voice | Amateur | GMSK decoder; external vocoder | M |
| 21 | NXDN | Digital voice | Commercial | 4FSK decoder; external vocoder | M |
| 22 | ISDB-S / ISDB-C | TV | Japan (satellite), Brazil (cable) | ISDB-T parts, satellite or cable signal | L |
| 23 | ISDB-Tsb | Radio | Japan | A subset of the ISDB-T receiver | M |
| 24 | NAVTEX | Utility | Maritime, 518 kHz | HF, narrow FSK | S |
| 25 | GOES HRIT | Utility | Weather satellite, L-band | Dish and LNA needed | L |
| 26 | CDR | Radio | China, FM band | Little public documentation | M |

Not recommended: Sirius XM and WorldSpace (proprietary); DVB-H, DVB-SH, ATSC-M/H and CMMB (switched off).

Popularity order is not a build order: the cheap ones (2, 3, 8, 15, 16) give the most for the least work, and 1 is the biggest job on the list. Start each new mode the way FM was done: a signal generator and a unit test first, then a check on a live signal.
