# 4.0 mode features, group D: more unique per-mode tools

Pure logic (header-only, `core/include/dect2/d40/`) with unit tests in `tests/d40/` (CTest names fm_d40, am_d40, dab_d40, sat_d40, tv_d40, scan_d40). All tests use simulated signals (real FFT spectra, modulated tones, noise-only false-alarm runs). Nothing was checked on air and none of it is wired into the GUI yet: core and test only.

| Mode | Feature | Why | Code |
|---|---|---|---|
| FM / RDS | Station naming: span hits become a named list (PS by majority of clean reads, one PI on two frequencies = one station) | A whole-band scan returns names, not frequencies | `fm_d40.h` `nameStations` |
| FM / RDS | DX opening alarm: foreign-country PIs (PI high nibble) not in this site's baseline, each confirmed by repeated reads | Tells you a tropo / Es opening has started (0 false alarms in 200 noise runs) | `DxOpening` |
| FM / RDS | Programme following inside a network: same PI and PS, switch only after a margin held for several readings | Follows the SFN without flapping (0 switches against a 3 dB alternation) | `SfnFollower` |
| AM / SW | Carrier offset against the 9 / 10 kHz (5 kHz above 1.7 MHz) raster | Off-raster and drifting transmitters; what the synchronous detector must pull in | `am_d40.h` `carrierOffset` |
| AM / SW | Heterodyne finder in the demodulated audio | Beat note = spacing of the interfering carrier | `findHeterodynes` |
| AM / SW | Best sideband for synchronous detection (cleaner of LSB / USB / DSB) | Pick the side without the splatter | `bestSideband` |
| AM / SW | AM / DRM auto-switch with 3-frame hysteresis (carrier line vs flat top) | One channel, the right decoder | `AmDrmSwitch` |
| AM / SW | Station ID by frequency and time from a user-loaded CSV (e.g. an EiBi export); past-midnight and weekday aware | Names the broadcaster; no third-party schedule is bundled | `ScheduleTable` |
| DAB | Service ranking by sustained quality (mean SNR, spread, share of scans decoded) | Best service for a preset is the one that stays good | `dab_d40.h` `ServiceRanker` |
| DAB | Transmitter table (user CSV main,sub,name,lat,lon): TII pairs to names, distance, bearing, unknowns listed | Know which tower you are hearing and where it is | `TiiTable` |
| DAB, HD Radio | Slideshow / logo / album-art collector, deduplicated by content hash, newest N per service | A carousel repeats the same picture; keep each once | `BlobCollector` |
| DVB-S / S2 / S2X | Blind scan of an IF span into a transponder list: centre, width, symbol rate (snapped to standard rates), C/N | One click, whole LNB band | `sat_d40.h` `blindScan` |
| DVB-S2 | Modcods that fit the measured Es/N0 with a margin, best efficiency first (EN 302 307 ideal values) | Shows what a transponder could carry before it locks | `modcodFor` |
| Satellite TV | Dish pointing aid: peak hold, distance below the peak, trend, beep rate | One person aligns the dish | `PointingAid` |
| Satellite TV | LNB / beacon drift: LO error in kHz and ppm, drift per minute, "settled" verdict | Wait for the LNB warm-up to end before fine alignment | `LnbDrift` |
| DVB-C / cable | QAM order guess from the symbol cloud (16 to 256, each fit judged against its own quantisation floor) | A finer grid always fits better; this does not fall for it (12 of 12 right, noise gives none) | `tv_d40.h` `guessQam` |
| HD Radio | Per-sideband level in dBc, imbalance, hybrid / all-digital / analogue only | Finds the side hurt by first-adjacent interference | `hdrLevels` |
| DRM, OFDM modes | MER per carrier, worst carrier, notches (runs of carriers well below the mean), tilt | Locates a narrowband interferer or fading hole | `merPerCarrier` |
| Analogue TV | Standard from line rate, colour burst, SECAM ident, PAL switch (PAL, NTSC, PAL-M, PAL-N, SECAM, mono) | Auto standard without a menu | `classifyAtv` |
| Analogue TV, FPV | Video quality score 0 - 100 with the weakest part named | One number for the link | `videoScore` |
| FPV | Free channels for N pilots across the 5.8 GHz plans (not occupied, guard, mutual spacing) | Race start without a clash | `pickFpvChannels` |
| Explorer / scanner | Classification of every sweep hit (width, duty, level swing, band) with a reason; "unknown" is allowed | Names what the sweep found | `scan_d40.h` `classifyHit` |
| Explorer / scanner | Persistent activity map per cell and hour of day, saved as text and merged over days; busy hours per cell | Band activity over days | `ActivityMap` |
| Explorer / scanner | New-signal alert against that baseline (confirmed over several sweeps, re-armed after it leaves) | A new transmitter in a quiet slot | `NewSignalWatch` |

## Not rebuilt (already present) or skipped
- Present in 3.x: AM census fade depth and selective fading, DRM schedule and AFS, DAB TII decode / SFN, ATV ghost, band survey heat map and diff, FPV channel plans and RSSI.
- ATSC / ATSC 3.0, ISDB-T, DTMB: the echo table and interferer finder of group A already work on the shared OFDM profile; PSIP names exist; nothing new added here.
- DRM service schedule "from the stream itself" is the AFS decoder that exists; MER per carrier is new.

## Limits
- The DVB-S2 Es/N0 table is the ideal-AWGN list from EN 302 307 as remembered by the author: check it against the standard before relying on the numbers.
- Blind scan was tested on a modelled periodogram, not on LNB recordings; symbol-rate snapping uses a fixed list of common rates.
- HD Radio levels compare per-sideband power with the host; the standard's dBc reference is not checked against a real signal.
- DX opening needs a learned baseline and the home country nibble; PI country nibbles are shared by several countries.
