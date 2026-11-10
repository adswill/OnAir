# Sub-GHz sensors: long history (3.9)

Round-4 item 26 (scratchpad r4-per-mode list; WeeWX, Cumulus). Branch feat39-r4.

- Every reading of every sensor is added to a record per local day: the minimum, maximum and mean of each value, and the rain of that day from the rain counter. The counter's rise is counted and a reset (a new battery) is skipped.
- Kept in `<data folder>/stats/sensors-days.txt`: saved every 10 minutes and when the receiver stops, and read back at start. At most ten years per sensor.
- **Sensors > History**:
  - pick a sensor (names are shown)
  - a table of days, newest first, with min / max of up to six values (temperature, humidity, pressure and wind first) and rain per day
  - the rain of this month and this year
- The in-memory graph of the Sensors view still holds its last 600 points. The history is the long view beside it.

## Tests
`tests/sensors/test_sensors_archive.cpp`:
- three days of readings every 60 s: daily min / max / mean of a sine
- rain per day across a counter reset and midnight, month and year totals
- ids and counters not taken as values
- a restart (save and load) at midnight, the local day at UTC+3, two sensors, the round trip with bad lines

Run locally: all passed. Simulated readings only. The view was syntax-checked only.
