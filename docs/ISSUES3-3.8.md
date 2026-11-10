# ISSUES3 (3.8): requests #183, #386, #458 and Repeaters near me

Branch `feat38-iss3`, on `integ-38`. Nothing was published or commented on GitHub.

## Status

| Item | State |
|---|---|
| #183 Android in-app update | done; Kotlin compiled against `android.jar` with stubs and the logic tests run on a JVM; the Compose screen and the install hand-over are untested on a phone |
| #386 jump count | done (packets table, CSV, `/api/v1/status`) |
| #386 RTL-SDR 1.024 Msps | rule checked and corrected (500 kHz channels do not fit), headless server and `dect2cli` follow it |
| #386 `dect2cli --rate` for radios | done |
| #458 generator into any TX standard | digital TV standards and analog TV done; several generators done; FFmpeg arguments done |
| #458 custom FFmpeg binary path | **not done** (see below) |
| Repeaters near me from Open Repeater | done, API terms below |

## #183 Android in-app update

Files: `android/app/src/main/java/io/github/adswill/onair/update/UpdateLogic.kt` (pure logic), `UpdateManager.kt` (network, verification, installer), `ui/settings/UpdateSettings.kt` (Settings > Help > Updates).

- The check asks `api.github.com/repos/adswill/OnAir/releases/latest` once at start, if the Settings switch "Check for updates" is on (default on; not in a debug build). The same source setting as the desktop: Automatic (GitHub, the mirror only when "Use the mirror when GitHub cannot be reached" is on), GitHub only, Mirror only. Default mirror address `https://mirror.on-air.dev/{url}`, the same default as the desktop. `mirrorUrl` is a port of `core/src/engine/updater.cpp`: https only, only the project's own GitHub addresses, `{url}` / `{urlenc}` or a base address.
- Download goes to `cacheDir/update` through the faster route (GitHub first, then the mirror) with a progress line and Cancel; redirects are followed by hand, https only, at most 5; the size is capped at the release's size plus 1 MB.
- Verification before anything is installed: SHA-256 against the release's `SHA256SUMS` (fetched from github.com; GitHub's API digest must agree when present). If GitHub cannot be reached and only the mirror lists the checksum, the user has to tap "Download anyway". Then `getPackageArchiveInfo` must show this app's package name, a higher version code, and exactly the installed app's signing certificates (SHA-256 of `apkContentsSigners`). The file is hashed again right before the hand-over.
- Install: `PackageInstaller` session with `USER_ACTION_REQUIRED`; the system shows its own confirm screen (`InstallResultReceiver` launches it). No silent install. `REQUEST_INSTALL_PACKAGES` is in the manifest; if Android has not allowed OnAir to install packages, the button opens the system screen for it first.
- A debug build (`.debug` package name) never matches the release APK, so the start check is skipped there.
- Tests: `UpdateLogicTest.kt` (mirror rules, refusals, versions, checksum file, release parsing incl. no unsigned/foreign asset, signer rule, SHA-256, small JSON reader), 11 tests pass on a JVM. A small JSON reader replaces org.json so the tests need no extra dependency.
- Not tested: the real download, `PackageInstaller` and the Compose UI on a device. Run the `android.yml` workflow (debug build and `testDebugUnitTest`) to check the Gradle build.

## #386

- `MeshPacket::jumpsFixed` (from `RxFrame::jumpsFixed`), column "Jumps" in the packets table (tooltip says what it means), CSV column `jumps_fixed`, `/api/v1/status` field `meshJumpFrames` (frames recovered since the program started, `MeshReceiver::framesWithJumpsFixed()`). Saved result files of the Mesh mode from older builds are discarded (the saved struct changed; that is how `results_io` works).
- 1.024 Msps on an RTL-SDR: the rule was `bandwidth <= 500 kHz`, but the mode's 300 kHz tune offset puts the channel at -300 kHz, and the receiver gives a channel a demodulator only if `|offset| + bw/2 + 25 kHz < 0.45 * rate` and `rate >= 2.2 * bw`. At 1.024 Msps that holds for 62.5, 125 and 250 kHz and fails for 500 kHz (it would reach -550 kHz), so a 500 kHz channel got no demodulator at all. Now `meshRtlRateHz(board, bw)` (mesh_rx.h) returns 1.024 Msps only when the channel fits, else 0 (the mode's 2 Msps). Of the 297 settings in the Meshtastic, MeshCore, LoRa APRS and MeshCom tables, 239 get 1.024 Msps on an RTL-SDR and 58 keep 2 Msps. Other radios are unchanged (test checks nine other board names).
- The headless server (`ServerHost::buildTune`) and `dect2cli` use the same rule for the default 250 kHz channel; a typed `--rate` or the server's rate setting wins. This answers the reporter's question that the server started at 2 Msps. The reporter saw 1.000 instead of 1.024 Msps in the window; I could not reproduce that without a radio and did not change it.
- `dect2cli --rate Msps` now also sets a live radio's rate (capped at the radio's maximum, and it replaces `linkRateFor`'s choice).
- Test: `tests/mesh/test_mesh_rtlrate.cpp` (rate rule over every preset; clean 1.024 Msps decode: 9 frames, 0 CRC errors, 0 jumps; 96 samples removed every 150000: counter equals the rows with jumps). **Simulated signals only**: no real recording with jumps exists in the repository.

## #458 video generator

Done:
- Several generators at once: Tools > Digital video generator has a row of generators (up to 8, "+ add generator"), each with its own source, settings, overlay and stream (`videogen.txt`, `videogen_2.txt` ...). Closing the window stops all of them.
- New output "Transmitter (OnAir TX)": the transport stream is queued (`generatorPush`) and the transmitter reads it as the video file `generator:N`. `txOpenMpegTs("generator:N")` serves every digital TV standard that already takes a transport stream (DVB-T/T2, ATSC, ISDB-T, DVB-S/S2 incl. QO-100 DATV, DTMB, DVB-C) with null packets while nothing is queued, so the transmitter's rate is kept. Analog TV (`VideoIn` with `generator:N`) gets the generator's pictures through a frame sink. In the Transmit panel, "Video generator: 1 2 3 4" buttons next to the video file field of the TV modes and of analog TV set the name.
- FFmpeg arguments box (Advanced, under Latency tuning): the text is split into an argument list by `splitArgs` (quotes group, nothing is expanded, no shell anywhere) and checked by `parseEncoderArgs`: only `-key value` pairs of the video encoder (`-b:v` is `b`); refused: stream specifiers other than `:v`, input/output/container/codec/filter/map/hardware options (`-i -f -c -vf -map -hwaccel -s -r -t ...`), values naming a file or a pipe. The options are set on the encoder's option dictionary and win over the settings; options the encoder did not use are listed in the notice under the picture.
- Test: `test_videogen` (`testArgs`, `testInstancesAndTransmitter`): argument splitting and refusals, two generators at once with separate queues and service names (programs 0x111 / 0x222), an unknown option reported and a known one not, null packet on an empty queue, 10+ analog pictures, bad generator number and bad arguments refused at start. Run locally against the current sources of the generator (the whole test passes); `txOpenMpegTs("generator:N")` is exercised only in CI (`VG_NO_TX` locally) and compile-checked here. The video encoders were MPEG-2 in the checks of the new tests; libx264, libx265 and VideoToolbox encoders ran in the existing tests.

Not done:
- **Custom FFmpeg binary path (NVENC / QSV).** The generator encodes with the FFmpeg libraries OnAir is linked with. Using another build means running an `ffmpeg` process with raw pictures and sound piped in and a transport stream read back, on macOS, Linux and Windows, with the microphone and overlay paths changed. That is a separate piece of work (a process runner with stdin and stdout pipes does not exist in core; `app/platform/spawn.cpp` has none). The arguments box covers encoder flags for the linked FFmpeg, and hardware encoders the linked build has (VideoToolbox, `_mf`, `_nvenc` where present) already appear in the encoder list.
- The per-programme encoder and audio settings of the TX panel (3.8 list) are separate work; the demo multiplex still encodes with its own settings. A generator feeds one transport stream (one programme set) per generator; to feed several programmes of one multiplex, a multiplexer of several generator streams would be needed.
- PRs #444 and #446 (already merged into 3.7 as the generator); the withdrawn #448 was not used.
- Not run: the app windows (syntax checks only for `videogen_ui.cpp` and `tx_payload_ui.cpp`).

## Repeaters near me from Open Repeater

"Repeaters near me" (`app/ease/ease_ui.cpp`, `core/src/ease/repeaters.cpp`) has a button "Download from Open Repeater" next to the CSV import. Nothing is bundled; both lists are kept on the user's computer (`<data>/repeaters/openrepeater.json`, `list.csv`) and the newer one is used at start. The attribution line "Repeater data: Open Repeater (openrepeater.org), CC0" is in the popup, and the message line shows the number of repeaters and the download date.

Terms read on 2026-11-10 (openrepeater.org: /api-docs, /docs, /terms, footer):
- Data licence: CC0 ("Repeater data is licensed under CC0", footer "Data licensed under CC0 1.0"). No attribution is required; it is given anyway.
- API: base `https://www.openrepeater.org/api/v1`; read only; endpoints `/repeaters`, `/repeater/{id}`, `/country/{code}`, `/search` (`q`, `lat`, `lng`, `radius`, `page`, `limit`). A personal API key from a free account is required (`X-API-Key` or `Authorization: Bearer`), **30 requests per key per day**, counter resets at midnight UTC, answers carry `X-RateLimit-*` headers, 429 when used up. No bulk export endpoint is documented.
- Terms of use, section 5: "You may not resell or commercially redistribute the database without written permission." Section 8: "Do not scrape the site in ways that overload our servers." Nothing is said about caching or bundling.
- How OnAir follows them: no key is shipped, the user enters their own (stored in `<data>/repeaters/openrepeater.key`); the download runs only when the user presses the button and needs a position; one `/search` per click for the 300 km around the position (at most 3 pages of 100) so at most 3 of the 30 daily requests; a 429 or 401/403 gets a plain message; the copy is for the user's own use and OnAir does not redistribute it.
- I could not read the documented response layout beyond the field list (`callsign, frequency, city, lat, lng, mode`, ...). The parser accepts `{"data":[...]}`, `repeaters`, `results` or a bare array, string or number values, and MHz, kHz or Hz. **Unverified against a live answer**: no key was available. If a field name differs the row is skipped and counted. The key is sent in the `X-API-Key` header of a curl process, so it is visible in the process list of the computer for a moment.
- The Open Repeater `mode` text is matched like the CSV's mode column (DMR, D-STAR, Fusion, M17; a row without a mode text is kept).
- Test: `tests/ease/test_repeaters_csv.cpp` (JSON answers: shapes, units, bad rows, error answer, search address). Download and UI not run.
