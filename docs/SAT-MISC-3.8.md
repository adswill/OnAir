# Satellite modes and identification (3.8)

Branch `feat38-sat-misc`, mode family block 550-562. Code: `core/src/satmisc`, `core/src/navic`, `app/satmisc`, `app/navic`, tests in `tests/satmisc` and `tests/navic`.

Everything here was checked **on simulated signals only**: no antenna was pointed at any of these signals, and no real recording of any of them went through a test.

## What was built

| Item | Mode | Status |
|---|---|---|
| What satellite is this / mystery hunter | family 551 | Ranks every satellite with elements by the shape of a measured Doppler curve (`sat_id`, constant offset solved, candidates refined with `doppler_fit`). Near-earth orbits only (`Sgp4`); geostationary satellites cannot be named. |
| GRAVES satellite echoes | Meteor scatter, tab Satellites | Lines near 143.050 MHz that last more than 1.5 s and sweep at least 15 Hz, named from the bistatic Doppler (transmitter at 47.348 N, 5.515 E, site from public amateur sources) with no free offset. Tracks crossing the beacon line are split there. |
| Zombie satellites | family 550 | AO-7, Transit 5B-5, LES-1, LES-5, IMAGE by carrier frequency, and by Doppler for AO-7, Transit 5B-5 and LES-1. LES-5 and IMAGE orbit too high for the predictor: carrier only. Tracker rows added for the near-earth ones. |
| Space station | family 562 | Every ISS downlink: shows which is on, moves between VHF and the UHF repeater when nothing is heard, opens the APRS decoder or the general receiver on the carrier. The tracker can run it per pass. It does not decode voice or SSTV itself. |
| ISS HamTV preset | DVB-S tuner | 2395 MHz, 2.0 Msym/s. Sources disagree on DVB-S against DVB-S2 (BATC wiki: DVB-S, FEC 1/2; SatNOGS: DVB-S2); the receiver detects either. Status of the transmitter: reported in testing in 2025, unknown now. |
| Satellite TV feed hunter | DVB-S, carrier list | Tries every carrier up to 12 Msym/s in turn with the receiver's own carrier survey, marks carriers that appear after the first 30 s. A carrier that never locks is listed as "no lock". |
| MEOSAR 406 MHz relay | SARSAT | Already there (issue #214): 1544.5 MHz preset and the whole-capture search. The preset is now labelled MEOSAR. No new mode, same protocol. |
| NavIC | family 561 | L5 SPS from the public ICD v1.1: codes, 50 sps, sync word, interleaver, Viterbi, CRC-24Q, subframe fields (time of week, week number, IDs). Tracks and decodes; no position fix. S band codes are present but not received. |
| UHF SATCOM skimmer | family 552 | Active 5 and 25 kHz channels, 240-270 MHz. Energy and width only. |
| Thuraya GMR-1 | family 553 | **Detection only**: active 31.25 kHz channels. The standard is public (ETSI TS 101 376); the requested control channel decoder was not built. |
| Starlink detector | family 554 | The 750 Hz frame gap in the power envelope. Needs a Ku LNB and dish. |
| OneWeb detector | family 555 | The 1 ms repeating sequence by delayed correlation. Needs a Ku LNB and dish. |
| TDRS carriers | family 556 | Carrier display on the public band. Whether a ground station can see these at all is unverified. |
| Tiangong / Shenzhou | family 557 | Carriers from SatNOGS / Orbital Focus lists. |
| Tsikada / Parus | family 558 | 150 and 400 MHz carriers with Doppler. |
| Gonets | family 559 | Bursts per channel, 387-390 MHz. |
| Tiantong signalling | family 560 | Channel energy in the ITU allocation only. |
| CubeSat telemetry per satellite | Smart CubeSat decoder | GOMX-1 beacon A added, chosen by NORAD number, layout from gr-satellites. |

## Not built, and why

- **INSAT**: no public downlink data format found. No mode: an entry that shows nothing but a carrier on a guessed frequency would invent more than it tells.
- **ABS-S**: no public specification found. No mode; a carrier in the feed hunter that never locks may be this, and is shown only as "no lock".
- **GMR-1 control channels**: spec is public but the decoder was not done in this branch. osmo-gmr's sources could not be read here (its server answers with a bot check, which was left alone).
- **Further CubeSat satellites**: only GOMX-1 was added. Most gr-satellites layouts need framing not verified here.
- **Android sky pointer**: skipped. Needs satellite data exposed to the Android side, which is not small.

## Licences

gr-satellites is GPL-3.0 (repository licence checked, files marked GPL-3.0-or-later): compatible, credited in `THIRD_PARTY.md` and in-app credits. SatNOGS decoders are AGPL-3.0: compatible with GPL-3 only through section 13 of each licence, and it would carry the AGPL network clause, so none of that code was used. The owner can decide otherwise.

## Measured on simulated signals

- Identification: 12 of 12 tracks among 1500 satellites, all confident, with unknown offset up to 9 kHz and 15 Hz of noise. Correct at 5 and 30 Hz noise; no answer at 60 Hz and above (the limit is `goodRmsHz`). No confident wrong answer at any noise. A fixed line gives no answer; a launch-train twin is reported ambiguous.
- Bistatic: 8 of 8. GRAVES through the meteor receiver: 3 of 3 named, no false echo with beacon and meteor pings only.
- Starlink frame gap: detected at 0 and -5 dB SNR in the capture, not at -10 dB. OneWeb: detected at +5, 0 and -5 dB. Noise alone detected neither. The test signals follow published numbers; the real signals have structure not modelled.
- NavIC: all 28 PRN codes (L5 and S) match the ICD's check column. FEC: all subframes at sigma 0.6, 25 of 30 at 0.8. Receiver on its generator with two satellites at 46 and 42 dB-Hz, Dopplers +2800 and -1400 Hz: both tracked, subframes decoded with good CRC, no phantom satellites. Its C/N0 readout was about 6 dB low at 46 dB-Hz (39.9). The week number field is checked by a direct parse test, not through the receiver: the 34 s run only reaches subframe 2.
- GOMX-1: fields against a frame built from the published layout, not against a real frame.

Not tested: the app GUI (syntax-checked only under the overnight limits; the integrator's build is the first real build of the screens) and any real hardware.

## Frequency sources

SatNOGS DB and Orbital Focus (Tiangong, Shenzhou), BATC wiki and ARISS (ISS, HamTV), Matt Blaze's UHF SATCOM list and NTIA (UHF SATCOM, TDRS), sigidwiki (Gonets), Humphreys et al. arXiv 2210.11578 and 2501.05302 (Starlink), Komodromos and Humphreys (OneWeb), ISRO IRNSS SPS ICD v1.1 (NavIC), KeepTrack and press reports of Scott Tilley's findings (zombie satellites; LES-1 NORAD 1002, LES-5 2866, Transit 5B-5 965, AO-7 7530). IMAGE at 2272.5 MHz and AO-7's beacon frequencies are from memory, not confirmed by those searches, and should be checked.
