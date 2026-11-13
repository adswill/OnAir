# 4.0 mode features, group E: satellite, HF, public safety, aviation, rail, LoRa, science

Pure logic (header-only, `core/include/dect2/d40/sat_e40.h`, `hf_e40.h`, `radio_e40.h`) with unit tests in `tests/d40/` (CTest names sat_e40, hf_e40, radio_e40). All tests use simulated data (noisy profiles, synthetic envelopes, noise-only false-alarm runs). Nothing was checked on air and none of it is wired into the GUI yet: core and test only.

| Mode | Feature | Why | Code |
|---|---|---|---|
| NOAA APT / Meteor / weather sat | Stitch the next pass image onto the last one by correlating row-mean profiles; reports lag and score, refuses unrelated strips | One long image across consecutive passes (19 of 20 noisy trials right, 0 false matches) | `sat_e40.h` `stitchLag` |
| All satellites | Pass link budget: slant range, free-space loss, C/N0, SNR at any elevation; lowest elevation that reaches a required SNR | Tells before the pass whether a low pass is worth recording | `passLinkBudget`, `minElevationFor` |
| All satellites | Satellite name in any spelling gives decoder, usual frequency and baud | One click from a TLE name to the right mode; a hint, the user confirms | `decoderHint` |
| All satellites | SatNOGS-style observation log: good / bad / failed per pass, CSV, success rate, lowest elevation that ever decoded | Shows what the station really receives | `ObsLog` |
| Weather / solar-powered beacons | Sunlit or in Earth shadow (low-precision Sun vector, cylindrical shadow) | Visible-light products and some beacons only work in sunlight | `sunUnit`, `sunlit` |
| Narrowband satellite modes | Retune plan from a Doppler curve: fewest retunes keeping the residual inside a fraction of the channel width | Fewer retunes on slow SDR drivers; narrow channels get more | `planRetunes` |
| HF beacons, time signals, ALE, HFDL | Best frequency now, and hours until it changes, learned from this site's own SNR per frequency and UTC hour; unknown hours borrow neighbours or say unknown | Auto frequency pick without a propagation model | `hf_e40.h` `BandClock` |
| HF beacons, time signals | Fade rate and depth from the envelope, verdict flat / QSB / flutter | Tells fading from a weak signal | `fadeStats` |
| P25, NXDN, DMR, TETRA trunking | Talkgroup alias import (RadioReference-style CSV or id,name), priority pre-emption, hold time, encrypted skipped, optional listen list | Follow the groups that matter by name | `radio_e40.h` `TgAlias`, `TgFollower` |
| Trunking | Control channel pick from scan hits (valid-frame share, cadence, level); none when nothing fits | Auto-find the control channel | `pickControlChannel` |
| ADS-B, AIS | Range per 10 degree sector (max, 95th percentile) and blind sectors | Antenna pattern and terrain shadow at a glance | `RangeMap` |
| LoRa | Spreading factor from symbol time and bandwidth (all SF 6 to 12 x 62.5 to 500 kHz with 3 % clock error), confidence | Auto SF / BW | `loraGuess` |
| LoRa, LoRaWAN, Meshtastic | Sync word from the two sync symbols, named (0x12, 0x34, 0x2B) | Tells the network without a menu | `loraSyncWord` |
| Meshtastic / LoRa mesh | Packet-type counts, share and rate, with Meshtastic port names | Traffic mix of a mesh | `PacketStats` |
| Meteor scatter | Ping detector: start, duration, peak, decay time, under- or over-dense (0 false pings in 200 noise runs) | Ping log for MS contacts and radar-style counts | `findPings` |
| GSM-R | Channel number to downlink and uplink frequency (R-GSM 955 to 973 = 921.2 to 924.8 MHz) | Frequencies from the channel list in SI messages | `gsmrArfcnToHz` |

## Not rebuilt (already present) or skipped
- Present in 3.x: pass scheduling and Doppler fit, SatNOGS client, wx pass quality, NCDXF slots and propagation table, hydrogen line with galactic coordinates, Sun drift scan, marine VHF channel names, MMSI country, NAVTEX subjects, lightning positions, mesh topology, trunk talkgroup activity.
- Skipped: NAVTEX station slot table (not sure of the exact standard slot list), time-signal station tone schedule, STANAG 4285/4539 mode auto-ID (needs the real preamble work), LZB / EVM (no public spec at hand), ERMES, POCSAG / FLEX and Olivia / PSK auto-config (covered by earlier groups).

## Limits
- The satellite name table has the usual downlink frequencies from memory, a hint only; check against the current frequency list.
- The link budget is a circular orbit over a spherical Earth with free-space loss only; no atmosphere, polarisation or antenna pattern.
- The Sun vector is low precision (about 0.01 degree); the shadow ignores penumbra.
- The range map, ping and fade numbers come from synthetic data. Ping classification thresholds (2 dB plateau test) are untested on real meteor echoes.
