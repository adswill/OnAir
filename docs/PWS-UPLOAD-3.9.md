# Personal weather station upload (3.9)

Round-4 item 19 (scratchpad r4-per-mode list; WeeWX, Cumulus). Branch feat39-r4.

- **Sensors > Upload** (off by default): pick the sensor that is your station, and optionally a second one that fills what the first does not send (a rain gauge next to a thermometer). Every 5 minutes or more, the values go to:
  - **Weather Underground**, with your station ID and key, over the PWS upload protocol (`updateweatherstation.php`, imperial units: tempf, humidity, baromin, windspeedmph, windgustmph, winddir, rainin, dailyrainin). The reply must say "success".
  - **CWOP**, as an APRS complete weather report (APRS101 chapter 12) over APRS-IS to cwop.aprs.net:14580. CW stations use passcode -1; a licensed amateur uses the APRS-IS passcode of the callsign. The position is the receiver position.
- Rain comes from the sensor's rain counter history: the last hour, the last 24 h and since local midnight. A counter reset (a battery change) is skipped.
- A sensor not heard for 15 minutes sends nothing, so stale values are never uploaded. The upload runs on its own thread and the status line shows the result. Keys are kept in the settings and shown as dots.
- **Not included: Windy and AWEKAS.** Their upload interfaces could not be checked here (Windy changed its station API), so no unverified protocol was built.

## Tests
`tests/sensors/test_sensors_pws.cpp`:
- readings from metric and imperial sensors
- rain over the last hour, 24 h and since midnight across a counter reset
- merging two sensors
- the full Weather Underground address with URL encoding
- the exact CWOP packet: wind from the north, temperature below 0 °F, 100 % humidity as h00, rain, pressure, position rounding to 60', missing values as dots
- the login and the packet to a fake APRS-IS server on the loopback

Run locally: all passed. No real upload was made, and no reply from Weather Underground or CWOP was seen.
