# Deploying OnAir Web (the browser client, the directory, the map site)

Nothing here is deployed by any script or CI job. Each step needs the owner. Commands are written for a Mac or Linux shell with `node` 20+ and `wrangler` (`npx wrangler`) logged in to the Cloudflare account that owns on-air.dev.

There are four separate things (the relay service is section 5):

| What | Where it lives | Where it runs |
| --- | --- | --- |
| The **browser client** (`web/onair-web/`) | inside every OnAir download and `onair-server` | on the host's own server, served at `/` of the web port. It talks only to that server. It is **not** a Cloudflare page: a static copy on web.on-air.dev could not reach other people's receivers (the page's CSP is `connect-src 'self'`, and the server only accepts WebSocket origins it is told about). |
| The **directory** (`web/onair-directory/`) | Cloudflare Worker + D1 + static page | https://web.on-air.dev (list of shared receivers, "Listen" opens the host's own address) |
| The **map site** (`web/map/`) | Cloudflare Worker + Durable Object | https://map.on-air.dev |

## 1. Check before anything (all local, no network)

```
cd <repo>
node tests/web/run.js                                  # the browser client's pure code (39+ tests)
(cd web/onair-directory && npm install && npm test)    # directory Worker tests
(cd web/map && npm install && npm test)                # map site tests
cmake --build build --target onair-server              # then: ctest -R "web_" in build/
```

Preview without a radio: `onair-server --web --device test` and open http://127.0.0.1:8073/ (the built-in test signal), or open `http://127.0.0.1:8073/?demo=1` for the simulated receiver with decoders. The directory page previews with any static server in `web/onair-directory/public/` and `?demo=1`.

## 2. Ship the browser client to listeners (a normal release)

The client travels with the program: `install(DIRECTORY web/onair-web ...)` in CMakeLists.txt puts it at `share/onair/web/onair-web`, and the macOS, Windows and Linux packages carry it. Release it with the next OnAir version through the normal release flow. Hosts get the new page when they update OnAir. A host can also point `web_root=<folder>` at a checkout of `web/onair-web/` to try it without updating the program.

Settings of existing listeners carry over: the first start of this version reads the old `onair.step`, `onair.palette2`, `onair.range`, `onair.floorAdj` and `onair.theme` keys once and then uses `onair.settings.v1`.

## 3. Deploy the directory page and API (web.on-air.dev)

Only needed when `web/onair-directory/` changed. The Worker is already set up as described in docs/ONAIR-DIRECTORY.md (D1 database, secrets `ADMIN_TOKEN` and `IP_SALT`, cron `17 * * * *`). Updating the static page and code:

```
cd web/onair-directory
npx wrangler d1 migrations list onair-directory 2>/dev/null || true   # schema.sql is unchanged by the page restyle
npx wrangler deploy                                                   # uploads ./public and src/ together
curl -sI https://web.on-air.dev/ | head -3
curl -s  https://web.on-air.dev/api/v1/servers?limit=3 | head -c 300
```

First deploy of a fresh account (once): `npx wrangler d1 create onair-directory`, put the printed id into `wrangler.jsonc`, `npx wrangler d1 execute onair-directory --remote --file schema.sql`, `npx wrangler secret put ADMIN_TOKEN`, `npx wrangler secret put IP_SALT`, then deploy and attach the custom domain web.on-air.dev to the Worker in the dashboard.

Rollback: `npx wrangler rollback` (previous version), or redeploy the previous git tag.

## 4. Deploy the map site (map.on-air.dev)

Only when `web/map/` changed (the feat35-map restyle, icons, ham layer):

```
cd web/map && npm install && npm test
npx wrangler deploy        # routes: map.on-air.dev custom domain (wrangler.jsonc)
```

## 5. Deploy the relay (relay.on-air.dev, *.relay.on-air.dev)

Needs a small Linux VPS (1 vCPU, 512 MB is plenty to start) with a public IPv4, Node 20+, and the domain on Cloudflare. Not deployed by any script. Run `cd relay && npm test` first.

1. DNS (Cloudflare): `A relay` -> the VPS address, `A *.relay` -> the same address. Orange cloud (proxied) is fine for both and gives TLS for free; the wildcard `*.relay.on-air.dev` is NOT covered by the free universal certificate (it covers only one label), so either buy Advanced Certificate Manager for `*.relay.on-air.dev`, or use grey cloud (DNS only) and let Caddy issue the wildcard (step 3).
2. On the VPS: `sudo useradd -r -m onair-relay`, copy `relay/relay.js` to `/opt/onair-relay/relay.js`, and make `/etc/onair-relay.env`:
   ```
   RELAY_DOMAIN=relay.on-air.dev
   RELAY_PORT=8080
   RELAY_BIND=127.0.0.1
   RELAY_ADMIN_TOKEN=<openssl rand -hex 16>
   # only when Cloudflare proxies (orange cloud): the real listener address
   RELAY_CLIENT_IP_HEADER=CF-Connecting-IP
   ```
   Do not set `RELAY_CLIENT_IP_HEADER` with grey cloud: anyone could forge it. `/etc/systemd/system/onair-relay.service`:
   ```
   [Service]
   User=onair-relay
   EnvironmentFile=/etc/onair-relay.env
   ExecStart=/usr/bin/node /opt/onair-relay/relay.js
   Restart=always
   LimitNOFILE=65536
   NoNewPrivileges=true
   ProtectSystem=strict
   [Install]
   WantedBy=multi-user.target
   ```
   `sudo systemctl enable --now onair-relay`.
3. TLS in front: Caddy with the Cloudflare DNS plugin (`xcaddy build --with github.com/caddy-dns/cloudflare`), `CLOUDFLARE_API_TOKEN` limited to DNS edit on on-air.dev, Caddyfile:
   ```
   relay.on-air.dev, *.relay.on-air.dev {
       tls { dns cloudflare {env.CLOUDFLARE_API_TOKEN} }
       reverse_proxy 127.0.0.1:8080 { flush_interval -1 }
   }
   ```
   (With the orange cloud and a Cloudflare origin certificate, use that certificate in Caddy instead of the `tls` block.) Behind Caddy the socket address is Caddy's, so set `RELAY_CLIENT_IP_HEADER=X-Forwarded-For` only if Caddy is the sole way in (firewall port 8080 to 127.0.0.1, which `RELAY_BIND` already does); Caddy replaces that header with the real address when it is not given trusted proxies.
4. Check: `curl -s https://relay.on-air.dev/relay/v1/status` gives `{"v":1,"ok":true}`; `curl -s https://relay.on-air.dev/api/v1/servers` gives an empty list; `curl -s https://nothing.relay.on-air.dev/` shows "This radio is offline".
5. Real test: on a computer, `onair-server --web --device test` with `web_relay=1`; the Share window (or `web_relay_status.json`) must say online with an `https://<label>.relay.on-air.dev/` address; open it from a phone on mobile data.
6. Ban: `curl -X POST -H "Authorization: Bearer $RELAY_ADMIN_TOKEN" -d '{"id":"onair-..."}' https://relay.on-air.dev/relay/v1/admin/ban` (in memory; put permanent bans in `RELAY_BAN_IDS` / `RELAY_BAN_IPS`). Update: copy the new `relay.js`, `sudo systemctl restart onair-relay` (hosts reconnect by themselves within seconds). Logs: `journalctl -u onair-relay` (server ids only, no traffic).
7. Public list on web.on-air.dev (optional, later): the directory page can read `https://relay.on-air.dev/api/v1/servers` (CORS open); not wired into the directory Worker yet.

## 6. After deploying

1. Open the directory page in a private window, check the list, a favourite, the FAQ and the theme switch; open a listed receiver with "Listen" and confirm the client loads, connects and shows the waterfall.
2. Check the browser console for errors on both pages (the CSP in `public/_headers` blocks anything external, so any console error is real).
3. Real-world checks that no preview here covers: Safari on iPhone and Chrome on Android (touch tuning, pinch zoom, audio start by tap, AudioWorklet), Firefox desktop, a slow mobile connection (the server lowers the spectrum rate; the footer says so), a server behind a reverse proxy or Tailscale funnel (WebSocket upgrade, `web_origin`), and TLS (`wss:`).

## What stays off the Cloudflare account

No listener data is stored by the client or the directory. The client keeps settings, memories and the name you typed in your own browser only (`localStorage`: keys `onair.settings.v1`, `onair.marks`, `onair.last`, `onair.vol`, `onair.name`, `onair.noticeAck`).
