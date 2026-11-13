# OnAir Web protocol, version 1

This is the contract between an OnAir Web server (`onair-server` with `web=1`) and a browser page or any other client. The server side is `server/web/web_proto.cpp` (message validation), `web_hub.cpp` (what the messages do) and `web_server.cpp` (HTTP and WebSocket). `tests/server/test_web_proto.cpp` checks the rules below.

Version rule: the version is the integer `proto` (now `1`) and the WebSocket subprotocol `onair-web.v1`. Within version 1 the server may add new **server** message types, new fields in server messages and new decoder kinds; clients must ignore what they do not know. The server never accepts unknown client fields (a message with one is refused), so a new client field means a new protocol version.

## Endpoints

All on one port (default 8073), HTTP/1.1, `Connection: close` on every plain HTTP answer.

| Method, path | What |
| --- | --- |
| `GET /`, `GET /<file>` | the browser UI (static files below `web_root`; a placeholder page when none is installed) |
| `GET /web/v1/info` | public server information, JSON (below) |
| `GET /web/v1/ws` | the listener's WebSocket |
| `GET`/`POST /web/v1/admin/...` | the admin API (see docs/ONAIR-WEB.md); never needed by a listener |

`/web/v1/info`:

```json
{"proto":1,"name":"OnAir Web","location":"Vilnius","antenna":"discone","users":3,"max_users":8,
 "capture":{"centre_hz":145000000,"rate_hz":2400000},"auth":"open","tls":true}
```

`auth` is `open`, `password` or `invite`. No listener addresses, names or frequencies are ever in it.

## WebSocket

* RFC 6455, version 13. The upgrade request may offer `Sec-WebSocket-Protocol: onair-web.v1` (echoed); another subprotocol is refused with 400.
* A request with an `Origin` header must come from the server's own origin (`http(s)://<Host header>`) or one listed in `web_origin`; otherwise 403. Programs that send no `Origin` are allowed (they still have to authenticate).
* Client to server: **text frames only**, masked, not fragmented, at most **4096 bytes**. A binary frame closes with 1003, a fragmented message with 1003, a bigger one with 1009, an unmasked one or bad reserved bits with 1002.
* Server to client: text frames (JSON) and binary frames (spectrum and audio, below). Never masked.
* Keepalive: the server sends a ping every 15 s; a connection from which nothing (a frame, a pong) has arrived for 45 s is closed with 1001. Browsers answer pings by themselves.
* Rate limit: a token bucket of 20 messages a second with a burst of 40. A message over it gets `{"type":"error","code":"rate_limited"}` and is dropped; the third time within 10 s the connection is closed with 4008.

### Close codes

| Code | Meaning |
| --- | --- |
| 1000 | normal (after `bye` or the client's close) |
| 1001 | keepalive timeout, server stopping |
| 1002 / 1003 / 1009 | protocol error / unsupported frame / too big |
| 4000 | invalid message before `hello` |
| 4001 | authentication failed, no `hello` within 5 s, or another message first |
| 4008 | rate limit |
| 4029 | server busy (full, processor or bandwidth cap), or too many failed logins from the address (wait 60 s) |
| 4040 | kicked (by the admin, the processor or bandwidth budget, a slow connection) |
| 4041 | idle or session time limit reached |

Before a close for a reason the server sends a text message (`error` or `kicked`) that says why.

## Session start

1. The client opens the WebSocket and sends, within 5 s:

   ```json
   {"type":"hello","proto":1,"name":"Kris","password":"...","invite":"..."}
   ```

   `proto` is required and must be 1. `name` (optional, up to 32 characters; markup characters removed) is shown to the host only. `password` for `auth: password`; `invite` (an invite token) works in every mode and makes the session an `invited` one. Both at most 128 characters. A wrong password or invite is answered after 300 ms with `error` (`auth`) and close 4001; five failures from one address within 60 s block that address for 60 s.

2. The server answers `welcome`, or `error` with `code: "busy"` and close 4029:

   ```json
   {"type":"welcome","proto":1,"session":17,"class":"public",
    "server":{"name":"OnAir Web","location":"","antenna":""},
    "capture":{"centre_hz":145000000,"rate_hz":2400000},
    "limits":{"max_bins":2048,"max_fps":15,"max_kbps":512,"idle_min":30,"time_limit_min":0,"msg_per_sec":20,"max_message":4096},
    "audio":{"codecs":["adpcm","pcm16"],"rates":[8000,12000,16000,24000,48000]},
    "demods":["am","nfm","wfm","usb","lsb","cw"],
    "decoders":[{"kind":"host","name":"This server's receiver (its mode's results)","needs_freq":false}]}
   ```

   When the host limits frequencies, the welcome also has a `ranges` object (absent otherwise: older servers and open servers never send it; clients that do not know it work as before, the server still refuses what is not allowed):

   ```json
   "ranges":{"restricted":true,"action":"refuse","guard":{"min_hz":200,"max_hz":5000,"fraction":0.05},
             "allow":[{"lo":144000000,"hi":146000000,"modes":["nfm"],"note":"2 m"}],
             "block":[{"lo":145800000,"hi":146000000}],"notes":["ham_hf: Amateur radio HF bands (typical ...)"]}
   ```

   `allow` is the listener's own class's list (not merged; `modes` missing: all modes; `note` optional); `restricted` false with `block` only means "everywhere except". Edges are inclusive Hz (10 significant digits). The passband plus the guard margin (`fraction` of the bandwidth, between `min_hz` and `max_hz`) must lie wholly inside one merged allowed range and must not touch a block. `action` is what a refused `tune` gets: `refuse` (an `error`) or `clamp` (the server moves the frequency, see `state`).

   `class` is `public`, `member` (password) or `invited`. `demods` and `decoders` list only what the host allows. The session starts tuned to the capture's centre in NFM with audio and spectrum off.

## Client messages

Every message is one JSON object with `type`. Unknown fields, wrong types and values out of range are refused with an `error` (`bad_message`, `out_of_range`) and change nothing. Numbers must be finite; integers must be whole.

| type | fields | notes |
| --- | --- | --- |
| `hello` | `proto` (1, required), `name`, `password`, `invite` | first message only |
| `tune` | `freq_hz` (0 .. 1e11), `mode` (`am` `nfm` `wfm` `usb` `lsb` `cw`), `low_hz` + `high_hz` (together, -150000 .. 150000, at least 50 Hz apart) | at least one of them. `freq_hz` must be inside the capture (centre +- 48 % of the rate). The passband is relative to `freq_hz`; a new `mode` without a passband takes the mode's default |
| `rx` | `squelch_db` (-200 .. 0; <= -150 is open), `agc` (`off` `slow` `fast`), `gain_db` (-20 .. 60, used with AGC off and for FM), `deemph_us` (0, 50, 75; WFM) | |
| `audio` | `on` (bool, required), `codec` (`adpcm` default, `pcm16`), `rate` (8000, 12000 default, 16000, 24000, 48000) | refused when the codec and rate would use more than 80 % of `max_kbps` |
| `spectrum` | `on` (bool, required), `bins` (64 .. 4096, default 1024, at most `max_bins`), `fps` (1 .. 30, default 10, at most `max_fps`), `start_hz` + `stop_hz` (together, absolute, at least 1 kHz) | without a span: the whole capture |
| `subscribe` | `decoder` (an id from `welcome.decoders`), `freq_hz` (required when `needs_freq`) | at most 4 a session |
| `unsubscribe` | `decoder`, `freq_hz` | |
| `ping` | `t` (any number, echoed) | |
| `control` | `take` (bool, required) | take (`true`) or give back (`false`) the control slot when the host set `web_control=slot`. Refused with `busy` while another listener holds it |
| `bye` | | the server closes with 1000 |

Passband limits per mode (Hz relative to the tuned frequency): AM and NFM -10000 .. 10000, WFM -110000 .. 110000, USB -1000 .. 10000, LSB -10000 .. 1000, CW -1500 .. 1500. Defaults: AM +-4500, NFM +-6250, WFM +-100000, USB 100 .. 2800, LSB -2800 .. -100, CW +-250 (heard at a 700 Hz tone).

**Forbidden types.** `tx`, `transmit`, `ptt`, `arm`, `tx_arm`, `carrier`, `key`, `keys`, `import_key`, `key_import`, `voicekeys`, `decrypt`, `record`, `recording`, `save`, `upload`, `exec`, `shell`, `config`, `admin` are answered with `{"type":"error","code":"forbidden"}`. There is no way to transmit, arm a transmitter, load or use keys, decrypt, or write to the server's disk from a web session; the server code cannot even reach the transmit path (see docs/ONAIR-WEB.md, Safety).

## Tuning permission

The welcome carries `control`: `{"type":"control","mode":"everyone|members|slot|nobody","can_tune":bool,"holder":bool,"busy":bool,"left_s":n,"slot_min":n}` (missing on older servers: everyone can tune). A `tune` without the right is refused with `not_allowed` and changes nothing; in slot mode the session keeps its receiver where it was. The server sends a `control` message whenever the slot changes hands or expires. Each listener still has their own receiver: the slot limits who may retune, it is not a shared dial.

## Server text messages

| type | fields |
| --- | --- |
| `welcome` | see above |
| `state` | the session's settings after every change: `freq_hz`, `mode`, `low_hz`, `high_hz`, `squelch_db`, `agc`, `gain_db`, `deemph_us`, `audio{on,codec,rate}`, `spectrum{on,bins,fps,start_hz?,stop_hz?}`, `subscriptions` (keys), `degrade` (0 .. 3: the server is sending fewer or smaller spectrum rows to keep within the session's budget) |
| `ranges` | `ranges`: the same object as `welcome.ranges`; sent when the host changes the frequency ranges while you listen. A tuning that is no longer allowed is silenced and answered with one `forbidden_range` error; the client should tune again. Not sent by servers without ranges |
| `capture` | `centre_hz`, `rate_hz`: the host retuned or changed the rate. Tuned frequencies are kept; a session now outside the capture is silent until it tunes again |
| `event` | `decoder` (the subscription key: `kind` or `kind@<kHz>k`), `data` (the decoder's JSON value, at most 64 KB) |
| `pong` | `t` |
| `error` | `code`, `message` (English text) |
| `kicked` | `reason`: `kicked`, `kicked: <admin's text>`, `idle`, `time_limit`, `cpu_budget`, `bandwidth_budget`, `server_busy`, `slow_client`, `bye` |

Error codes: `bad_json`, `bad_message`, `unknown_type`, `forbidden`, `out_of_range`, `not_allowed` (the host has turned it off), `forbidden_range` (the host limits the frequency ranges and this tuning, passband or decoder is outside them or blocked; the text says why and may name the nearest allowed frequency; the session keeps its previous tuning), `rate_limited`, `busy`, `auth`, `not_running` (a `tune` while the host's receiver delivers no samples: the text says whether it is stopped, being started for you (when tuning is allowed and `web_listener_start` is on, the default) or could not start, with the host's reason; tune again when `capture` comes).

**Frequency ranges and the spectrum.** When the host limits ranges, spectrum rows are masked on the server: bins the listener may not see carry the noise floor of the visible part (so the row is still a normal `0x10` frame; the client needs no change to stay correct, only to look good). A `tune` is checked as a whole (frequency, mode, passband together) and, with any range set, the passband must also lie inside the usable 96 % of the capture. A `subscribe` with `freq_hz` is refused with `forbidden_range` when the decoder's slice is not visible; the `host` decoder is silent unless the whole capture is visible. This is an additive change: `proto` stays 1.

The `host` decoder's `data` is `{"mode":"<the server's mode id>","telemetry":{...}}` with the receiver's telemetry (the same JSON as `/api/v1/telemetry` of the remote-control API). It is not sent while the server runs a digital voice or pager mode the host has not enabled for web listeners.

## Decoder events (schema version 1)

Besides `host`, the server can offer these decoders (`welcome.decoders` lists those the host has enabled; all need `freq_hz`). One instance serves everyone who subscribes to the same kind on the same kHz; the key in `event.decoder` is `kind@<kHz>k` (for example `adsb@1090000k`). The instance gets its own slice of the capture at the rate it needs (`rate` below), so the frequency must be inside the capture and inside the decoder's band (otherwise `subscribe` is answered with `out_of_range` or `busy`).

| kind | frequency | slice rate | result |
| --- | --- | --- | --- |
| `adsb` | 1087 .. 1093 MHz | 2 Msps | aircraft list with positions |
| `ais` | 161.9 .. 162.1 MHz (both channels 161.975 / 162.025 are decoded from a slice centred on 162.000) | 250 ksps | vessel list |
| `aprs` | 29 .. 470 MHz (1200 and 9600 baud on the channel) | 192 ksps | stations and frames |
| `ft8`, `ft4`, `wspr` | 0.1 .. 450 MHz, the dial frequency (USB) | 250 ksps | decodes |
| `sstv` | 0.1 .. 450 MHz (USB below 30 MHz, narrow FM above) | 250 ksps | pictures |
| `wefax` | 0.1 .. 30 MHz, the dial frequency (USB) | 12 ksps | picture |
| `meteor` | 136 .. 138.5 MHz (Meteor-M LRPT) | 288 ksps | picture of one channel |
| `rds` | 64 .. 110 MHz (the station's carrier) | 500 ksps | RDS text |
| `cw` | 0.1 .. 450 MHz, skimmer around the dial frequency | 250 ksps | signals with text |
| `rtty` | 0.1 .. 450 MHz, the dial frequency | 250 ksps | text |

Digital voice and pager decoders are not part of this list: they stay off for web listeners unless the host accepted the legal notice (see docs/ONAIR-WEB.md, Safety). NOAA APT is not offered (the program has no APT decoder).

Every `event.data` is one JSON object with `v` (1), `kind` and `type`; unknown fields must be ignored. Tables are complete snapshots (the client replaces what it had), at most once a second; a listener who subscribes late gets the current state at once. Numbers are plain JSON numbers; text is valid UTF-8 with control characters removed. An event is at most 64 KB; a longer table is cut (newest first).

| `type` | kinds | fields |
| --- | --- | --- |
| `aircraft` | adsb | `state`, `msgs_per_s`, `snr_db`, `noise_dbfs`, `messages_ok`, `messages_bad`, `aircraft_count`, `with_position`, `aircraft[]`: `icao` (hex), `callsign`, `category`, `alt_ft`, `speed_kt`, `track_deg`, `vrate_fpm`, `lat`, `lon`, `squawk`, `ground`, `emergency`, `msgs`, `age_s`, `level_dbfs`, `trail` (up to 8 `[lat, lon]`); fields the aircraft has not sent are left out |
| `vessels` | ais | `state`, `snr_db`, `messages_ok`, `messages_bad`, `vessel_count`, `vessels[]`: `mmsi`, `class`, `name`, `callsign`, `ship_type`, `ship_type_text`, `nav_status`, `nav_status_text`, `lat`, `lon`, `sog_kt`, `cog_deg`, `heading_deg`, `destination`, `imo`, `msgs`, `age_s` |
| `packets` | aprs | `state`, `snr_db`, `frames_ok`, `frames_bad`, `frames_1200`, `frames_9600`, `frames[]` (the newest 30, newest last: `t`, `from`, `to`, `path`, `type`, `summary`, `baud`, `lat`, `lon`), `stations[]`: `call`, `symbol`, `last_heard_s`, `count`, `lat`, `lon`, `alt_m`, `course_deg`, `speed_kt`, `comment`, `object` |
| `decodes` | ft8, ft4, wspr | `dial_hz`, `clock_err_s`, `clock_n`, `decodes[]` (the decodes since the last event, oldest first; a late subscriber gets the last 40 as one event): `slot_utc` (seconds since 1970), `snr_db`, `dt_s`, `audio_hz`, `rf_hz`, `msg`, `cq`, `call`, `grid`, `dbm` (WSPR) |
| `rds` | rds | `state`, `carrier`, `stereo`, `snr_db`, `level_dbfs`, `sync`, `block_ok_pct`, `groups`, `pi`, `ps`, `radiotext`, `radiotext_complete`, `pty`, `callsign`, `tp`, `ta`, `music`, `rbds` |
| `signals` | cw | `state`, `snr_db`, `window_hz`, `chars`, `signals[]`: `id`, `offset_hz`, `wpm`, `snr_db`, `locked`, `chars`, `text` (last 600 characters), `calls` |
| `text` | rtty | `state`, `mark_hz`, `space_hz`, `baud`, `shift_hz`, `quality`, `chars`, `text` (last 2500 characters) |
| `status` | sstv, wefax, meteor, any | free fields (`state`, `mode`, `lines`, ...); `state: "no_data"` with `why` when no samples reach the decoder for 4 s (the capture is too narrow for its rate) |
| `image` | sstv, wefax, meteor | a strip of a picture, see below |
| `image_end` | sstv, wefax, meteor | `id`, `rows`: the picture with this `id` is complete (sent only when the last strip did not already carry `final`) |

**Pictures.** Nothing is stored on the server for a listener: rows go out as they are decoded, each row once, in strips. An `image` event has `id` (one number per picture; a new `id` starts a new picture), `label` (SSTV mode, fax IOC / LPM, ...), `w`, `h` (the canvas, `h` is the most the picture can have), `ch` (1 grey, 3 RGB), `y`, `rows` (the strip covers canvas rows `y .. y + rows - 1`), `png` (base64 of a PNG that is `pw` x `ph`), `preview`, `scale`, `final`. A normal strip is `w` x `rows` pixels, 8 bit grey or RGB, in a PNG with stored (uncompressed) deflate blocks. A listener who joins in the middle first gets one `preview` strip: rows `0 .. rows - 1` shrunk by the integer `scale` (so the PNG is `pw` x `ph`), which the client draws stretched over `w` x `rows`; strips that follow replace it with the real pixels. Rows the decoder never received (Meteor packets lost) are not sent and stay empty. Limits of this version: a row is never re-sent, so a fax picture whose slant correction changes later keeps the rows it already sent as they were; Meteor sends the channel (MSU-MR channel 1 .. 6) that has the most packets, chosen when the first strips arrive, and a strip is sent when all its packets arrived or three later strips exist.

The `host` decoder is unchanged. A decoder on a frequency the capture no longer covers (the host retuned) stays subscribed and sends nothing until the capture covers it again.

## Binary frames

Little endian. The first byte says what it is.

**Spectrum row** (`0x10`), 32-byte header:

| Offset | Type | |
| --- | --- | --- |
| 0 | u8 | `0x10` |
| 1 | u8 | format version, 1 |
| 2 | u16 | bins (n) |
| 4 | u32 | row sequence number (per session) |
| 8 | f64 | start Hz (absolute, left edge of bin 0) |
| 16 | f64 | stop Hz (right edge of the last bin) |
| 24 | f32 | floor dBFS (value 0) |
| 28 | f32 | top dBFS (value 255) |
| 32 | u8 x n | levels, linear in dB between floor and top |

Rows are paced by the samples (the requested `fps` of signal time), each bin keeps the peak of the finer bins it covers, and the floor follows the noise slowly (about 10 dB under the quietest fifth). A listener whose connection cannot keep up gets fewer rows, never old ones: at most two rows wait in its queue.

**Audio** (`0x20`), 24-byte header, mono:

| Offset | Type | |
| --- | --- | --- |
| 0 | u8 | `0x20` |
| 1 | u8 | codec: 0 PCM 16-bit, 1 IMA ADPCM |
| 2 | u16 | samples in this frame (n) |
| 4 | u32 | frame sequence number (per session) |
| 8 | u32 | sample rate, Hz |
| 12 | f32 | channel level, dBFS (S-meter) |
| 16 | u8 | flags: bit 0 = squelched (no payload, n samples of silence) |
| 17 | u8 | 0 |
| 18 | i16 | ADPCM predictor at the start of the frame |
| 20 | u8 | ADPCM step index at the start of the frame (0 .. 88) |
| 21 | u8 | 0 |
| 22 | u16 | 0 |
| 24 | ... | payload: PCM, n x i16; ADPCM, ceil(n / 2) bytes, low nibble first |

Frames are 40 ms. Every ADPCM frame carries its decoder state, so a lost or dropped frame never breaks the next one. The ADPCM is the standard IMA step table (the same as WAV `0x11` / DVI). A listener that falls behind loses the oldest audio first (after its spectrum rows).

No patented codec is used. Opus is not in OnAir's dependencies; if it is added later it gets a new codec number and is offered only when the server has it.

## Limits

| | |
| --- | --- |
| client message | 4096 bytes, 20 a second (burst 40) |
| JSON | depth 4, 96 values, strings 1024 bytes, valid UTF-8, no NUL, no repeated keys |
| HTTP request head | 8 KB, 48 header fields, 5 s to arrive |
| HTTP body (admin) | 4 KB, `Content-Type: application/json` |
| connections | `max(16, 3 x web_max_users + 8)` in total, 12 per address |
| sessions | `web_max_users` (8), `web_max_per_ip` (2) |
| decoders | `web_max_decoders` shared instances (8), 4 subscriptions a session |
| outgoing queue | 384 KB a session: spectrum rows dropped first, then the oldest audio; control messages and events are never dropped (a client that does not read at all is closed with `slow_client`) |

## Example (JavaScript)

```js
const ws = new WebSocket(`wss://${location.host}/web/v1/ws`, "onair-web.v1");
ws.binaryType = "arraybuffer";
ws.onopen = () => ws.send(JSON.stringify({type: "hello", proto: 1, name: "me"}));
ws.onmessage = (e) => {
  if (typeof e.data === "string") {
    const m = JSON.parse(e.data);
    if (m.type === "welcome") {
      ws.send(JSON.stringify({type: "tune", freq_hz: 145500000, mode: "nfm"}));
      ws.send(JSON.stringify({type: "audio", on: true, codec: "adpcm", rate: 12000}));
      ws.send(JSON.stringify({type: "spectrum", on: true, bins: 1024, fps: 10}));
    }
    return;
  }
  const v = new DataView(e.data);
  if (v.getUint8(0) === 0x10) { /* spectrum: v.getUint16(2, true) bins from offset 32 */ }
  if (v.getUint8(0) === 0x20) { /* audio: decode, play through an AudioWorklet */ }
};
```
