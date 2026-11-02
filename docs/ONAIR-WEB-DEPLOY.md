# Deploying OnAir Web (the browser client, the directory, the map site)

Nothing here is deployed by any script or CI job. Each step needs the owner. Commands are written for a Mac or Linux shell with `node` 20+ and `wrangler` (`npx wrangler`) logged in to the Cloudflare account that owns on-air.dev.

There are three separate things:

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

## 5. After deploying

1. Open the directory page in a private window, check the list, a favourite, the FAQ and the theme switch; open a listed receiver with "Listen" and confirm the client loads, connects and shows the waterfall.
2. Check the browser console for errors on both pages (the CSP in `public/_headers` blocks anything external, so any console error is real).
3. Real-world checks that no preview here covers: Safari on iPhone and Chrome on Android (touch tuning, pinch zoom, audio start by tap, AudioWorklet), Firefox desktop, a slow mobile connection (the server lowers the spectrum rate; the footer says so), a server behind a reverse proxy or Tailscale funnel (WebSocket upgrade, `web_origin`), and TLS (`wss:`).

## What stays off the Cloudflare account

No listener data is stored by the client or the directory. The client keeps settings, memories and the name you typed in your own browser only (`localStorage`: keys `onair.settings.v1`, `onair.marks`, `onair.last`, `onair.vol`, `onair.name`, `onair.noticeAck`).
