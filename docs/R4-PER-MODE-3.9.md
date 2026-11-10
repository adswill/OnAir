# Round 4 per-mode features (3.9): what was built

Branch feat39-r4. The list is scratchpad/smart37-backup/r4-per-mode.md (owner-approved for 3.9). Each item has its own doc with what was tested.

| # | Item | Doc |
|---|---|---|
| 1, 2 | APRS weather stations, messages and bulletins | APRS-BOARD-3.9.md |
| 3, 5, 7 | Sensor names, Home Assistant device per sensor, threshold alerts | SENSORS-HOME-3.9.md |
| 4 | ADS-B filter and search | ADSB-FILTER-3.9.md |
| 6, 14, 35 | ham.decode alerts (JTAlert-style), HF calls in the message history, country column in CW / keyboard modes | HAM-ALERTS-3.9.md |
| 8 | ADS-B overhead log | ADSB-OVERHEAD-3.9.md |
| 9 | Meshtastic channel link import / copy | MESH-CHANNEL-URL-3.9.md |
| 10 | Airband clip per transmission, 30 s replay | AIRBAND-CLIPS-3.9.md |
| 11, 23, 33 | ADS-B and AIS history over days, first-ever airframes, vessel log | SEEN-STORE-3.9.md |
| 12, 32 | FM / DAB played list, artist / word / traffic alerts | PLAYED-3.9.md |
| 13 | image.done event, pass-to-Discord recipe | IMAGE-DONE-3.9.md |
| 15 | AIS arrivals board | AIS-ARRIVALS-3.9.md |
| 16 | ADS-B theoretical range outline | ADSB-HORIZON-3.9.md |
| 17 | FT8 / WSPR band hopping | HFDIG-HOP-3.9.md |
| 18 | Records board | RECORDS-3.9.md |
| 19 | Weather station upload (Weather Underground, CWOP) | PWS-UPLOAD-3.9.md |
| 20 | Meteor pass report card | PASS-REPORT-3.9.md |
| 24 | Geostationary full-disk loops | GEO-LOOP-3.9.md |
| 25 | Daily digest | DIGEST-3.9.md |
| 26 | Sensor long history | SENSORS-HISTORY-3.9.md |
| 27 | Mesh node watch | MESH-WATCH-3.9.md |
| 28 | ADS-B arrivals and departures board | ADSB-AIRPORT-3.9.md |
| 30 | SatNOGS DB live forwarding | SATNOGS-SIDS-3.9.md |
| 31 | plane-alert-db categories | PLANE-ALERT-3.9.md |
| 36 | LoRaWAN receive-only forwarder | LORAWAN-FWD-3.9.md |
| 37 | Radiosonde flight track KML / GPX | SONDE-TRACK-3.9.md |
| 38 | When is the band open | BAND-OPEN-3.9.md |
| 39 | Radiosonde launch sites | SONDE-SITES-3.9.md |

## Not built
- **21 flight routes, 22 solar / K-index, 29 trunk clip upload, 34 WSPR neighbour comparison**: left out as asked (internet items that need the owner's decision).
- **40 NMEA / gpsd output**: already exists. The GNSS data feed (Feeds, kFeedGnss) sends NMEA RMC and GGA of the SDR fix over UDP (127.0.0.1:10111 by default). gpsd reads it with `gpsd udp://127.0.0.1:10111`. Nothing was added.
- Inside items:
  - Windy and AWEKAS uploads (#19): their interfaces were not verified.
  - QR scanning for Meshtastic links (#9): OnAir has no QR decoder.
  - MP4 loops (#24): no video encoder is shipped.
  - Attaching the picture to the Discord post (#13): the webhook action sends text only.
  - Pass reports for HRPT / FY-3 / AWS (#20): Meteor only.
  - Horizon rings on the AIS map (#16).
  - FM / DAB DX on the records board (#18).

## New automation families
`sensor`, `ham`, `broadcast`, `image` (added after `msg`). Integrations and the message database list them.

Simulated data only for every item. No real recording, broker, network server or web service was used, except where a doc says otherwise.
