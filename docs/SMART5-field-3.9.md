# Smart features, round 5 (3.9): field insights for aviation, marine and IoT

Branch `feat39-smart5d`. These eight items were left between the smart5b and smart5c lists. They use the insight hub of
smart5b (`core/include/dect2/insight`, View > Insights, automation events `insight.<kind>`).

**Simulated data only.** No real recording of these situations was used.

The analysers are pure headers with no clock. `core/include/dect2/insight/field.h` joins them to the hub:

- ADS-B, AIS and NAVTEX come in through the automation events the decoders already send (`adsb.aircraft`, `ais.vessel`,
  `msg.navtex`).
- FLARM, HFDL, LoRa mesh, LoRaWAN and the water meter come from the receivers' telemetry, once a second
  (`app/panels/insight_ui.cpp`).

Findings appear under Insights > Findings and go out as `insight.<kind>` events. The Insights window has three new tabs:
Aviation, Marine and IoT. The HFDL map, the NAVTEX warnings and the meter history are kept between runs in the data folder
(`insight_hfdl.txt`, `insight_navtex.txt`, `insight_meter_*.txt`).

Privacy: everything stays on the computer. Positions are only what transmitters broadcast to everyone. The mesh census
keeps node IDs and roles but no positions. The leak check follows only meters the user marks with "My water meter" in
Wireless M-Bus > Meters. Nothing is decrypted.

| # | Item | Where | What was measured |
|---|---|---|---|
| 1 | ADS-B manoeuvre alerts | `core/include/dect2/adsb/adsb_manoeuvre.h`, Insights > Findings | Simulated 2 s updates, 0 to 40 % lost, ±64 fpm rate noise and 0.2 deg heading noise, 25 ft altitude steps, 5 seeds. Squawk 7700 is reported once. -6500 fpm gives a warning and -9000 fpm an alarm. -7000 fpm is also found from the altitude alone. -4000 fpm is not reported. +8000 fpm counts as a climb. A 7 deg/s turn at 300 kt is reported; a rate-one turn is not. A go-around from about 700 ft is found. So is a touch-and-go (20 s on the runway). A full-stop landing followed 5 min later by a departure is neither. A plain departure gives nothing. |
| 2 | Glider thermals map | `core/include/dect2/flarm/flarm_thermals.h`, Insights > Aviation, circles on the FLARM map | Simulated circling: 70 - 100 m radius, 24 - 28 s per turn, 1.2 - 2.2 m/s climb, 4 m/s wind drift, 5 - 11 m GNSS noise, 0 to 30 % lost packets, 4 seeds. One thermal per climb. The centre is within 150 m after the drift, the climb within ±0.4 m/s. A second glider joins the same thermal. Sinking circles and straight glides make none. A climb 4 km away is a second thermal. |
| 3 | HFDL propagation map | `core/include/dect2/hfdl/hfdl_prop.h`, Insights > Aviation (station × UTC hour grid) | 3 simulated days of squitters, one per 32 s frame per frequency, at hourly probabilities of 0.5 - 0.9; one run with the receiver off at 13 UTC. Rates come within ±0.12 of the model. The best frequency per station and hour is right, and so is the best frequency overall. Hours that were not listened to give no rate. A station heard again after 6 h counts as "station up" (2 and 3 times as expected). Save and load keep the map. |
| 4 | AIS ducting detector | `core/include/dect2/ais/ais_ducting.h`, Insights > Marine | 2 simulated days, 60 ships 3 - 40 km away, 3 seeds. The learned normal range is 33 - 42 km (radio horizon 33.6 km for 10 m and 25 m antennas). A 2 h duct with 8 ships at 120 - 260 km starts one event in its first 10 min, pointing west. The event ends 30 - 60 min after the duct. Left out: a SAR aircraft at 220 km, a base station at 150 km, a 0/0 position, a 5000 km position, channel 75 reports, and one far ship now and then. |
| 5 | NAVTEX warnings in force | `core/include/dect2/marine/marine_navtex_force.h`, Insights > Marine | Test texts written in the forms used on the air: UK `WZ nnnn`, series `123/26`, NAVTEX headers, `CANCELS ... AND ...`, `CANCEL THIS MSG DDHHMM UTC MON YY` (also with `Z` and 4-digit years), and in-force lists. Cancellations and self-expiry work. An in-force list drops warnings it does not name. Repeats with damaged characters are kept as one warning, and damaged headers are ignored. Other stations are not touched. A gale warning ages out after 24 h. Save and load keep the set. |
| 6 | LoRa / Meshtastic census | `core/include/dect2/mesh/mesh_census.h`, Insights > IoT | Airtime matches the Semtech calculator: SF7 41.216 ms and SF12 991.232 ms for 10 bytes. 2 simulated days of 20 nodes (3 routers), 1 - 3 relayed copies, 5 % CRC failures, an evening peak and one flood hour, 3 seeds. Every node and its fewest hops is found. Each packet is counted once. Airtime is the sum of all copies. The flood hour and two evening hours are the busiest. One "channel busy" finding (above 25 %); one finding per router. |
| 7 | LoRaWAN health | `core/include/dect2/lorawan/lorawan_health.h`, Insights > IoT | 2 simulated days of 30 devices (5 % loss, 2 % CRC failures), 3 seeds. The device with 50 % loss measures 50 ± 8 % and is reported once. A 16-bit counter wrap is not counted as loss. A reset device counts as one restart. NbTrans 2 repeats are counted as repeats. One device that cannot join gives one join storm. One hour with 14 requests and no accept gives one "no accepts" finding. The move to SF12 in the last 4 h gives one SF shift. |
| 8 | wM-Bus leak check | `core/include/dect2/wmbus/wmbus_leak.h`, Wireless M-Bus > Meters ("My water meter"), Insights > IoT | 10 simulated days of a household meter: 1 L steps, a reading every 2 min with 15 % lost, UTC+2, 3 seeds. Day use comes from flushes, taps and showers. There are no findings before a 3 L/h leak starts on day 6. Night flow before it is under 0.6 L/h and after it 3 ± 0.8 L/h. Night findings start on the first leak night and become warnings on the second. Continuous-flow findings come after 24 h and become alarms at 72 h. A meter heard only every 6 h gives no continuous-flow finding. A replaced meter counts as a reset. |

The hub test (`tests/marine/test_marine_field_hub.cpp`) sends the decoders' automation events through `insight::Hub`. It
checks the emergency, descent, NAVTEX, ducting and join-storm findings and their `insight.*` events.

## Notes and limits

- **Manoeuvres:** the thresholds are fixed and simple (5000 / 8000 fpm, 6000 fpm, 5 deg/s above 140 kt). They are not
  aircraft-type aware: fighter jets will raise climb and turn findings. These have severity 0. A go-around is reported
  without an airport check unless a nearest-airfield function is given; the app does not set one yet.
- **Thermals:** circling is found from the positions, so packets must come about once a second or more. The altitude
  reference is whatever the aircraft sends (FLARM: WGS84 height), so base and top are not heights above the ground.
- **HFDL:** the rate assumes one squitter per 32 s frame on every frequency in use. A frequency a station has switched
  off therefore reads as "not heard", which is also true for the user. The map is per UTC hour, merged over all days (no
  season split).
- **AIS ducting:** the normal range is learned from this station's own traffic. It falls back to the radio horizon until
  200 samples exist. The antenna height is fixed at 10 m for now; it is not a setting.
- **NAVTEX:** the lifetimes for warnings with no end are assumptions (B 24 h, D 72 h, others 60 days). Findings from
  those say "assumed". In-force lists come in many layouts. Only numbers after a `YYYY SERIES` word or in `n/yy` form are
  read, and only warnings with a reference from a listed year are dropped.
- **Mesh:** the census is for the current session (not kept between runs). Roles come from the node table (NodeInfo).
- **LoRaWAN:** downlinks are encrypted and carry no DevEUI, so join accepts cannot be matched to requests; they are
  counted per hour. The "no accepts" finding can also mean this receiver does not hear the gateway's downlinks.
- **Leak check:** a meter whose register moves in 10 L steps hides a slow drip from the continuous-flow test. The night
  flow still shows it over several nights. Encrypted meters need the user's own key in the wM-Bus mode first.

No third-party code. The LoRa airtime formula is from Semtech AN1200.13 / the SX1276 data sheet (section 4.1.1.7). The
radio horizon is the standard 4/3-earth formula. Squitter timing is from ARINC 635. NAVTEX message forms follow
ITU-R M.540 and the IMO NAVTEX Manual (the texts were written from memory of real broadcasts).
