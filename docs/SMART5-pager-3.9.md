# Smart pager insights (3.9, round 5 items 8-9)

Branch `feat39-smart5b`. Code: `core/include/dect2/insight/pager_patterns.h`, fed by `insight::Hub` from `pager.message` (POCSAG, FLEX).
Shown in View > Insights > Pagers. Test: `test_pager_insight` (tests/pager/test_pager_insight.cpp), which also runs the hub end to end.

Keyword alerts and repeated-page combining already exist (automation watch list, `pager_combine.h`), so they are not repeated here.

| # | Feature | State |
|---|---------|-------|
| 8 | Automatic and test pages: 6 steady gaps (median absolute deviation under 7 % of the median, none off by more than 25 %, at least 20 s) of near-identical text (word overlap 0.8 with digits ignored); event `insight.pager_auto`, `PagerPatterns::automatic()` for a hide filter. It stays automatic while text and pace hold (one missed page is allowed) | done; "Hide automatic pages" switch in the pager message view (feat39-smartui) |
| 9 | Paging burst: 5 pages of one capcode within 10 min after an hour without one (`insight.pager_burst`); silent channel: a channel with 20 pages silent for 10 of its usual gaps and at least 30 min (`insight.pager_silent`) | done |

## Measured (simulated page streams only)

- Automatic: 12 of 12 steady streams (every 1, 5, 15, 60 min; jitter 0, 1, 4 %) found with the right interval; at most 1 of 60 irregular (30 %
  jitter), varied-text or random-time streams wrongly marked.
- Bursts: 20 of 20 bursts found, 0 false in 40 h of ordinary traffic (30 capcodes, a page every 90 s).
- Silent channel: not after 1000 s, once after 2000 s.

Page text is only compared in memory (the last 8 pages of a capcode); nothing is stored.
