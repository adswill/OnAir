# Headless server (onair-server)

`onair-server` is OnAir without a window. The SDR is plugged into the computer that runs it (a Raspberry Pi in the attic, a Linux box, a Windows PC with no screen); you control it from a web browser on any other device, or from the desktop app or a script through the network. There is no cloud: the browser connects straight to the server. Nothing leaves your network unless you forward a port yourself, or turn on the optional OnAir Web relay (`web_relay=1`, docs/ONAIR-WEB-RELAY.md), which gives the web page a public address through one outbound connection.

Status: built and tested with the synthetic test signal and fake clients only. **Not tried with a real radio, a real Raspberry Pi, a real browser on a phone or on a real Windows service.**

## Quick start

After installing OnAir, `onair-server` is already on your computer; nothing else to download:

| System | Where it is | Start it with |
|---|---|---|
| macOS (.dmg) | inside the app: `/Applications/OnAir.app/Contents/MacOS/onair-server` | `/Applications/OnAir.app/Contents/MacOS/onair-server --web --web-lan` |
| Linux (.deb) | `/usr/bin/onair-server` (on the PATH), plus an optional systemd unit, installed but not enabled | `onair-server --web --web-lan` |
| Linux (portable .tar.gz) | `bin/onair-server` in the unpacked folder | `./bin/onair-server --web --web-lan` |
| Windows (installer) | `C:\Program Files\OnAir\onair-server.exe`; Start menu: "OnAir Web server" | `"C:\Program Files\OnAir\onair-server.exe" --web --web-lan` |
| Windows (zip) | `onair-server.exe` next to `OnAir.exe` | run it from a terminal in that folder |

Without `--web`, the control page alone:

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

TLS needs OpenSSL. The release packages from 3.2 on have it (macOS and Linux carry libssl/libcrypto, Windows has it built in); a build you make yourself needs `cmake -DDECT2_WITH_TLS=ON` (it is off by default so a development build needs no extra library). 3.1 and older packages, and builds without it, say so and refuse a certificate. For OnAir Web the keys are `web_tls_cert` / `web_tls_key`. A reverse proxy (Caddy, nginx) in front of the server works as well; then keep the server on `127.0.0.1` and let the proxy do the TLS and the access control.

## Bandwidth

The page's "Data use" menu sets how much the server sends. Over Wi-Fi on a phone, "Low" is about 2 spectrum lines a second of 256 bytes and 8 kHz audio (roughly 16 KB/s with audio, 1 KB/s without).

* Spectrum: a WebSocket subscription `{"spectrum":true,"bins":256,"fps":2,"fmt":"u8"}`. `fmt:"u8"` sends one byte per bin in a binary message (`0x01`, u32 sequence, four little-endian floats: centre Hz, rate Hz, floor dB, top dB, then the bytes) instead of JSON text, about a tenth of the size. Default is JSON at 5 lines a second like before.
* The server also watches itself: if sending a spectrum line takes more than half the line interval, the interval doubles (up to 16 times, never slower than 2 s) and comes back when the link keeps up. A slow client therefore never builds a queue.
* Audio: `{"audio":true,"arate":16000}` (8000, 12000, 16000, 24000 or 48000 Hz; mono, 16 bit; binary messages starting with `0x02`). A listener that does not fetch loses the oldest audio, never more than one second is kept. At most 4 listeners.

## A sound card as the input

A receiver's audio or IF output, or an SDR that shows up as a sound card, can be the input instead of a radio. `onair-server --list-audio` prints the sound-card inputs; pick one by name (or part of it) or number with `--audio-device NAME` (or `device=Sound card: NAME` in the settings file; the same as `--device`). Stereo I/Q is the default (`--audio-iq 1` or `2` for a plain USB or LSB audio output); the other `audio_*` keys are in the table below. A sound card is never picked by itself: without `device` the server still takes the first radio, else the test signal. Based on a patch by @cleo2024 (#554).

## The settings file

Plain `key=value` lines, `#` for comments. Default place: `~/.config/onair/server.conf` (`%APPDATA%\OnAir\server.conf` on Windows), or `--config FILE`, or the `ONAIR_SERVER_CONFIG` environment variable. The file is created on the first run (mode 0600 because it holds the token). What the browser changes (mode, frequency, gain, rate, radio, demodulator, start/stop) is written back, so the server comes up the same way after a restart; your comments and unknown keys are kept.

| Key | Meaning |
| --- | --- |
| `port` | 8765 |
| `lan` | `1`: listen on all interfaces (needs a token; one is made when missing) |
| `token` | the access token |
| `tls_cert`, `tls_key` | PEM files, see TLS |
| `device` | the radio: its name (or part of it), its number in the device list, or `test` for the built-in test signal. Default: the first radio found |
| `audio_iq`, `audio_chan`, `audio_rate`, `audio_if_hz`, `audio_invert`, `audio_gain_db`, `audio_dc` | when `device` is a sound card (below): how the audio becomes I/Q (`audio_iq` 0 stereo I/Q, 1 USB, 2 LSB, 3 mono real), channel (0 left, 1 right, 2 mix), sample rate (8000 to 192000, default 48000), IF offset in Hz, swap I/Q, gain in dB, DC removal (`1`). Same as the desktop app's sound-card source |
| `net_radio` | a radio on the network, as the app keeps it (`rtltcp,192.168.1.20,1234`); may be repeated |
| `file`, `file_rate` | play a recording (SigMF, cs8, ...) as another "radio" |
| `mode` | a mode id: `fm`, `dab`, `t2`, `atsc`, `adsb`, `dvbs`, ... (all of `/api/v1/modes`) |
| `freq_mhz`, `rate_msps` (0 = automatic), `gain_db` | tuning |
| `amp` | HackRF: `1` lets the gain use the +14 dB amp (up to 116 dB). Off by default: the amp can be damaged by strong signals, so the gain then ends at 102 dB |
| `audio_demod` | `auto`, `nfm`, `am`, `usb`, `lsb` for Listen (automatic picks by frequency; FM broadcast uses its own stereo audio) |
| `autostart` | `1`: start the receiver when the server starts, and again 10 s after the radio comes back if it was unplugged. Start and Stop in the page set it |
| `share_rtltcp`, `share_port` | also act as an rtl_tcp server (see above) |
| `codec_notice_<id>` | `1`: you accepted the speech codec notice for the voice codec plug-in `<id>` (`tetra`, `ysf`, `ambe2`, `ambe`, `imbe`, `tetrapol`, `iridium`, `aeroambe`), see "Voice codecs" below. The same key the desktop app keeps |

## Voice codecs

OnAir includes no patented speech codec. Digital voice needs plug-ins that you build or get yourself and put in a codecs folder of the user the server runs as: `~/.local/share/onair/codecs/` (for the packaged service `/var/lib/onair/.local/share/onair/codecs/`), the folder in `ONAIR_CODEC_DIR`, or `codecs` next to the program. See [CODECS.md](CODECS.md); `tools/codec-builder/build-codecs.sh --out <folder>` builds `tetra`, `ambe2`, `ambe`, `imbe` and `ysf`.

The desktop app loads a plug-in only after you accepted its legal notice in a dialog. The server has no dialog; accept the notice in one of three ways:

* **In the browser app:** Settings (owner) > **Voice codecs** lists every codec plug-in with its state. **Read the notice** and **I accept** store the key and load the plug-in at once.
* **On the command line:** `onair-server --accept-codec-notice tetra,ysf` (or `all`) prints the notice, stores the keys in the settings file and starts as usual.
* **In the settings file:** `codec_notice_tetra=1`, one line per codec, then restart.

The server looks for accepted plug-ins at start and again every 10 s, so a file copied in while it runs is found without a restart, and logs one line per codec when its state changes (`voice codec tetra: Codec loaded: tetra.`). The decoded speech of TETRA, TETRA DMO, Yaesu System Fusion, DMR, P25, NXDN, dPMR, D-STAR and M17 goes to the browser's Listen button. DMR, D-STAR, P25 and NXDN also play through mbelib (`libmbe-neo.so*` or `libmbe.so*` in the codecs folder), which needs no notice. `get_codec_status` (MCP) reports per codec whether it is installed and loaded, and why not (for example `notice_required`). Encrypted calls are never decoded.

## Running it all the time

**Linux (systemd).** `packaging/linux/onair-server.service` has the steps in its header: a system user in `plugdev`, the binary in `/usr/bin` (the .deb installs the binary and the unit, not enabled; `systemctl enable --now onair-server` after the one-time setup in the unit header), the settings and the token in `/var/lib/onair`. `journalctl -u onair-server` shows the log.

**Raspberry Pi.** The same; use a 64-bit OS and a Pi 4 or better for the heavier modes (DVB-T2 needs far more CPU than FM or ADS-B). `--share-rtltcp` makes it a network radio for other computers as well.

**Windows.** `onair-server.exe` is a normal console program. To run it without a login, use Task Scheduler ("run whether the user is logged on or not", trigger "at startup") or a service wrapper such as NSSM pointing at `onair-server.exe --config C:\ProgramData\OnAir\server.conf`. Allow the port in the firewall for the networks you trust. A built-in Windows service is not done.

## From the desktop app or a script

* **Control:** any program that speaks the remote-control API (`/api/v1/status`, `/devices`, `/modes`, `/start`, `/stop`, `/mode`, `/frequency`, `/samplerate`, `/gain`, `/ws`; see the README) can drive the server with the token. The server adds `GET /api/v1/x/server`, `POST /api/v1/x/demod` and `POST /api/v1/x/autostart`.
* **Samples:** start the server with `--share-rtltcp` and add it in OnAir (or SDR++, SDR#) as an rtl_tcp network radio with the **share port (1234 unless `share_port` says otherwise), not 8765** (8765 is the web page and remote control; it answers HTTP, not rtl_tcp, and needs the token; the rtl_tcp share needs no token): you get the samples at the server's own rate as 8-bit IQ, and the desktop app does the decoding. The server then must run in a mode where the radio is open (receiver started).

The desktop app has no "remote server" screen of its own yet.

## Many listeners: OnAir Web

`--web` (or `web=1`) adds a second port (8073) where many people at once each get their own virtual receiver inside the radio's capture: own frequency, mode, audio and waterfall, with passwords or invites, per-listener limits and an admin API. Web listeners can never transmit or control the radio itself. See docs/ONAIR-WEB.md and the protocol in docs/ONAIR-WEB-PROTOCOL.md.

## Limits

* One receiver, one radio at a time (the server runs the main receiver only: no second mode, no multi-radio). OnAir Web listeners tune inside that receiver's capture.
* The browser page covers tuning, spectrum, audio and the common telemetry. The big per-mode tables and screens (service lists, maps, decoded text) are in the desktop app; the telemetry JSON carries the main figures of each mode.
* Voice modes play only with a codec you installed yourself (see Voice codecs); encrypted traffic is never decoded. TETRAPOL, Iridium and Inmarsat Aero voice decode with their plug-ins but are not sent to the browser yet.
* Sound card audio of the server machine is silent on purpose: the sound goes to the browser.
