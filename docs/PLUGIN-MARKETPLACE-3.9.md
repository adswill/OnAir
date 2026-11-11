# Plugin marketplace (3.9)

**Design in two sentences.** The marketplace is a *static registry*: a signed `index.json` plus one page and one package per plugin, hosted as plain files on any HTTPS host (GitHub Pages is enough) or in a local folder, so no server exists to run or attack. OnAir's Store tab (View > Plugins > Store) reads it only when asked, accepts it only if the index is signed with the registry's Ed25519 key, checks every package against the sha256 in the signed listing before a byte reaches the plugins folder, and then uses the existing install path, watchdog, helper process and safe mode.

Status: the registry is live at https://adswill.github.io/onair-plugins/ (repository adswill/onair-plugins). It is signed and empty: no plugins are listed yet. Its public key is built into OnAir (`kBuiltInRegistryKeyHex` in `core/src/pstore/store.cpp`); the private key is kept offline by the owner. The section below, "Host a registry", is for anyone who wants a custom registry.

## 1. The registry

```
index.json                 {"schema":1, "serial":N, "generated":"...", "registry":"...", "plugins":[...], "revoked":[...]}
index.json.sig             128 hex digits: Ed25519 over the exact bytes of index.json
plugins/<id>/plugin.json   per-plugin page
packages/...               optional; a page may point to any https URL (a GitHub release asset)
```

**Index entry** (everything the list needs): `id, name, summary, version, author, license, kind (mode|action|source|panel-lite), min_onair, abi_version, review (listed|reviewed|verified), permissions[], categories[], tags[], platforms[], manifest:{path, sha256}`. The `sha256` of the page is inside the signed index, so the page is covered by the signature without being signed itself.

**Plugin page** (`plugin.json`, fetched when a plugin is opened): `id, version` (must equal the index), `description` (long), `author, license, homepage, source, screenshots[]`, `downloads` per platform `windows-x64 | macos-arm64 | macos-x86_64 | linux-x86_64 | linux-aarch64`: `{url, size, sha256}`. `url` is `https://...` or a path relative to the registry (never `..`).

**Revocations**: `revoked: [{id, version: "*" | "1.2.0" | "<=1.2.0", reason}]`, part of the signed index (maintainers keep them in `revoked.json`).

**Serial**: a whole number that rises with every published index.

Version history is not kept in the index: one current version per plugin. Older packages can stay at their URLs; OnAir never installs a lower version than the installed one.

## 2. Trust and security

Plugins are native code, so the design assumes a plugin can be bad and limits *who can get one onto the computer* and *how fast it can be pulled back*.

| Threat | What stops it |
| --- | --- |
| A tampered or substituted list (host breach, MITM, mirror) | Ed25519 signature on the exact bytes of `index.json`. The public key is built into OnAir (`kBuiltInRegistryKeyHex`); a custom registry needs its own key typed under Advanced. A list that fails is ignored and the previous one stays. |
| A tampered package or page | sha256 and size from the signed index (pages) and the signed-index-covered page (packages), checked before anything is written. The plugins folder is only touched through `Manager::installBytes` after the hash, the package manifest (id, version, kind, ABI, permissions not above the listing) and the user's confirmation. |
| An old list replayed (rollback of a revocation, a downgrade) | `serial` must not be lower than the highest seen (kept in `plugins/.store/store-state.json`); the installer refuses a version lower than the installed one. |
| A wrong or too-new plugin | ABI, minimum OnAir version and platform are checked on the list (hidden, with a count) and again at install; the loader checks the ABI again at load. |
| A plugin that is found to be bad later | **Kill switch**: the revocation list. At the next refresh (and at every start from the saved list, without network) the plugin is stopped, shown as failed with the registry's reason, and cannot be turned on. Written to `plugins/revoked.json`, so it applies even to a sideloaded copy. |
| A bad update | The old version is copied aside first and restored if the new one fails to install or does not check out. |
| Surprise permissions | The permissions are shown in plain words in the plugin page and again in the install dialog, before anything is downloaded to disk. A package that asks for more than the listing shows is refused. |
| Blind clicking | Install needs an explicit tick "I understand that this plugin is native code" next to the review level. The core API refuses without it. |
| A plugin that misbehaves once running | Unchanged: 2 s watchdog, optional helper process, crash marker and safe mode (`docs/plugins.md`). |

Crypto: OnAir already contains a unit-tested Ed25519 (RFC 8032) and SHA-256 (`core/src/mesh/mesh_crypto*.cpp`, used for MeshCore); the store reuses them rather than adding a second implementation. The maintainer tool `onair_registry.py` has its own pure-Python Ed25519 (RFC 8032 section 6 reference algorithms, public domain; credited in `THIRD_PARTY.md` and `tools/credits_data.tsv`) that checks itself against RFC 8032 test vector 1 on every start, and the C++ test verifies a signature the Python tool made (so both implementations agree).

**Network and privacy.** Opt in by action: OnAir contacts the registry only when you press Refresh / Check for updates, open a plugin's page, or install. Nothing at start-up, nothing in the background, no account, no telemetry, no tracking ids. Each request is an HTTPS GET (certificates verified; plain `http://` is refused) with `User-Agent: OnAir-Store/<version>`; the host therefore sees your IP address, the time and the files you asked for. This is stated in the Store tab, in Settings > Privacy and here. The registry address and key are under Store > Advanced.

**Honest limits.**
- There is **no sandbox for untrusted plugins yet** (planned: macOS sandbox profile, Linux seccomp, Windows has no helper process at all, see `docs/PLUGIN-SDK.md`). An in-process plugin has the program's rights. The Store says so in the tab and in the install dialog.
- `listed` = meets the rules (validator); `reviewed` = a person looked at the source; `verified` = reviewed and rebuilt from the tag. **None of these is a guarantee.**
- A signed registry proves that the *maintainer* published it, not that the plugin is harmless. The signing key is the root of trust: lose it and you must ship a new OnAir with a new key; leak it and an attacker can publish plugins until a new build ships.
- Revocation reaches only computers that refresh. A computer that is offline keeps running the plugin.
- `https` trust relies on the system's certificate store (the signature does not depend on it).

**Listing policy** (`tools/marketplace/sample-registry/POLICY.md`, copied into the registry): open or freely redistributable licence with an SPDX id; no patented codecs or codec builds (users install codecs themselves, `docs/CODECS.md`); no encryption-cracking tools (decoding with the user's own imported key is fine); permissions declared, and a plugin that reads files outside its own folder must declare `filesystem`; honest description; artefacts match the page; versions only go up.

## 3. Submission

An author publishes `.onairplugin` files as release assets of their own repository and sends a pull request to the registry repository with `plugins/<id>/plugin.json` (`CONTRIBUTING.md` in the sample registry). CI runs `onair_registry.py lint . --fetch`, which checks per plugin: schema and required fields, id, version, ABI (must be 1), kind, permission names, https links, the SPDX licence (open licences only), the policy words (codecs, cracking), and for every platform the download: size, sha256, that it is a zip with a safe layout, that the package manifest equals the page (id, version, kind, ABI, permissions not above the page, licence), that the library for the platform is present, no codec files, and no references to credential or system paths without the `filesystem` permission. Heuristics only: a maintainer reads the rest. Pull requests may not touch `index.json` / `index.json.sig`.

The maintainer merges, then runs `index` (builds `index.json` from the pages and `revoked.json`, computing page hashes), `sign` (with the offline key) and `verify`, and pushes. `onair_registry.py keygen` refuses to write the private key inside a git work tree.

## 4. In this repository

| Path | What |
| --- | --- |
| `core/include/dect2/pstore/store.h`, `core/src/pstore/store.cpp` | the client: fetch (https or local folder), verify, cache, filter, detail, install with rollback, updates, uninstall, revocations |
| `core/src/plugin/manager.cpp` (`setRevoked`) | the kill switch: `plugins/revoked.json`, revoked entries are invalid and cannot be enabled |
| `app/pstore/store_ui.cpp`, `app/plugin/plugin_app.cpp` | the Store tab beside Installed in View > Plugins; background thread for network work |
| `tools/marketplace/onair_registry.py` | lint, index, sign, verify, keygen, artefact |
| `tools/marketplace/sample-registry/` | a complete registry (3 stub packages, 5 platforms each, about 90 KB), signed with a published test key; policy, contributing guide and CI workflow ready to copy |
| `tools/marketplace/make_sample_registry.py` | regenerates the sample |
| `tests/pstore/test_pstore_store.cpp` | the client against local mock registries |

Store tab: browse, search, category filter, trust badge per plugin (listed / reviewed / verified with its meaning), plugin page with author, licence, kind, permissions in plain words, project and source links, Install / Update / Remove, Check for updates, the privacy note, the registry address and key under Advanced. Screenshots listed in a page are not rendered yet. UI strings use `tr()`/`trw()` with German added. No settings-table rows or web-schema entries: the address and key live in the Store tab only (preferences `storeRegistryUrl`, `storeRegistryKey`). The plugin manager is not built on Android, so nothing was added there.

## 5. Tests (`ctest -R pstore_store`)

Against local registries built on the fly and signed with a test key; no network host is used. Good install (and refusal without the confirmation), tampered index, flipped signature, wrong key, no key configured, wrong sha256 (nothing written), `..` in a download path, a package asking for more permissions than listed, wrong ABI, too-new minimum version, other platform, platform filter, update, downgrade refused, replayed older index refused, mismatching package refused, damaged archive with a valid hash (the old version stays), revocation (reason shown, cannot enable, survives a restart, lifted later), offline cache with its notice, damaged cache ignored, uninstall; and the checked-in sample registry (signed by the Python tool) verifies and installs a stub. What is **not** tested: real HTTPS (the HTTP client has its own tests in `iptv`), the ImGui tab (checked to compile with g++-16 `-fsyntax-only` only), and no real plugin binary is installed by these tests (the stubs are text files; the three real examples already load in `tests/plugin`).

## Host a registry (the project's own steps, for a custom registry)

The project's registry was set up this way and is already live; follow the steps with your own key and address for a custom one.

1. Make the key on an offline computer: `python3 tools/marketplace/onair_registry.py keygen /Volumes/USB/onair-registry.key`. Back it up twice. Never commit it. It prints the public key.
2. Put the public key into `kBuiltInRegistryKeyHex` (`core/src/pstore/store.cpp`) and set `kDefaultRegistryUrl` to the address. A build without a key says the key is not published and fetches nothing.
3. Create the public repo (suggested `adswill/onair-plugins`) and copy `tools/marketplace/sample-registry/*` into it, but **without** the three stub plugins, `packages/`, the test key and the sample `index.json*`; copy `tools/marketplace/onair_registry.py` to its `tools/`, and `sample-registry/.github/` to its root `.github/`.
4. Add the first real plugins (or none), then `lint`, `index --serial 1`, `sign`, `verify`, commit, push.
5. Enable GitHub Pages (branch `main`, root). The address is `https://adswill.github.io/onair-plugins/`. Check that `.../index.json` and `.../index.json.sig` open in a browser. (Any other HTTPS host works the same; a custom domain survives a change of host.)
6. Branch protection on `main`: pull requests only, the Validate workflow required, you as the only one who can push `index.json*`.
7. Publish the next OnAir with the key. Test once from a clean install: Store > Refresh, install a plugin, remove it.
8. Revoking: add an entry to `revoked.json`, `index --serial N+1`, `sign`, push. Users get it at their next refresh.
9. Key rotation: ship a new OnAir with the new key first, keep signing with the old one until that release is common, then switch (there is only one key in a build; plan this before it is needed).
