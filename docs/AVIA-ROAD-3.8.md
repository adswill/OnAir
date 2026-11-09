# Aviation, rail and road modes in 3.8

Twelve new mode-list entries, families 400 to 411 (`docs/MODE-NUMBERS.md`). One receiver object (`core/src/avroad`, `AvRoadReceiver`) holds a decoder per mode. Every mode is receive only: the test generators live in `tests/avroad` and are not reachable from the transmit side of the application. `pub/feat38-adsl` (ADS-L with FLARM) is merged into this branch.

## What each mode does

| mode | family | decodes | validated on |
|---|---|---|---|
| FANET | 400 | Tracking, ground, name, message, service (weather) frames, map positions. Uses the LoRa (raw) receiver with sync word 0xF1. 868.2 MHz 250 kHz, or 920.8 MHz 500 kHz by the tuned frequency | simulated only |
| OGN tracker | 401 | Position and status packets on 868.2 / 868.4 MHz; LDPC (208,160) check and soft bit repair; encrypted positions counted, not read | simulated only |
| GBAS VDB | 402 | Burst detector only: burst count, slots busy of 8, 10 500 symbol/s line. **Message content not decoded** | simulated only |
| LDACS | 403 | Structure only: 625 kHz OFDM by cyclic prefix (102.4 + 17.6 us), carrier offset | simulated only |
| AeroMACS | 404 | Structure only: 5 MHz OFDM by cyclic prefix, 5 ms frame bursts. Needs 5.6 Msps | simulated only |
| End of train | 405 | AAR EOT frames: unit, brake pipe pressure, battery, motion, valve, marker. BCH(63,45) repair of up to two bits | **real recording** (PyEOT `demo3eot.wav`, 3 frames, `tests/avroad/data`) and simulated |
| DSRC road toll | 406 | EN 12253 downlink frames: length, broadcast or private link, control byte. Identifying structure only | simulated only |
| 802.11p V2X | 407 | BSM (SAE J2735) and CAM (ETSI) with position; DENM counted. 802.11p at 3 and 6 Mbit/s on a 10 MHz channel | BSM layout on one published example hex; radio path simulated only |
| C-V2X | 408 | Structure only: SC-FDMA subframes by cyclic prefix | simulated only |
| ERMES | 409 | Network identity, addresses, numeric and text messages, from ETS 300 133-4 | simulated only |
| Marine radar | 410 | Rotation period, PRF, pulse width, beamwidth | simulated only |
| Weather radar | 411 | Rotation, single or dual PRF, pulse width, sweep sequence and volume restarts | simulated only |

Measured sensitivity in the tests: end of train all frames down to 6 dB in 12.5 kHz; OGN down to 8 dB in 200 kHz; FANET down to -12 dB in 250 kHz; ERMES down to 10 dB in 25 kHz; 802.11p QPSK 1/2 8/8 at 8 dB in 10 MHz; radar rotation within 2%.

## Limits

- **Bearing of a radar**: the request asked for rotation rate and bearing. One antenna only gives the rotation period and the beam crossing, not a bearing. A true bearing needs two coherent channels (the existing DF modes); this is not connected to these modes. The Receiver tab says so.
- **Marine radar**: pulse radars only. FMCW broadband radars send no pulses and are not seen.
- **End of train, head of train**: the frame check and field layout come from PyEOT, which covers EOT frames. HOT frames (452.9375 MHz) are shown with unit and type only when they pass the same check; their fields are not decoded and no recording of one was available.
- **OGN**: the LDPC tables and field layout are from OGN-Tracker as carried in SoftRF (GPL-3, credited).
- **802.11p**: BPSK and QPSK rate 1/2 only (the rates BSM and CAM use). The BSM and CAM are found by scanning the frame for their headers, so signed and unsigned frames are handled alike; security fields are not verified.
- **DSRC tolling**: the framing follows HDLC (flags, bit stuffing, X.25 FCS). The downlink modulation values are from memory of EN 12253, and EN 12795 / EN 12834 are not public, so the field layouts could not be checked. Link identifiers and frame content are not shown, logged or saved.
- **ERMES**: no public recording and no known live networks; numeric codes above 9 are shown as hexadecimal digits (the mapping is in ETS 300 133-2 annex B); national character variants are not applied.
- **App not run**: per the overnight limits only syntax checks and standalone tests were run; the GUI of these modes has not been opened.

## Skipped, with reasons

- **ReFLEX**: the Motorola specification is not public; only overview descriptions were found. Nothing was built.
- **ATCS**: the AAR specifications (200, 250) are not public and no open decoder or layout was found; hobbyist notes (MDC-4800-style sync, block size) are unverified and do not give the coding. Nothing was built.
- **GBAS VDB content**: the burst format (training sequence, scrambler, FEC layout) is in ICAO Annex 10 Volume I Appendix B and RTCA DO-246, not available publicly here. Only the burst detector exists.
- **LDACS and AeroMACS content**: structure only, as requested where unpublished.
- **C-V2X messages**: the specifications are public but decoding needs SLSS sync, PSCCH and turbo PSSCH; not built in this lane.

## Files

`core/src/avroad`, `core/include/dect2/avroad`, `app/avroad`, `tests/avroad`, `tests/avroad/data/demo3eot.wav` (GPL-3, PyEOT, Eric Reuter). The Wi-Fi OFDM receiver gained `setAllowQpsk()` (default off, so Wi-Fi behaviour is unchanged).
