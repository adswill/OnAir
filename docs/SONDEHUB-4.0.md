# SondeHub upload and the radiosonde map (4.0)

A radiosonde user in Germany reported that with 3.9's Data feeds > Radiosonde upload (APRS-IS) panel the uploads to wettersonde.net,
radiosondy.info and rotate.aprs2.net worked, but SondeHub did not, because SondeHub is not an APRS-IS server. 4.0 adds a SondeHub
uploader and three things on the radiosonde map.

## 1. SondeHub uploader

View > Data feeds > Radiosonde upload has a fixed first entry, **SondeHub (tracker.sondehub.org)**. It is off by default, like the
APRS-IS targets. Its fields are the uploader callsign, an optional antenna text, "Send my station position", "Chase car", and, under
Advanced, the upload interval, an optional contact e-mail and the server. It has the same status line as the APRS-IS targets: frames
uploaded, station uploads, dropped, retries, the last error in red and the last upload line. Below that are the frames that were held
back and the last reason. "What is sent" shows the last station record and the first frame of the last telemetry upload as JSON.

Code: `core/include/dect2/net/sondehub.h`, `core/src/net/sondehub.cpp`; the panel is in `app/panels/feeds_ui.cpp`; the receiver hands
each good frame over in `core/src/sonde/sonde_rx.cpp`.

### What the rules are, and where they come from

Read from the SondeHub wiki (github.com/projecthorus/sondehub-infra/wiki: "SondeHub Telemetry Format", "API (Beta)") and from the
reference uploaders radiosonde_auto_rx (`auto_rx/autorx/sondehub.py`) and ChaseMapper (`chasemapper/sondehub.py`):

| Rule | Source | OnAir |
|---|---|---|
| Telemetry: PUT `https://api.v2.sondehub.org/sondes/telemetry`, a JSON array, gzip-compressed | API page, auto_rx | same, with `Content-Encoding: gzip`, `Content-Type: application/json` |
| `Date` header (RFC 7231) is required: SondeHub uses it to correct a wrong receiver clock | API page | `Thu, 12 Nov 2026 10:20:30 GMT` |
| `User-Agent`: the software and version | API page (`autorx-1.4.1-beta5`) | `OnAir-<version>` |
| Mandatory fields `software_name`, `software_version`, `uploader_callsign`, `time_received`, `manufacturer`, `type`, `serial`, `frame`, `datetime`, `lat`, `lon`, `alt` | format page | all sent; `software_name` is `OnAir`, `software_version` the OnAir version |
| Optional fields only when available and reliable | format page | `subtype`, `temp`, `humidity`, `pressure`, `vel_v`, `vel_h`, `heading`, `sats`, `batt`, `burst_timer` (RS41), `frequency` (MHz, the receiver's estimate), `snr`, `uploader_position` `[lat, lon, alt]`, `uploader_antenna` |
| `datetime` comes from the sonde, never from the computer clock | format page | a frame without the sonde's time is held back |
| `frame`: for M10, M20 and DFM the sonde time as a Unix timestamp | format page | same |
| DFM: do not upload before the serial number is known | format page | no frame without a serial |
| Batch every 15 s; with DFM or Meisei sondes 1.5 times the interval, at most 30 s (z-check) | auto_rx `station.cfg` and `sondehub.py` | same; the interval is 10 to 60 s under Advanced, and the longer wait never makes it shorter than the interval |
| 200 success; 201 / 202 taken with notes; 5xx retried up to 5 times; other codes not retried, the answer text shown to the user | API page | same: the notes and refusals are shown in red |
| Station record: PUT `/listeners` with `mobile: false`, at the start and every 6 hours | API page, auto_rx | same, and again when the station details or position change (at most once a minute) |
| Chase car: PUT `/listeners` with `mobile: true` every 30 s | ChaseMapper | only with "Chase car" ticked and a live GPS fix |
| Encrypted sondes are discarded | auto_rx | OnAir decodes no position from them, so none is sent |

OnAir adds these checks of its own. A frame needs a GPS position that is not 0,0 and an altitude between -500 and 50 000 m. Where the
sonde reports satellites it needs at least 4. The sonde's time has to be within one hour of the computer clock, so an old recording
played back is not uploaded as live data. Only frames whose check passed reach the uploader.

Sonde names, as the format page lists them:

| OnAir | manufacturer | type | subtype | serial |
|---|---|---|---|---|
| RS41 | Vaisala | RS41 | RS41-SG / RS41-SGP | as decoded (S1234567) |
| RS92 | Vaisala | RS92 | RS92-SGP / RS92-NGP | as decoded |
| DFM | Graw | DFM | DFM06 / DFM09 / DFM17 | the number |
| M10 / M20 | Meteomodem | M10 / M20 | - | the label format (802-2-11234) |
| LMS6 | Lockheed Martin | LMS6-400 below 1 GHz, LMS6-1680 above | - | the number |
| iMet-54 | Intermet Systems | iMet-54 | - | the number |
| MRZ | Meteo-Radiy | MRZ | MRZ-N1 | 5667-39155 |
| iMS-100 | Meisei | iMS-100 | - | the number |
| iMet-1/4 | not sent | | | |

iMet-1 and iMet-4 send no serial. auto_rx makes one up from the power-on time, the frequency and whether it is an iMet-1 or an iMet-4.
OnAir does not tell those two apart, so its name would not match and the same sonde would appear twice on the map. They are held
back, and the reason is shown.

For the 400 MHz LMS6, OnAir uses `LMS6-400` from the format page's allowed values. auto_rx sends `LMS6-403` for the same sonde.

### Network and privacy

- The requests go through the app's existing HTTP client (`dect2/tv/iptv.h`, the Internet TV one), which now also does PUT with a body.
  It uses OpenSSL on Linux and the system TLS on macOS (NSURLSession) and Windows (WinHTTP). A Linux build without OpenSSL says so in
  the status line.
- Nothing is connected or sent until the box is ticked. Turning it off drops whatever has not been sent yet.
- SondeHub publishes the uploader callsign and, when "Send my station position" is ticked, the station position. The frames become
  part of its open database (CC BY-SA). Without the box, `uploader_position` is left out of the frames and sent as `null` in the station
  record, as auto_rx does. The contact e-mail is optional, and only the SondeHub admins see it.

### Tests (`tests/sonde/test_sonde_hub.cpp`, ctest `sonde_hub`)

- Names and serials for every type, plus the cases held back (iMet-1/4, an unknown or half-known serial, an unknown type).
- The telemetry JSON. A separate strict JSON checker validates it, and the test checks every mandatory field name, the units (MHz,
  m/s, the heading wrapped to 0-360, hPa), which fields are left out when the sonde did not send them, the uploader position with and
  without the privacy box, and string escaping.
- The rules that hold a frame back: no position, 0,0, out of range, 3 satellites, too high, no time, more than an hour off, no callsign.
- Times: ISO 8601 with microseconds, the RFC 7231 date (including a leap day).
- gzip: the CRC-32 check value 0xCBF43926. The test inflates the output with stb_image's own inflate, checks the trailer's CRC and
  size, and checks that 40 frames compress at least 4 times. An empty input and a one-byte input are also covered.
- The batch: the 15 s interval, 22 s with a DFM, the 30 s cap that never shortens the interval, duplicates, the queue bound.
- The upload thread against a mock HTTP server on localhost, never the real SondeHub. It checks that nothing is sent while off, the
  station record at the start, the gzip PUT and its headers, a 500 retried once, a 400 shown and not retried (the frames counted as
  dropped), nothing after switching off, and the chase-car record repeated.

Not tested against the real SondeHub server: this was built while the laptop was on battery, with CI only. The HTTPS path is the same
one Internet TV uses. The first real upload should be checked on tracker.sondehub.org.

## 2. The radiosonde map

### a. Launch sites: "Site 1" at Bergen

The cause: the sites on the map came only from what the station had learned. `SondeSites::update` (`core/src/sonde/sonde_sites.cpp`)
makes a site from the first low, climbing fix of a sonde and named it `"Site " + id`. The user could rename it with a click in the Sites
view, but OnAir had no list of real launch sites, so every site started as "Site 1", "Site 2" and so on. The map showed nothing else.

4.0:
- There is a built-in list of launch sites, `core/src/sonde/sonde_sites_known.cpp`. It has 885 stations: the stations in NOAA NCEI's
  Integrated Global Radiosonde Archive version 2 station list that have soundings in 2025 or later. That list is a US Government work,
  in the public domain. It is cited in the header, in THIRD_PARTY.md and in the in-app Credits: Durre et al. 2016, doi:10.7289/V5X63K0Q,
  https://www.ncei.noaa.gov/data/integrated-global-radiosonde-archive/doc/igra2-station-list.txt. `tools/gen_sonde_sites.py`
  regenerates it. The names are the archive's names (at most 30 characters, put in title case). Some are truncated or spelled the
  archive's way, for example "Oberschleia Heim (Aerology)" for Oberschleissheim.
- The user's own file is `<data folder>/sonde-sites-known.txt`, one site per line: `name; latitude; longitude[; altitude]`. Commas or
  tabs also work, and `#` starts a comment. Within 15 km a user site is named before a built-in one, and on the map it hides a built-in
  site within 2 km.
- A new learned site takes the name of the nearest known site within 15 km (first fixes are often a few km from the launch site). The
  learned launch clock per site is unchanged. A 3.9 sites file is renamed when it is loaded: a site still called "Site <its id>" takes the
  known name ("Site 1" at Bergen becomes "Bergen"). A name the user gave is kept.
- The map shows every launch site in view. A learned site is a filled triangle with its name, plus its landing heat map. A known site
  OnAir has not heard yet is an outline triangle, with its name from zoom 6. Far out, at most 150 are drawn (400 from zoom 6), thinned
  evenly. A learned site hides the known site it sits on. The Sites view has an "All known launch sites" switch and the count.

### b. My station and the chase car

The map draws the receiver as a small house labelled with the callsign: the SondeHub uploader callsign, or else the APRS-IS one. The
position is the location set in View > Map. The "me / prediction" button on the map sets the position source:
- Location set in View > Map (default),
- GPS (NMEA): a serial GPS (raw 9600 baud on macOS and Linux; Windows `\\.\COM3`) or an NMEA log file, read through the RFI
  mapper's GGA/RMC parser. GGA also gives the altitude,
- gpsd: TCP, port 2947 by default, `?WATCH={"enable":true,"json":true}`, TPV reports (mode 2/3, `altMSL`).

With a fix younger than 10 s, the station is drawn as a chase car (a ringed dot, "<callsign> (GPS)"), so the chase car and the sonde
are on the same map. The same live position is used for SondeHub's station record. "Chase car" in the SondeHub entry sends it every 30 s
as `mobile: true`. Code: `core/include/dect2/sonde/sonde_chase.h`, `app/sonde/sonde_chase_ui.cpp`. Before 4.0, no screen read a GPS
for the receiver position: the RFI mapper had its own NMEA reader, and the GNSS mode only offers its fix once in the first-start helper.

### c. Predicted flight path and landing

OnAir's own landing estimate (the cross at the end of a dashed line while falling, `sonde_land.h`) is unchanged. In the "me /
prediction" popup, "Online flight prediction of the selected sonde" (off by default) asks Tawhiri, the SondeHub predictor at
`https://api.v2.sondehub.org/tawhiri` (API: tawhiri.readthedocs.io, `standard_profile`). It asks at most once a minute for each sonde, and
only for the selected one. The request is built as ChaseMapper builds it:
- climbing: the measured climb rate (at least 0.1 m/s) up to the assumed burst altitude (30 km by default; 100 m above the sonde when it
  is already higher), then the sea-level descent rate set in the popup (5 m/s by default),
- falling: the burst 1 m above the sonde (Tawhiri needs the burst above the start), and the sea-level rate worked out from the measured
  fall with OnAir's atmosphere model: v * exp(-h / 14 km).

The path is drawn dotted in amber, with the predicted burst as a ring and the predicted landing as a cross, each labelled "predicted",
plus the landing time. A line at the bottom of the map says "Prediction (Tawhiri, SondeHub predictor), N s old: a forecast from wind
models, not a measurement", or the predictor's error. The popup shows the last request URL. The request sends the sonde's position,
altitude, rates and time, nothing about the station.

### Tests

- `tests/sonde/test_sonde_sites.cpp` (ctest `sonde_sites`) checks the built-in list (Lindenberg, WMO 10393), and that a launch 2.4 km
  from Bergen is called Bergen. It checks that a site with no known site near keeps "Site 2", that a 3.9 file has its placeholder renamed
  and the user's own name kept, and that the user's file is read (bad lines skipped) and wins.
- `tests/sonde/test_sonde_map_overlay.cpp` (ctest `sonde_map_overlay`) checks the marks:
  - the ten German sites in a box over Germany, nothing outside the box,
  - a learned Bergen hiding the known one,
  - the station with its callsign, the chase car with a live fix, a station outside the view,
  - a view across the 180 degree line (Shemya and a user site at 170 W),
  - the world view thinned to 100 with user sites first, and unthinned.

  It checks the Tawhiri requests for climbing, above the burst, slow, falling (sea-level rate), and without speed, position or time. It
  checks the exact URL (longitude 0-360, RFC 3339). It parses an answer (burst at 359.9 E back to -0.1, the landing point and time) and
  an error answer, and rejects junk. For the chase GPS it checks NMEA GGA with altitude and satellites, RMC, RMC without a fix, a bad
  checksum, and gpsd TPV in 3D and 2D, mode 1 and other reports.

Simulated and offline only: neither the predictor nor gpsd was called during the tests. The map drawing was compiled but not looked at
on screen (the laptop was on battery, no local app build).
