# Integrations: MQTT, Home Assistant, webhook, JSON lines

View > Integrations. Send what OnAir decodes to other programs. Everything is off until you tick a sink and type its address. There are no default servers, nothing is sent while a sink is off or its address is empty, and nothing goes to the OnAir map from here (the map guard that holds uploads while and after you transmit is untouched).

## MQTT

* Broker address, port (1883; 8883 with TLS), optional user and password, optional client id (random when empty).
* QoS 0 (at most once) or 1 (at least once: the hub waits for the PUBACK, reconnects after a drop and tries once more). Retain is off by default.
* TLS verifies the broker certificate against the system store. A build without TLS refuses to connect rather than fall back to plain MQTT; use a TLS proxy or port 1883 on a trusted network.
* The password is saved in `integrations/integrations.json` in the data folder (readable by you only, like the watchlist tokens). It is not written to a log, the status text, a message or a backup without the passphrase block.
* Connection failures retry with a back-off from 2 s to 60 s; events wait in a queue of 2000 (the oldest is dropped and counted).

### Topic template

Default: `onair/{family}/{sub}/{ident}`, for example `onair/adsb/aircraft/4840D6`.

| Placeholder | Value |
|---|---|
| `{family}` | `adsb`, `ais`, `aprs`, `pager`, `acars`, `vdl2`, `hfdl`, `sonde`, `lora`, `voice` ... |
| `{type}` | the whole event type, `adsb.aircraft` |
| `{sub}` | the part after the dot, `aircraft` |
| `{mode}` | the mode name (ADS-B, POCSAG ...) |
| `{ident}` | the best identifier: ICAO hex, MMSI, call sign, id, serial, address, source ... |
| `{freq_mhz}` | the frequency in MHz |
| `{field:NAME}` | any field of the event |

Every substituted value is made safe for a topic level: `/ + #` and control characters become `_`, spaces become `_`, an empty value becomes `_`. A template with wildcards, a leading `/` or an unknown placeholder is refused and shown in red.

### Payload

One JSON object per event:

```json
{"source":"OnAir","type":"adsb.aircraft","mode":"ADS-B","freq_hz":1090000000,"time":1792232100,"ident":"4840D6","fields":{"callsign":"KLM1023","alt_ft":30000}}
```

### Home Assistant

Tick "Home Assistant discovery". On each (re)connect OnAir publishes, retained, `<state base>/status = online` (the last will sets it to `offline`) and the config of five sensors under `homeassistant/sensor/onair_<key>/config`: last aircraft, vessel, APRS station, pager message and radiosonde. The state topics are `<state base>/ha/<key>` (default `onair/ha/adsb` ...) with the event JSON as payload and as attributes. The prefix and the state base are editable.

## Webhook

One POST per event (the JSON above, `Content-Type: application/json`) to the http(s) address you give, with an optional `Authorization: Bearer` token. Different from the watchlist webhook, which posts only matches of your lists in the watchlist's own shape.

## JSON-lines file

The same JSON, one object per line, appended. When the file passes the size you set (default 100 MB) it is renamed to `<file>.1` (the previous `.1` is replaced) and a new one starts. 0 = never rotate.

## What goes out

The "What to send" tab ticks the families. The default is the decoded-message families (aircraft, ACARS, VDL2, HFDL, pagers, AIS, DSC, APRS, sondes, GNSS, LoRa and mesh, Bluetooth, Remote ID, signals). Not ticked by default: digital voice call records (who, which talkgroup, never audio), plug-in events, finished sessions, satellite passes, time signals and receiver status. A limit of events per second (default 100; 0 = none) protects the broker; what goes over it is dropped and counted on the Status tab. Encrypted traffic is never decoded, so nothing of it is published.

## Where it lives in the code

`core/src/integrations/mqtt.cpp` (packet encoder/decoder, the client), `core/src/integrations/integrations.cpp` (topic templates, event JSON, Home Assistant, settings file, the Hub with its worker thread), `app/integrations/integrations_ui.cpp` (the window). Events arrive through a fifth hook of the automation engine (`Engine::setPublisher`), beside the tap (message database), observer (plug-ins), watcher (watchlists) and streamer (remote API). Tests: `tests/integrations/test_integrations.cpp` (packet bytes, topics, JSON, settings, an in-process fake broker for login, QoS 0 and 1, retain, reconnect, Home Assistant, the sinks, the rate limit and "nothing sent unless switched on").

## Not covered yet

Mesh, SARSAT and other decoders that do not raise an automation event yet are not published (they appear as soon as they emit one). The headless server (`server/`) has no Integrations settings yet. Subscribing (commands from MQTT) is not part of this.
