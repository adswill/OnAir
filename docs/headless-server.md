# Headless server (onair-server)

`onair-server` is OnAir without a window. The SDR is plugged into the computer that runs it (a Raspberry Pi in the attic, a Linux box, a Windows PC with no screen); you control it from a web browser on any other device, or from the desktop app or a script through the network. There is no cloud and no relay: the browser connects straight to the server. Nothing leaves your network unless you forward a port yourself.

Status: built and tested with the synthetic test signal and fake clients only. **Not tried with a real radio, a real Raspberry Pi, a real browser on a phone or on a real Windows service.**

## Quick start

```
onair-server                      # this computer only: http://127.0.0.1:8765/
onair-server --lan                # other computers too: makes a token, saves it, prints it
```

Open `http://<address of the server>:8765/?token=<token>` in the browser. The page keeps the token for the tab and removes it from the address bar. From the server itself (`127.0.0.1`) no token is needed.

The page shows the spectrum and a waterfall (click to tune), the radio, mode, frequency, gain and sample rate, a **Listen** button (demodulated audio in the browser) and the receiver's telemetry. Every mode that the command line `onair-cli --standard <id>` knows is in the mode list.

## Access control

| | |
| --- | --- |
| Loopback | No token. A request must name a loopback `Host` (protects against DNS rebinding). |
| `--lan` / `lan=1` | Needs a token of 16 characters or more from every other computer: `Authorization: Bearer <token>` or `?token=`. A bad token is answered after 300 ms; there is no other login. Without a token the server refuses to listen on the network. |
| Pages | Cross-origin requests are refused (no CORS headers are ever sent), POST needs `Content-Type: application/json`, and the page has a strict Content-Security-Policy (no outside script, font or image). |
| rtl_tcp share | `--share-rtltcp` has **no** password (the rtl_tcp protocol has none): use it on a trusted network only. Off by default. |

## TLS

The token travels in clear text on plain HTTP. On a network you do not trust, turn TLS on:

```
openssl req -x509 -newkey rsa:3072 -nodes -days 825 -keyout server.key -out server.crt -subj "/CN=onair"
onair-server --lan --tls-cert server.crt --tls-key server.key
```

or the same as `tls_cert=` / `tls_key=` in the settings file. The page is then `https://...`; a self-signed certificate gives the browser's warning once (add an exception, or use a certificate from your own CA or Let's Encrypt). TLS 1.2 and up.

TLS needs a build with OpenSSL: `cmake -DDECT2_WITH_TLS=ON`. It is off in the default build so the packages need no extra library; without it the server says so and refuses a certificate. A reverse proxy (Caddy, nginx) in front of the server works as well; then keep the server on `127.0.0.1` and let the proxy do the TLS and the access control.

## Bandwidth

The page's "Data use" menu sets how much the server sends. Over Wi-Fi on a phone, "Low" is about 2 spectrum lines a second of 256 bytes and 8 kHz audio (roughly 16 KB/s with audio, 1 KB/s without).

* Spectrum: a WebSocket subscription `{"spectrum":true,"bins":256,"fps":2,"fmt":"u8"}`. `fmt:"u8"` sends one byte per bin in a binary message (`0x01`, u32 sequence, four little-endian floats: centre Hz, rate Hz, floor dB, top dB, then the bytes) instead of JSON text, about a tenth of the size. Default is JSON at 5 lines a second like before.
* The server also watches itself: if sending a spectrum line takes more than half the line interval, the interval doubles (up to 16 times, never slower than 2 s) and comes back when the link keeps up. A slow client therefore never builds a queue.
* Audio: `{"audio":true,"arate":16000}` (8000, 12000, 16000, 24000 or 48000 Hz; mono, 16 bit; binary messages starting with `0x02`). A listener that does not fetch loses the oldest audio, never more than one second is kept. At most 4 listeners.

## The settings file

Plain `key=value` lines, `#` for comments. Default place: `~/.config/onair/server.conf` (`%APPDATA%\OnAir\server.conf` on Windows), or `--config FILE`, or the `ONAIR_SERVER_CONFIG` environment variable. The file is created on the first run (mode 0600 because it holds the token). What the browser changes (mode, frequency, gain, rate, radio, demodulator, start/stop) is written back, so the server comes up the same way after a restart; your comments and unknown keys are kept.

| Key | Meaning |
| --- | --- |
| `port` | 8765 |
| `lan` | `1`: listen on all interfaces (needs a token; one is made when missing) |
| `token` | the access token |
| `tls_cert`, `tls_key` | PEM files, see TLS |
| `device` | the radio: its name (or part of it), its number in the device list, or `test` for the built-in test signal. Default: the first radio found |
| `net_radio` | a radio on the network, as the app keeps it (`rtltcp,192.168.1.20,1234`); may be repeated |
| `file`, `file_rate` | play a recording (SigMF, cs8, ...) as another "radio" |
| `mode` | a mode id: `fm`, `dab`, `t2`, `atsc`, `adsb`, `dvbs`, ... (all of `/api/v1/modes`) |
| `freq_mhz`, `rate_msps` (0 = automatic), `gain_db` | tuning |
| `amp` | HackRF: `1` lets the gain use the +14 dB amp (up to 116 dB). Off by default: the amp can be damaged by strong signals, so the gain then ends at 102 dB |
| `audio_demod` | `auto`, `nfm`, `am`, `usb`, `lsb` for Listen (automatic picks by frequency; FM broadcast uses its own stereo audio) |
| `autostart` | `1`: start the receiver when the server starts, and again 10 s after the radio comes back if it was unplugged. Start and Stop in the page set it |
| `share_rtltcp`, `share_port` | also act as an rtl_tcp server (see above) |

## Running it all the time

**Linux (systemd).** `packaging/linux/onair-server.service` has the steps in its header: a system user in `plugdev`, the binary in `/usr/local/bin`, the settings and the token in `/var/lib/onair`. `journalctl -u onair-server` shows the log.

**Raspberry Pi.** The same; use a 64-bit OS and a Pi 4 or better for the heavier modes (DVB-T2 needs far more CPU than FM or ADS-B). `--share-rtltcp` makes it a network radio for other computers as well.

**Windows.** `onair-server.exe` is a normal console program. To run it without a login, use Task Scheduler ("run whether the user is logged on or not", trigger "at startup") or a service wrapper such as NSSM pointing at `onair-server.exe --config C:\ProgramData\OnAir\server.conf`. Allow the port in the firewall for the networks you trust. A built-in Windows service is not done.

## From the desktop app or a script

* **Control:** any program that speaks the remote-control API (`/api/v1/status`, `/devices`, `/modes`, `/start`, `/stop`, `/mode`, `/frequency`, `/samplerate`, `/gain`, `/ws`; see the README) can drive the server with the token. The server adds `GET /api/v1/x/server`, `POST /api/v1/x/demod` and `POST /api/v1/x/autostart`.
* **Samples:** start the server with `--share-rtltcp` and add it in OnAir (or SDR++, SDR#) as an rtl_tcp network radio: you get the samples at the server's own rate as 8-bit IQ, and the desktop app does the decoding. The server then must run in a mode where the radio is open (receiver started).

The desktop app has no "remote server" screen of its own yet.

## Limits

* One receiver, one radio at a time (the server runs the main receiver only: no second mode, no multi-radio).
* The browser page covers tuning, spectrum, audio and the common telemetry. The big per-mode tables and screens (service lists, maps, decoded text) are in the desktop app; the telemetry JSON carries the main figures of each mode.
* Voice modes that need a user-installed codec (mbelib, AMBE) are not played by the server; encrypted traffic is never decoded.
* Sound card audio of the server machine is silent on purpose: the sound goes to the browser.
