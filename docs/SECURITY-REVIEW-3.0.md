# Security review of the new 3.0 network-facing and privacy-relevant code

Date: 2026-10-23. Method: read-only review of each pushed branch through `git show pub/<branch>:<path>` (no merge, no CI, nothing started).
Small fixes are NOT applied to this branch's tree (the code is not in `main` yet): they are patches against the named source branch in
`docs/security-patches-3.0/`, each with a test. The integrator applies them on the source branch (`git apply docs/security-patches-3.0/NN-*.patch`).

Ratings: HIGH = exploitable with one user action and a serious effect; MEDIUM = real weakness, needs a precondition or has a limited effect;
LOW = hardening / defence in depth; INFO = a note, not a defect.

## Findings

### HIGH

**H1. backup-030: a crafted `.onairbackup` can write any file under the data folder, including `codecs/` (libraries that OnAir loads with dlopen / LoadLibrary).**
- `core/src/backup/backup.cpp`, `checkManifestAgainst()` and `planRestore()`/`extractTo()`; UI in `app/panels/backup_ui.cpp` (`makePlan`, `doRestore`).
- The manifest lists `file<TAB>name<TAB>bytes<TAB>item`. `checkManifestAgainst` checks that `name` is a safe relative path and that `item` exists, but not
  that `name` is a place that item may use (`itemOfPath(name) == item`). `planRestore` then writes every manifest file whose item is ticked. "Presets" and
  most items are ticked by default.
- Reproduction idea: build a stored zip with `manifest.txt` containing `file<TAB>codecs/libmbe.dylib<TAB>4<TAB>presets` and the entry `data/codecs/libmbe.dylib`;
  restore with the default ticks; the next "Look again" in the codec help loads the planted library (code execution as the user). Same for any other
  file the program reads from its data folder (`automations/` scripts are in the catalog, so they are by design).
- Path traversal, symlinks, zip bombs (only stored entries, sizes and totals limited, overlap checked), CRC and encryption wrapper are fine; this is the one gap.
- Fix: patch `02-backup-030-manifest-item-paths.patch` (one line in `checkManifestAgainst` + test).

### MEDIUM

**M1. external-api-030: the rigctl TCP server can be driven by a web page (cross-protocol request).**
- `core/src/net/api_servers.cpp` (`RigctlServer::serve`) + `core/src/net/api_policy.cpp` (`rigctlHandle`). No authentication (as in rigctld), default loopback.
- A page in the user's browser can POST `text/plain` to `http://127.0.0.1:4532/`; the request line and every header line answer `RPRT -4`, but body lines such as
  `F 145500000` / `M USB 2400` are executed. Effect: the page retunes the radio. It cannot transmit (`T`/`S` are refused).
- Fix: patch `01-external-api-030-rigctl-http-guard.patch` (hang up on a line containing ` HTTP/`) + test. With `allowLan` the server stays unauthenticated by the protocol: keep it documented as trusted-network only.
- The HTTP API itself is good: Host must be a loopback name on loopback (DNS rebinding), a foreign Origin is refused, JSON content type required for POST, constant-time token compare over 256 bytes, rate limit, size limits, route parser rejects `//`, `/..`, `%`, backslash and non-ASCII.

**M2. webonair-030: the web policy decides "data" versus "voice/pager" by a name list, so everything not on the list is a data decoder and is ON for all web listeners by default.**
- `server/web/web_policy.cpp` `decoderClass()`; the host decoder uses the running mode id as the kind (`server/main.cpp` `HostDecoder::policyKind`).
- Mode ids that carry private messages or voice but are not in the `voice`/`pager` sets: `freedv`, `hamdrm`, `meshtastic`, `meshcore`, `meshcom`, `lorawan`, `loraaprs` and any future id (also TETRA/DMR variants spelled differently).
  A listener who subscribes to `host` while the receiver sits in such a mode receives its telemetry (decoded text) without the host having accepted the legal notice.
- Fix (not small, decision needed): make the classification fail closed, i.e. a kind is "data" only when it is in an explicit allow-list (the `decoderCatalogue()` data entries plus data mode ids); everything else is restricted.
- Related (could not fully verify, see below): the notice text says "OnAir never decodes encrypted traffic", but 3.0 adds the voice key store; if keys are loaded and a voice kind is enabled, decrypted speech/data would be offered to web listeners. The headless server does not call `loadFromFile` in the code I read, so this is a policy/wording risk today, not a live path.

**M3. backup-030: a crafted backup can set any application setting (settings.txt is applied key by key).**
- `app/panels/backup_ui.cpp` `doRestore` -> `applyPref`; `core/src/backup/backup.cpp` `parsePrefs`/`selectPrefs`.
- A backup from someone else can switch on network exposure (remote API / rigctl / web server on the LAN), change the map-server URL, upload targets, etc. Secret-looking keys are meant to live only in the passphrase block, but nothing stops a plain-section key with such a name.
- Suggested fix: on restore ignore plain-section keys for which `isSecretPrefKey()` is true, and ask for a second confirmation (listing the keys) for a short deny-list of exposure settings. I did not verify the actual pref key names, so no patch.

**M4. bluefile-030: the converted cf32 copy is written to a predictable name in the shared temp folder.**
- `core/src/source/iq_file.cpp` `probeBlue()`: `temp_directory_path()/onair-blue-<hash(path, mtime, size)>.cf32`, reused when it exists with the expected size, created with `fopen("wb")` (follows symlinks).
- On a multi-user Linux machine another user can pre-create a file of the right size (data injection into the decoder) or a symlink (overwrite of a file the victim can write). macOS and Windows temp folders are per user, so the exposure is Linux multi-user only.
- Fix: put the copy in the per-user cache folder (`plat::cacheDir()`), or create it with `O_EXCL|O_NOFOLLOW` in a 0700 subfolder. The same note applies to `onair_txstudio_<ns>.cf32` in `core/src/tx/tx_studio.cpp` (name is a nanosecond clock, much harder to guess).
- The BLUE parser itself is sound: every read is bounds-checked, sizes come from `fileSize`, keyword records are walked with `lkey`/`lextra` checks and a 100000-record guard, extended header limited to 4 MB, no allocation is sized from header fields. One hardening: reject a sample rate outside 1 Hz .. 1e12 Hz in `probeBlue` (`xdelta` of 1e-300 gives 1e300).

### LOW

**L1. webonair-dir-030 (Worker): without the `IP_SALT` secret the rate-limit key is `sha256("unsalted|<ip>")`**, a fixed function of the address that can be reversed over the IPv4 space from the `rl` rows (kept up to two days for the "new registration" key). `web/onair-directory/src/index.js` `ipKey()`. Patch `05-webonair-dir-030-ip-salt.patch` uses a random per-instance salt when the secret is missing.

**L2. webonair-dir-030 (Worker), protocol details.**
- Replay: `ts` must be within 300 s of the clock and newer than `last_ts`, but `last_ts` is only known while the row exists. A captured *heartbeat* can be replayed after the owner's signed *delete* (within 300 s) and re-lists the server. Fix: keep a tombstone (id, last_ts) for 10 minutes on delete.
- First registration trust is first-come: anyone can claim an unused id with their own key; later requests need that key. Ids are chosen by the server operator (derived from the public key in the C++ client), so squatting only matters for id strings an attacker can guess. Listing names are free text (shown with `textContent` on the page: no XSS).
- Reachability probe: the DoH check runs only when the URL changes, the hourly probe does not re-resolve (DNS rebinding window between check and fetch); Workers cannot reach private ranges anyway, and the answer is reduced to one boolean (`name` string + `proto` number must be present). Any public host and port >= 1024 can be probed once per hour per registered record (a weak port-scan oracle).
- Admin: constant-time compare, per-IP limit 30/min on every request, but the `admfail:` counter is written and never read; no lock-out beyond the 30/min limit (use a long random `ADMIN_TOKEN`).
- SQL: all statements use bound parameters; the only interpolated name (`${col}`) comes from a fixed map. CORS `*` only on public GET answers; admin answers carry no CORS headers and the preflight allows only `content-type`.
- The page script `maplibre-gl` comes from unpkg without Subresource Integrity and the CSP allows any script on `https://unpkg.com` (`public/index.html`, `public/_headers`). Same in `web/map/public/index.html`. Pin an `integrity=` hash or self-host.

**L3. web-map-030.** All track text is escaped with `esc()` before `innerHTML` (quotes included), photo credit links are restricted to planespotters.net, photo requests are gated by the consent popup (`photoAllowed`) and nothing is stored before the choice. Third-party requests that are made *before* the choice and are not covered by the popup: `unpkg.com` (maplibre JS/CSS) and `demotiles.maplibre.org` (glyphs), plus OpenStreetMap tiles in demo mode. They need no consent to function, but they do disclose the visitor's IP; say so in the privacy text or self-host.

**L4. ham-map-feed-030: callsign parts around a slash are not validated.** `hamCallValid()` checks only the longest slash-separated part, so `DL1ABC/"},"x":{"` passes and `to` goes unescaped into the `extra` JSON (`core/src/ham/ham_mapfeed.cpp` `extraJson`). The outer JSON is escaped (`jsonEsc`) and the map page escapes its output, so the effect is a tampered or broken `extra` hint, not XSS. Patch `04-ham-map-feed-030-callsign-chars.patch` allows only A-Z, 0-9 and `/`. Privacy of the feed itself is good: double opt-in (master + ham, each with its own consent), 4-character locator centres by default, no logbook notes, no own location, rate limit, de-duplication, queue dropped when switched off.

**L5. voice-keys-030 / voice-keys-gui-030 key store.**
- `core/src/voicekeys/key_persist.cpp`: the PBKDF2 round count is read from the file and was not bounded (4 billion rounds hangs the loader on an imported file). Patch `03-voice-keys-030-pbkdf2-rounds.patch` bounds it to 1000..5,000,000 (the writer uses 200,000). The file is new in 3.0, so these can also be done in the format: authenticate the header (flag, salt, rounds, iv) in the HMAC and derive separate AES and HMAC keys from the master (today the same 32 bytes feed both). Without header authentication an attacker with write access can change the IV; the result is garbage plaintext, not a chosen change, so I rate it low.
- `keychainMasterKey()` creates and stores a NEW master key whenever the keychain read fails for any reason (locked keychain, denied access), replacing the old one: the existing key file then no longer opens (loss of the user's keys, not a leak). Create a new key only on "item not found".
- The key file is written with default permissions (`std::ofstream`); it is encrypted, but 0600 costs nothing. CSV export writes the name unescaped (`"` inside the name breaks the row; a name starting with `=` is a formula in spreadsheets).
- Nothing leaks key bytes into logs, the diagnostics text, the bug-report zip (`writeZip` drops `isKeyMaterialPath`, and the zip only gets `diagnostics.txt`, `onair.log`, `onair-prev.log`, `crash.txt`), the remote API, the web server (it serves only `web_root`), or the plain backup (keys only in the passphrase-sealed block; "Voice keys" is not ticked by default on restore). The per-call status line carries the key *name*, not the key. Providers store only the 32-byte master key (macOS generic password, Windows Credential Manager `CRED_PERSIST_LOCAL_MACHINE`, Linux libsecret).

**L6. webonair-030 (server), notes.**
- Good: `web=0` default, loopback bind by default, plain HTTP only for private addresses unless `web_plain_internet=1`, TLS 1.2+ (shared context), request head/body/idle timeouts, 48 header cap, no chunked bodies, strict URL decoding and a segment-whitelist static path with canonical-path check (no dot files, no symlink escape), password >= 8 characters, invite tokens >= 16 characters, constant-time compares, failed-attempt limiter (5 per minute per address, then 60 s), admin token auto-generated and the config file saved 0600, admin API loopback-only unless `web_admin_lan`, per-IP and total connection caps, per-session CPU and bandwidth budgets with kick, message token bucket, WebSocket frames must be masked and unfragmented, payload limited before allocation, JSON parser with depth/node/size limits, display names stripped of markup, session addresses shown as a salted tag unless `web_log_ip`. Listener isolation: every `tune` stays inside the shared capture and changes only that session's state.
- Weak spots (LOW/INFO): Origin is accepted when it equals the `Host` header, so a DNS-rebinding page passes the origin check (only matters on an `open` server that is already open to the LAN; admin still needs the token). The static handler reads a whole file (up to 16 MB) per request, times the connection cap. The login limiter is shared between listeners and the admin API, so one noisy peer behind a NAT can lock the admin out for a minute. `newToken()` uses `std::random_device` (fine on the toolchains in CI: UCRT64 GCC, libc++); prefer `/dev/urandom` / `BCryptGenRandom` as the directory client already does for its seed.
- Compile-time TX guard: `tx/tx.h`, `tx/tx_iqfile.h` and the `TxSink` section of `source.h` are `#error`/`#ifndef` guarded under `ONAIR_WEB_SESSION`, and ctest `web_tx_guard` expects the compile to fail. Sound for the headers it names; it does not stop web code from declaring its own extern to a function in `dect2core` (nothing does today). `onair-server` (`server/main.cpp`) itself is not built with the define.
- TLS client for the directory heartbeat (`core/src/net/tls_conn.cpp`): TLS 1.2+, `SSL_VERIFY_PEER`, host name checked with `SSL_set1_host`, SNI set. The heartbeat shows the exact record and needs `web_directory_ack=<code>` before anything is sent; the signing key file is 0600.

**L7. presets-030.** Imports are validated against a fixed key list with type and range checks, secret-looking keys are dropped on import and export, location is omitted from exports unless chosen, the JSON parser has a depth limit (32), file names for export are sanitised, nothing is executed and no path is taken from the file. Remaining: an imported preset can set `biasTee` (antenna power) without a prompt, and `spec.fftSize` up to 1,048,576 with `spec.avg` 1000; confirm bias-tee changes that come from an import. Preset count per store is not limited.

### INFO / checked and found sound
- **web UI (webonair-ui-030, `web/onair-web/`)**: no `innerHTML`, `eval`, `document.write` or inline script anywhere; all rendering through `textContent`; localStorage holds only UI preferences, the display name and bookmarks (never the password or invite; the invite fragment is removed from the address bar); the server's CSP is `script-src 'self'`.
- **datapacks-030**: https only, allow-list match on a path boundary (rejects `host.evil`, `host:port`, user-info, backslash), size and SHA-256 checked before anything is written, then the schema validation runs again on import, atomic write. The checksum comes from the same manifest as the URL, so it protects against corruption and a swapped download host, not against a malicious manifest source (the user picks the source). `curl` follows up to 5 redirects, https only, but the bytes still have to match the manifest hash.
- **tx-studio-030**: confirmed it cannot transmit: it generates into memory, writes files, and the loopback test starts a receive `Engine` on a `DeviceInfo::File` source. No `TxSink`, `makeTxSink` or radio open appears in `tx_studio.cpp`, `tx_measure.cpp` or the panel.
- **backup-030 crypto**: PBKDF2-HMAC-SHA256 with 64 bytes split into AES-CTR and HMAC keys, MAC over salt+iv+ciphertext, constant-time compare, rounds bounded 1000..5,000,000 on open. Good.

## Patches (apply on the named source branch)

| patch | branch | files |
| --- | --- | --- |
| 01-external-api-030-rigctl-http-guard | external-api-030 | core/src/net/api_policy.cpp, tests/net/test_api_policy.cpp |
| 02-backup-030-manifest-item-paths | backup-030 | core/src/backup/backup.cpp, tests/backup/test_backup.cpp |
| 03-voice-keys-030-pbkdf2-rounds | voice-keys-030 (and voice-keys-gui-030, same files) | core/src/voicekeys/key_persist.cpp, tests/voicekeys/test_voice_keys.cpp |
| 04-ham-map-feed-030-callsign-chars | ham-map-feed-030 | core/src/ham/ham_mapfeed.cpp, tests/ham/test_ham_mapfeed.cpp |
| 05-webonair-dir-030-ip-salt | webonair-dir-030 | web/onair-directory/src/index.js, web/onair-directory/test/handlers.test.js |

Verification state of the patches: 05 ran with `node --test` against the branch's own test file (11 of 11 pass). 01 to 04 were NOT compiled or run (the build guard kept the machine's two heavy-job slots busy and a hand compile needs the branch headers); each is a few lines on top of code that the existing tests cover, and each added test is written to fail without the fix. After applying, run `ctest -R "api_policy|backup|voice_keys|ham_mapfeed"`.

## What I could not check
- Nothing was built or run except the Worker tests and the backup test; no fuzzing, no sanitizers, no live TLS handshake.
- Not read: `server/web/web_dsp.cpp` (resampling and codecs; the ADPCM decoder in `web_proto.cpp` is bounded), `web_hub.cpp` lines 1-400 and 700-780 (DSP scheduling, budgets; the budget logic was only skimmed), `app/platform/keychain_linux.cpp`, `core/src/voicekeys/voice_crypto.cpp` / `voice_decrypt.cpp` (the RC4/DES/AES code and the P25/NXDN call-site changes), the Android and Windows packaging of the keychain, `web/map/src/index.js` (the map ingest Worker, which is older code), `integ-30`.
- Whether the headless `onair-server` loads the voice key store anywhere (M2, last bullet): I saw no call, but did not search the whole tree.
- The exact application setting names for M3, and whether `plat::cacheDir()` is per user on every platform for M4.
- Cloudflare behaviour (what a Worker `fetch` may reach; rate-limit row cleanup relies on the cron trigger).

## Fix status (2026-10-23, paused by the owner)
Fixes are committed on local branches of the fixing worktree; only the tested one is pushed. Nothing else was pushed to a source branch.
| source branch | fix | state |
| --- | --- | --- |
| webonair-dir-030 | L1 rate-limit salt, L2 replay after delete (tombstone) | PUSHED, `node --test` 24/24 |
| backup-030 | H1 manifest binding, code/library locations refused, symlink/non-file targets refused, M3 settings allow-list with per-key tick (UI) | committed (local secfix-backup, bcfce5dc8), NOT compiled or run, not pushed |
| external-api-030 | M1 rigctl HTTP/header hang-up, `\auth <token>` for F M S T | committed (local secfix-api, 3c2cb7640), NOT compiled or run, not pushed |
| webonair-030 | M2 allow-list of web-safe modes, registry classification test | committed (local secfix-web, ae9b93033), NOT compiled or run, not pushed |
| voice-keys-030, voice-keys-gui-030 | PBKDF2 round bound, failed keychain read keeps the master key, 0600 | committed (local secfix-vk 7ddce7a1b, secfix-vkgui cdc119f22), NOT compiled or run, not pushed |
| ham-map-feed-030 | L4 callsign characters | committed (local secfix-ham, d5c07fd6a), NOT compiled or run, not pushed |
| bluefile-030 | M4 private per-user folder + mkstemp, sample-rate range | committed (local secfix-blue, 95616b9b2), NOT compiled or run, not pushed |
Not done: unpkg integrity hash (the hash is not known and a download is not allowed; vendoring needs a download). Test harness snippets are in the session scratchpad (`scratchpad/sec`).

### Update (after the owner plugged in)
- external-api-030: PUSHED (f6f169051). Token only when "let other computers connect" is on (sess.required = cfg.allowLan); T 0 and S 0 stay free; HTTP and header lines hang up. Built test_api_policy, ctest -R api_policy passed.
- webonair-030: PUSHED (ae9b93033). Allow-list; HostDecoder uses the sub-mode id (ft8, wspr, sstv, wefax) as the kind, so the approved modes stay on. Built test_web_safety, passed. Same area as webonair-ranges-030 and webonair-decoders-030: the integrator merges server/web/web_policy.h and web_policy.cpp (new DecoderClass::Other, the lists in web_policy.cpp).
- discard-keyfile-030 (new branch on top of untested-secfix-vkgui-030): discardKeyFile() and a "Move the old key file aside" button in the Keys window. ctest -R ^voice_keys$ passed; the window code was not compiled (app target not built).
- backup, voice-keys, voice-keys-gui, ham, blue: merged by the integrator from the untested-secfix branches; the vk/vkgui merge needs discard-keyfile-030 on top.
- Still open: unpkg integrity hash.
