# OnAir Web: share your radio with many listeners

OnAir Web turns `onair-server` (the headless server, docs/headless-server.md) into a WebSDR / KiwiSDR-style receiver for many people at once. Your SDR captures a wide piece of spectrum; everyone who opens your page gets their **own virtual receiver** inside that capture: their own frequency, mode (AM, NFM, WFM, USB, LSB, CW), passband, squelch, AGC and audio, their own zoomable waterfall, and the results of decoders they subscribe to. One radio, many listeners, nobody tunes anybody else away.

Status: the server core is built and tested with synthetic signals and fake clients (loopback sockets, many simulated sessions). **Not tried with a real radio, real browsers over the internet, a Raspberry Pi, or a real certificate.** The browser interface is in `web/onair-web/` (vanilla JS, no build step, no internet needed; not yet tried in real browsers against a live server); without it the server shows a placeholder page, and any client that speaks docs/ONAIR-WEB-PROTOCOL.md works.

## How it works

```
radio --(raw samples, the radio's rate)--> onair-server
            |-- the main receiver (the mode chosen in the control page, as before)
            '-- OnAir Web hub
                  shared once:  overlap-save FFT of the capture, waterfall
                  per listener: the FFT bins of its channel -> small inverse FFT -> exact resampler -> passband filter -> demodulator -> squelch / AGC -> ADPCM or PCM
                  per decoder:  the same slice at the decoder's rate; listeners who ask for the same decoder on the same frequency share one instance
```

Because every listener only cuts its own few bins out of the shared transform, the cost of a listener does not grow with the radio's sample rate (see the sizing table). The control page and the remote-control API (port 8765) are unchanged and stay private: web listeners never control the radio itself (its centre frequency, rate, gain or mode), only their own virtual receiver.

## Quick start (your own network)

After installing OnAir, `onair-server` is already on your computer; nothing else to download:

| System | Where it is | Start it with |
|---|---|---|
| macOS (.dmg) | inside the app: `/Applications/OnAir.app/Contents/MacOS/onair-server` | `/Applications/OnAir.app/Contents/MacOS/onair-server --web --web-lan` |
| Linux (.deb) | `/usr/bin/onair-server` (on the PATH), plus an optional systemd unit, installed but not enabled | `onair-server --web --web-lan` |
| Linux (portable .tar.gz) | `bin/onair-server` in the unpacked folder | `./bin/onair-server --web --web-lan` |
| Windows (installer) | `C:\Program Files\OnAir\onair-server.exe`; Start menu: "OnAir Web server" | `"C:\Program Files\OnAir\onair-server.exe" --web --web-lan` |
| Windows (zip) | `onair-server.exe` next to `OnAir.exe` | run it from a terminal in that folder |

Then, with the right path for your system:

```
onair-server --web --web-lan --device test      # the built-in test signal; drop --device test for your radio
```

or in the settings file (`~/.config/onair/server.conf`, see docs/headless-server.md):

```
web=1
web_lan=1
autostart=1
```

Then open `http://<address of the server>:8073/` on any computer or phone of your network. Without TLS only private addresses (10.x, 172.16-31.x, 192.168.x, 169.254.x, 100.64-127.x, this computer) are let in; anybody else gets "this server needs HTTPS".

The radio's centre frequency and rate are what the listeners can tune inside: set them in the control page (port 8765) or with `freq_mhz=` / `rate_msps=`. A wider rate gives listeners more spectrum but costs the shared FFT more processor time.

## On the internet

New to this? docs/HOST-A-RECEIVER.md walks through it for complete beginners (Tailscale Funnel, Cloudflare Tunnel, router + Caddy).

1. **TLS.** Listeners' passwords and invites must not travel in clear text, and browsers only allow some audio features on HTTPS. Either:
   * a certificate in OnAir itself (the release packages from 3.2 on have TLS; your own build needs `-DDECT2_WITH_TLS=ON`):
     ```
     web_tls_cert=/etc/letsencrypt/live/radio.example.org/fullchain.pem
     web_tls_key=/etc/letsencrypt/live/radio.example.org/privkey.pem
     ```
     The files are read once at start: restart the server after a renewal (for certbot: `--deploy-hook "systemctl restart onair-server"`). The key file must be readable by the server's user (for example a copy with mode 0640 owned by `onair`);
   * or a reverse proxy (Caddy, nginx) that does the TLS and forwards to `127.0.0.1:8073`. Set `web_trust_proxy=1` so the listener's address comes from `X-Forwarded-For` (only believed from a proxy on this computer) and the per-address limits keep working. Caddy example:
     ```
     radio.example.org {
         reverse_proxy 127.0.0.1:8073
     }
     ```
     Caddy gets and renews the Let's Encrypt certificate itself and passes WebSockets through. For nginx add `proxy_http_version 1.1; proxy_set_header Upgrade $http_upgrade; proxy_set_header Connection "upgrade"; proxy_set_header Host $host; proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for; proxy_read_timeout 120s;`.
   * Let's Encrypt needs a domain name that points at your public address (a dynamic DNS name works) and port 80 or 443 reachable for the challenge. A self-signed certificate works for testing, but every listener gets the browser's warning.
2. **Router.** Forward one TCP port (443 with a proxy, or 8073) from the router to the server. Forward nothing else: the control page (8765) and the rtl_tcp share must stay inside your network.
3. **Firewall.** Allow that port in. On Linux with ufw: `ufw allow 443/tcp` (proxy) or `ufw allow 8073/tcp`. Under CGNAT (many mobile and some fibre connections) you cannot forward a port: use a tunnel (Cloudflare Tunnel, Tailscale Funnel, a small VPS with a reverse SSH tunnel) to the proxy.
4. **Access.** Decide who may listen (`web_auth`, below) and how many (`web_max_users`). Start with a password or invites; open access to the whole internet means anyone, and they share your upload bandwidth.

`web_plain_internet=1` lets plain HTTP in from every address. Do not use it except behind something else that does the encryption.

## Easy setup inside OnAir (two buttons)

In the app open **Server** and use the OnAir Web block:

1. **Share my radio on the web** starts `onair-server --web` (from the OnAir package) and a Cloudflare quick tunnel, which gives your page a public `https://....trycloudflare.com` address. No account, no router settings. The first time, OnAir asks before downloading the small helper program `cloudflared` from github.com/cloudflare/cloudflared (it is used from your PATH or Homebrew if you already have it). The app's own receiver stops while you share: the web server uses the radio. The address changes each time you share.
2. **List it on the public directory** shows exactly what web.on-air.dev will receive (the same preview the server prints, see docs/ONAIR-DIRECTORY.md), and lists the server after one **List it** click. You can give a name, the radio and a rounded location (about 1 km); leave the location empty to send none.

**Stop sharing** (or closing OnAir) ends the server and the tunnel and the listing expires by itself after a few minutes. Files and logs are in the `webshare` folder of OnAir's data folder (`server.conf`, `server.log`, `tunnel.log`). The tunnel address is a random one, so the directory entry changes when you share again; for a fixed address use a named Cloudflare tunnel or your own domain as described above.

## Access and limits

| Key | Default | Meaning |
| --- | --- | --- |
| `web` | 0 | 1: OnAir Web on (also `--web`) |
| `web_port` | 8073 | its port (also `--web-port`) |
| `web_bind` / `web_lan` | 127.0.0.1 | the address to listen on; `web_lan=1` is `0.0.0.0` (also `--web-lan`) |
| `web_tls_cert`, `web_tls_key` | `tls_cert`, `tls_key` | PEM files for HTTPS / WSS |
| `web_trust_proxy` | 0 | a reverse proxy on this computer: take the address from `X-Forwarded-For` |
| `web_plain_internet` | 0 | allow plain HTTP from non-private addresses (not recommended) |
| `web_origin` | | an extra page origin allowed to open the WebSocket (repeatable), e.g. `https://radio.example.org` behind a proxy on another name |
| `web_auth` | open | `open` (anyone), `password` (`web_password`, 8 characters or more), `invite` (invite tokens only) |
| `web_invite` | | `token,label,max_minutes` (repeatable). Valid in every mode; the admin API makes and revokes them |
| `web_admin_token` | made at first start | the admin API's token (Bearer); 32 hex characters |
| `web_admin_lan` | 0 | the admin API from other computers too (otherwise from this computer only) |
| `web_max_users` | 8 | listeners at once |
| `web_max_per_ip` | 2 | listeners from one address |
| `web_cpu_cap_pct` | 75 | all web processing together, % of the whole machine: above it new listeners get "busy"; well above it for 10 s the newest open-access listener is disconnected |
| `web_session_cpu_pct` | 25 | one listener, % of one core: over it the listener gets fewer and smaller spectrum rows, still over after 10 s it is disconnected (`cpu_budget`) |
| `web_max_kbps` | 8000 | all listeners together, kbit/s (new listeners get "busy" above 90 %) |
| `web_user_kbps` | 512 | one listener, kbit/s (audio settings over 80 % of it are refused; spectrum is slowed down above it) |
| `web_max_decoders` | 8 | shared decoder instances |
| `web_idle_min` | 30 | no tuning or other action for this long: disconnected (0: never) |
| `web_public_max_min`, `web_member_max_min` | 0 | session time limit for open access / for password and invite users (0: none; an invite's own limit wins) |
| `web_max_fps`, `web_max_bins` | 15, 2048 | spectrum rows a second and bins a row, per listener |
| `web_threads` | 0 | worker threads (0: half the cores, 1 to 4) |
| `web_log_ip` | 0 | 1: listeners' addresses appear in the log and the admin user list. Off: the log says only "session 7 joined" and the admin list shows a short tag that cannot be turned back into the address |
| `web_name`, `web_location`, `web_antenna` | | shown to listeners |
| `web_root` | | the browser UI's folder; empty: `web/onair-web` beside the program or `../share/onair/web/onair-web`; none found: the placeholder page |
| `web_demods` | all six | the demodulators listeners may use, e.g. `am,usb,lsb,cw` for an HF-only server |
| `web_allow_range`, `web_block_range`, `web_range_preset`, `web_range_action` | | which frequencies listeners may tune and see; see Frequency ranges below |
| `web_decoders_off` | | data decoders turned off for listeners |
| `web_allow_tx` | 0 | `1`: listeners may transmit through the host's radio (you are responsible for the law and the hardware); keys, decryption and recording stay forbidden |
| `web_decoders_on`, `web_legal_ack` | | voice / pager decoders turned on, and the version of the legal notice the host accepted (set by the admin API; see Safety) |
| `web_directory`, `web_directory_url`, `web_public_url`, `web_directory_*` | 0 | list this server in a public directory with a signed heartbeat; off by default, shows exactly what is sent first and needs `web_directory_ack` (docs/ONAIR-DIRECTORY.md) |

Failed logins (wrong password or invite, wrong admin token) from one address: after 5 within a minute that address is refused for a minute. Every wrong answer comes after 300 ms.

## Admin API

Same port, `Authorization: Bearer <web_admin_token>`, from this computer unless `web_admin_lan=1`. POST bodies are JSON (`Content-Type: application/json`, at most 4 KB).

```
T=$(grep ^web_admin_token ~/.config/onair/server.conf | cut -d= -f2)
curl -s -H "Authorization: Bearer $T" http://127.0.0.1:8073/web/v1/admin/users
curl -s -H "Authorization: Bearer $T" http://127.0.0.1:8073/web/v1/admin/stats
curl -s -H "Authorization: Bearer $T" -H 'Content-Type: application/json' -d '{"id":7,"reason":"please share"}' http://127.0.0.1:8073/web/v1/admin/kick
curl -s -H "Authorization: Bearer $T" -H 'Content-Type: application/json' -d '{"label":"Ana","max_minutes":120}' http://127.0.0.1:8073/web/v1/admin/invite
curl -s -H "Authorization: Bearer $T" -H 'Content-Type: application/json' -d '{"prefix":"3fa9c1"}' http://127.0.0.1:8073/web/v1/admin/invite/revoke
curl -s -H "Authorization: Bearer $T" -H 'Content-Type: application/json' -d '{"demod":"wfm","on":false}' http://127.0.0.1:8073/web/v1/admin/policy
```

| Call | |
| --- | --- |
| `GET users` | the sessions: id, name, class, address (or its tag), seconds, idle seconds, frequency, mode, audio / spectrum on, kbit/s, CPU %, degrade level, dropped frames, decoders |
| `GET stats` | users, CPU % (of the machine) and its cap, kbit/s and its cap, admitted / rejected / kicked counts, samples dropped because the DSP fell behind, the capture, the shared decoders and their listener counts |
| `POST kick` | `{"id":n,"reason":"..."}` |
| `GET policy`, `POST policy` | the demodulators and decoders listeners may use; `{"demod":"am","on":true}` or `{"decoder":"adsb","on":false}` |
| `GET legal` | the legal notice text and its version |
| `GET ranges`, `POST ranges` | the frequency ranges (see Frequency ranges): `{"allow":["144-146MHz,nfm"],"block":["..."],"presets":["ham_hf@open"],"action":"refuse"}`; a key you leave out stays as it is, `{"clear":true}` removes all; nothing changes when a line is bad (400 with the reason); listeners are told at once |
| `GET invites`, `POST invite`, `POST invite/revoke` | invites (the list shows only a 6-character prefix of each token; the token is shown once, when it is made); revoke by `token` or `prefix` |

Changes made through the admin API are written to the settings file.

## Frequency ranges

The host decides what listeners may tune and see. Without any of the settings below nothing changes (every listener can use the whole capture).

```
web_allow_range=144-146MHz,nfm                 # allow-list: ONLY inside the allow rules (repeatable)
web_allow_range=7.000-7.200MHz,usb,lsb,member  # a mode list limits the rule to those modes; a class tag limits it to one kind of listener
web_allow_range=430-440MHz,note=70 cm, repeaters off-limits at night
web_block_range=145.8-146MHz                   # block-list: ALWAYS wins, also inside an allow range (repeatable)
web_range_preset=ham_hf@open                   # a built-in set, optionally for one class (name@class, several: name@open+member)
web_range_preset=no_cellular_no_public_safety
web_range_action=refuse                        # refuse (default) or clamp: what a tune request outside the ranges gets
```

**Rule format.** `<start>-<end>[,mode...][,class...][,note=text]`. Frequencies take `Hz`, `kHz`, `MHz`, `GHz` (any case, `k` `M` `G` alone work); a start without a unit takes the end's unit (`144-146MHz`, `100-200kHz`); numbers with no unit anywhere are Hz. Modes: `am nfm wfm usb lsb cw`. Classes: `open` (no password needed), `member` (knew the password), `invite` (came with an invite token); none given: all classes. `note=` runs to the end of the line and is shown to listeners. Refused (the line is ignored and named at start-up): start above end (reversed), start equal to end (empty), negative, above 1000 GHz, unknown words. Values are rounded to 1 mHz. If every `web_allow_range` line is bad the server says so loudly, because then nobody is restricted.

**What the rules mean.**

* Allow-list: for a class, when at least one allow rule (or allow preset) applies, listeners of that class can tune only inside those rules, and with the listed modes if the rule names modes. A class with no allow rule is not restricted by allow rules (`web_allow_range=144-146MHz,open` limits only open-access listeners). Rules of one class that overlap or touch (within 1 mHz) are merged.
* Block-list: a block applies to every class unless it has a class tag, and always wins. A block that names modes blocks only listening in those modes.
* Edge handling: the listener's passband (tuned frequency plus the passband edges) grows by a guard margin on both sides (5 % of the bandwidth, at least 200 Hz, at most 5 kHz, for the filter skirts). That whole span must lie inside ONE merged allowed range (never spread over a gap) and must not touch a block (touching counts). So an NFM channel cannot be tuned closer than about 7 kHz to the edge of an allowed band, and USB at the top edge of an allowed band is refused while LSB there is fine.
* Capture edge: when any rule is set, the passband must also lie inside the usable 96 % of the capture. A passband running over the capture edge would otherwise pick up the other end of the capture (FFT wrap-around), which may be a blocked range.
* Enforcement is in the hub, on the server: a `tune` that does not fit is answered with `error` `forbidden_range` (and the nearest allowed frequency in the text), or with `web_range_action=clamp` moved to the nearest allowed frequency (the `state` message tells the new one). A listener that is already tuned somewhere that stops being allowed (the host changed the ranges, the capture moved) gets no receiver at all, so no audio leaves, and one `forbidden_range` error. New listeners start at the nearest allowed frequency to the capture centre.
* Spectrum and waterfall: rows are masked on the server before they are scaled and sent. Every bin that is not wholly inside a visible range, and one bin on each side of it, is replaced by the noise floor of the visible part (nothing visible: a flat -150 dB). Visible means allowed and not inside a block that names no mode (a block that names modes is about listening, so the spectrum stays). A band narrower than three bins at the listener's zoom therefore shows nothing. FFT leakage between neighbouring bins beyond that one bin is not removed.
* Decoders: a decoder that works on a frequency (`needs_freq`) is refused when its whole slice (the decoder's sample rate, at least 25 kHz, around the frequency) is not visible to the listener; active subscriptions are dropped when the ranges change. The `host` decoder (the results of the server's own receiver, whose frequency listeners do not control) is passed only to a listener who may see the whole capture; otherwise it is silent for that listener.
* Listeners are told: `welcome.ranges` (and a `ranges` message when the host changes them) lists the allowed and blocked ranges of the listener's class, the host's notes and the guard margin. The browser page greys out modes and band buttons that are not allowed, darkens what is not visible on the spectrum, draws allowed bands as green strips and blocked ones red, snaps tuning (digits, clicks, typing, scrolling) to the nearest allowed frequency, and shows the host's notes. The page is advisory; the server decides.

**Presets** (`web_range_preset`, comma separated or repeated; `name@class` for one class; `GET ranges` lists them). They are typical band plans, **not authoritative**: allocations differ by country and ITU region, change over time, and may be narrower where you are. You are responsible for what a preset allows or blocks; check the national plan. The tables are in `server/web/web_ranges.cpp` (data only, each range with its note).

| name | kind | contents (typical) |
| --- | --- | --- |
| `ham_hf` | allow | amateur HF bands 160 m to 10 m |
| `ham_vhf_uhf` | allow | 6 m, 4 m, 2 m, 1.25 m, 70 cm, 33 cm, 23 cm |
| `broadcast_am_fm` | allow | long, medium and shortwave broadcast bands (AM, SSB) and 87.5-108 MHz FM (WFM, NFM) |
| `airband` | allow | civil aircraft voice 118-137 MHz (AM) |
| `marine` | allow | marine VHF 156-162.025 MHz (NFM) |
| `weather_sat` | allow | 137-138 MHz weather satellites, 400.15-406 MHz radiosondes, 1670-1710 MHz |
| `no_cellular_no_public_safety` | block | TETRA 380-430 MHz, 450-512 MHz public safety / business, 698-960 MHz, 1710-2170 MHz, 2300-2400, 2500-2690 and 3400-3800 MHz cellular. Not complete: VHF public safety (150-174 MHz) overlaps marine and amateur use and is not in it |
| `all` | allow | everything: lifts the allow-list for the class it is given to (`all@member`) |

Changes through `POST ranges` are written to the settings file (the `web_allow_range`, `web_block_range`, `web_range_preset` and `web_range_action` keys, one line per rule, in plain Hz) and take effect at once. Unit tests of the pure code: `tests/server/test_web_ranges.cpp` and `tests/web/run.js`.

### Frequency ranges: what is not covered

* Block and allow rules are about what listeners can receive from this server. They do not change what the host's own receiver does, and they do not hide that the server exists.
* The guard margin is a fixed approximation of the filter skirts; a very strong signal just outside an allowed band can still leak a little into the edge of a listener's audio. If that matters, leave a gap in the allow rule.
* The host decoder is withheld from restricted listeners unless the whole capture is visible to them; there is no per-frequency view of the host's own receiver.

## Safety and legal defaults

These are fixed rules, not settings:

* **Web listeners cannot transmit unless the host allows it.** The host setting `web_allow_tx=0` (the default; `1` turns it on) decides. Off, a transmit, arm, PTT or carrier message is answered "forbidden". On, those messages reach the host's transmit handler (`WebHub::setTxHandler`); the host owner is responsible for the law and for the hardware (overdriving or a long duty cycle can damage the radio), and the map guard still holds: while transmitting and for 10 minutes after, nothing is uploaded to the OnAir map server. Keys, decryption and recording stay forbidden either way. `tests/server/test_web_tx_guard.cpp` (ctest `web_tx_guard`: the default is off and the setting allows it) and `test_web_safety.cpp` check it.
* **No keys, no decryption.** Web listeners cannot import, see or use keys; encrypted traffic is never decoded; the voice key store is not part of the server.
* **Digital voice (DMR, P25, NXDN, TETRA, dPMR, D-STAR, Yaesu System Fusion, M17 and trunking) and pagers (POCSAG, FLEX) are off for web listeners.** The host can enable each one, and only after accepting the legal notice (`GET /web/v1/admin/legal`; `POST policy` with `"accept_legal_notice":"onair-web-legal-1"`). The acceptance is stored as `web_legal_ack`; a later version of the notice has to be accepted again. While the server's own receiver runs such a mode, its results are not passed to web listeners unless that kind is enabled.
* Voice codecs that are patented (AMBE / IMBE) are only used when the host installed the plug-in (docs/CODECS.md); web audio itself is IMA ADPCM or plain PCM.
* **Nothing is written to the host's disk for a listener.** There is no recording, upload or file message; the web code has no file-writing call (the scan above checks it). The only file it changes is the settings file, through the admin API, on the host's own request.
* **No addresses are logged** unless `web_log_ip=1`. Addresses are kept in memory while a listener is connected, for the per-address limits and the failed-login throttle (bounded table, entries expire after a minute).
* The browser page gets a strict Content-Security-Policy (no outside scripts, no framing), no CORS headers are ever sent, and listeners never see each other's names or frequencies.

### Host responsibility

By running OnAir Web you make what your receiver hears available to other people. You are responsible for that:

* Check the law where your receiver is (and, for an internet server, where your listeners are). Receiving may be allowed while passing on what you received is not: many countries forbid disclosing the content of messages that are not meant for the general public (for example the UK Wireless Telegraphy Act 2006 s.48, the US 47 U.S.C. 605 and ECPA, the EU member states' telecommunications laws). Some bands (police, military, mobile phones, some air traffic in some countries) may not be received at all.
* Listeners act on your connection: their traffic is your upload, their abuse reaches your address. Use a password or invites for anything beyond friends, set `web_max_users` and the bandwidth caps to what your line can carry, and keep the control page private.
* Keep the software and your certificates up to date, and run the server as an unprivileged user (packaging/linux/onair-server.service).

## Hardware sizing

Measured with `onair-web-load` (tools/onair_web_load.cpp: the hub on one thread, synthetic capture with noise and 12 FM carriers, each listener tuned to its own channel with audio on and a 1024-bin waterfall at 10 rows a second) on an Apple M2 Pro, one core = 100 %:

| Capture | Listeners | Mode, audio | Total CPU | Per listener | Data per listener |
| --- | --- | --- | --- | --- | --- |
| 2.4 Msps | shared part only | waterfall | 1.0 % | | |
| 2.4 Msps | 1 / 4 / 16 / 32 | NFM, ADPCM 12 kHz | 1.3 / 2.7 / 8.0 / 15.7 % | 0.2 to 0.46 % | 126 kbit/s |
| 10 Msps | shared part only | waterfall | 4.3 % | | |
| 10 Msps | 8 / 32 | NFM, ADPCM 12 kHz | 8.2 / 24.6 % | 0.5 to 0.6 % | 140 kbit/s |
| 2.4 Msps | 1 / 8 | WFM, ADPCM 24 kHz | 1.9 / 9.3 % | 0.9 to 1.0 % | 172 kbit/s |
| 2.4 Msps | 8 / 32 | USB, ADPCM 12 kHz, no waterfall | 9.4 / 36.6 % | 1.1 % | 54 kbit/s |

What that means:

* The listener's cost hardly depends on the capture's rate (10 Msps costs about the same per listener as 2.4 Msps); only the shared part grows with the rate.
* Data per listener: ADPCM audio is 4 bits a sample (48 kbit/s at 12 kHz, 96 at 24 kHz), PCM four times that; a 1024-bin waterfall at 10 rows a second is about 82 kbit/s. Your upload is the usual limit: 30 listeners at 130 kbit/s need about 4 Mbit/s up.
* A Raspberry Pi 4 core is roughly 4 to 6 times slower than the core measured above (estimate, not measured): about 2 to 3 % per NFM listener and 5 to 6 % per SSB listener, so a Pi 4 can serve 10 to 20 listeners next to a light main mode (FM, ADS-B), fewer beside DVB-T2 or DAB. Keep `web_cpu_cap_pct` so that the main receiver keeps enough.
* The radio itself: an RTL-SDR (2.4 Msps) covers 2 MHz (for example the 2 m band, the air band in parts, the FM band in parts); an Airspy, SDRplay or HackRF at 10 Msps covers 8 to 10 MHz; HF direct-sampling radios (RX888, Red Pitaya, SDRplay) cover all of HF in one capture.
* Memory: about 1 MB per listener (filters and queues) plus the shared FFT (a few MB at 10 Msps).

Run it on your own machine:

```
onair-web-load --rate 2.4e6 --users 0,1,8,32 --seconds 5 --mode nfm
onair-web-load --rate 10e6 --users 0,8 --mode usb --no-spectrum
```

## Decoders

Listeners subscribe to decoders by kind (`welcome.decoders` lists what the server offers and the policy allows; at most 4 a listener). Besides **`host`** (the results of the server's own receiver, as its telemetry JSON) the server offers the receivers below as shared web decoders (`server/web/web_decoders.cpp`; the result schema is in docs/ONAIR-WEB-PROTOCOL.md, "Decoder events"):

| kind | what | needs |
| --- | --- | --- |
| `adsb` | aircraft list with positions | capture of at least 2 Msps around 1090 MHz |
| `ais` | vessels | 250 kHz around 162 MHz |
| `aprs` | APRS / AX.25 stations and frames (1200 and 9600 baud) | the channel, e.g. 144.800 MHz |
| `ft8`, `ft4`, `wspr` | decodes (call, grid, SNR, time offset) | the dial frequency of the band (USB) |
| `sstv`, `wefax`, `meteor` | pictures, sent to the listener as PNG strips, nothing kept on the host | the dial frequency; Meteor-M LRPT on 137.1 / 137.9 MHz |
| `rds` | station name, radio text, programme type | the FM station's carrier |
| `cw`, `rtty` | text (CW: every signal near the dial frequency) | the dial frequency |

Each decoder gets its own slice of the capture at the rate it needs (2 Msps for ADS-B, 12 ksps for fax, 192 to 500 ksps for the others), and listeners who ask for the same kind on the same kHz share one instance and one slice. A listener picks the frequency itself (`freq_hz`); the decoder must lie in the capture (centre +- 48 % of the rate) and in its own band, so a server whose radio is tuned to 100 MHz cannot offer ADS-B (the subscribe is refused; the browser client starts ADS-B at 1090 MHz when the capture covers it). The same policy applies as to everything else: a data decoder is on unless the host turned it off (`web_decoders_off`), digital voice and pager decoders are not part of this build's list at all and stay off unless the host accepted the legal notice. The decoders only receive (transmit is the separate host setting `web_allow_tx`, off by default), and they never see keys or write a file.

CPU: a decoder counts as one of the `web_max_decoders` instances (8) and its slice and DSP are in the web DSP total that new listeners are refused above (`web_cpu_cap_pct`; a decoder is not charged to any one listener's `web_session_cpu_pct`); the decoders' own worker threads (FT8 / FT4 / WSPR decode a slot on a thread of their own) are not in the per-listener figures, so keep `web_max_decoders` low on a small machine (not measured: ADS-B at 2 Msps, Meteor and FT8 are the heavy ones, AIS, APRS, CW and the fax light). The browser client shows them in the Decoders tab: aircraft, vessel and APRS tables with a small map, the FT8 / FT4 / WSPR list, picture viewers (with "Save PNG" in the listener's browser), RDS, CW and RTTY text.

## The browser UI and the directory (extension points)

* **Browser UI.** Static files in `web/onair-web/` (or `web_root`) are served at `/`: `index.html` and whatever it loads from the same origin (scripts, styles, images, WebAssembly). Only plain file names are served (letters, digits, `.`, `-`, `_`), never dot files or anything outside the folder, at most 16 MB a file. The page talks to `/web/v1/ws` (docs/ONAIR-WEB-PROTOCOL.md) and may read `/web/v1/info` before connecting.
* **Directory.** `server/web/web_directory*.cpp` is the optional heartbeat that lists the server in a public directory of OnAir Web servers (Worker and page in `web/onair-directory/`). It is off by default, HTTPS only, signed with an Ed25519 key made on first start, and sends nothing until the host has read the exact JSON it prints and confirmed it with `web_directory_ack`. It sends name, address, coarse location, coverage and the number of users, never a listener's address or activity. Details: docs/ONAIR-DIRECTORY.md.

## Not done yet / known limits

* Frequency ranges are UNBUILT and UNVERIFIED (written while the host was on battery): see the NEEDS BUILD/TEST list at the end of this file.
* Not tried on a real radio, a real browser over the internet, a phone, a Raspberry Pi, or with a real certificate.
* IPv4 only.
* No browser UI in this build (the placeholder page); no Opus (no codec library in OnAir: ADPCM and PCM only).
* The web decoders (ADS-B, AIS, APRS, FT8 / FT4 / WSPR, SSTV, fax, Meteor, RDS, CW, RTTY) are tested on synthetic signals and noise, not with a real radio or a real browser. NOAA APT, ACARS and the voice / pager decoders are not wired (no APT decoder exists in the program; voice and pager need the legal notice). A fax picture's rows are sent once, so a later slant correction does not reach listeners who already have them; pictures are drawn as they arrive, with no re-sync.
* No S-meter while a listener has audio off (the receiver of a listener without audio is not run at all, which keeps waterfall-only listeners nearly free).
* WFM is mono (no stereo decoder, no RDS for web listeners).
* The admin user list and stats are JSON only (no admin page).

## Browser client (web/onair-web/)

Files: `index.html`, `style.css`, `app.js` (UI and WebSocket), `onair-lib.js` (pure helpers: ADPCM decode, frame parsing, messages, frequency and bin mapping, zoom and pan, palettes, S-meter, result tables), `onair-decoders.js` (the Decoders tab: tables, the map, FT8 history, picture strips; pure helpers plus the DOM views) and `onair-worklet.js` (audio jitter buffer, run in an AudioWorklet). Everything is served from the same origin (the server's content security policy allows nothing else). Tune by clicking a digit half, scrolling on the digits, arrow keys (Shift or Page Up/Down for 10 steps), or by clicking the spectrum; drag to pan, wheel or pinch to zoom (the zoomed slice is requested from the server as a finer spectrum span). Keys: 1 to 6 modes, M mute, + and - zoom. An invite link is `http://host:8073/#invite=TOKEN` (the token never reaches the server's logs). The notice banner shows the server's `notice` text from `/web/v1/info` when it sends one, else a generic one. The S-meter scale is relative to full scale (S9 = -66 dBFS), not calibrated.

Tests for the pure code (node built-ins only, not part of CMake): `node tests/web/run.js`.

## NEEDS BUILD/TEST (frequency ranges, branch webonair-ranges-030)

Nothing of the frequency-range work has been compiled or run (one `-fsyntax-only` of `server/web/web_ranges.cpp` before its last small edits). To do, in this order:

1. Build `onair_web`, `onair-server`, and run `test_web_ranges` (new, `tests/server/test_web_ranges.cpp`, registered as ctest `web_ranges`), then the existing `web_proto`, `web_dsp`, `web_hub`, `web_server`, `web_safety` and `node tests/web/run.js` (new test "host frequency ranges"). Expect compile slips in `web_hub.cpp` (the new `refreshView` / `rangesChanged` and the inserts in `handle`, `subscribe`, `configureRx`, the spectrum loop and the decoder poll) and in the tests' hand-computed numbers.
2. Add hub-level tests (not written): a tune outside the ranges gets `forbidden_range`; `clamp` moves it; a listener whose range is removed through `rangesChanged()` gets silence (no audio frames) and an error; spectrum rows of a restricted listener are flat in the hidden part (feed a tone inside a blocked range and look at the row); host decoder events withheld; per-class views through `admit` with `UserClass::Member`; a `needs_freq` decoder refused outside.
3. Admin API test in `test_web_server.cpp`: `GET/POST ranges`, a bad line gives 400 and changes nothing, the settings file gets the lines.
4. A real browser: strips and greyed buttons on the spectrum, snapping by digits / click / typing, the notes box, a ranges change while connected.
5. Check `onair-web-load` still runs with ranges set (cost of `refreshView` and `maskSpectrumRow` per spectrum row; nothing measured yet).
6. Possible conflicts with `pub/untested-secfix-web-030` (it changes the mode policy to an allow-list): this branch touches `web_proto.h` (one added error code), `web_hub.h/.cpp` (new members and functions, plus inserted blocks, none of them in the mode-policy lines: in `handle` the block before `if (m.hasFreq)` and one line before `s.rx = r;`), `web_server.cpp` (a new `ranges` admin branch before `legal`), `main.cpp` (two lines), `CMakeLists.txt` (source list and test list). `web_policy.*` and `web_config.*` are not touched.
