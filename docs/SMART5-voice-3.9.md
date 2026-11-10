# Smart voice insights (3.9, round 5 items 5-7)

Branch `feat39-smart5b`. Code: `core/include/dect2/insight/voice_activity.h`, fed by `insight::Hub` from the end-of-call summaries
(`session.call`) of DMR, P25, NXDN, dPMR, TETRA, TETRAPOL, EDACS, SmartNet, MPT1327, LTR, D-STAR, Fusion, M17 and analog channels.
Shown in View > Insights > Voice. Test: `test_voice_insight` (tests/voice/test_voice_insight.cpp).

Per talkgroup or per system only: keys with private, unit, radio, individual or ISSI are skipped, so no radio is tracked. Encrypted calls
count as airtime and as a verdict; nothing is decrypted.

| # | Feature | State |
|---|---------|-------|
| 5 | Talkgroup heatmap: weekday x hour airtime per talkgroup (local time), normalised by how often each hour was listened to, half-life 4 weeks, kept in `insight_heatmap.txt` in the data folder; busiest hours, next busy hour, event `insight.voice_busy` when an hour has 3 times its usual airtime and at least 120 s | done |
| 6 | Trunk load: per closed hour, calls, carried Erlangs, offered Erlangs (Erlang B inverted for the channels seen, since blocked calls are never heard), blocking and peak concurrent calls; event `insight.trunk_load` above 5 % blocking | done |
| 7 | Talkgroup changes: clear to encrypted or back after 3 calls in one state (`insight.tg_encrypted` / `tg_clear`), a talkgroup silent for 7 days waking up (`tg_awake`), 2 emergency calls on one talkgroup within 10 min (`tg_emergency`, alarm) | done |

## Measured (simulated call streams only)

- Heatmap, six weeks of a talkgroup busy on Tuesdays 18-20 and a background talkgroup round the clock, at UTC, UTC+2 and UTC-5: busiest
  hours Tuesday 18 and 19, next busy hour from Monday noon exactly Tuesday 18:00 local, no busy alert in ordinary weeks, one alert for an
  unusual Wednesday morning, save/load gives the same map and prediction.
- Erlang B: B(10, 5) = 0.018385; offered-from-carried inverts exactly for 1-20 channels.
- Trunk, 8 channels, blocked-calls-lost simulation with Poisson arrivals and 20 s mean holding time, 24 h each at 1, 3, 5, 7 and 9 Erlangs:
  offered traffic estimated within 12 % (CI: 1.01, 2.95, 5.02, 7.04, 9.02 Erlangs), blocking within 0.03 of the measured share (CI: 0.071 against 0.072
  at 5 Erlangs, 0.290 against 0.291 at 9), no alert at 1-3 Erlangs, an alert in at least 20 of 24 hours
  at 7 and 9 Erlangs, each hour reported once.
- Changes: one alert per real change, none for flip-flopping, one emergency alarm per cluster.

No real trunked recording was used.

## Limits

- The channel count is the number of distinct voice channel and slot pairs seen; set it with `TrunkLoad::setChannels` when known.
  Until every channel has been seen once, blocking is overestimated.
- Session keys of some modes may not say "TG"; those modes are only counted once their keys say group, talkgroup, channel, reflector or room.
