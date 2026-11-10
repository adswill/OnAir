# Smart HF utility insight (3.9, round 5 item 10)

Branch `feat39-smart5b`. Code: `core/include/dect2/insight/schedule_watch.h`, fed by `insight::Hub` from the session and `msg.*` events of
non-voice modes between 100 kHz and 30 MHz (WEFAX, RTTY, SITOR, NAVTEX, STANAG, ALE ... whichever of them send events), keyed by mode and
frequency. The tuned frequency comes from the Insights tick. Shown in View > Insights > HF utility. Test: `test_hfdig_overdue`.

| # | Feature | State |
|---|---------|-------|
| 10 | Overdue broadcast: a UTC time of day becomes a learned slot after decodes within 6 min of it on 3 days and in 60 % of the days the receiver was on that frequency then. 16 min after a learned slot with no decode, and the receiver on the frequency for 80 % of the slot window, `insight.overdue` ("DWD 10100.8 kHz overdue (usually at 10:30 UTC, heard on 9 of 10 days)") | done |
| 11 | Nothing more: ALE link quality, the schedule display and HFDL frequency learning already cover the rest | not proposed |

## Measured (simulated schedules only)

10 runs of 14 days, three broadcasts a day with 2 min jitter, 10 % not decoded, a stray decode most days, one day tuned elsewhere: every run
learned the 3 slots within 2 min, every missed broadcast from day 5 on was reported, 0 false alerts and 0 alerts on the day tuned elsewhere.

## Limits

- Many HF utility decoders do not send automation events yet; those modes are not learned until they do (`Hub::onUtility` is the direct call).
- Slots within about 22 min of midnight UTC are learned but not judged (the day boundary).
- Session events carry the end time, so a long chart is learned at its end time; the schedule stays consistent.
