# HFDL: band choice and nearby ground stations (4.0)

Branch `feat40-hfdlst`. A user reported that "Follow the best band" was misnamed. It left a silent band, but then stayed on the first band
where it heard one ground station or one aircraft, even when another band was busier. 4.0 renames that choice, adds a real "busiest band"
choice, and adds a Nearby view with the ground stations near you and the frequencies they use now.

## Band choice (Tuner panel, HFDL)

A combo box replaces the old check box (pref `hfdlHopMode`; the old `hfdlHop` = on becomes "Find a working band"):

| Choice | What it does |
|---|---|
| Stay on this band | Never retunes. |
| Find a working band | The 3.x behaviour, renamed: it leaves a silent band and stays on the first band that decodes something (`HfdlBandHopper`). |
| Busiest band | New (`HfdlBusiestBand`, `core/include/dect2/hfdl/hfdl_hop.h`). It runs a survey: 75 s on each candidate band (two 32 s squitter frames of every station there). Each band gets a score of 1 x good PDUs per minute, + 2 x distinct aircraft, + 3 x distinct ground stations heard in the window, + 0.1 x mean SNR (dB) of the good bursts (only when PDUs were decoded). The radio then goes to the best band and stays. The score is measured again every 75 s. After 20 min it surveys again; the band it stays on keeps its latest score. A window with nothing at all starts a survey at once. If the user tunes to another HFDL band, it stays on that band until the next survey. |

Candidates: if the active frequencies of nearby ground stations are known (from squitters heard here and, when it is on, the live list), only
the bands that hold them are surveyed, best hinted first, plus the band the radio is on. Otherwise every common band is surveyed. Both
automatic choices tune to a tune point (`hfdlTunePointMhz`). This is the band centre when the sample rate covers the whole band. Otherwise it
is a centre whose span holds the nearest station's frequency and as many others as fit, with a lone signal kept 10 kHz off the DC spike.

## Nearby view (HFDL > Nearby)

The view follows the layout of the weather-fax station directory and the NAVTEX nearest-station list. Stations are listed nearest first,
with:
- the distance and bearing,
- the active frequencies as one-click tune buttons (a highlighted button is already in the span being decoded),
- when the list was last updated,
- the source: squitter (heard here), live (hfdl.observer), or both when they agree.

The position comes from a locator or coordinates typed in the view (pref `hfdlMyPos`), or else from the station location (View > Map). The
Tuner panel also shows the nearest active frequency with tune buttons.

Merge rule (`hfdlNearby`, `core/include/dect2/hfdl/hfdl_live.h`): a squitter replaces a station's whole active list, so the two lists are
not unioned. For each station, the newer of the fresh live entry and the fresh squitter is shown. A frequency that the other fresh source also
lists (within 1 kHz) carries both sources. Data older than an hour is dropped. The data source, hfdlobserver, keeps squitter and cache data
for an hour too. A time more than 5 min in the future counts as a bad clock and is not used. Squitter frequencies go through the frequency
learner (`HfdlFreqLearner::announced`), so a station whose system table changed shows the frequencies it was heard on.

## Data source and terms

- **Squitters (always, offline):** every HFDL ground station broadcasts an SPDU every 32 s on each of its frequencies, listing its active
  frequencies. OnAir already decoded these (`HfdlGroundStation::activeKhz`, `hfdl_learn.h`); the Nearby view and the band choice now use
  them.
- **hfdl.observer (opt-in):** `https://hfdl.observer/active.json` is the page's own "Source Data" link: JSON with `ground_stations[]`
  (`id`, `name`, `last_updated` UNIX s, `frequencies.active` in kHz, `stratum`, `update_source`). It is the Airframes ground-station API
  shape. The site is run by the author of HFDLObserver (github.com/hfdl-observer/hfdlobserver, BSD-3-Clause). That client fetches the same
  URL every 61 s by default. No terms of use, robots.txt or licence are published on the site (checked 2026-11-12: /robots.txt, /terms and
  /about return 404). The data is offered openly through the Source Data link and read by the site's own open-source client, so OnAir uses it
  as an opt-in viewer:
  - off by default (Settings > Privacy, "HFDL: live ground-station list", key `hfdlLiveOn`; also a check box in the Nearby view),
  - never in Offline mode (`offlineMode`),
  - only while the Nearby view is open or an automatic band choice runs,
  - at most once every 2 min (5 min by default, `hfdlLiveMin`), and 10 min after a failure,
  - in the background with User-Agent `OnAir/<version> (HFDL ground-station list; +https://github.com/adswill/OnAir)`; the position is never
    sent,
  - cached in the data folder (`hfdl-live.json`).

  The URL can be changed with the pref `hfdlLiveUrl`, for example to the Airframes API.
- **Airframes.io API** (`api.airframes.io/hfdl/ground-stations`): same format, but on 2026-11-12 every station had an empty active list
  (`last_updated` 0), so it is not the default.
- Credits: hfdl.observer and Airframes.io in `tools/credits_data.tsv`, which generates THIRD_PARTY.md, the in-app Credits and
  docs/credits-for-website.md.

## Tests (no network)

- `hfdl_live`: parses a saved hfdl.observer list (`tests/data/hfdl/hfdl_observer_active.json`, 2026-11-12 08:11 UTC, 16 stations). It also
  checks:
  - relative times, junk entries, duplicates, out-of-range frequencies, and malformed or too-deep JSON,
  - the merge rules (newer wins, both sources, 1 kHz rounding, stale, future clock, unknown station listed last),
  - the distance and bearing sort from Dubai and Vilnius, the hints, and the tune points.
- `hfdl_busy`: `HfdlBusiestBand` on a simulated sky, with Poisson PDUs from a pool of aircraft per band, squitters every 32 s (70 % decoded),
  a 1-5 s retune and SNR noise. It covers:
  - the user's report: 8.9 MHz with 5 PDU/min, 1 aircraft and 1 station, against 11.3 MHz with 30/min, 12 aircraft and 3 stations. The old
    hopper stays on 8.9 MHz for over 95 % of the time; the busiest band spends over 60 % of 15-90 min on 11.3 MHz, the rest in rescans,
  - hints cutting the survey to the hinted bands (over 80 % of the time on the busiest band),
  - a band closing while another opens,
  - a sky with only noise: it keeps surveying, never stays and no band scores,
  - the user's tuning, off-band tuning, counters restarting, a radio that does not retune (one try per window), and slow retunes.
- `hfdl_hop` (the old hopper) is unchanged.

These are simulated signals only. A real sky has fading, bursts and aircraft that log on and off, which the simulation does not model. The
score weights are a reasonable first choice and have not been tuned on recordings.

## Limits

- A frequency in the live list that the built-in station table does not have is tuned but only decoded when the channel list (Tuner,
  "Channels kHz") or the learner (`hfdl_learn.h`) has it.
- The busiest-band survey takes about 75 s per candidate band. With no hints, a full survey of all 8 common bands takes about 10 min.
