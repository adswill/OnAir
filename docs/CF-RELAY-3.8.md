# CF-RELAY (3.8): the OnAir relay on Cloudflare instead of a VPS

Question from the owner: why can't the WebOnAir relay run on Cloudflare instead of a VPS?

Answer: it can. Nothing in Cloudflare's limits stops it. The relay is built as a Worker plus Durable Objects in `web/relay-worker/`. It speaks the existing relay protocol (docs/ONAIR-WEB-RELAY.md, version 1) and passes the end-to-end test with the real `onair-server`. **The app needs no change**: the default `web_relay_url` is already `wss://relay.on-air.dev/relay/v1/host`, and the client accepts any `https://` address the relay hands back. **Not deployed.** Deploying needs the owner's OK.

The only thing that changes for users is the shape of a shared radio's address: `https://<label>-r.on-air.dev/` instead of `https://<label>.relay.on-air.dev/`. The reasons are in "Addresses and certificates" below.

## What the Cloudflare docs say (read 2026-11-10)

| Question | Answer (developers.cloudflare.com) | What it means here |
| --- | --- | --- |
| Durable Objects on the Free plan | Free can create **only SQLite-backed** objects. Free gets 100,000 requests a day and 13,000 GB-s of duration a day. | Both classes are SQLite-backed (`new_sqlite_classes`), so the Worker also runs on Free. |
| WebSocket Hibernation | No duration is billed while the object hibernates. Incoming protocol pings get automatic pongs without waking the object. | A shared radio that nobody is listening to costs about nothing: the host's 20 s pings never wake it. |
| WebSocket billing | 20 incoming WebSocket messages count as 1 request. Outgoing messages are free. Opening a WebSocket is 1 request. | The bill is driven by the host's DATA frames (audio and waterfall), not by listeners' downloads. |
| Message size | 32 MiB per received message | The host sends at most 16 KB a frame and the relay sends at most 64 KB. |
| CPU | DO: 30 s by default, and every incoming request or WebSocket message resets it. Worker: 10 ms per request on Free, 30 s on Paid. | Each message needs microseconds. The Worker only routes, and the host link and listener sockets terminate in the object, not in the Worker. |
| Duration / wall time | No limit while the client stays connected. A runtime update gives existing connections up to 20 minutes. | Host links can stay up for days. A deploy or runtime restart drops them, and the client reconnects in 2 to 60 s with the same address. |
| Connections per object | No number is published ("thousands of clients per instance"). There is a soft limit of 1,000 requests a second per object. | The relay caps a radio at 64 connections, as the Node relay does. At 64 listeners the host sends about 2,200 messages a second into one object, which has **not been tested**. |
| Bandwidth | "There are no additional charges for data transfer (egress) or throughput (bandwidth)" | On a VPS, bandwidth is the limit once many radios are shared (100 busy radios ≈ 21 TB a month). On Cloudflare it costs nothing. |
| Pricing, Paid | $5 a month minimum. Workers: 10 M requests and 30 M CPU-ms included, then $0.30 per M and $0.02 per M ms. DO: 1 M requests included, then $0.15 per M. 400,000 GB-s included, then $12.50 per M GB-s. SQLite storage: 5 GB-month included. | See the cost table. |
| Universal SSL | Covers the apex and first-level names (`*.on-air.dev`). It does **not** cover `a.b.on-air.dev`. | `*.relay.on-air.dev` would need Advanced Certificate Manager. The Worker uses first-level names instead. |
| Worker routes vs Custom Domains | The most specific route wins. A **route takes precedence over a Custom Domain** on the same hostname. | A route `*.on-air.dev/*` would capture `web.`, `map.`, `rooms.` and `mirror.on-air.dev` (all Custom Domains). The relay uses the narrower `*-r.on-air.dev/*`. |

## Addresses and certificates

* **Path-based links (`relay.on-air.dev/r/<id>/`) do not work with the current web app.** `web/onair-web/app.js` fetches `/web/v1/info` and opens `/web/v1/ws` by absolute path, and every radio would also share one browser origin. One radio's page could then read another radio's local storage (saved passwords, invite tokens) and open its WebSocket as the visitor.
* **`<label>.on-air.dev` works for the certificate, but routing it is fragile.** It needs a route `*.on-air.dev/*`, which overrides the existing Custom Domains (above). Every other subdomain would then have to be passed through the relay Worker.
* **Chosen: `<label>-r.on-air.dev`.** It is first level, so Universal SSL covers it. Each radio gets its own origin. Its route pattern `*-r.on-air.dev/*` only matches names ending in `-r.on-air.dev`; the docs' own example `*example.com/*` matches `myexample.com`. The label is the same 10 base32 characters as before, so an address never changes across restarts. The suffix is a variable (`RELAY_LABEL_SUFFIX`).
* Fallback if Cloudflare refuses that route pattern on deploy: buy Advanced Certificate Manager ($10 a month) for `*.relay.on-air.dev`, then set `RELAY_LABEL_SUFFIX=.relay.on-air.dev` and the route `*.relay.on-air.dev/*`. The addresses then look exactly like the Node relay's.

## How it works (`web/relay-worker/`)

```
desktop app --wss--> relay.on-air.dev/relay/v1/host --Worker--> new Room DO (unique id): challenge, signed hello, ready
                                                                  |  registers label -> room in the Registry DO
browser --https/wss--> <label>-r.on-air.dev --Worker--> Registry: which room? --> that Room DO --DATA frames--> host link
```

* **Why a unique Room per link rather than one named after the server id:** the relay only learns the id from the hello, and the hello arrives after the WebSocket has been accepted by some object. A WebSocket cannot move between objects, so the link stays in the object it landed in. The global **Registry** object (SQLite) maps label to room, and it holds the opt-in public list, the ban lists and the host-attempt counters. The Worker caches the label-to-room lookup for 10 s and the address bans for 30 s, so a page load costs one Registry call. If a cached room has lost its link, the Worker asks the Registry again once.
* **Plain HTTP:** the Room writes the browser's request to the host as an HTTP/1.1 head plus body. `Host` is kept. `X-Forwarded-*`, `Forwarded`, `X-Real-IP`, `cf-*`, hop-by-hop and the relay's own headers are dropped, and `Connection: close` is forced. The host's answer is parsed back into a Response, with Content-Length kept or chunked encoding decoded. The listener's address in OPEN is `CF-Connecting-IP`, which a browser cannot set at the edge.
* **WebSocket:** the Worker cannot hand raw bytes through, so the Room translates. It opens a WebSocket to the host's web server inside the tunnel as the client: masked frames, a fresh `Sec-WebSocket-Key`, and the browser's `Sec-WebSocket-Protocol`, `Origin` and `Cookie`. It waits for the host's answer. On 101 it accepts the browser's socket (hibernatable) and maps the frames both ways: text, binary, fragments, close codes, and answers to the host's pings. Any other answer, for example 401 or 403, goes to the browser as that HTTP response. No compression extension is negotiated.
* **Limits, as in relay/relay.js:**
  * hello within 15 s, with challenge, Ed25519 signature, id = hash of the key, nonce and clock checks (120 s)
  * one live link per id: a new link evicts the old room
  * 6 host-link attempts a minute per address and per id
  * 2,000 hosts
  * `min(max_conns, 64)` connections per radio, 24 per address per radio
  * 8 MB/s per radio (a connection over the budget is closed)
  * a 16 KB request head, a 1 MB request body, 2 minutes idle in the HTTP phase, and 30 s for the host to start answering
  * bans by id and by address, persistent in SQLite (the Node relay keeps them in memory), with `POST /relay/v1/admin/ban|unban` behind `ADMIN_TOKEN` (24+ characters)
  * `GET /relay/v1/status` and `GET /api/v1/servers` work as in the Node relay
  * the relay stores no traffic
* **Differences from the Node relay:**
  * The per-address connection cap is per radio, not relay-wide, because a relay-wide count would need the Registry on every request.
  * The relay does not end a silent host link after 70 s itself, because pings never reach the object's code. The client does that check on its side.
  * Cloudflare normalises the request path (`/%2e%2e/x` reaches the host as `/x`). The host still refuses it.
  * A room whose object is reset without a close event drops out of the list after 2 hours. A room refreshes its entry every 30 minutes with an alarm (about 1,440 wake-ups a month per shared radio, which is negligible).

## Tests (all local; nothing contacted Cloudflare)

* `cd web/relay-worker && npm test`: 16 tests.
  * 8 unit tests: id and label agree with relay/relay.js for 200 random keys; the hello checks (tampered, wrong id, wrong nonce, clock ±119/121/600 s, version, junk); frame round trips for every type, size and id edge; masked client frames in every length class; the server-frame decoder for every split of the stream from 1 byte up, with fragments, pings between fragments, close codes and oversize; the header rewrite; response heads; a chunked body in every split; the attempt limiter.
  * 8 tests against the Worker under `wrangler dev --local` (workerd with local Durable Objects), with a fake host that signs like onair-server and carries connections to a real local web server:
    * offline page, status and unknown hosts
    * HTTP with the real address and a forged `X-Forwarded-For` dropped, POST, chunked and a 300 KB body
    * **two listeners** with separate streams (40 binary frames each), echo, pings answered and the listener count in the list; a host-side close code (4008) reaching the browser; **the host link dropping and reconnecting with the same address**, and a listener joining again afterwards
    * **wrong password** refused as HTTP 401, right password accepted
    * one link per id
    * bad signature, clock, and ban/unban (token checked)
    * listeners per radio, head size and byte rate
    * host-attempt rate
* `ONAIR_E2E_RELAY=worker node tests/web/webonair_e2e.js <onair-server>` runs the existing end-to-end test with the Worker in place of relay/relay.js. It uses the real onair-server (integ-38 build) with the test signal and the real directory Worker, all 32 checks:
  * online on the relay, and listed in the directory with the relay address
  * **two guests** with waterfall and audio (25 rows and 62 audio frames each in 2.5 s), independent receivers
  * transmit, keys, files and settings refused, bad messages refused
  * `/web/v1/info` through the relay, traversal and admin paths refused, a flood rate limited
  * **the relay stopped and restarted: the server reconnects by itself with the same address** and a guest connects again
  * **wrong password refused with 4001**, right password accepted
  * the control slot
* **Results:** passed in 5 of 6 runs. One early run failed two path checks (no answer within 5 s). Those checks passed in the 5 runs since; the cause was not found and is not known to be fixed.
* Three runs of `npm test` hung for about 20 minutes. Each time the laptop lid was closed, and `pmset -g log` shows "Wake from Deep Idle ... lid" at the end. Run with the Mac awake, the suite passes 16 of 16 in 71 s.
* **Not tested:**
  * real Cloudflare: TLS, the route pattern, CF-Connecting-IP and deploy-time restarts
  * a real browser through the Worker (the Playwright part of the e2e needs `ONAIR_E2E_PLAYWRIGHT`)
  * more than 2 listeners per radio, or hours of traffic
  * an object hibernating with a listener still connected (the frame decoder keeps a partial frame in memory, which is lost if the object is evicted mid-frame)

## Expected monthly cost

Measured traffic per listener (e2e, default settings): 10 waterfall rows plus 25 audio frames a second, so about 35 DATA messages a second from the host, about 16 KB/s. Each message is one incoming WebSocket message (1/20 of a request). A radio's object is billed 128 MB × wall time while anyone is connected, and about nothing while nobody is. The sums assume the host does not merge frames, which is the worst case. Workers requests (page loads) and SQLite storage stay inside the included amounts.

| Shared radios | Typical: listeners 4 h a day, 2 on average | Busy: listeners all day, 5 on average |
| --- | --- | --- |
| 1 | **Free plan fits** (50,400 of 100,000 requests a day, 1,800 of 13,000 GB-s a day); on Paid **$5.08** | Paid **$8.25** (22.7 M requests, 324,000 GB-s) |
| 10 | Paid **$8.87** (15.1 M requests, 540,000 GB-s) | Paid **$74.37** (227 M requests, 3.24 M GB-s) |
| 100 | Paid **$90.03** (151 M requests, 5.4 M GB-s) | Paid **$745** (2,268 M requests, 32.4 M GB-s) |

* On Free, when the daily limit is used up, the relay stops for the rest of the UTC day. The docs do not describe what happens to a Durable Object over its limit. Use the Paid plan ($5) for anything public.
* The biggest cost lever is the number of messages. If the relay client merged DATA for up to 40 ms (it sends one frame per read today), the request part would shrink several times over. That is an app change and was **not** made.
* For comparison: a 1 vCPU VPS is about €4 to €6 a month, but 100 busy radios send about 21 TB a month, which no small VPS includes.

## Deploy steps (owner's OK needed; nothing below has been run)

1. `cd web/relay-worker && npm install`, then `npm test` (needs about 1 minute).
2. On Cloudflare, zone on-air.dev, DNS: add a proxied (orange) wildcard record `*` → `AAAA 100::`. It only answers names that have no record of their own; existing records are untouched. `relay.on-air.dev` gets its record from the Custom Domain in step 4.
3. `npx wrangler login` (the owner's account), then `npx wrangler secret put ADMIN_TOKEN` (`openssl rand -hex 16`).
4. `npx wrangler deploy`. This creates the Worker `onair-relay`, the two SQLite Durable Object classes (migration `v1`), the Custom Domain `relay.on-air.dev` and the route `*-r.on-air.dev/*`. If the route pattern is refused, use the ACM fallback under "Addresses and certificates".
5. Check:
   * `curl -s https://relay.on-air.dev/relay/v1/status` gives `{"v":1,"ok":true}`
   * `curl -s https://relay.on-air.dev/api/v1/servers` gives an empty list
   * `curl -s https://abcdefghij-r.on-air.dev/` shows "This radio is offline"
   * `curl -sI https://web.on-air.dev/` is still the directory, so the route did not capture it
6. Real test: in the app, **Share my radio**. The window must say online with an `https://<label>-r.on-air.dev/` address. Open it from a phone on mobile data, and start a second listener.
7. Bans: `curl -X POST -H "Authorization: Bearer $TOKEN" -d '{"id":"onair-..."}' https://relay.on-air.dev/relay/v1/admin/ban` (or `{"ip":"..."}`; `/unban` to undo). Bans are stored in the Registry's SQLite. Logs: `npx wrangler tail onair-relay` (server ids only).
8. Optional abuse protection: one free WAF rate-limiting rule on `relay.on-air.dev/relay/v1/host`, for example 20 requests per 10 s per IP.
9. The Node relay (`relay/`, docs/ONAIR-WEB-DEPLOY.md section 5) stays as the self-hosting option. Use one or the other for `relay.on-air.dev`, not both.

Updates: `npx wrangler deploy` again. Hosts reconnect by themselves within 2 to 60 s, with the same address.
