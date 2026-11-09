# SatDump parity, tier 1 (3.7)

The first batch of the SatDump parity list: weather satellite links on cheap or moderate gear. All links run in one receiver class (`WxsatReceiver`, profile per mode-list entry), so each has its own mode-list row, numbers (docs/MODE-NUMBERS.md, families 260 - 268, poll slots 220 - 228; may need to move at merge) and screen.

## Modes

| Mode | Family | Link | Status | Validated on |
|---|---|---|---|---|
| NOAA APT | 260 | 137 MHz FM, 2400 Hz AM, sync A, 2080 words a line | new (retired 2025, recordings) | real IQ (sigidwiki APT_IQ.zip, 4 s: 5 lines); synthetic: noise, offset, Doppler |
| NOAA HRPT | 261 | PM residual carrier, Manchester 665.4 kbit/s, 11090-word frames | new entry and a PM demodulator (the old path assumed suppressed carrier BPSK) | real baseband (NOAA_HRPT.wav, 3 Msps, 4.7 s: 23 frames, AVHRR channels plausible, night pass); synthetic with pixels checked |
| NOAA GAC | 262 | BPSK 2.6616 Msym/s, scrambled 3327-word frames, 409-pixel AVHRR | new | synthetic only (no public recording found) |
| NOAA DSB | 263 | PM Manchester 8320 bit/s, 104-byte TIP frames, HIRS | new; HIRS only from the instruments on TIP (SEM, analog telemetry, AMSU not yet) | synthetic only |
| Meteor HRPT | 264 | PM Manchester 665.4 kbit/s, 1024-byte CADU, MSU-MR | new | real baseband (METEOR_HRPT.wav, 3 Msps, 4.7 s: 384 CADUs, 29 MSU-MR lines, channels plausible); synthetic with pixels checked |
| Elektro-L RDAS | 265 | BPSK 30.72 Msym/s, NRZ-M, CADU, MSU-GS IR (7 channels), visible reduced 4:1 | new | synthetic only (needs 40 Msps and X-band) |
| Arktika-M RDAS | 266 | as Elektro-L RDAS at 7865 MHz | new | synthetic only |
| Arktika-M GGAK | 267 | PM 5 ksym/s on 1703 MHz, Elektro GGAK receiver | new entry on the existing receiver | NOT validated: the existing GGAK receiver does not frame-lock on its own generator in a standalone test (state stays at carrier); that needs its own look |
| AWS L-band | 268 | QPSK 1.785 Msym/s, CCSDS K=7 r=1/2, ASM, RS(255,223) x5, MWR 19 channels | new (MWR only, NAVATT not decoded) | synthetic only (down to 4 dB Es/N0) |
| Elektro-L LRIT 294 ksym/s, HRIT 1.15 Msym/s OQPSK | 80 (existing entry) | link choices added; OQPSK delays I by half a symbol into the QPSK path (rail delay as in SatDump's pipeline, not tested on real signals) | rates added | not tested on a signal (the HRIT QPSK/LRIT BPSK path below is the existing one) |
| Meteor-M LRPT rates | 59 | QPSK 72k, OQPSK 72k (differential), OQPSK 80k | already there; re-read, no change needed | existing tests |
| NOAA HRPT in the HRPT / AHRPT entry | 64 | kept for compatibility | | |

Automatic on top: the Passes planner knows the new satellites (APT, DSB, HRPT, Meteor HRPT, Arktika GGAK, AWS), "Follow the next pass" in the mode's panel arms tracking with auto start and Doppler, and the picture saves itself when no new lines come for 90 s (setting per mode). Manual tuning stays in the normal controls.

Products: pictures grow line by line in the panel and are saved as PNG under `<data folder>/wxsat/<mode>/`. Telemetry: frame counts, spacecraft id, HIRS picture where the frames carry it.

## Left out / next
- GAC tape-reversed dumps (SatDump's "backward" option): the reversed sync is not searched.
- NOAA instruments beyond AVHRR and HIRS; Meteor MTVZA / BIS-M; AWS NAVATT; calibration and projection of any of the new instruments.
- Lines placed by counter except NOAA HRPT (uses the on-board time).
- RDAS lines are published per row; the 12 MB visible plane is reduced 4:1 to fit memory.
- Real signals for GAC, DSB, RDAS, AWS, GGAK: no public recordings found.

Sources: formats and parameters from SatDump (GPL-3.0, Copyright Aang23 and contributors): plugins noaa_metop_support, meteor_support, elektro_arktika_support, aws_support, analog_support (noaa_apt) and resources/pipelines/{NOAA,Meteor-M,Elektro_Arktika,AWS}.json. Code written for OnAir; notice in each file; THIRD_PARTY.md and in-app credits.
