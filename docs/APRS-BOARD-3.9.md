# APRS weather stations and messages (3.9)

Round-4 items 1 and 2 (scratchpad r4-per-mode list). Branch feat39-r4.

## Weather
- The receiver keeps the weather values it already decoded (APRS101 chapter 12) on each station: temperature, wind direction, wind and gust, rain for 1 h, 24 h and since midnight, humidity and pressure. Values are converted to metric: °C, km/h, mm and hPa.
- A report that leaves out a value keeps the station's previous value for up to 2 h in the table. The graph shows a gap instead.
- History covers the last 24 h with at most one point per 5 minutes. A newer report within 5 minutes replaces the point. A delayed copy is put in time order.
- Impossible values are dropped: temperature outside -90 to 65 °C, humidity outside 0-100 %, pressure outside 800-1100 hPa.
- **APRS > Weather** has a table of the weather stations with distance from the receiver, CSV/JSON/KML export and a 24 h graph of the selected station. The graph shows temperature, wind and gust, pressure, humidity or rain.
- On the map, a weather station's label shows temperature and wind (`LY4WX  12.3 °C  SW 6 km/h`). The station card shows every value.
- Whole-span decoding merges the weather of every carrier.

## Messages
- Messages, acks and rejects are kept as chapter 14 describes. The addressee sets the kind:
  - `BLN0`-`BLN9`: bulletins
  - `BLNA`-`BLNZ`: announcements
  - `BLN<n><group>`: group bulletins
  - `NWS-`, `SKY`, `CWA`, `BOM`: weather service bulletins
- Retries count as copies of one message: within 1 h for messages, 12 h for bulletins. An ack or reject marks the matching message. The ack must come from the addressee, back to the sender, with the same number.
- Telemetry definitions (`PARM.`, `UNIT.`, `EQNS.`, `BITS.`) are not listed.
- **APRS > Messages** has:
  - **Conversations**: one entry per pair of stations, with acknowledged, rejected or "no ack heard" on each message.
  - **Bulletins**: a separate list.
  - **My callsign**: without an SSID, every SSID of that call counts. When empty, the APRS-IS login callsign is used.
  - **Only mine**: shows only your conversations.
  - Export of every message.
- Messages to you are highlighted, and the tab label counts the ones not opened yet.
- **Alert**: the new event `aprs.message` has the fields from, to, text, msgno, kind and to_me. It fires once per message (not for retries, acks or rejects). The built-in automation **APRS message to me** sends a notification and a sound. It is off by default, like every built-in rule.

## Tests
`tests/packet/test_packet_aprsboard.cpp` checks:
- addressee kinds and callsign matching
- retries, acks, rejects, acks for another number or from another station
- telemetry definitions, the late-retry window, setting the callsign later, the list cap
- unit conversions, merging and expiry, the 24 h window (26 h of reports every 60 s), out-of-order reports
- a round trip: the generator's cycle through the real receiver, clean, at ±8-10 kHz offset, +80 ppm clock, and -8 kHz with -80 ppm and an echo, each at 25 and 18 dB SNR. The weather station must show the exact values and the message must show as acknowledged and "to me".

Simulated signals only: no real recording of an APRS weather station or message exchange was used.
