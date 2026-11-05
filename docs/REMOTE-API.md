# Remote control API

A small HTTP/1.1 + WebSocket server so scripts and other programs can control OnAir. Version 1, all paths under `/api/v1/`.
Settings: View > Data feeds > Remote control API. It is off until you turn it on.

## Safety rules

- Listens on `127.0.0.1` only. "Let other computers connect" (LAN) is an explicit switch and needs a token.
- Token: generated on first use, stored in the preferences, never written to the log. Send `Authorization: Bearer <token>` (or `?token=` for WebSocket
  clients that cannot set headers). Compared in constant time. "Programs on this computer need the token too" is on by default.
- No CORS headers are ever sent; a request with a foreign `Origin` or a non-loopback `Host` on a loopback connection is refused (403).
- Rate limit 20 requests/s per address (burst 40), then `429` with `Retry-After`. Header block at most 8 KB, body at most 4 KB, path at most 512 bytes,
  16 connections (8 WebSockets), 5 s to send a request. POST needs `Content-Type: application/json` and a flat JSON object.
- The API can transmit (3.2): `GET /api/v1/tx` (state), `POST /api/v1/tx/start` (body: `mode`, `mhz` or `hz`, `gain_db`, `amp`, `loop`, `seconds`, `test`, `callsign`, `grid`, `dest`, `name`, `text`, `device`, `carrier`) and `POST /api/v1/tx/stop`; rigctl `T 1` starts the transmission set up in the Transmit window, `T 0` stops it, `t` reads it. The explicit command is your decision: the answer carries a `warnings` list (licence, band, gain, amplifier, hardware). You are responsible for the law and for your hardware; overdriving or a long duty cycle can damage the radio. The one-time transmit notice must have been accepted in the app. The map guard applies: nothing is uploaded to the map server while transmitting and for 10 minutes after.
- The API has no way to read or write voice keys or credentials, or change security settings. A path naming such a thing
  (key, credential, password, secret, token, security, settings, config, ...) is refused with 403 whatever the program offers.
- Errors are `{"error":"..."}` with the HTTP status. `501`: this program does not offer that endpoint.

## Endpoints

GET (no body):

| Path | Answer |
|---|---|
| `/api/v1` | name, version, the endpoint list |
| `/status` | running, device, mode, frequency, rate, gain |
| `/devices` | the radios and sources |
| `/modes` | modes (`family`, `name`, `sub`) that `/mode` accepts |
| `/telemetry` | the active mode's telemetry |
| `/spectrum?bins=N` | latest spectrum line, 16 to 4096 bins (dBFS, strongest bin of each group) |
| `/memories` | memory list, read only: id, name, freqHz, mode, group, tags |
| `/scheduler` | the TV recorder's scheduled jobs, read only |
| `/audio` | `{"volume":0..1,"muted":bool}` |
| `/recording` | IQ and audio recording state (file name only) |
| `/events?since=N&max=N` | decoded messages since number N (omit `since`: from now); answer `{"gap":bool,"next":N,"events":[...]}` |
| `/rotator` | antenna rotator state |
| `/ws` | WebSocket, see below |

POST (JSON body, answer `{"ok":true}`):

| Path | Body |
|---|---|
| `/start` | `{"device":2}` (optional index) |
| `/stop` | `{}` |
| `/mode` | `{"family":N,"sub":"id"}` (receiver must be stopped) |
| `/frequency` | `{"hz":162550000}` or `{"mhz":162.55}` |
| `/samplerate` | `{"hz":2400000}` or `{"msps":2.4}`; 0 = automatic |
| `/gain` | `{"db":30}` (also turns the AGC off) |
| `/autogain` | `{"on":true}` |
| `/bandwidth` | `{"hz":12500}` or `{"khz":12.5}` (not the General receiver) |
| `/squelch` | `{"on":true,"auto":false,"db":-60}` (General receiver) |
| `/volume` | `{"level":0.5}` |
| `/mute` | `{"on":true}` |
| `/recording/start` | `{"kind":"iq"}` or `{"kind":"audio"}` (General receiver) |
| `/recording/stop` | `{}` |
| `/rotator/goto` `/stop` `/park` | `{"az":120,"el":30}`, `{}`, `{}` |

Waterfall: poll `/spectrum` or use the WebSocket spectrum stream (`fmt:"u8"` gives bytes); a client builds the waterfall from the lines.

## WebSocket `/api/v1/ws`

Send flat JSON to choose what you get: `{"telemetry":true,"spectrum":true,"bins":512,"fps":5,"fmt":"u8","events":true,"audio":true,"arate":16000}`.
Frames: `{"type":"telemetry","data":{..}}`, `{"type":"spectrum","data":{..}}`, `{"type":"event","data":{<message line>}}`, `{"type":"gap"}` (messages were
dropped because you were too slow), `{"type":"error",...}`.

## Decoded-message line

One JSON object per message (WebSocket, `/events`, UDP):

```
{"v":1,"type":"pager.message","time":1760000000.5,"mode":"POCSAG","freqHz":153350000,"fields":{"text":"hello","address":1234}}
```

`type` is the automation event type (see `docs/automations.md`); `fields` depends on the type. A line over 1399 bytes is sent without `fields` and with
`"truncated":true`. The ring behind `/events` keeps the last 512 messages.

## UDP output

Settings: "Send decoded messages as UDP JSON lines". One line per datagram (newline ended, at most 1400 bytes) to the dotted IPv4 address and port you
give; `255.255.255.255` broadcasts. Off by default; nothing is sent anywhere else.

## rigctl (Hamlib rigctld subset)

Settings: "rigctl server", default port 4532, `127.0.0.1` only unless "let other computers connect" (the protocol has no login). Commands: `f`/`F`
frequency, `m`/`M` mode (General receiver: FM WFM AM USB LSB CW), `t`/`T` (always receiving; `T 1` is refused with `RPRT -11`), `s`/`S` (split off), `l STRENGTH`,
`v`, `q`. Unknown: `RPRT -4`. Use it as a "Hamlib rigctld" radio in gpredict for Doppler tuning (`F`). 4 connections, 256 byte lines.

## Examples

`docs/examples/remote-api/` has curl scripts and a Python client. Quick check:

```
TOKEN=...   # View > Data feeds > Remote control API > Copy token
curl -s -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8765/api/v1/status
curl -s -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' -d '{"mhz":162.55}' http://127.0.0.1:8765/api/v1/frequency
echo "f" | nc 127.0.0.1 4532
```

## Live RDS (FM transmitter)
`GET` / `POST /api/v1/x/rds` change the station name, radio text, PI, PTY and flags while the FM transmitter is on the air. See [RDS-LIVE.md](RDS-LIVE.md).
