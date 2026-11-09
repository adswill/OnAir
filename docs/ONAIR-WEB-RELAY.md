# OnAir Web relay: a public address with no port forwarding

Status: the client (`server/web/web_relay.*`, started by `onair-server` when `web_relay=1`) is built and tested against a fake relay on this computer (`tests/server/test_web_relay.cpp`). The relay service is built (`relay/`, one file, Node 18+, no dependencies; tested end to end on this computer) but **not deployed yet**, so nothing in OnAir has ever contacted `relay.on-air.dev`. Until it exists the Share window falls back to the Cloudflare tunnel (see "What the window does" below). This file is the specification the relay service has to meet.

**Live status (2026-11-09):** `relay.on-air.dev` is not deployed (the name does not resolve). OnAir 3.7 therefore defaults to "automatic": relay first, then a Cloudflare tunnel, so sharing works before the relay exists. See docs/ONAIR-WEB-DEPLOY.md, section 5b.

## Why a relay

Most listeners' computers sit behind a home router, a mobile network (CGNAT) or a company firewall: nobody on the internet can open a connection to them. The relay turns it around. The server makes ONE outbound connection (a WebSocket, port 443, which every network lets out) to a service on the internet. The service owns a public https address, and every browser that opens that address is carried to the server inside the server's own connection. Nothing to open on the router, no helper program, no account.

```
browser --https--> relay.on-air.dev (abc123.relay.on-air.dev) --- one outbound wss from the server ---> onair-server --127.0.0.1--> web server
```

The relay sees the traffic of its own domain (it ends TLS there), like any reverse proxy or tunnel service (Cloudflare Tunnel, ngrok). A host who does not want that uses their own address (docs/ONAIR-WEB.md, "On the internet") or runs their own relay: the address is a setting (`web_relay_url`).

## The server side (what OnAir does)

Settings (`server.conf`): `web_relay=1`, `web_relay_url=wss://relay.on-air.dev/relay/v1/host` (wss only; `ws://` is accepted only for 127.0.0.1 and localhost, for tests). Setting `web_relay=1` also sets `web_trust_proxy=1`.

* Identity: the Ed25519 key of the directory listing (`web_directory_key`, made on first start beside the settings file). The server's id is `onair-` plus the first 16 hex characters of the SHA-256 of the public key, the same id the directory uses. The relay address is derived from the id, so it does not change when the server restarts, and nobody else can take it over without the key.
* The client keeps the link up: on a drop it waits 2 s, 4 s, 8 s ... up to 60 s (plus jitter) and connects again; a refusal with code `banned` or `version` is retried every 5 minutes only. It sends a WebSocket ping every 20 s and gives up on a link that has been silent for 70 s.
* Status: `web_relay_status.json` beside the settings file (state `off`, `connecting`, `online`, `retrying`, `failed`; the public address; the last problem with its code, message and advice; connections now and in total; bytes in and out; seconds to the next try). The Share window reads it.
* Limits: at most `max(16, 3 * web_max_users + 8)` virtual connections at once; more are closed at once. A listener's first request head must come within 16 KB.
* A build without TLS (the Windows packages have none today) cannot reach a `wss://` relay: the server prints `web_relay_error=...`, the window says so and uses the tunnel instead.

## Wire protocol, version 1

Connect to `wss://<relay>/relay/v1/host` (a normal WebSocket upgrade). Text frames are JSON (at most 4 KB), binary frames carry listeners' bytes.

1. Relay to host, text, right after the upgrade: `{"type":"challenge","v":1,"nonce":"<random, 16 to 64 hex characters, new for every connection>"}`.
2. Host to relay, text:
   ```
   {"type":"hello","v":1,"id":"onair-5e2a91c0d4b7f318","pub":"<64 hex, Ed25519 public key>","ts":1793440000,"nonce":"<the challenge's nonce>",
    "name":"Roof antenna","max_conns":64,"client":"onair-server 3.6","sig":"<128 hex>"}
   ```
   `sig` is the Ed25519 signature, with the key `pub`, over this exact text (UTF-8, every line ends with `\n`):
   ```
   OnAir-Relay-v1
   id=<id>
   pub=<pub>
   ts=<ts>
   nonce=<nonce>
   ```
   The relay checks: the signature, that `id` is `onair-` plus the first 16 hex characters of SHA-256 of the 32 key bytes, that the nonce is the one it sent, and `ts` within 120 s of its own clock (otherwise `{"type":"error","code":"clock",...}`; the host window tells the owner to set the clock to automatic).
3. Relay to host, text: `{"type":"ready","url":"https://abc123.relay.on-air.dev/","max_conns":64}` (`max_conns` may be lower than the host asked), or `{"type":"error","code":"...","message":"..."}` and the relay closes the link. Codes the client understands: `clock`, `busy` (the relay is full: retried soon), `banned`, `version` (the relay needs a newer client; both retried every 5 minutes), anything else is shown as a refusal. The address in `url` must be `https://host[/path/]`; the client refuses anything else.
4. From then on, binary frames in both directions: `[type: 1 byte][connection id: 4 bytes, big endian][payload]`.
   * `1 OPEN` (relay to host): a listener connected. The id is chosen by the relay (unique among the open ones); the payload is the listener's address as text (at most 64 bytes, for example `203.0.113.9`).
   * `2 DATA` (both ways): bytes of that listener's HTTP / WebSocket stream, in order, up to 16 KB per frame from the host (the relay may send larger frames, up to 1 MB).
   * `3 CLOSE` (both ways): the connection ended on that side; nothing more is sent for the id. Closing a connection that is already gone is allowed.
5. Control: the client sends a WebSocket ping every 20 s; the relay must answer pongs and may ping too. No frame (of any kind) for 70 s ends the link.

What the client does with a virtual connection: it connects a TCP socket to the web server on `127.0.0.1:<web_port>`, and carries bytes both ways. It reads the listener's first request head (up to the empty line) and rewrites it before the web server sees it: any `X-Forwarded-For`, `X-Real-IP`, `Forwarded`, `X-Forwarded-Host` and `X-Forwarded-Proto` a listener sent is dropped, `X-Forwarded-For: <address from OPEN>` is added, and for a request that is not a WebSocket upgrade `Connection: close` is forced (so a later request on the same connection cannot carry a header nobody checked). Everything after the head passes unchanged. The web server believes the header because the peer is 127.0.0.1 and `web_trust_proxy=1`, so the per-address limits, the kick and the block of a listener work on the real address.

## What the relay service has to do

1. **Terminate TLS** for `*.relay.on-air.dev` (wildcard certificate) and for the host WebSocket on `relay.on-air.dev` (or any name; the host uses the URL it is given).
2. **Route by name**: the first label is derived from the server id (for example the first 10 characters of the base32 of the key hash), so it is stable. A request for a name with no server online gets a clear page ("this radio is offline"), not an error from the proxy.
3. **Forward** the browser's request head as it arrived (`Host` stays the relay name: the web server compares it to the browser's `Origin`), send OPEN with the real client address (taken from the connecting socket, never from a header the browser sent), pass the bytes as DATA frames, and close both sides together on CLOSE. WebSockets are plain bytes after the upgrade: no special handling.
4. **Protect itself and the host**: a cap on connections per server (`max_conns`), per client address and in total; a cap on bytes per second per server; idle connections closed after 2 minutes without bytes in the HTTP phase; a maximum of 16 KB of request head; a ban list by server id and by client address; one live link per id (a new link for the same id replaces the old one).
5. **Rate-limit the host WebSocket** (a few connection attempts per minute per address and per id) and keep logs without listener content.
6. **Stay blind where it can**: it never stores the bytes it carries and never lists servers (the public directory, docs/ONAIR-DIRECTORY.md, is a separate opt-in service; `web.on-air.dev` can run on the same Worker infrastructure).
7. Optional but wanted: `GET https://relay.on-air.dev/relay/v1/status` returns `{"v":1,"ok":true}` for the window's connection test.

A Cloudflare Worker with a Durable Object per server id (the host's WebSocket is held by the object, browsers' requests arrive at the Worker, which forwards them to the object and gets the frames back) fits this design; so does any small server in Go, Rust or Node. The client does not care.

## What the window does (Server > Share on web)

The big button **Share my radio** starts `onair-server` with `web_relay=1`, waits up to 25 s for `web_relay_status.json` to say `online`, and shows the address, the QR code and a copy button. If the relay does not answer in that time:

* with the Cloudflare helper already on this computer, the window uses the quick tunnel for the link and says so; the server keeps trying the relay, and when it comes online the window switches the link to the relay address and stops the tunnel;
* without the helper it stops with the relay's message and advice, and points to the tunnel option (which asks before downloading anything).

While sharing, the window shows the relay state in plain words (connected, connecting, the link dropped and the seconds until the next try, refused with the reason and what to do), the listeners with **Kick** and **Block 24 h** (the admin API calls `kick` and `ban`, docs/ONAIR-WEB.md), blocked addresses with **Allow again**, and invite links.

## Tests

`build/test_web_relay` (ctest `web_relay`): address parsing, the signing text, the frame format, the header rewrite, then the real client against a fake relay and a fake web server on 127.0.0.1: handshake, challenge, signed hello (the fake relay verifies the signature), the address, a virtual connection with a forged `X-Forwarded-For`, the web server's answer carried back, close, reconnect after the link is cut, a refusal ending in the failed state, and stop. Not tested: a real `wss://` connection (needs a TLS build and a real relay), long-running traffic, many connections at once.

## The service in this repo (`relay/`)

`relay/relay.js` is the whole service: it speaks plain TCP/HTTP and does the WebSocket framing itself (TLS comes from the proxy in front, see docs/ONAIR-WEB-DEPLOY.md section 5). Settings are environment variables: `RELAY_DOMAIN` (default `relay.on-air.dev`), `RELAY_PORT`, `RELAY_BIND`, `RELAY_ADMIN_TOKEN` (24+ characters, enables `POST /relay/v1/admin/ban|unban` with `{"id":...}` or `{"ip":...}`), `RELAY_BAN_IDS`, `RELAY_BAN_IPS`, `RELAY_CLIENT_IP_HEADER` (only behind a proxy you control, e.g. `CF-Connecting-IP`; otherwise the socket address is used), `RELAY_MAX_BPS`, `RELAY_MAX_CONNS_PER_HOST`.

* Address of a server: `https://<10 characters of base32(SHA-256 of the key)>.<RELAY_DOMAIN>/`; one live link per id, a new one replaces the old.
* Limits: 16 KB request head, 2 minutes idle, connections per server (`min(max_conns, 64)`), per client address (24) and in total, 8 MB/s per server (a connection over the budget is closed), 6 host-link attempts per minute per address and per id, hello within 15 s, 70 s of silence ends a link, ban lists by id and address.
* `GET /relay/v1/status` returns `{"v":1,"ok":true}`.
* Public list: `GET /api/v1/servers` returns `{"v":1,"servers":[{id,name,url,listeners,since}]}` for servers whose hello carries `"public":true` (setting `web_relay_public=1`, off by default). The relay never stores traffic; the list lives in memory only and vanishes with the link.
* The web policy (docs/ONAIR-WEB.md, `server/web/web_policy.cpp`: the decoder allow-list, no transmit, no keys) is enforced by the host's own web server, which the relay cannot see into by design; the relay only makes sure nothing reaches the host faster or in larger numbers than the host announced.
* Tests: `cd relay && npm test` (9 end-to-end tests with a fake host that signs the hello and a fake browser, all on 127.0.0.1).
