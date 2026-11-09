# Smart FM / AM (3.7)

What was built from broadcast.md 1-7 and r2-broadcast.md 10 and 18. All figures are from the synthetic tests named below.

## FM

| Item | Where | Result |
|---|---|---|
| FM census (1) | `fm/fm_census`, FM Scan tab | One capture, all stations at once. Shared FFT channeliser, one analysis-only `FmReceiver` per station. 6 stations at 3.2 and 4 Msps with levels -8..-32 dB, carrier offsets, DC spike and an unmodulated spur: every PI and name right, stereo right, no false stations. About 0.6 s of CPU per 8 s of signal. `test_fm_census` |
| AF follow (2) | `fm/fm_af`, RDS details | Per-AF level from the spectrum, button and an auto-follow. The PI is proved after every retune, so a regional variant is rejected and not retried for 10 minutes. AFs outside the capture get a blind probe at most once a minute, and only when the station is weak. `test_fm_af` |
| Soft combining (3) | `fm_rds` | RDS already had LLR voting of repeated blocks. Added keyed combining (C/D copies grouped by a good block B). +0.6 % groups over the sweep, +5.7 % under fast fading at Eb/N0 5 dB and up, no wrong names. Looser versions gave more groups but failed the wrong-text guard in `test_fm_rds`, so they were dropped. `test_fm_vote` |
| DX alerts (4) | `fm/fm_dx`, FM Scan tab | Learns the local stations for 10 minutes, then alerts once per new foreign PI, with the country from the ECC or a "likely" list without it. CSV export. No distance: that would need a transmitter list. `test_fm_dx` |
| Click repair (r2 18) | `fm_rx` | Interpolates over discriminator clicks below 32 dB SNR. +1.3 dB audio at 10 dB C/N. No RDS gain (the existing equaliser path and Gaussian noise dominate). A clean station is untouched. Above 110 clicks per second it stands aside. `test_fm_click` |
| First-adjacent cancel (r2 10) | `fm/fm_neighbour`, `fm_rx` | Automatic when a +-200 kHz neighbour is 10 dB or more above the station, at capture rates up to 2.6 Msps. +12 dB audio with a +20 dB mono neighbour, up to about +25 dB at +35 dB; RDS held at 112 of 112 groups. `test_fm_neighbour` |

## AM / shortwave

| Item | Where | Result |
|---|---|---|
| AM census (6) | `general/am_census`, Shortwave tab | One FFT over the capture, ranked list. AM, DRM, SSB and bare carrier told apart, fading and sideband interference flagged, medium-wave raster 9 or 10 kHz found by itself. `test_am_census` |
| Fade predictor (7) | `general/am_diversity` FadeTracker | Depth, period and next dip from the carrier history. Regular 8 s fade found to within 1 s. `test_am_diversity` |
| Frequency diversity (7) | `AmDiversity`, `AmDivPlayer`, census "Also on" | Sound aligned by cross-correlation (lags -90 to +180 ms found exactly), crossfaded switching. 7.0 dB against 4.3 / 3.4 dB for the channels alone with alternating fades. `test_am_diversity` |
| Station estimate (5) | `general/sw_ident`, Shortwave tab | Always labelled estimated. Uses the user's own schedule file (EiBi format), the user-installed speech model's language, and interval signals the user has taught. Ships no station list and no interval-signal library. `test_sw_ident` |

## Not done

- First-adjacent cancellation for HD Radio: only FM is wired, the HD path needs its own test first.
- Census, canceller and diversity have only been tested on synthetic signals, not on the air.
