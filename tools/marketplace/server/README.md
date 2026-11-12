# OnAir plugin marketplace server

One Cloudflare Worker (`src/`), a D1 database (`schema.sql`) for the listings, likes, download counts and reports, and an R2 bucket for the
`.onairplugin` files. How it works for users and authors, privacy and the security note: `docs/PLUGIN-MARKETPLACE-4.0.md`.

Nothing in this repository deploys it or creates cloud resources. The owner runs the setup below once, on the Cloudflare account that
serves on-air.dev.

## One-time setup (owner)

Needs Node 20 or newer. All commands run in this folder.

```sh
cd tools/marketplace/server
npm install
npx wrangler login                     # the Cloudflare account of on-air.dev
```

1. **R2 bucket** (package files). Leave public access off; the Worker serves the files itself.

   ```sh
   npx wrangler r2 bucket create onair-plugins
   ```

2. **D1 database** and its tables.

   ```sh
   npx wrangler d1 create onair-plugins
   ```

   Paste the `database_id` it prints into `wrangler.jsonc` (replace `REPLACE-WITH-D1-ID`). Then:

   ```sh
   npx wrangler d1 execute onair-plugins --remote --file=schema.sql
   ```

3. **GitHub OAuth app** (sign-in for uploading and reporting). On GitHub: Settings > Developer settings > OAuth Apps > New OAuth App.

   | Field | Value |
   | --- | --- |
   | Application name | `OnAir Marketplace` |
   | Homepage URL | `https://on-air.dev` |
   | Application description | `Sign in to publish and report OnAir plugins` |
   | Authorization callback URL | `https://plugins.on-air.dev/` (the form needs one; the device flow does not use it) |
   | Enable Device Flow | **ticked** |

   Press Register application. Copy the **Client ID** into `wrangler.jsonc` as `vars.GITHUB_CLIENT_ID`. No client secret is needed: do not
   generate one. The app asks for no scopes, so a sign-in only tells the Worker the user's public id and login name.

4. **Deploy** (until the secrets are set, every endpoint that needs one answers 503 "not set up yet", so nothing unsigned is served).

   ```sh
   npx wrangler deploy
   ```

   The route `plugins.on-air.dev` (custom domain) is created on the on-air.dev zone.

5. **Secrets** (each command asks for the value or reads it from the pipe; none of them is ever written to a file in this repository).

   ```sh
   npm run keygen                                   # prints SIGNING_KEY (secret) and the public key
   npx wrangler secret put SIGNING_KEY              # paste the 64-hex-digit seed
   openssl rand -base64 48 | npx wrangler secret put SESSION_SECRET
   openssl rand -base64 48 | npx wrangler secret put HASH_SALT
   openssl rand -hex 32                             # keep this one in your password manager: it is the admin token
   npx wrangler secret put ADMIN_TOKEN              # paste it
   ```

   Keep a copy of the SIGNING_KEY seed offline (password manager). Put the **public key** into `kBuiltInMarketKeyHex` in
   `core/src/pstore/market.cpp`: builds without it show "the marketplace has no key yet" and fetch nothing. Changing SESSION_SECRET signs
   everyone out; changing HASH_SALT starts likes and download de-duplication from zero (counts stay); changing SIGNING_KEY needs a new
   OnAir release.

6. **Check**

   ```sh
   curl https://plugins.on-air.dev/api/v1/status
   ```

   Expect `"signing":true`, `"github":true` and `public_key` equal to `kBuiltInMarketKeyHex`.

Free-plan limits that matter: Workers 100 000 requests a day, D1 5 million rows read a day, R2 10 GB stored with no egress charge.

Optional: `FILES_BASE_URL` (a public custom domain on the bucket) lets downloads skip the Worker. Then a pulled plugin's files are moved
under `pulled/` so their public address stops working. Leave it empty unless traffic needs it.

## Moderation (admin token)

```sh
T='<admin token>'; H="Authorization: Bearer $T"; A=https://plugins.on-air.dev/api/v1/admin
curl -H "$H" "$A/reports"                                  # open reports (?status=all for every one)
curl -H "$H" -X POST "$A/reports/12/close"
curl -H "$H" -X POST -d '{"reason":"malware"}' "$A/plugins/<id>/pull"     # hidden at once, downloads refused, installed copies switched off at the next refresh
curl -H "$H" -X POST "$A/plugins/<id>/restore"
curl -H "$H" -X POST -d '{"reason":"spam","pull_plugins":true}' "$A/users/<github-login>/ban"
curl -H "$H" -X POST "$A/users/<github-login>/unban"
curl -H "$H" "$A/bans"
```

## API (`/api/v1`)

Signed answers are `{"signed": "<JSON text>", "sig": "<Ed25519, 128 hex>"}`; the JSON text holds `v`, `kind`, `request` (the path and query
it answers), `time` (unix seconds) and the data. The app verifies them with its built-in public key.

| Method and path | Who | Answer |
| --- | --- | --- |
| `GET /status` | anyone | `{signing, github, public_key}` |
| `GET /plugins?q=&sort=relevance\|likes\|downloads\|newest&tag=&kind=&platform=macos\|linux\|windows&page=&limit=` | anyone | signed list, plus `pulled` ids |
| `GET /tags` | anyone | signed, the 40 most used tags |
| `GET /plugins/<id>?install=<id>` | anyone | signed page with versions (size, sha256) and `liked` |
| `GET /plugins/<id>/download?version=&install=&format=json` | anyone | counts once per install a day; 302 to the file, or signed `{url, size, sha256}` |
| `POST /plugins/<id>/like`, `POST /plugins/<id>/unlike` (or `DELETE .../like`) `{"install": "<32-64 hex>"}` | anyone | `{likes, liked}` |
| `POST /auth/device`, `POST /auth/poll {"device_code"}` | anyone | GitHub device flow; poll ends with `{status:"ok", token, login}` |
| `GET /me` | signed in | your plugins |
| `POST /plugins` multipart `package`, `title`, `description`, `repo`, `tags` | signed in | `{id, version, sha256}` |
| `POST /plugins/<id>/versions` multipart (title etc. optional) | the author | new version |
| `POST /plugins/<id>/report {"reason": malware\|broken\|licence\|spam\|other, "text", "version"}` | signed in | `{ok}` |
| `/admin/...` | admin token | above |

Limits: 25 MiB per package (`MAX_PACKAGE_BYTES`), 64 MiB unpacked, 10 accepted uploads a day per account (20 per address, 60/120 attempts),
30 likes an hour per install and 120 per address, 10 reports a day per account, 30 sign-ins an hour per address, download counting stops
after 300 an hour per address (the files are still served).

## Tests

```sh
npm test               # TypeScript build, then every endpoint and the package checks against real SQLite (sql.js) and an in-memory R2
npm run test:workerd   # after npm test: a smoke run in the real Workers runtime (Miniflare's workerd, local D1 and R2)
```
