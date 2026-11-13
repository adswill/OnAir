# Sensitivity 4.0, lane A, part 4 (feat40-sensA4)

Method as in SENSITIVITY-4.0-A3.md (mode generator signal, random start and carrier phase, white noise, SNR in 10 kHz, fraction of the clean run's frames decoded, 4 trials a point,
12 s trials for iMet-54, 24 s for MRZ). All numbers are simulated signals only (the generator's deviation, skirts and levels are assumed values). Run on GitHub (quick.yml, Linux),
because the part was done with the Mac on battery: no local build.

## 1. iMet-54 and MRZ: slicing threshold from the smoothed extremes

Why iMet-54 was about 7 dB worse than RS41 (and MRZ 4 dB worse than its Eb/N0): both sondes use the "midpoint slicer" (their short preamble and unbalanced line code do not let the
discriminator's mean settle), which put the threshold in the middle of the recent maximum and minimum of the **raw** discriminator. At low SNR the maximum and minimum are noise spikes
(clicks), so the threshold wandered and the bit decisions were wrong far more often than the SNR required. Not the bit rate, the preamble sync or the filter. The extremes are now tracked on the
0.7-symbol moving average that the slicer uses anyway (`SymbolChain` thrMode 2).

Tried and not kept (same harness, carrier 0):
* Tone-energy symbols (as RS41) on the midpoint chains with the old threshold: iMet-54 worse (16 dB 95 % to 86 %), MRZ clean frames 7 to 5.
* Running mean of the discriminator as threshold after 64 symbols: MRZ 50 % point about 8.5 dB (better than before, worse than below), iMet-54 decodes nothing (its UART line code is unbalanced).
* Smoothed extremes plus tone energies: iMet-54 11 dB 41 %, MRZ 8 dB 28 %: worse than smoothed extremes alone.
* Tone energies for iMS-100 (2400 Bd, not midpoint): 6 dB 77 % to 90 % only, one point; not kept (assumed deviation, small gain).

| SNR in 10 kHz, carrier 0 | 19 / 16 / 13 / 11 / 9 / 7 dB iMet-54 | | 14 / 12 / 10 / 8 / 6 / 4 dB MRZ |
|---|---|---|---|
| before | 100 / 95 / 25 / 0 / 0 / 0 % | | 100 / 36 / 0 / 0 / 0 / 0 % |
| after | 100 / 100 / 89 / 52 / 2 / 0 % | | 100 / 100 / 98 / 75 / 8 / 0 % |

50 % point: iMet-54 about 13.3 to 11.0 dB (2.3 dB), MRZ about 12.3 to 7.5 dB (about 4.5 dB). MRZ clean frames a 24 s trial 7 to 10. Guard `sens40a4` (carrier 0, +8 kHz, 50 ppm clock:
iMet-54 13 dB 89 / 91 / 93 % (before 25 % at carrier 0), MRZ 10 dB 98 / 100 / 100 % (before 0 %); noise-only 0 units). Existing tests `sonde_basic`, `sonde_extra`, `sonde_extra_rf`, `sonde_all_rf`,
`sonde_interf`, `sonde_engine`, `sonde_rs92_rf`, `sonde_rf_dfm`, `sonde_sens40` pass. iMS-100 unchanged (not a midpoint chain).

## Checked, nothing to add

* **Mode S / ADS-B**: CRC-guided correction already exists (`AdsbTracker`, `fixBits` up to 2, 3 optional; known-address plausibility, `fixUnknown` off by default). UAT already corrects with Reed-Solomon
  and erasures. ACARS, AX.25 (packet), POCSAG / FLEX (pager), DSC, FLARM, ELOran already repair or vote on CRC/BCH failures.

## Not done (and why)

* **Iridium soft BCH** (about 3 dB expected): no soft value exists between the burst demodulator and the frame layer (hard bits only), so it is a plumbing change through `iridium_phy`, `iridium_rx` and
  `iridium_frame`, to be written and measured on CI only (battery rule). Left for a later part.
* **Zigbee coherent chip correlator** (about 5 dB), **HaLow SIG / channel estimate** (1 to 2 dB), **soft or Chase decoding of the other FEC decoders**, **AIS CRC bit repair**, **the Mesh / LoRa harness metric**:
  not started in this part; each needs a measured CI loop of about an hour.
* **RS92, DFM, M10, M20, RS41**: tone-energy symbols already in 4.0 (A and A2).

## Modes at theory (as far as measured with these tools)

Those listed in SENSITIVITY-4.0-A3.md "At theory", plus iMet-54 and MRZ now to within about 3 dB of non-coherent 2-FSK with the sonde's hard Hamming(8,4) / frame CRC (the iMet-54 Hamming code
corrects one error per 8 bits: soft decoding of it would be the next step). Not at theory: Iridium (soft BCH), Zigbee, HaLow, iMS-100 and MRZ soft Manchester decisions.
