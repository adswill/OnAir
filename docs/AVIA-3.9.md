# Aviation package (3.9, branch feat39-avia)

Five items: military UHF airband, VDL Mode 3, FANS-1/A from ACARS / VDL2 / HFDL, TACAN, ILS marker beacons.
Everything here is receive only, and everything is tested on simulated signals only. No recording of any of these signals was available.

## 1. Military UHF air band (225 - 400 MHz AM), in the Airband mode

Before this, the band was only an entry in the band plan (General receiver), and Airband accepted 118 - 137 MHz only.

- Airband takes 225.000 - 399.975 MHz on the 25 kHz raster (7000 channels). Channel names parse there, and the band watch names unlisted carriers on that raster.
- The **Military UHF** button adds the published common channels and tunes to 243.000 MHz when the dial is outside the band. The channels are 243.000 (UHF guard / emergency, priority) and 282.800 (NATO on-scene SAR / DF). Everything else in the band is assigned per country and base, so nothing else is preset. Add other channels by hand, from the band watch, or from a fast scan.
- There is a fast scan profile, "Military UHF air 225-400" (25 kHz steps, 243.0 and 282.8 marked), and the band plan entry now offers Airband.
- `core/src/airband/airband_mil.cpp`, `core/include/dect2/airband/airband_mil.h`.
- Test `airband_uhf`: names and the raster, the known channels, and the receiver around 243 MHz. It checks that keyed channels open and the empty neighbours 25 kHz away stay shut. Impairments: a radio tuning error up to 30 ppm (7.3 kHz), a noise sweep, a sample clock error and 8-bit clipping.

## 2. VDL Mode 3 (new mode, family 640, engine standard 642)

VDL Mode 3 is the TDMA voice and data link of ICAO Annex 10. It was trialled in the US (NEXCOM) and never went into service, so live signals are unlikely.

**Implemented from ICAO Annex 10 Vol III, Part I, 6.4.2 and 6.4.4** (publicly available):
- D8PSK at 10 500 symbols/s, with the Mode 2 Gray code and a raised-cosine filter (alpha 0.6).
- The training sequences S1, S1*, S2 and S2*. Only the sign of the correlation separates S1 from S1*, and S2 from S2*: each phase change differs by 180 degrees, as the Annex notes. This works for carrier offsets up to +-2.6 kHz.
- These burst kinds:

  | Burst | Sync | Symbols after the training |
  |---|---|---|
  | M uplink | S2* | 32 (4 Golay words) |
  | 3T M uplink | S2* | 128 |
  | H handoff check | S2* | 40 |
  | Poll response | S2* | 16 |
  | M downlink | S1 | 16 |
  | Net entry | S1* | 16 |
  | V/D | S2 | 8 header + 192 user information |

- The S2* length is found from where the burst's power ends, before the next training sequence.
- The Golay (24,12) code, with the G matrix of 6.4.4.1.2 (the test checks the weight distribution 759/2576/759/1).
- The scrambler restarted on every burst.
- The Reed-Solomon (72,62) code over GF(256): field x^8+x^7+x^2+x+1, roots alpha^120..129.

**Not decoded, because ICAO Doc 9805 (the VDL Mode 3 technical manual) is not public:**
- What the system data, header and handoff bits mean. They are shown as raw 12-bit values.
- The slot timing.
- The bit order of the RS octets. This code assumes LSB first and data before parity, as in Mode 2.

**How bursts are classed:**
- A V/D burst whose user information is a good RS word is shown as data, with its octets and any text.
- Every other V/D burst is shown as voice. Voice uses the Mode 3 vocoder, which is not decoded: no codec is shipped.

**Test signal:** the bursts follow the Annex formats, but the slot layout and field values are made up. Each 30 ms slot has an M burst, then a V/D burst 12 symbols later. Voice bursts carry random bits. The generator can add drift, a sample rate offset, an off-centre channel and an adjacent station.

**Files:** `core/src/vdl3`, `core/include/dect2/vdl3`, `app/vdl3`.

**Test `vdl3_all`:**
- Codes: Golay with 0-3 errors corrected and 4 detected; RS with up to 5 octet errors corrected.
- Sync: the transcription is checked, and the S1/S2 cross-correlation is low.
- Bits: all eight burst kinds round trip with errors.
- Receiver: 250 kHz and 2.4 Msps with a -1500 Hz offset, at Es/N0 30 and 17 dB. Set ONAIR_TEST_LONG=1 for the full grid: 3 rates x 3 offsets.
- Combined impairments: 150 Hz/s drift, 60 ppm SRO and a channel 100 kHz off centre.
- An adjacent-channel station at the same level.
- Noise alone, which must give no bursts, and unusable sample rates.

**Measured in CI (Linux, quick run):**

| Condition | Result |
|---|---|
| Es/N0 30 dB, -1500 Hz, 2.4 Msps | 66 of 66 bursts |
| Es/N0 30 dB, -1500 Hz, 250 kHz | 64 of 66 (one handoff check and the V/D burst after it were lost; cause not found) |
| Es/N0 21 dB | 44 of 44 at 250 kHz, 43 of 44 at 2.4 Msps |
| Drift + SRO + offset | 54 of 54 |
| Adjacent channel station | 52 of 52 |
| Es/N0 18 dB | 40 of 44 |
| Es/N0 15 dB | 10 of 44 |
| Noise only | 0 syncs |

- No wrong bursts were passed in any of these runs.
- Below about 20 dB, data bursts whose Reed-Solomon word fails are shown as voice, as described above. The receiver has no matched filter (it uses a +-13.5 kHz low-pass), which costs about 3 to 4 dB.
- CPU: about 0.06 to 0.1 s per second of signal.

## 3. FANS-1/A ADS-C and CPDLC from ACARS, VDL2 and HFDL

This already existed. `core/src/aero/aero_adsc.cpp` decodes ADS-C, and `aero_cpdlc.cpp` decodes CPDLC using the vendored libacars asn1 tables (MIT, credited in THIRD_PARTY.md). VDL2, the ACARS mode and HFDL all call them.

Changes in this branch:
- HFDL and the ACARS panel label CPDLC as CPDLC; they used to call everything ADS-C.
- New end-to-end tests: `acars_fans` sends a real CPDLC downlink through the VHF ACARS signal with offsets (+-2.5 kHz) and noise, plus ADS-C reports that must not be labelled CPDLC. `hfdl_fans` sends CPDLC and ADS-C in HFDL bursts with tuning error and noise, and checks that a damaged CPDLC text reports the CRC failure.

## 4. TACAN

This already existed: the DME mode has decoded the TACAN bearing (15 / 135 Hz, X and Y channels) since 3.8. New test `dme_tacan_impair`:
- Rotation rate off by 0.5 %.
- Sample clock off by 100 ppm.
- Pulse jitter.
- Noise sweep.
- Weak and strong beacons.
- Odd block sizes.
- The Morse ident keyed during the bursts.

Receiver fixes this needed (`core/src/dme/dme_rx.cpp`, `tacan.cpp`):
- The ident run now survives the reference bursts. Pairs between grid points are ignored once a run has started, and one or two missing grid points still count.
- One or two main bursts lost to noise no longer restart the bearing count.
- A group stays whole when noise hides one pair (65 us gap).

Results in CI: 35 of 36 radials within 2 degrees, of which 11 of 12 at the hardest noise (0.10, required 80 %). The ident is read during the bursts without noise.

Known limit: with envelope noise of 0.05, the ident run still breaks between the bursts, and the letters are not read. The test reports this but does not require it.

## 5. ILS marker beacons

These already existed: the VOR / ILS mode detects the 75 MHz marker tones (400, 1300 and 3000 Hz).
- New: it reads the keying as well (Annex 10 Vol I 3.1.7.4): outer dashes, middle dot-dash, inner dots. It reports when the keying does not match the tone.
- Test `vor_marker` covers carrier offset, noise, clock error and 8-bit clipping. A localizer signal or noise alone must not produce a marker.
