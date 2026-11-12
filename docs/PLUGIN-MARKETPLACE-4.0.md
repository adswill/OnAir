# Plugin marketplace (4.0)

The 3.9 store was a signed static registry that only maintainers could add to (`docs/PLUGIN-MARKETPLACE-3.9.md`). 4.0 adds a **community
marketplace** next to it. Anyone can upload a plugin from the app. Others can find it, like it and download it. The Store tab
(View > Plugins > Store) shows both sources with a switch at the top:

- **Community marketplace** (the default). Anyone with a GitHub account can publish. Plugins go live at once and are marked
  **unreviewed**.
- **Reviewed registry**: the 3.9 signed list, unchanged. Plugins there are listed, reviewed or verified by the maintainers.

Status: the server code, the app and the tests are done. **Nothing is deployed yet.** The owner runs the one-time setup below. Until the
public key is in a build, that build says "the community marketplace is not set up in this build yet" and contacts nothing.

## For users

- **Browse.** Use the search box, the sort menu (Relevance, Most liked, Most downloaded, Newest) and the tag chips. By default only
  plugins for your system are listed; tick "Other systems too" to see the rest. Each card shows the title, the author's GitHub login,
  likes, downloads, tags and the amber `[unreviewed]` badge.
- **Plugin page.** It shows the description, licence, kind, the permissions the plugin says it needs, and a link to its source
  repository if the author gave one. It has Install (or Update, or Remove), Like / Unlike and Report.
- **Install** opens a second confirmation. It explains that the plugin is unreviewed native code that runs with the program's full
  rights, and you must tick "I understand" first. After installing, the plugin stays off until you turn it on in the Installed tab, as
  in 3.9.
- **Like**: one like per OnAir installation; press it again to unlike. **Report** needs a GitHub sign-in. Pick a reason (harmful, does
  not work, licence, spam, something else) and add a note if you want.

## For authors

1. Press **Upload plugin** and **Sign in with GitHub**. OnAir shows a code. Enter it at github.com/login/device. OnAir never sees your
   password. The server keeps no GitHub token.
2. Press **Choose file** and pick your `.onairplugin`. It is checked at once with the installer's own rules: the manifest, a library for
   every system named in it, and the zip layout. The result shows the id, version, kind, licence, systems and permissions.
3. Fill in the title and description (both required), and optionally a link to the repository (https://) and up to 8 tags. Fields
   already in the manifest are filled in for you.
4. Check the preview and press **Publish**. The plugin is listed at once. Your GitHub login is shown as its author.
5. **New version**: upload a package with the same id and a higher version. The app sees that the id is yours and publishes it as the new
   version. Only the account that first published an id can upload new versions of it. Ids that start with `onair` are kept for the
   project (`OWNER_LOGINS`).

The server refuses a package with a bad manifest, a wrong ABI, a missing library or an unsafe zip. It also refuses packages that break
the 3.9 listing policy: a licence that is not open, patented voice codec files or text, encryption-cracking tools, or references to
credential or system files without the `filesystem` permission. Limits: 25 MiB per package and 64 MiB unpacked. Each account may publish
10 packages a day, and 20 a day may come from one address.

## Owner's setup checklist

The exact commands are in `tools/marketplace/server/README.md`.

- [ ] `npm install` and `npx wrangler login` in `tools/marketplace/server` (the on-air.dev Cloudflare account)
- [ ] R2 bucket `onair-plugins` (leave public access off)
- [ ] D1 database `onair-plugins`: paste its id into `wrangler.jsonc`, then apply `schema.sql`
- [ ] GitHub OAuth app (fields in the README; **Enable Device Flow** ticked; no client secret): paste its Client ID into `wrangler.jsonc`
- [ ] `npx wrangler deploy` (creates `plugins.on-air.dev`)
- [ ] Secrets: `SIGNING_KEY` (from `npm run keygen`), `SESSION_SECRET`, `HASH_SALT`, `ADMIN_TOKEN` (keep it in a password manager)
- [ ] Put the public key from `npm run keygen` into `kBuiltInMarketKeyHex` (`core/src/pstore/market.cpp`) for the next release
- [ ] `curl https://plugins.on-air.dev/api/v1/status` shows `signing: true`, `github: true` and the same public key

## Privacy

What the server stores:

- for each plugin: its title, description, link, tags, files and versions, and the **GitHub user id and login of its uploader**
- the **GitHub id and login of each reporter**, with the report
- **likes**: a salted hash of the install id per plugin. The install id is a random number OnAir makes once and keeps in
  `plugins/.market/install-id`. The salt is a server secret, so the database alone cannot be matched to an installation.
- **download counts**: one count per installation (salted hash) per plugin per day, or per address hash when no install id is sent.
  These rows are deleted after two days; only the total stays.
- **rate limits**: salted hashes of IP addresses with a counter, deleted when their hour or day is over.

Nothing else is kept: no accounts beyond the GitHub login, no tracking, no analytics. Browsing and downloading need no account.
Cloudflare, as the host, sees your IP address. OnAir contacts the marketplace only when you open the community panel (one list
request), search, open a plugin, like, install, sign in, upload or report. The panel says so.

On your computer, `plugins/.market/` holds the install id, the list of plugins you installed from the marketplace, the last list of
withdrawn ids, and `session.json`. That file holds the marketplace session token (valid 30 days, private file mode 0600 on macOS and
Linux; under your user profile on Windows). Sign out deletes it.

## Moderation

With the admin token (curl commands in the server README):

- **list reports**, and close them
- **pull** a plugin: it disappears from the list and its page, its downloads answer "withdrawn", and every OnAir that installed it from
  the marketplace switches it off at its next list refresh. It uses the 3.9 kill switch (`plugins/revoked.json`), so the plugin stays
  off without network. **Restore** undoes it.
- **ban** a GitHub account (by login; the numeric id is kept too, so renaming the account does not escape the ban). A banned account
  cannot sign in, upload or report. `pull_plugins: true` pulls all its plugins at once. **Unban** undoes it.

## Security (the honest version)

- **Unreviewed means unreviewed.** A community plugin is native code from a stranger. It can do anything OnAir can do on your computer.
  The permissions in its manifest are what it *says* it needs; nothing enforces them yet, because the sandbox for untrusted plugins
  (`docs/PLUGIN-SDK.md`) does not exist. The server's checks are heuristics that catch mistakes and the lazy cases, not malware. That is
  why the badge, the second confirmation, Report and Pull exist.
- **The signature proves the answer came from the marketplace server, not that a plugin is safe.** Every list, plugin page and download
  answer is signed with the marketplace's Ed25519 key (the `SIGNING_KEY` Worker secret) and names the request it answers and the time.
  OnAir verifies it with the built-in public key, using the same Ed25519 code as 3.9. A changed answer is refused, and so is an answer to
  another request or one older than 7 days. The package must match the signed sha256 and size, and its manifest must match the signed
  page (id, version, kind, no more permissions), before anything is written. So a tampered CDN, proxy or mirror cannot slip in another
  file. If someone takes over the Worker or its secret, they can sign anything until the key is replaced in a new OnAir release.
- **A pull only reaches computers that refresh.** An offline computer keeps running the plugin until it next opens the community panel.
- **GitHub sign-in** goes through the device flow with no scopes. The server uses the GitHub token once to learn the user id and login,
  then drops it. The app gets an HMAC-signed session token from the server and keeps it in `session.json`. Anyone who copies that file
  can upload as you until it expires (30 days) or `SESSION_SECRET` is changed.
- **Like and download counts can be gamed** by someone with many addresses. The limits (30 likes an hour per installation, 120 per
  address, download counting stopped after 300 an hour per address) make it slow, not impossible. The counts are a hint, not a review.
- The 3.9 reviewed registry is unchanged. Use it when you want a person to have looked at the code.

## In this repository

| Path | What |
| --- | --- |
| `tools/marketplace/server/` | the Worker (`src/`), `schema.sql`, `wrangler.jsonc` (bindings and placeholders only), README with the setup, tests |
| `core/include/dect2/pstore/market.h`, `core/src/pstore/market.cpp` | the client: signed answers, browse, likes, install, sign-in, preview, upload, report |
| `core/src/pstore/store.cpp` (`setExtraRevocations`) | withdrawn marketplace plugins join the 3.9 revocation list |
| `app/pstore/market_ui.cpp`, `app/pstore/store_ui.cpp` | the community panel and the Upload plugin window; the source switch in the Store tab |
| `app/platform/platform*.{h,cpp,mm}` (`httpCall`) | HTTP with any method, a content type and private headers (curl on Linux and Windows, NSURLSession on macOS) |
| `tests/pstore/test_pstore_market.cpp` | the client against an in-process fake of the server API |
| `.github/workflows/market-server.yml` | the server tests on GitHub |

## Tests

- **Server** (`npm test` in `tools/marketplace/server`; 32 Node tests). These run every endpoint against a real SQLite running
  `schema.sql` (sql.js), an in-memory R2 and a scripted GitHub. They cover: signed answers (the signature verifies, a changed byte fails
  it); uploading without a sign-in or with a forged or expired session; required fields and their limits; oversized uploads (the size
  is checked before and after reading); bad manifests (the client's rules); a zip bomb and a zip whose header understates an entry;
  CRC damage; unsafe names; the licence, codec, cracking and sensitive-path checks; taken, own and reserved ids; new versions (higher
  only, owner only, same kind); the upload rate limit; download counting (once per install a day; flooding from one address stops
  counting); likes (once per install, unlike, flooding from one install and from one address, install ids stored only as hashes);
  reports and the admin list; pull and restore with and without a public files domain (downloads, files and likes answer 410 while
  pulled); bans (a renamed account stays banned, pull_plugins); the device flow (pending, ok, the GitHub token is stored nowhere, a
  banned account gets no session); sign-in rate limits; search relevance, sorting, tag, kind and platform filters, `%` and `_` taken
  literally, paging; the hourly clean-up. `npm run test:workerd` repeats the main path in the real Workers runtime (Miniflare's workerd
  with its local D1 and R2), with the multipart body exactly as the C++ client writes it. All pass on GitHub (`market-server.yml`, run
  43157632653: type check, 32 of 32, workerd smoke), and they passed locally too. The workflow runs on every push that touches the server.
- **Client** (`ctest -R pstore_market`). This runs against an in-process fake of the API. It covers: refused answers (tampered, wrong
  key, another request, another kind, replayed after 8 days, from the future, unsigned); a build without the key making no request at
  all; the install id (32 hex, kept, sent with likes and downloads); install only after the confirmation; update; downgrade refused;
  a broken update rolled back; refusals that leave nothing on disk (wrong sha256, size lie, non-https address, another plugin's
  package, more permissions, another kind, other platform, too-new minimum version, unknown permission, withdrawn, tampered page,
  unknown id); a withdrawn plugin switched off through the Store's kill switch, and on again after a restore; the sign-in (the address
  shown is always github.com, the session file is private, kept across restarts, dropped on a 401 and on sign-out); the upload
  preview (good, missing library, not a zip, bad id, no manifest); the multipart body and the bearer header; the new-version path;
  report escaping. Built and run on GitHub `quick.yml` only (no local builds on battery): `pstore_market` and `pstore_store` pass, and the
  whole app (`dect2`, with market_ui.cpp and both `httpCall` versions) builds on Linux (run 43151317970), macOS (43151322480) and
  Windows (43151326598).
- **Not tested**: the deployed Worker against real GitHub and Cloudflare (nothing is deployed), and `plat::httpCall` against a live
  server (it builds on Linux, macOS and Windows in CI; its curl flags match `httpRequest`). The ImGui panel was not checked on screen:
  it compiles in the CI app build only.

## Notes for later

- The Settings > Privacy text was not changed. It still describes only the 3.9 registry, and changing the English text would drop that
  row's translations. This page and the panel itself state what the marketplace sends.
- The panel's new strings have no translations yet; they show in English.
- Screenshots are not part of a listing yet.
