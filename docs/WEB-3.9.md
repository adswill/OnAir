# OnAir Web 3.9: the whole receiver in a browser

OnAir Web is OnAir itself, used from a browser: every mode of the mode list (290 of the 295 rows have a live panel, the rest are explained
below), the spectrum and waterfall with drag-to-tune, the mode picker with search, the radio controls, recording, audio, and a settings page
in plain words. It runs on the headless server (`onair-server`, docs/headless-server.md) and is reachable on your own network, from
anywhere with a login, or publicly if you choose, through the relay or a forwarded port. docs/WEB-SCHEMA-3.9.md is the developer side.

What it replaces: the 3.8 "OnAir Web" page (six demodulators and twelve decoders, every listener with an own receiver) is still there at
`/index.html` for many-listener use; the new page is at `/app/` and `/` opens it.

## Quick start

```
onair-server                                      # the browser app is on, http://127.0.0.1:8073/app/
onair-server --app-password "a long password"     # sets the owner's password (stored hashed), then runs as usual
```

The first time you open the page from the same computer or your own network you are the owner and are asked to set a password (or press
Later). Settings > Server > "Reachable from other computers" lets the rest of your network in; "Relay" gives the server a public https
address without touching your router (docs/ONAIR-WEB-RELAY.md, docs/CF-RELAY-3.8.md).

The page: the frequency at the top (click it to type one: `100.5`, `145800k`, `7074000` all work), **Mode** (search by name, band or what it
decodes: "aircraft", "weather", "ft8"), **Start**, **Listen**, volume, **Record** and **Settings**. Below: spectrum and waterfall (drag or
click to tune, wheel or arrow keys to step, Shift for ten steps), the decoder panel with **Advanced** for every figure the receiver reports,
and the Radio card (radio, gain, Listen demodulator, audio rate, sample rate and capture bandwidth under Advanced). On a phone the three
parts become tabs. Keys: M mode, S start / stop, L listen, F frequency, A advanced.

## Every mode

The decoder panel is not written per mode: the server exports the receiver's own report (the same struct the desktop panels read) and a
schema says what each field is, so one set of components shows all modes: figures with units, yes / no, text, tables (sortable; a **Map**
button when rows carry positions, drawn on a grid with no internet tiles), pictures (SSTV, fax, weather satellites, KAWS), constellations,
text logs and sparklines. Simple shows the signal block and the first ten figures of the mode; Advanced shows everything.

Coverage per mode row of the mode table (app/ui/mode_table.cpp, 295 rows):

| rows | what the panel shows |
|---|---|
| 283 | the mode's own block, generated from its telemetry struct (tests/web/test_web_schema.cpp checks each one) |
| 7 TV / broadcast families (DVB, TV auto, ATSC, ATSC 3.0, ISDB-T, DAB, FM) | the common signal block and the standard's hand-written details (lock, FEC, MER, services ...) |
| Internet TV (IPTV), ASTERIX, ISDB-TSB | the common block only: their state is not in the telemetry report (IPTV plays a stream; ASTERIX reports through the sessions API; ISDB-TSB is new in 3.9 and has no telemetry block yet) |
| Mode Builder, Plugins | not receivers; a plugin mode shows the common block |

Every one of the 290 receiver rows can be selected through the page; tests/server/test_webapp.cpp selects each and checks the server
reports it back. What the panel cannot do yet: TV video and the sound of the digital voice modes are not streamed (the server's audio is
the Listen demodulator, as in 3.8); the hand-drawn desktop plots (channel response, impulse response) appear as lists.

## Access and security

Three kinds of people:

| who | how they are recognised | may |
|---|---|---|
| **Owner** | logged in with the password (a cookie), or on this computer, or (Settings > Access > "Your own network needs no password", on by default) on the private network and not behind the relay or a proxy | everything: tune, modes, gain, start / stop, record, settings, transmit (the server has no transmitter yet: the API answers "not supported") |
| **Visitor** | Settings > Access > "Public: anyone may watch" is on and they opened the page without a login | watch (status, spectrum, panel, audio); with "Public may tune" also tune and change the mode, but only into the modes the host's web policy allows (the web-safe list: analog audio, ADS-B, AIS, APRS, ACARS / VDL2, FT8 / FT4 / WSPR, SSTV, fax, satellite images; digital voice and pagers only after the legal notice, as in 3.8). Never settings, recording, transmit, or a private mode's data |
| nobody | the rest | the login form |

How it is built (server/app/webapp_auth.cpp, checked by tests/server/test_webapp.cpp):

* **Password**: PBKDF2-HMAC-SHA256, 60 000 iterations, a random salt, stored as `app_password_hash`; the plain password is on no disk.
  8 characters or more. Set with `--app-password`, by the first owner on the LAN from the page, or changed in Settings.
* **Sessions**: a random 256-bit token in a cookie `onair_session` that is `HttpOnly`, `SameSite=Strict`, `Secure` on https, with
  `Max-Age` = Settings > "Stay logged in for" (a week by default). At most 256 sessions; the oldest goes.
* **CSRF**: every POST must carry the session's CSRF token in the `X-OnAir-CSRF` header (GET /app/v1/session gives it; `lan` for an owner
  by address, `public` for a visitor); the WebSocket takes it as `?csrf=`. A page of another origin cannot set the header or know the token,
  and the `Origin` header, when a browser sends one, must be this server. The web server already refuses WebSockets from other origins.
* **Rate limits**: five failed logins from one address block it for a minute (and a failed login waits 300 ms); the API allows 30 requests
  a second with a burst of 100 per address; the web server's own connection limits apply. A password set only from this computer or the LAN:
  nobody from the internet can create the first password.
* **The relay and proxies**: connections through the relay arrive from 127.0.0.1 with the visitor's address in `X-Forwarded-For`; the server
  trusts that header only from loopback and only when the relay or `web_trust_proxy` is on, and a connection whose address was rewritten
  is never an owner by address, whatever address it reports (a loopback connection without the header was not rewritten: the relay
  always adds it, so that is a program on this computer).
* **No secrets in the page**: the page has no token, key or password built in; it learns the CSRF token after the server has identified the
  session. The static files are served with `Content-Security-Policy: default-src 'self'` (no inline scripts, nothing from other hosts),
  `X-Frame-Options: DENY`, `nosniff`, no referrer. The page loads no map tiles, fonts or scripts from the internet.
* **Transmit** stays the owner's: visitors get 403 whatever the host's `web_allow_tx` says (that setting is the 3.8 listener page's).
* **Private modes for visitors**: while the owner runs a mode the policy keeps private, visitors see "this mode is private" instead of the
  panel and get no audio; a visitor with "Public may tune" cannot switch into one.
* **Visitor cap**: Settings > Access > "Visitors at once" (20 by default): the next one is told "busy".
* **What is not protected**: plain http on your own network is plain http (the LAN owner needs no password there); over the internet use
  the relay (https) or a certificate; "Allow plain http from the internet" exists for the people who know why, off by default.

Reviewed and open: the session store is in memory (a server restart logs everyone out, which is the safe direction); there is no second
factor; the password is one for the owner (no per-person accounts: a shared radio with several owners shares one password or uses the LAN
rule); the fail limiter is per address, so a botnet can try five passwords per address per minute, which with a 60 000-iteration hash and an
8-character minimum is slow but not impossible: use a long password when public.

## Settings (rebuilt)

Settings is one page in groups, every item with one line of help; the ones that need a restart say so. The keys below are the same
settings file (`server.conf`), so the old ways keep working. What changed from 3.8:

| group | setting (key) | default | change from 3.8 |
|---|---|---|---|
| Access | Your own network needs no password (`app_lan`) | on | new |
| Access | Public: anyone may watch (`app_public`) | off | new; replaces the question "open / password / invite" (`web_auth`) for the new page. The classic page keeps `web_auth` |
| Access | Public may tune (`app_public_tune`) | off | new; replaces `web_control` (everyone / members / slot / nobody) for the new page |
| Access | Visitors at once (`app_public_max`) | 20 | new; `web_max_users` stays for the classic page |
| Access | Stay logged in for (`app_session_hours`) | 168 | new |
| Access | Owner password (`app_password_hash`) | none | new; `web_password` / `web_password_hash` stay for the classic page |
| Server | Name, Location, Antenna (`web_name`, `web_location`, `web_antenna`) | | same keys, now with help text |
| Server | Reachable from other computers (`web_lan`) | off | same key; was "bind address" |
| Server | Port (`web_port`) | 8073 | same |
| Server | Relay, List on the relay (`web_relay`, `web_relay_public`) | off | same keys, plain names |
| Server | TLS certificate / key (`web_tls_cert`, `web_tls_key`) | | same |
| Radio | Start receiving when the server starts (`autostart`) | off | same key, also on the Radio card |
| Radio | HackRF +14 dB amplifier (`amp`) | off | same, with the warning in the help |
| Radio | Listen demodulator (`audio_demod`) | auto | same, also on the Radio card |
| Audio | Audio rate for the page (`app_audio_rate`) | 16000 | new |
| Limits | Listeners at once, idle minutes (`web_max_users`, `web_idle_min`) | 8, 30 | same; marked as the classic page's |
| Advanced | Behind a reverse proxy, plain http from the internet, log addresses (`web_trust_proxy`, `web_plain_internet`, `web_log_ip`) | off | same |
| Advanced | Open this page at / (`app_home`) | on | new |
| | `app` | 1 | new: 0 turns the browser app off (the classic page alone) |
| | `app_root` | found beside the program | new: the page's folder (`web/onair-app`) |
| | `rec_dir` | `~/OnAir-recordings` | new: where Record writes |

Not on the page (unchanged, in the file): the classic page's demodulator and decoder lists, invites, frequency ranges, the directory, the
admin API token and `mcp_*`. Those are docs/ONAIR-WEB.md.

## Audio and latency

Listen opens an AudioWorklet fed by 16 kHz PCM frames (40 ms each) from the server's Listen demodulator; the jitter buffer aims at 100 ms
and drops the oldest audio past 400 ms, so on a LAN the delay is about 150 ms, through the relay about 150 ms plus the round trip. The rate
is a setting (8 to 48 kHz; 16 kHz is 256 kbit/s); the owner's demodulator choice (auto, NFM, AM, USB, LSB) applies to everyone listening.
"Save audio in the browser" writes a WAV of what you heard; nothing is written on the server. **Record** (owner) writes the raw IQ of the
capture as `.cf32` into `rec_dir`, named with the mode, frequency, time and rate so OnAir can play it back.

## Phones and touch

Below 640 px the page becomes three tabs (Spectrum, Decoder, Radio) with 44 px targets; drag-to-tune uses pointer events, so a finger on
the waterfall tunes exactly like the mouse. The spectrum keeps a light payload (256 bins at 2 rows a second are a choice in the toolbar).

## CPU and bandwidth

Measured with `onair-app-load` (tools/onair_app_load.cpp): the real server on the built-in test signal (FM, 2.4 Msps) with N fake viewers
in the same process, each receiving status, the panel twice a second, 1024-bin spectrum rows at 10 a second and 16 kHz audio, 10 s per step
on a GitHub Actions runner (the Mac could not link against a current library during the overnight rules). CPU is the whole process, the receiver included, in % of one core:

| viewers | process CPU (% of one core) | per viewer | kbit/s each | frames/s each |
|---|---|---|---|---|
| 0 (the receiver alone) | 45.5 | | | |
| 1 | 46.6 | 1.1 | 355 | 33 |
| 5 | 46.4 | 0.2 | 354 | 33 |
| 20 | 50.1 | 0.23 | 352 | 32 |

(GitHub's `ubuntu-24.04` runner, quick CI run 42891394337; the run-to-run noise is about 1 %, so the first viewer's 1.1 % includes the
thread and socket of the fake viewer itself.)

The spectrum is decimated once per viewer from the shared FFT, audio is one shared demodulator copied per viewer, and the panel JSON is
built once per telemetry report and shared, so a viewer costs a few percent of a core and about 300 kbit/s with audio (about 50 kbit/s
without: 1024 bytes a row times 10, plus the panel).

## The API

Everything the page does is HTTP under `/app/v1/` (JSON, POST bodies are flat objects, `X-OnAir-CSRF` on every POST) and one WebSocket:

| | |
|---|---|
| `GET info` | public: name, version, your role, whether a password exists, public mode, visitor count |
| `POST login {password}`, `POST logout`, `GET session` | the cookie; `session` gives the CSRF token |
| `POST setup {password}` | the first password (an owner by address only); `POST password {old, password}` changes it |
| `GET modes` | the mode list as the desktop shows it (groups, rows, which have a panel); `GET schema` all panel schemas |
| `GET status`, `GET devices`, `GET panel`, `GET telemetry` | the receiver; `panel` is the schema-driven report |
| `POST start {device?}`, `stop`, `mode {family, sub} or {id}`, `frequency {hz}`, `gain {db}`, `samplerate {hz}`, `bandwidth {hz}`, `demod {demod}`, `autostart {on}` | control (owner, or visitors when allowed) |
| `POST record/start {kind:"iq"}`, `record/stop`, `tx/start`, `tx/stop` | owner |
| `GET settings`, `POST settings {key: value, ...}`, `GET viewers` | owner |
| `GET ws?csrf=...` | the WebSocket: send `{"type":"sub","panel":true,"status":true,"spectrum":true,"bins":1024,"fps":10,"panelMs":500,"audio":true,"rate":16000}`; `{"type":"tune","hz":..}`, `{"type":"gain","db":..}`, `{"type":"ping","t":..}`, `{"type":"bye"}`. Receive text `hello`, `status`, `panel`, `pong`, `error`; binary `[1]` spectrum (u32 seq, f32 centre, rate, floor, top, u8 bins) and `[2]` audio (u32 rate, PCM16 mono) |

## Tests and CI

* `web_schema` (C++): every mode row's schema, every standard's export as valid JSON, the writers.
* `webapp` (C++): the access layer; the real server over HTTP and WebSocket from 127.0.0.1 and through a trusted proxy header as an
  internet visitor: login, cookies, CSRF, origins, rate and fail limits, public mode, the visitor cap, private modes, every mode row
  selectable, settings written to the file, recording a file.
* `web_app_client` (node, no dependencies): frequencies, frames, search, the panel model, settings, the audio ring.
* `web_app_smoke` (node + Playwright 1.49.1, pinned in tests/web/app_smoke.mjs): a headless Chromium with an isolated profile opens the
  real page against `onair-server`, starts, switches to ADS-B, opens Settings, checks the phone tabs and that no console error happened.
  It is **opt-in** (`ONAIR_PLAYWRIGHT=1` and `npm i playwright@1.49.1 && npx playwright install chromium`), skipped otherwise: the shared CI
  runners do not download a browser. The WebSocket-level checks of `webapp` cover the server side without one.

## What is left

* TV video and digital voice audio in the browser (the server streams the Listen demodulator only).
* Per-mode controls beyond the shared ones (sub-mode buttons exist; a mode's own options such as a PLP or a service need a settings schema
  like the panel's).
* The waterfall shows the capture's span only (no zoom into a part of it; the classic page's per-listener receivers do that).
* Several owner accounts, a second factor, and a persistent session store.
* The smoke test in CI once a runner with a browser is acceptable.
* Real-radio and real-internet trials: everything above ran on the test signal and 127.0.0.1 (the proxy header stands in for the relay).
