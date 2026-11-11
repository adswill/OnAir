# FM and DAB played list, artist and word alerts (3.9)

Round-4 items 12 and 32 (scratchpad r4-per-mode list). Branch feat39-r4.

## Played list (item 12)
- Every song a station tags (RadioText+ artist and title on FM, DL Plus on DAB) is kept with the time and the station: **FM > Radio > Played** and **DAB > Radio > Played**. Both lists are one log, so a search finds songs from either.
- One entry per song:
  - The same tags again within 20 minutes are the same song.
  - A title that comes before the artist (or the other way) within a minute completes the entry.
  - The same song after a longer gap is a new entry.
- Search: every word must be found in the station, artist or title. **Save CSV...** exports what is shown.
- The log is appended to `<data folder>/played/played-YYYY-MM.csv`. This month and the last one are read back at start. At most 20 000 entries are kept in memory.
- FM is noted only when RDS is in sync and the station name is known. DAB is checked every 2 s for the playing service.

## Alerts and recording (item 32)
- New automation family `broadcast`:
  - `broadcast.now_playing`: a new song (source, station, artist, title, pty, freq_mhz)
  - `broadcast.text`: the radio text or DAB dynamic label changed, or an FM traffic announcement started or ended. Fields: source, station, text, artist, title, pty, ta, ta_start and freq_mhz.
- Built-in recipes, off by default:
  - **Favourite artist on the radio**: notify and record 4 minutes of the sound. Change the name in the rule.
  - **Word in the radio text**: notify. "traffic" is the example word.
  - **Traffic announcement**: when the FM station starts a TA, notify and record 2 minutes.
- Recording uses the existing `record_audio` action, so it records the service that is playing. OnAir does not switch to another service by itself.

## Tests
`tests/dab/test_dab_played.cpp`:
- repeats over a whole song, title first and artist first, two stations at once, case differences, the same song 40 minutes later, empty tags
- search, the CSV round trip with commas and quotes, the cap
- the three recipes on the engine (fire, stay quiet, record), the family and the catalog

The log part was run locally (all passed). The recipe part runs in CI. Simulated tags only: no recording of a station that sends RadioText+ or DL Plus was used. The UI was syntax-checked only.
