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
| Arktika-M GGAK | 267 | PM 5 ksym/s on 1703 MHz, Elektro GGAK receiver | new entry on the existing receiver | NOT validated: run through this mode, the existing GGAK receiver finds the carrier (right offset, 28 dB) but never frame-locks on its own test generator. Not isolated further, so Elektro GGAK may share the fault; the check in tests/wxsat prints the state and does not fail on it |
| AWS L-band | 268 | QPSK 1.785 Msym/s, CCSDS K=7 r=1/2, ASM, RS(255,223) x5, MWR 19 channels | new (MWR only, NAVATT not decoded) | synthetic only (down to 4 dB Es/N0) |
| Elektro-L LRIT 294 ksym/s, HRIT 1.15 Msym/s OQPSK | 80 (existing entry) | link choices added; OQPSK delays I by half a symbol into the QPSK path (rail delay as in SatDump's pipeline, not tested on real signals) | rates added | NOT tested on any signal, synthetic or real |
| Meteor-M LRPT rates | 59 | QPSK 72k, OQPSK 72k (differential), OQPSK 80k | already there (the header lists all three); no change | not re-run in this batch |

Automatic on top: the Passes planner knows the new satellites (APT, DSB, HRPT, Meteor HRPT, Arktika GGAK, AWS), "Follow the next pass" in the mode's panel arms tracking with auto start and Doppler, and the picture saves itself when no new lines come for 90 s (setting per mode). Manual tuning stays in the normal controls.

Products: pictures grow line by line in the panel and are saved as PNG under `<data folder>/wxsat/<mode>/`. Telemetry: frame counts, spacecraft id, HIRS picture where the frames carry it.

The app was checked with an isolated renamed copy (hidden, muted, own shot) playing METEOR_HRPT.wav: Meteor HRPT locked, 930 frames, 72 lines, the picture and buttons drawn. Setting HOME did not move macOS preferences (they went to the renamed copy's own domain in the real ~/Library/Preferences, deleted afterwards; dect2.plist unchanged): only the rename and DECT2_DATA_DIR isolate a run.

The Doppler and pass start button runs the existing pass tracker; it has not been exercised on a live pass.

Synthetic thresholds seen: NOAA HRPT decodes at 4 dB Es/N0 (chips), AWS at 4 dB and fails at 2 dB, APT at 6 dB.

## Left out / next
- GAC tape-reversed dumps (SatDump's "backward" option): the reversed sync is not searched.
- NOAA instruments beyond AVHRR and HIRS; Meteor MTVZA / BIS-M; AWS NAVATT; calibration and projection of any of the new instruments.
- Lines placed by counter except NOAA HRPT (uses the on-board time).
- RDAS lines are published per row; the 12 MB visible plane is reduced 4:1 to fit memory.
- Real signals for GAC, DSB, RDAS, AWS, GGAK: no public recordings found.

Sources: formats and parameters from SatDump (GPL-3.0, Copyright Aang23 and contributors): plugins noaa_metop_support, meteor_support, elektro_arktika_support, aws_support, analog_support (noaa_apt) and resources/pipelines/{NOAA,Meteor-M,Elektro_Arktika,AWS}.json. Code written for OnAir; notice in each file; THIRD_PARTY.md and in-app credits.
