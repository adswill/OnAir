# Sub-GHz sensors at home (3.9)

Round-4 items 3, 5 and 7 (scratchpad r4-per-mode list). Branch feat39-r4.

## Names (item 5)
- In **Sensors**, right-click a sensor and choose **Name...** ("Garden", "Freezer"). The name is kept per model, id and channel (the sensor's key) and is shown in the table, the side list, the graph title, the automations (`name` field) and Home Assistant. An empty name removes it.
- Names are saved with the settings (`sensorsNames`), with the time each named sensor was last heard.
- **Battery change.** Most sensors pick a new id when the battery is changed. OnAir shows a hint when a named sensor has gone quiet and a sensor with the same model and channel appears after it (within 6 h). "Quiet" means it has not been heard for 3 of its learned intervals, or for 10 minutes when the interval is not known. **Move the name** moves the name to the new sensor. **No** hides the hint for this session. A new sensor is offered for one name only, the one that went quiet closest in time.

## The sensor.reading event
- The receiver sends `sensor.reading` once per transmission. The radio's repeats within 2 s count as one reading. The event only goes out when something wants it: an automation, Integrations or the message archive.
- Fields: `key`, `name` (your name, else model and id), `named`, `model`, `id`, `channel`, `kind`, `rssi_db`, `snr_db`, `battery_low`, and every value the decoder gave. Numbers stay numbers (`temperature_C`, `humidity`, `wind_avg_km_h`, `rain_mm`, `battery_ok`, `detect_wet`, `contact_open` ...) and text stays text.
- New automation families: `sensor` (Integrations: "Sub-GHz sensors (readings)"), plus `ham`, `broadcast` and `image` for the items that follow.

## Home Assistant, one device per sensor (item 3)
- With Integrations > MQTT > Home Assistant discovery on and the Sub-GHz sensors family ticked, every sensor becomes its own Home Assistant device, named with your name. It gets one entity per value it sends:
  - temperature (°C and °F, also temperature 1 and 2), humidity, moisture
  - pressure (hPa, kPa, psi, bar)
  - wind speed and gust (km/h, m/s, mph) and wind direction
  - rain total and rain rate
  - UV, light, battery voltage, power, energy, current, voltage, CO2, PM2.5, PM10, depth, lightning count and distance
  - battery (a binary sensor: on means low)
  - signal level (diagnostic)
- Each entity has its device class, unit and state class, so Home Assistant draws history and statistics. Values OnAir has no entry for are still in the state JSON (`value_json.fields.<key>`) but get no entity.
- Config topics: `<prefix>/sensor/onair_sensor_<key>_<value>/config` (retained), `binary_sensor` for the battery. One retained state topic per sensor: `<base>/sensor/<key>`, the event JSON. Availability uses the same `<base>/status` as the five summary sensors.
- The configs are sent the first time a sensor is heard on a connection, and again when its name changes. After a reconnect they are sent again.
- The entity table is OnAir's own (not copied from rtl_433_mqtt_hass.py).

## Threshold alerts (item 7)
- **Alert...** in the right-click menu of a sensor: choose one of its values, above or below, and a number. This adds an enabled automation that sends a notification and a sound, at most every 30 minutes while the value holds. Only that sensor (by key) triggers it. It shows in View > Automations, where you can edit it or switch it off.
- Built-in recipes, off by default like every built-in rule, for named sensors only (so a neighbour's sensor does not trigger them):
  - **Freezer too warm**: a sensor with "freezer" in its name reads above -12 °C.
  - **Frost**: below 0 °C.
  - **Water leak**: `detect_wet`, `water` or `leak` is 1.
  - **Door or window opened**: `contact_open` or `opened` is 1.
  - **Sensor battery low**: at most once a day per sensor.

## Tests
- `tests/sensors/test_sensors_home.cpp`:
  - names: cleaning, the 60-byte limit, the settings round trip with bad lines, labels and moves
  - move hints: same model and channel only, after the named sensor went quiet, not while it still sends, not after 6 h, the closest of two names
  - sensor.reading from the real receiver on generated Nexus-TH signals at 250 ksps, 1 Msps, 2 Msps, 30 kHz and 250 kHz off the centre, a 4 kHz carrier offset, 16 dB and 18 dB SNR. Each case has three transmissions: a named sensor, the same sensor 30 s later with a low battery, and an unnamed sensor. The test checks one event per transmission, the values and the names. No event goes out without a listener.
  - each recipe on the engine (fires, stays quiet, cooldown per sensor), and the Alert rule (that sensor only, above and below, the 30-minute repeat, the filled-in title)
- `tests/integrations/test_integrations.cpp` `testHaSensorsOnBroker`: the entities, device class, unit, template and device name, the battery binary sensor, and the hub against the in-process MQTT broker. It checks configs once and again after a rename, and one retained state per reading.

Simulated signals only. No real sensor recording was used, and no real Home Assistant was connected (the payloads follow the MQTT discovery format and were checked by the test, not by Home Assistant).
