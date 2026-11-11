# Lane B group 2: mode-specific features (4.0)

Each entry: what it is and why a power user wants it. Pure logic has a test in tests/feat40b (simulated data only).

## Digital voice and trunking (DMR; the core works for P25, NXDN, TETRA, dPMR, EDACS, LTR)
- **Talkgroup activity** (dect2/trunkpool/tg_activity.h, DMR calls view "Talkgroup activity"): airtime, calls and distinct radios per talkgroup over 10 minutes, a 5-minute-bucket timeline per talkgroup, and the busiest talkgroup. Why: on a busy site the call log hides which talkgroups matter.
- **Follow the busiest talkgroup** (`TgFollower`): ratio, minimum airtime and minimum dwell, so two alternating talkgroups do not make it flap (8 switches in a simulated hour of two equal talkgroups). Why: auto-follow without being thrown around.

## Radiosondes
- **Launch clock** (dect2/sonde/sonde_launch_clock.h, Sites view "Next launch (UTC)"): learns each site's usual launch times of day from the launches this station heard (kept in sonde-sites.txt), predicts the next one (clusters, midnight wrap, needs 3 days) and shows "now: tune in" 15 minutes ahead. Why: launches are scheduled; the sonde is only on air a few minutes before launch.

## ADS-B
- **Track replay** (dect2/adsb/adsb_replay.h): bounded per-aircraft fix store with interpolated position at any time, no bridging of coverage holes, antimeridian-safe, rejects impossible jumps (bad CPR). Why: "where was it ten minutes ago" and a whole-sky replay slider. Core and test only so far; the map slider is not wired.

## Transmit panels
- **Frequency plan check** (dect2/tx/tx_plan.h, TX settings): warns when the carrier or its 2nd to 5th harmonic lands on airband, VOR/ILS, 121.5, 243, 406 MHz beacons, marine ch 16, DME/ADS-B, GNSS or Iridium. Warning only, never blocks.
- **Link budget, power ramp, SWR advice** in the same header (fspl, margin, maximum free-space range, ramp steps, back-off dB for an SWR). Core and test only so far; not shown in the panel.
