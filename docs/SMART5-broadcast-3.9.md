# Smart features in 3.9: broadcast

Ten engines for FM, AM, DAB, HD Radio and DRM, in `core/src/smart5` with one header each in `core/include/dect2/smart5`. Each runs on figures the receivers already measure (deviation, envelope, RDS fields, service lists, spectra, decode results), so none needs a button. Test: `tests/smart5/test_smart5_broadcast.cpp`.

**Validated on simulated signals only.** The tests use synthetic receiver reports, deviation series, carriers, envelopes and programme audio with offsets, noise and drift. No recording of these faults (a station with dead air, a misaligned HD blend, an over-modulating AM transmitter) was on hand.

| # | Engine | Header | What it reports |
|---|---|---|---|
| 1 | FM station watchdog | `fm_watchdog.h` | Dead air (deviation at the pilot + RDS floor for 10 s on a strong signal), pilot lost on a station that is normally stereo, RDS radio text frozen on a station that changes it, RDS clock (4A) off the computer's clock with the drift per day per PI, over-deviation (peak above 75 kHz more than 1 % of the time). A weak signal is not judged: noise lifts the deviation. |
| 2 | Multiplex power, ITU-R BS.412 | `mpx412.h` | The 60 s integrated multiplex power in dBr (0 dBr = a sine of 19 kHz peak deviation), the 50 / 99 / 99.9 % peak-deviation points and the share of samples above 75 kHz. |
| 3 | Transmitter fingerprint | `fm_txprint.h` | One signature per PI and frequency (carrier offset after the radio's own error, pilot injection, multiplex power); says when the PI on air now is a different transmitter of the same network. |
| 4 | AM split-carrier analyser | `am_carriers.h` | Every carrier within the span to about 0.05 Hz (averaged long FFTs, Hann window, interpolation), level against the strongest, drift, the het tone, names from the user's carrier book. |
| 5 | AM modulation health | `am_health.h` | Positive and negative peaks, negative-peak clipping (over-modulation), carrier shift under modulation, dead air. |
| 6 | Ensemble change log | `ens_changes.h` | Every reconfiguration with its time: service added or removed, label, bitrate, protection, sub-channel moved. A service must be missing from several lists in a row before it counts as removed. |
| 7 | Fault or reception? | `fault_split.h` | Per service: errors while the multiplex signalling and the other services are clean mean the station's feed; everything failing together means reception; nothing at all from a service that was sending means off air at the source. Works for DAB, DVB, ATSC, ISDB, DTMB, DRM and CDR. |
| 8 | HD Radio blend alignment | `blend_align.h` | Time offset (ms, sub-sample, GCC-PHAT) and level difference (dB) between the analogue FM audio and the HD main programme, with a confidence. |
| 9 | HD Radio sideband level | `hd_sideband.h` | Lower and upper primary main sideband power against the analogue FM signal in dBc, the nearest FCC step (-20, -14, -10 dBc) and the imbalance; noise floor taken off. |
| 10 | DRM best-hour book | `drm_besthour.h` | Per station and frequency, decode success per UTC hour, kept between sessions; the best hours and the next good window. |

## What was checked first

MODES.md, docs/*-3.7.md and *-3.8.md and the earlier lists (broadcast, round 2 to 4, SatDump parity, ease of decoding), then a case-insensitive search of `core/src` and `app` for each item. Already present and so not repeated: the DRM AFS, the station book, SDC combining, the DAB census, the FM census, AF follow and the FM DX log.
