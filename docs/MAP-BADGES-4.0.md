# Map badges (4.0)

Users can sign in with GitHub, on map.on-air.dev and in the app. The map then counts what they actually share and gives tiered badges, a
public profile at `map.on-air.dev/u/<github-login>` and a leaderboard at `map.on-air.dev/leaders`. Everything is opt-in. Signed-out sharing
works exactly as before.

Nothing is deployed. The owner's setup steps are at the end.

## One sign-in for the app, the website and the plugin marketplace

- **One GitHub app**, called "OnAir" (owner adswill, Client ID `Ov23li3cNdYJUxpl572P`), serves both Workers.
  - It has two redirect URIs: `https://map.on-air.dev/auth/` and `https://plugins.on-air.dev/`.
  - Device flow is on. The app asks for no scopes.
- **In the app**, sign-in is the plugin marketplace's device flow (`core/src/pstore/market.cpp`, unchanged). It writes
  `<data>/plugins/.market/session.json`.
  - The Badges window can start the same sign-in. Signing in or out there does it for the marketplace too.
  - While **Earn badges** is on, the map share sends that session token as `Authorization: Bearer ...`.
  - The map Worker checks the token with `SESSION_SECRET`, which has the same value on both Workers. The format is the marketplace's:
    `base64url(JSON {uid, login, iat, exp})` `.` `HMAC-SHA256`.
- **On the website**, sign-in is GitHub's web flow: `/auth/login` → github.com → `/auth/callback`.
  - The session cookie `oa_s` (HttpOnly, Secure, SameSite=Lax, 30 days) uses the same format, but is signed with a key derived for the map
    website only. So a website cookie is never valid at the marketplace, and an app token is never valid as a cookie.
- **The GitHub token** (expiring, with a refresh token, because "Expire user access tokens" is on) is used once to read the user's id and
  login, then dropped. The refresh token is never read or kept.
- **Accounts are keyed by the GitHub numeric id**, so renaming a GitHub account keeps its stats. The login is kept for the profile address.

### OAuth state and CSRF (src/auth.js)

- **State:** `/auth/login` makes a random 128-bit state. The state is sent to GitHub, and it is also stored in an `oa_state` cookie
  (Path=/auth, HttpOnly, Secure, SameSite=Lax, 10 minutes). The cookie is HMAC-signed together with its expiry and the return path.
- **Callback:** the callback needs that cookie, a valid MAC, no expiry, and the query state equal to the cookie's (constant-time compare).
  Anything else gets a 400 page. A sign-in forced on the user from another site has no cookie, so it fails.
- **Return path:** only a local path such as `/u/x` or `/leaders`. Absolute URLs, `//host`, `..` and odd characters are refused.
- **Changes with a cookie** (settings, sign out, sign out apps, delete) need two things:
  - an `Origin` header from the map's own origin;
  - `X-CSRF-Token`, which is an HMAC of the session hash and is handed to the page by `/api/me`.
- **Changes from the app** use the Bearer token. That is not an ambient credential, so it needs neither check.

## What is counted (src/badge-stats.js)

**Each unique object counts once per user, ever.** The key is one of:

| Kind | Key |
| --- | --- |
| Aircraft | ICAO hex |
| Ships | MMSI, ship stations 2xx-7xx only |
| Sondes | serial |
| APRS stations | callsign with a digit |
| Satellites | satellite id |
| Ham contacts | contact id |
| FLARM, drones, buoys, DSC, GNSS satellites | their own ids |
| DF fixes | the fix's frequency and 10-minute slot, when this user's bearing took part |
| ACARS messages | aircraft and message hash |

The keys are stored in `bu_seen`, so a restart does not count anything twice.

**Other stats:**
- **Time:** the gap since the user's last credited post, at most 120 s, and 60 s for the first post of a session. It is per user, so
  several stations or a faster post rate do not add up.
- **Days and streaks:** distinct UTC days, and the current and longest daily streak.
- **First share:** the time of the first credited post.
- **Night owl** (00-04) and **Early bird** (04-07): local solar time from the longitude, so no time zone is needed.
- **Record range:** see below.
- **Countries:** from the ICAO address blocks of the aircraft and the MID of the ships (partial tables of published allocations, in
  `src/badge-geo.js`).
- **Fields:** the Maidenhead fields the objects were in.
- **Rare catches:**
  - squawks 7500, 7600 and 7700;
  - AIS SART (970), man overboard (972) and EPIRB (974);
  - ADS-B categories B1 (glider), B2 (balloon or airship), B3 (parachutist) and B7 (space vehicle).
- **Kinds shared:** for the multi-mode badge.
- **Weekly and monthly counters** (ISO week, UTC month): hours, new objects, aircraft, ships and range, for the leaderboard.

**Record range** needs all of these:
- the user ticked **Use my area for range badges** in the app, so the post carries the receiver's 6-character locator square (about
  5 x 2.5 km);
- a direct source: ADS-B or UAT, AIS, sonde, APRS, FLARM or Remote ID. Estimated positions do not count;
- two reports of the same object 20 s to 15 min apart, whose implied speed fits the kind;
- a distance under the kind's plausible maximum (aircraft 650 km, ships 400 km, sondes 800 km, APRS 600 km).

If the locator square moves more than 30 km, range credit pauses for an hour.

**Rare catches** also need two reports at least 20 s apart.

### Anti-cheat

1. Every track first passes the hub's existing `sanitize()`: kind, position, age limits per kind, and the 250-track and 1.5 s limits per
   station.
2. **Ids must look real.** These earn nothing: ICAO `000000` and `FFFFFF`, altitudes outside -1500..65000 ft, speeds over 1300 kt, MMSI
   that is not a ship station, and APRS calls without a digit.
3. **Rate limits:**
   - at most one credited post per user every 2 s (the data is still accepted);
   - at most 10 stations per user per day;
   - hourly caps of new objects per kind (for example 3000 aircraft, 60 sondes). An object over the cap can count in a later hour.
4. **No credit for duplicates or replays:**
   - a post identical to one of the user's last six posts (same objects and positions) earns nothing;
   - a post in which nothing moved earns no time.
5. **Conflicts:** another station may have reported the same object in the last 30 s, far away (aircraft 40 km, ships 10 km). Then that
   object earns nothing. If most of a post's checked objects conflict, the whole post earns nothing.
6. **Admin hide:** `POST /api/badges/admin {login, hidden}` with `BADGE_ADMIN_TOKEN` hides an account from the leaderboard and stops
   its credit.

**Limits, said plainly:** a determined cheat with a real-looking replay of fresh, plausible data, in an area no other station covers,
can still earn credit. The caps make this slow, and admin hide undoes it.

## Badges (public/badges/defs.js, the one data file)

The 19 badges, their names, descriptions, icons, the stat each one reads and the tier thresholds (bronze, silver, gold, platinum) are in
`web/map/public/badges/defs.js`:

| Badge | Counts | Tiers (bronze / silver / gold / platinum) |
| --- | --- | --- |
| First light | first share | 1 |
| On the air | hours | 10 / 100 / 1000 / 5000 |
| Regular | days | 7 / 30 / 100 / 365 |
| On a roll | longest streak | 3 / 7 / 30 / 100 days |
| Plane spotter | aircraft | 100 / 1k / 10k / 100k |
| Harbour master | ships | 10 / 100 / 1k / 10k |
| Sonde hunter | sondes | 1 / 10 / 50 / 250 |
| Digipeater | APRS stations | 25 / 250 / 2500 / 10000 |
| Orbit watcher | satellites | 5 / 25 / 100 / 500 |
| QSO party | ham contacts | 100 / 1k / 10k / 50k |
| Fox hunter | DF fixes | 1 / 10 / 100 / 1000 |
| Text reader | ACARS messages | 10 / 100 / 1k / 10k |
| Long range | record range | 100 / 200 / 300 / 400 km |
| Globetrotter | countries | 5 / 15 / 30 / 60 |
| Field day | Maidenhead fields | 3 / 10 / 25 / 60 |
| Night owl | hours 00-04 | 5 / 25 / 100 / 500 |
| Early bird | hours 04-07 | 5 / 25 / 100 / 500 |
| Rare catch | kinds of rare catch | 1 / 3 / 5 / 8 |
| Jack of all modes | kinds shared | 5 / 7 / 9 / 11 |

**Tuning:** change the numbers in `defs.js`. The hub, the pages and the app all read them from there. The app gets them inside `/api/me`.

**The date of each tier is kept.** One post can reach several tiers at once, and each tier gets the date it was reached.

**Icons** (`public/badges/art.js`) are original art in the style of `shapes.js`: a notched medal in the tier's colour, a dark disc and a glyph.
- The aircraft, ship, sonde, APRS, satellite and contact glyphs are the map's own shapes.
- The other glyphs (clock, calendar, flame, globe, grid, moon, sun, star, range, ACARS, DF, modes) were drawn for this.
- Nothing is copied from other projects.

## Showing them off

- **`/u/<login>`** (`public/profile.html`, `badges/page.js`) shows:
  - the stats and every badge with its tier, the date each tier was earned, and progress to the next;
  - rare catches;
  - a "Copy link to brag" button.

  The owner of the profile also sees their settings: Earn badges, Public profile, the linked map name, Sign out OnAir apps (N), and
  Delete my badge data.
- **`/leaders`** (`public/leaders.html`) has these boards: hours, new objects, aircraft, ships, longest range (weekly, monthly, all time),
  and longest streak and countries (all time). It shows the top 50 public users, each with their top badge. Results are cached for 60 s.
- **Map entries:** a track whose sender is signed in, has a public profile and has linked the map name to the account carries
  `bb: "id:tier:login"` in `/live`. This is additive; old clients ignore it. The card shows "Station badge" with the medal and a link to
  the profile. The page header has a "Badges" link to the leaderboard.
- **In the app:** Server > Badges... (also in the classic Server button) and Settings > Network and updates > Map badges open the Badges
  window:
  - the next step, as one hint: sign in, turn on Earn badges, turn on sharing;
  - Sign in with GitHub (device flow) or Sign out;
  - Earn badges;
  - Public profile and leaderboard;
  - Use my area for range badges;
  - Link my map name to this account;
  - stats, and the badge list with medals, earned dates and progress bars;
  - Open my profile and Leaderboard.

  A **"Badge earned" toast** shows at the bottom right when a post earns a badge. Each badge and tier is toasted once, also across
  restarts (`<data>/badges-shown.txt`).

## The map name (`by`)

The existing optional name stays as it was. A signed-in user can link a name to the account (website or app). From then on:

- only posts signed in with that account may use the name;
- anyone else's post is still taken, without the name, with `by_error` ("That name is linked to an OnAir account...");
- one name per account and one account per name; the comparison uses the same folding as the name rules;
- unlinked names work exactly as before.

## Privacy

- **No IP address** is read or stored anywhere in the badge code.
- **No receiver position** is stored. The optional locator square is used for the range of that one post and dropped; the profile has
  distances only.
- **Tokens are stored hashed:**
  - `bu_tokens` keeps the first 32 hex digits of SHA-256 of website sessions (required, so sign-out works) and of app tokens seen (for
    "Sign out OnAir apps");
  - the GitHub token is never stored.
- **Opt-in:**
  - nothing is counted until Earn badges is on. The app turns it on with the user's first sign-in from the Badges window, and the user
    can turn it off at any time;
  - the profile, the leaderboard entry and the map-entry badge appear only with Public profile on;
  - a private profile is visible to its owner only.
- **Delete my badge data** removes the user's row, unique keys and tokens.
- **Kept per account:** GitHub id and login, the settings, the stats JSON, and the unique object keys.

## Storage and cost

- **No D1, no new Durable Object class, no wrangler migration.** The `MapHub` Durable Object creates three tables in its own SQLite at
  start (`CREATE TABLE IF NOT EXISTS`, as it already does for `trails`):
  - `bu_users`;
  - `bu_seen` (WITHOUT ROWID, primary key (uid, key));
  - `bu_tokens`.
- **Write-behind**, in the existing flush transaction:
  - a user's row at most every 30 s, and soon after a new badge (the next flush, within 10 s);
  - each new unique object once.
- **Reads:** a user's keys are read once per hub start, the first time the user posts. Leaderboards read the public users at most once a
  minute per board.
- **Signed-out posts cost nothing extra.**

## Files

| Path | What |
| --- | --- |
| `web/map/src/auth.js` | tokens (marketplace format), web-session key, OAuth state, CSRF, cookies, GitHub code exchange |
| `web/map/src/badge-stats.js` | `BadgeBook`: credit per post, unique counting, streaks, range, anti-cheat, badges, public profile |
| `web/map/src/badge-geo.js` | distance, locators, fields, ICAO and MID country tables |
| `web/map/src/badge-service.js` | `/auth/*` and the badge API, the name lock, the map-entry badge |
| `web/map/src/badge-store.js` | the SQLite tables |
| `web/map/src/index.js` | the hub: Bearer check on `/ingest`, credit, flush, routes; the Worker: `/u/*`, `/leaders` |
| `web/map/public/badges/` | `defs.js` (data), `art.js` (icons), `page.js`, `badges.css`; `profile.html`, `leaders.html` |
| `core/include/dect2/badges/badges_client.h`, `core/src/badges/badges_client.cpp` | app side: session, header, locator, replies, toast queue, panel model |
| `app/badges/badges_ui.cpp` | the Badges window and toast |
| `app/map/map_send.cpp`, `app/ui/ui2_shell.cpp`, `app/ui/settings_*` | hooks |

### API

| Route | What |
| --- | --- |
| `GET /auth/login?next=` | start the web flow |
| `GET /auth/callback` (and `/auth/`) | finish the web flow |
| `POST /auth/logout` | sign out of the website |
| `GET /api/me` | who is signed in (cookie or Bearer), settings, CSRF token, own profile |
| `POST /api/me/settings` | `{badges, public, by}` |
| `POST /api/me/apps/revoke` | sign out every app |
| `POST /api/me/delete` | delete the account's badge data |
| `GET /api/u/<login>` | a public profile |
| `GET /api/leaderboard?board=&period=` | a leaderboard |
| `POST /api/badges/admin` | hide an account (admin token) |

`/ingest` takes two optional, additive fields:
- in the request: `home` (a locator) and the `Authorization` header;
- in the reply: `account`, `awards` and `account_error`.

## Tests

- **`web/map`: `node --test test/`, 172 tests, all pass.** The new files are:
  - `badge-stats.test.js` (17): unique counting across posts, stations and a restart; duplicate, replay and rate rules; hours with gaps
    and sessions; days and streaks; range confirmation, teleports, too far, indirect sources, home moves; rare catches; conflicts; hourly
    and station caps; opt-in and hidden; ACARS, fixes and kinds; ISO weeks and months; solar time; the profile has no position.
  - `auth.test.js` (8): a token made like the marketplace's (`node:crypto`, independent of our code) is accepted; tampering, expiry and
    other keys are refused; web and app keys are separate; hashes; OAuth state round trip, wrong state, forged expiry or return path, no
    cookie; return paths; CSRF origin and token; GitHub exchange (the token is not returned).
  - `badge-service.test.js` (8): the whole web flow against a scripted GitHub; not configured; settings by cookie with and without CSRF
    and by Bearer; name linking and the ingest lock; private and public profiles; leaderboard periods and admin hide; sign out apps,
    delete, logout with CSRF; map-entry badge.
  - `hub-badges.test.js` (4): the real `MapHub` ingest (fake Durable Object context): signed-out replies unchanged; a signed-in post
    counts, returns badges and puts `bb` on the track; a bad token keeps the data; a linked name is refused to others.
  - `badges-ui.test.js` (3): the art for every badge and tier (self-contained, escaped titles, one icon each), the map-entry string, the
    page helpers.
- **C++: `tests/badges/test_badges_client.cpp`** (ctest `badges_client`) covers the session file, header injection (CR/LF), locators, ingest
  replies, the toast queue (dedupe, restart, two threads), the `/api/me` model (order, progress, tiers), the panel steps and request bodies.
  It is run on GitHub `quick.yml` together with `settings`, `i18n` and a build of `dect2`.
- **Preview:** `node test/dev-server.js --serve --seed`, then open `/u/demo-station` and `/leaders`. `/auth/dev?login=NAME` signs in without
  GitHub (dev server only; the Worker has no such route).
- **Not tested:** the deployed Worker against real GitHub and Cloudflare, and the app against a live server. Nothing is deployed.

## Owner setup (one list for both services)

1. **GitHub app "OnAir"** is done. Its redirect URIs are `https://map.on-air.dev/auth/` and `https://plugins.on-air.dev/`, and device flow is on.
   - **Generate one client secret:** GitHub > Settings > Developer settings > OnAir > Generate a new client secret. Only the map uses it;
     the marketplace's device flow needs none.
   - The map sends `redirect_uri=https://map.on-air.dev/auth/callback`. If GitHub answers "redirect_uri is not associated with this
     application", set the var `GITHUB_REDIRECT_URI` in `web/map/wrangler.jsonc` to `https://map.on-air.dev/auth/`; the Worker accepts
     both paths. Or add `https://map.on-air.dev/auth/callback` as a third redirect URI.
2. **One shared `SESSION_SECRET`** for both Workers. It must be the same value, so the app's marketplace sign-in counts on the map:
   ```sh
   openssl rand -base64 48 > /tmp/onair-ss
   (cd tools/marketplace/server && npx wrangler secret put SESSION_SECRET < /tmp/onair-ss)
   (cd web/map && npx wrangler secret put SESSION_SECRET < /tmp/onair-ss)
   rm /tmp/onair-ss
   ```
   If the marketplace already has a `SESSION_SECRET`, put that same value into `web/map` instead of making a new one.
3. **Map secrets:**
   ```sh
   cd web/map
   npx wrangler secret put GITHUB_CLIENT_SECRET            # paste the secret from step 1
   openssl rand -hex 32                                     # keep it in your password manager
   npx wrangler secret put BADGE_ADMIN_TOKEN               # optional: paste it (hide cheats from the leaderboard)
   ```
4. **Storage migration: none.** The hub makes its tables at start. `wrangler.jsonc` keeps its one `v1` migration. The Client ID and
   redirect URI are already in `vars`.
5. **Deploy:** `cd web/map && npx wrangler deploy`.

   The app sign-in goes through the marketplace, so it also needs:
   - `plugins.on-air.dev` deployed (`tools/marketplace/server/README.md`);
   - its public key in `kBuiltInMarketKeyHex`, in a release build.

   Until then the Badges window says sign-in is not set up in this build. The website sign-in works without it.
6. **Check:**
   - `curl https://map.on-air.dev/api/me` shows `{"signedIn":false,"configured":true,"web":true}`;
   - open `https://map.on-air.dev/auth/login`, sign in, and land on your profile.
