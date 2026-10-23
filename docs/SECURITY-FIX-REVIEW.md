# Read-only review of the eight untested security-fix branches (3.0)

Date: 2026-10-23. Method: `git diff pub/<source>-030 pub/untested-secfix-<name>-030` and reading the touched code with `git show`. Nothing was compiled or run.
Findings come from `docs/SECURITY-REVIEW-3.0.md`.

| branch | verdict |
| --- | --- |
| untested-secfix-backup-030 | looks right (closes H1 and M3); two notes |
| untested-secfix-api-030 | NEEDS CHANGE: closes M1 but breaks every normal rigctl client |
| untested-secfix-web-030 | looks right in design; needs one check (mode ids) and cannot be called safe to merge before a build |
| untested-secfix-vk-030 | looks right; one lock-out note |
| untested-secfix-vkgui-030 | identical to vk (same three files, same diff) |
| untested-secfix-ham-030 | looks right |
| untested-secfix-blue-030 | looks right |
| untested-secfix-dir-030 | looks right (note: `webonair-dir-030` already points at this commit, so diff against `~1`) |

## api-030: NEEDS CHANGE (regression)

The HTTP guard (`" HTTP/"` anywhere in a line, or a `Name:` header line, hangs up before any body line runs) closes M1. Bypass thinking: a browser always sends
`POST / HTTP/1.1` as the first line, which the server reads first, so the body line `F 145500000` is never reached; there are no long-name set commands
(`\set_freq` is not implemented, only `\chk_vfo`, `\get_powerstat`, `\auth`), so the F/M/S/T/V guard is complete.

Problem: the same branch also adds a token requirement. `core/src/net/api_servers.cpp` (the `sess.required` line in `RigctlServer::Impl::serve`):

    sess.required = cfg.allowLan || !cfg.token.empty();

and `app/platform/remote_api_app.cpp` sets `s.rigCfg.token = c.token` (in `remoteApiLoadPrefs`, `applyAll` and `remoteApiSection`). The API token is always generated
(`remoteApiNewToken()` when unusable), so `cfg.token` is never empty and `required` is always true. Every normal rigctl client (Hamlib `rigctl`, WSJT-X, GQRX-style
tools, loggers) sends `F`/`M` without `\auth` and gets `RPRT -1`: the rigctl server stops working for its whole purpose, also on loopback. The HTTP guard alone
is enough for M1 on the loopback.

Fix (one line, `api_servers.cpp`): require the token only when the server is open to the LAN, and keep the label text honest:

    sess.required = cfg.allowLan;          // loopback: the HTTP guard is the protection; LAN: changes need \auth <token>

Also in `tests/net/test_api_policy.cpp` the server test `ct.token = "..."` with `allowLan` false then expects "F refused without \auth": after the fix set `ct.allowLan = true`
for that block (the bind address then becomes 0.0.0.0; use a loopback connect, which still works) or drop the token assertions there and keep the unit tests of
`RigctlSession` (those construct `required = true` by hand and stay valid). Say in the UI/docs that with "let other computers connect" clients must send `\auth <token>` first.

Other notes: the `\auth` line is accepted with `+`/`;`/`|` prefixes stripped (harmless). `T 0` and `S 0 ...` are refused before auth when required (some clients send them at start-up; they only read state, so consider allowing `T 0`). `<cctype>` is included in `api_policy.cpp`; `tokenEqual` exists (line 19) and is visible in the anonymous namespace order used. Could not check: that the test lambda `hs("m")` expectation (`mode0 + "\n" + pb`) matches `FakeRadio` formatting exactly (it follows the `m` handler, so it should), nor the `err`/`s` variable names inside `testRigctlServer`/`testRigctl` for clashes (a local `s` in `testRigctl` would be shadowed or redeclared: rename `RigctlSession s` to `sess1` to be safe).

## backup-030: looks right

- H1: `checkManifestAgainst` now refuses a name whose `itemOfPath(name)` differs from the manifest item, adds `restorablePath()` (code folders `codecs plugins plugin bin lib libs sdk`, executable and library extensions, `.so.N`, `.onairtmp`, case-folded), refuses duplicates, and `planRestore` and `extractTo` repeat the checks (defence in depth) plus refuse a non-regular existing target (symlink). Case-insensitive file systems: the check lower-cases, good. Windows drive/UNC/colon/backslash/ADS: already rejected by `safeRelPath`. Unicode normalisation: the deny list is ASCII, a normalised form cannot turn into `codecs`. A symlinked parent folder inside the data folder is handled by the existing `insideRoot` (not re-read here).
- Registered correctly: `restorablePath` is declared in `backup.h` and defined after `safeRelPath` is declared; the test file already has `using namespace dect2::backup`, `fs` alias and `<fstream>`. `Manifest::File` aggregate init `{name, 4, item}` is valid. `testMalicious` would fail without the fix (the `layouts` item with `codecs/...` paths). One more worthwhile test line: `checkManifestAgainst` on a manifest from the real `writeManifest` of the full catalog (to prove normal backups still pass); the existing round-trip test in `test_backup.cpp` already uses `layouts/a.ini`, `memories.json`, `onair_ham.adi`, `voicekeys/keys.oavk`, all matching their items.
- Regression risk: only a file written by an older/newer build under a path not in `catalog()` is now refused (the old manifest check let it through if `item` existed). Backups made by this same code only list catalog paths, so normal use is fine. `.js .py .sh` extensions are refused anywhere, so a future item that stores scripts (automations are `.json` today) would need an exception.
- M3: default-deny table. Keys in `kRestoreSafe` apply; `kRestoreConfirm`, unknown keys, and everything from the sealed block need a per-key tick; credential-like keys in the plain part and odd key names are dropped. It is fail-safe: a pref missing from both tables is only `Confirm`. Ordinary settings are not blocked (table has ~490 keys). I spot-checked that the exposure keys (`apiOn apiLan shareOn shareLan feedLan monNet monBindAll mapServer rcUrl txEnabled`) are in the confirm table. Not verified: that each table key exists with that exact spelling in `Prefs` (a quick grep of the string literals found only keys that default to Confirm, e.g. `webhook_url`, `ntfy_server`, which is the safe side); not verified: that type `'C'` (scan channel) can never carry an exposure setting (it is channel data).
- UI (`backup_ui.cpp`): `restorePrefSets` is declared before `doRestore` and the window code; `s.confirmTick` is a `std::map` (needs `#include <map>` if not already in the file; check). The `prefNote` is appended in both branches. `ImGui::Checkbox(p.key.c_str(), &s.confirmTick[p.key])` is fine (`std::map<..., bool>` element address). Unlocked secrets: when `s.unlocked` is false the sealed prefs are not in `confirm`, so no ticks are shown for them (correct).
- Could not check without compiling: `std::isalnum`/`std::tolower` use in the new code (`<cctype>` is included), `static` table helpers inside `namespace backup` after `isSecretPrefKey` (order is fine), and `std::find_if` in `backup_ui.cpp` (already used there).

## web-030: looks right in design; verify mode ids

- Closes M2: unknown ids are now `Other` (fail closed, off until the host accepts the legal notice via `setDecoder`). `decoderAllowed`/`setDecoder`/`load` already treat every non-`Data` class the same way as voice/pager, so `Other` works with no further change. Case and spelling tricks (`DMR`, `Meshtastic`, empty string) fall into `Other`: blocked. The hub checks both the decoder kind and the host's mode id (`pk`), so a safe kind `host` with a restricted running mode is still blocked.
- Regression risk to check before merging: the safe list is in kind names (`ft8 ft4 wspr sstv wefax apt rds ais aprs acars vdl2 ...`), but `HostDecoder::policyKind()` returns `h_.modeId()`. In the registry `ft8 ft4 wspr sstv wefax` are sub-mode ids (core/src/engine/modes.cpp), and the family ids `hfdig`, `hfbeacon`, `marine`, `mesh`, `goesdcs`, `time`, `same`, `sonde`, `dab`, `hfdl` are on the RESTRICTED list. If `modeId()` returns the family id (for example `hfdig` while FT8 is running) the approved FT8/WSPR/SSTV/wefax decoding is silently blocked for web listeners. Check what `modeId()` returns for a sub-mode and either map it or add the approved family ids that carry only approved sub-modes. Also `rds` is a decoder kind but not a mode id; `cw`/`rtty` were moved from Data to Other by this fix (an owner decision: Morse/RTTY are plain amateur traffic; if approved before, put them back in `webSafeKinds`).
- Test: `tests/server/test_web_safety.cpp` `registryModeIds()` parses `core/src/engine/modes.cpp` text and every `XxxTuning()` body, then asserts every id is classified. That is a good design (new mode forces a decision) but it is fragile: any id the scanner picks up that the hand-written `restrictedKnownKinds()` list lacks (for example a sub-mode id such as `psk31`, `olivia`, `jt65`, `thor`, sub-ids from other tuning files, or ids built with ternaries) makes the test fail. Expect a first red run and add the printed ids. The `CHECK(cond, "%s ...", arg)` printf-style macro is used elsewhere in the file (line 61), so signature is fine; `ONAIR_SOURCE_DIR` is defined for this target in CMakeLists (line 1112). `readFile` is a static helper above (line 52). It would fail without the fix (`freedv`, `meshtastic` ... were Data before).
- JSON class name `"restricted"` is new in `WebPolicy::json()`; I found no consumer in `web/onair-web` that switches on `class`, but if the admin page does, add the new value.

## vk-030 and vkgui-030: looks right

Same three files in both (the diff of `voice-keys-gui-030` shows the identical patch).
- Bounds the PBKDF2 round count (1000..5,000,000) before the loop; the writer uses 200,000. The test patches bytes 23..26 (magic 6, flag 1, salt 16, then rounds): matches the layout in `encryptStore`. The 0xFFFFFFFF case would hang forever without the fix, so the test is meaningful; the 0 case only checks the refusal.
- `keychainMasterKey(out, createIfMissing)`: load never creates a key; save creates one only when no existing keychain-sealed file would be orphaned. Header sniff `hdr[6] == 0` is the keychain flag (1 = passphrase). `<filesystem>` added; 0600 applied after close (a short window with the default mode remains; acceptable, or create the file with `open(O_CREAT, 0600)`).
- Test: `setSecretProvider` with lambdas matches `SecretGet`/`SecretSet`; calling `saveToFile(path)` uses the default empty passphrase; `parseKeyHex`, `KeyStore::clear/add`, `Protocol::Nxdn`, `Algorithm::Scrambler` all appear in the existing test code. It would fail without the fix (a failed read used to create a new key, `sets == 1` check). Test uses `TMPDIR` or `/tmp` and leaves nothing behind.
- Notes: (1) a keychain item that is really gone (user wiped the keychain) now blocks saving over the old keychain-sealed file forever with the message "nothing was saved"; the UI needs a "discard the old key file" action or the user must delete the file by hand. (2) With a provider set, `encryptStore` ignores a typed passphrase (existing behaviour; the keychain wins). (3) `keychainMasterKey` still cannot distinguish "not found" from "denied" (the provider interface returns bool); the new logic is the safe conservative rule.

## ham-030: looks right

`callChars` (upper-case A-Z, 0-9, `/`, length 1..12) is applied to `from` and `to` before `hamCallValid`, after `up()` upper-casing, so quotes, brackets, spaces and unicode are refused; `<cctype>` is included. `LY2XX/P` and `DL1ABC/MM` pass. The test is failing-without-fix for the quote case. Regression: calls with a longer prefix/suffix chain over 12 characters (`HB9/DL1ABC/P` is 12, fine) are dropped; `-` (SSID) is dropped (APRS style `LY2XX-9`): if APRS-style SSIDs can reach this feed, they were previously accepted only if `hamCallValid` accepted them (it does not, I believe), so no change. Not checked: `hamCallValid` rules.

## blue-030: looks right

- `privateTempDir()` makes `<temp>/onair-<uid>` mode 0700, verifies with `lstat` that it is a directory (not a symlink) owned by us, repairs loose mode; `convertPrivately()` uses `mkstemp` (O_EXCL, 0600) in that folder and `rename`s onto the target; reuse only if `symlink_status` says regular file of the expected size. On a multi-user Linux box another user cannot enter the folder, pre-create the file or plant a symlink. TOCTOU between `mkdir` and `lstat` is covered by the owner check. Windows keeps the temp folder (per user) and writes directly.
- Sample rate bound `1 Hz .. 1e12` (NaN also fails the `>=` test): closes the xdelta hardening point.
- Compile in principle: `<cerrno>`, `<sys/stat.h>`, `<unistd.h>` added under `#ifndef _WIN32`; `std::remove` needs `<cstdio>` (present); on Windows the `dir` parameter of `convertPrivately` is unused (warning only; add `(void)dir;` if warnings are errors in CI). `blue::convertToCf32(src, dst, err)` has the same signature as the old call. Android (bionic) has `mkstemp`/`fchmod`.
- Test: checks the parent is not the shared temp folder, starts with `onair-`, folder has no group/other bits, copy has none. It fails without the fix. `fs2::perms` bit operators are in `<filesystem>`.
- Not covered: `core/src/tx/tx_studio.cpp` `onair_txstudio_<ns>.cf32` (low, as in the review).

## dir-030: looks right

- `webonair-dir-030` is now the same commit as the fix (`1b167d113`), so a diff against it is empty: use `untested-secfix-dir-030~1`.
- IP salt: without the secret a random 16-byte salt per Worker isolate is used; the rate limits work per isolate (a weaker limit across isolates, but never worse than before) and the keys cannot be reversed from the table. Test deletes `IP_SALT`, posts a bad record and asserts no key ends with the old `sha256("unsalted|ip")`; it depends on `ipKey()` being called before the body is validated (it is the first rate-limit key; `post()` accepts the IP as 4th argument, confirmed in the test file).
- Replay tombstone: a signed delete writes `tomb:<id>` into `rl` with `win = delete ts`, `exp = now + 600`; a later non-delete record for an id that has no row and `ts <= tomb.win` gets 409. Fake D1 handles the two new statements by exact SQL string; the SQL strings in `index.js` match them character for character. Real D1: `rl` has columns `k win n exp` (schema.sql) and `k` is the primary key, so `INSERT OR REPLACE` is valid; the cron cleanup of expired `rl` rows removes the tombstone after 10 minutes, which is longer than the 300 s clock window.
- Gap: the tombstone check is in the `!row` branch only, which is exactly the post-delete case. A replayed *delete* is harmless.
- Verified here only by reading; the Worker tests (`node --test`) were not run because of the no-test rule.

## What I could not check (no compile, no test run)

- Anything involving C++ compilation or linking: tests registration in CMake (only `test_web_safety` seen), name clashes (`s` in `testRigctl`), missing includes (`<map>` in `backup_ui.cpp`), ODR of the two `rigctlHandle` overloads.
- Test pass/fail for all branches; the registry scanner in `test_web_safety` is the one most likely to be red first.
- The real preference key spellings in `Prefs` beyond the spot-checks; `modeId()` values of `HostDecoder`; `hamCallValid` rules; the real D1 behaviour.
