# Smart ham insights (3.9, round 5 items 1-4)

Branch `feat39-smart5b`. Code: `core/include/dect2/insight/ham_heard.h`, `aprs_health.h`, fed by `insight::Hub` (`hub.h`) and shown in
View > Insights > Ham (`app/panels/insight_ui.cpp`). Test: `test_ham_insight` (tests/ham/test_ham_insight.cpp).

| # | Feature | Input | State |
|---|---------|-------|-------|
| 1 | Who hears me: stations that call or answer you, their report, locator, distance, bearing, link balance, per band; event `insight.heard_me` the first time per station and band | FT8 / FT4 / JS8 decodes of the FT8 view, your call and locator from the Ham radio mode | done |
| 2 | Antenna pattern from decodes: SNR fitted against log10(distance), the rest averaged per 30-degree sector with its standard error and count | located decodes (locators learned from CQ and exchange messages) | done |
| 3 | Busted-call filter: a call heard once that is one character (CW: one Morse element) from a call heard often, or that is not a valid amateur call | the call signs of the decodes | analyser done; shown only through the API (no skimmer view uses it yet) |
| 4 | APRS network health: over-beaconing (fixed under 10 min, moving under 1 min, over 6 beacons), obsolete or long paths (RELAY, TRACE, WIDE, WIDEn-N with n > 2), one packet through more than 4 digipeaters within 30 s, a regular digipeater silent for 6 of its usual gaps (at least 30 min) | `aprs.packet` events | done |

## Measured (simulated decode streams only)

- Antenna pattern: 3000 decodes from random places, a 7 dB cosine pattern toward 75 degrees, 3 dB station power spread and 4 dB fading;
  best sector right and every sector within 2 dB in at least 19 of 20 runs. A flat antenna shows no sector above 1.5 dB.
- Busted calls: 200 known calls heard 5 times each, one suffix letter changed: at least 198/200 flagged and 190/200 with the right call;
  at most 20/200 new valid calls flagged by chance (a real call one letter from a known one). CW: DL1XEE flagged as DL1XI (one element).
- Who hears me: Vilnius KO24 to QF56 in the 14000-16500 km range, link balance and per-band rows checked.
- APRS: over-beaconing at 2 min fixed / 30 s moving, none at 30 min fixed / 2 min moving or with only 4 beacons; flood said once; silent
  digipeater said once after 6 gaps.

No real recording was used: all inputs are generated decode lists.

## Limits

- Locators are only known for stations that sent one; who-hears-me rows without a locator have no distance.
- The antenna pattern mixes propagation with the antenna: a sector toward a quiet ocean or an often-open path looks better than it is.
- M17, D-STAR, Fusion and ham DMR talkgroups and reflectors are covered by the voice analysers (SMART5-voice-3.9.md).
