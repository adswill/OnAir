# Release 3.0 checklist

3.0 is released only when the owner says so. Everything below is ready so that it takes hours, not days. **[OWNER]** marks a decision or an
action only the owner can take. Nothing here pushes main, pushes a tag or publishes by itself.

## Files and workflows

| What | Where |
|---|---|
| Nix flake | `gh workflow run nix.yml --ref <branch>` green on the release commit (x86_64 and aarch64; `release.sh` warns when there is no green run); version comes from CMake, nothing to bump |
| Version and credits checks | `tools/release/check_versions.sh <version> [--final]` |
| The release script | `tools/release/release.sh <version> [options]` (builds, collects, makes a DRAFT; `--publish` makes it public) |
| macOS arm64 + x86_64, Linux x86_64 + arm64 | `.github/workflows/release.yml` in adswill/OnAir (started by hand on the branch, no tag needed); Linux has a smoke test |
| Android APK | `.github/workflows/android.yml` in adswill/OnAir |
| Source tarball, Windows zip + installer | `package.yml` in the private adswill/OnAir-ci (Windows: MinGW cross build on a GitHub Ubuntu runner from the prefix in `win-prefix/`) |
| Release notes | `docs/CHANGELOG-3.0.md` |

## Assets (names must match: the website, the updater and the Docker image use them)

`OnAir-3.0.0-macos-arm64.dmg`, `OnAir-3.0.0-macos-x86_64.dmg`, `onair_3.0.0_amd64.deb`, `onair_3.0.0_arm64.deb`,
`onair-3.0.0-linux-x86_64-portable.tar.gz`, `onair-3.0.0-linux-aarch64-portable.tar.gz` (Raspberry Pi), `OnAir-3.0.0-windows-x64.zip`,
`OnAir-3.0.0-windows-x64-setup.exe`, `OnAir-3.0.0-android.apk`, `SHA256SUMS`. The source tarball (`onair-3.0.0-source.tar.gz`) comes from
`package.yml` as a check; GitHub also makes its own source archive for the tag.

## Before the day (can be done now)

1. Branch: one integration branch (`integ-30`) with everything merged, pushed to `pub`, tests green. **[OWNER]** says when it is final.
2. `tools/release/check_versions.sh 3.0.0 --final` passes: versions (CMake 3.0.0, Windows resource, Info.plist, Android versionCode 3000099
   from CMake), credits table in step with `THIRD_PARTY.md`, every `third_party/` folder named, changelog title no longer says "draft".
   Anything new in 3.0 that is a library, data set or ported code needs a row in `tools/credits_data.tsv` (then `python3 tools/gen_credits.py`).
   Known gap at the time of writing: "Mercury" and the KAAL modulation have no row in `THIRD_PARTY.md` (searched the tree: no third-party code
   under those names; if third-party code is in, add rows, if it is OnAir's own design nothing is needed). **[OWNER]** confirms.
3. CI green on the final commit: `gh workflow run ci.yml --repo adswill/OnAir --ref integ-30`, plus the laptop worker with the full test list.
   `release.sh` refuses to continue without a green `ci.yml` run on the commit (`--skip-ci-check` overrides).
4. **[OWNER] Android signing.** The CI has no signing secrets, so its APK is unsigned and `release.sh` stops on it. Either add the four
   repository secrets (`ANDROID_KEYSTORE_BASE64`, `ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS`, `ANDROID_KEY_PASSWORD`; android/README.md
   "Signing") or build the signed APK on the Mac: `ONAIR_VERSION=3.0.0 android/tools/build_native.sh && (cd android && ./gradlew assembleRelease)`
   with the keystore in `android/keystore.properties`, copy `android/app/build/outputs/apk/release/*.apk` and pass `--apk <file>`.
   Same key as 0.2.8 / 0.2.9, otherwise users cannot update.
5. **[OWNER] Mirror and website.** The website project (`~/onair-website-new`): `tools/config.json` version `3.0.0` and the date, the changelog from
   `docs/CHANGELOG-3.0.md`, download page file list (printed by `check_versions.sh`). Mirror links: `https://mirror.on-air.dev/https://github.com/adswill/OnAir/releases/download/v3.0.0/<file>`
   (the Cloudflare Worker in `web/mirror-worker` forwards only the project's own releases, nothing to change).
   Deploy only after the release is public: `sh tools/make_dist.sh && npx wrangler deploy`.

## Release day

6. **[OWNER] says "release 3.0".** Nothing before this line publishes anything.
7. From a clean checkout of the final branch (a separate worktree, never a tree that is being merged):
   `git worktree add /tmp/relwt pub/integ-30 && cd /tmp/relwt && tools/release/release.sh 3.0.0 --ref integ-30 [--apk <signed apk>]`
   - starts the builds on GitHub (about 1 to 2 hours: Windows and the Linux arm64 job are the longest), waits, downloads, checks that all
     nine files are there, writes `SHA256SUMS` and `SIZES.txt`, makes the **draft** release `v3.0.0` on the exact commit (no tag exists yet).
   - Nothing else is changed. Use `--no-draft` to stop after collecting the files, `--reuse-runs` to use finished runs instead of building again.
8. **[OWNER]** looks at the draft: file list (10 assets), sizes (`SIZES.txt`), notes. Optional: install the DMG / setup / APK on a real device.
9. Publish: `tools/release/release.sh 3.0.0 --ref integ-30 --reuse-runs --publish` (or press "Publish" on the release page).
   GitHub creates the tag `v3.0.0` on the commit. `release.yml` starts for the tag, sees the public release with its files and skips the rebuild;
   only the Docker image (`ghcr.io/adswill/onair:3.0.0` and `:latest`, amd64 + arm64) is built, from the published `.deb`.
10. Merge the branch into `main` and push main **[OWNER]** (the tag already points at the release commit). Delete the betas if any.
11. Website deploy (step 5), update-mirror check: the in-app update check on a 0.2.9 install must find 3.0.0; `SHA256SUMS` is attached.
12. Announcements (Discord, GitHub issue replies in `scratchpad/replies-3.0/`) **[OWNER]**.

## Tests per asset (what the pipeline already proves, what is left)

| Asset | Automatic check | Left for a person |
|---|---|---|
| Linux x86_64, arm64 | built on the oldest supported Ubuntu with the static LGPL FFmpeg; portable package unpacked and started in xvfb with `--hidden --mute --no-first-run --shot` (one frame, PNG larger than 10 KB); `.deb` metadata printed | install the `.deb` on a clean machine; a Raspberry Pi 4/5 start |
| macOS arm64, x86_64 | built by `make_dmg.sh` (target 15.0, bundle checks inside the script) | open the DMG on a Mac without Homebrew: `open OnAir.app --args --hidden --mute --no-first-run --shot /tmp/s.png` |
| Windows | cross build, `check_win_dlls.sh` (every DLL present and not too new), PE64 check, version string in the exe, zip listing | run the setup on Windows 10/11, start OnAir, plug a radio; the Smart App Control / SmartScreen prompt is expected (no code signing) |
| Android | JNI table check, Kotlin compile, native library for arm64-v8a | install on a phone (uninstall 0.2.9 first only if the signing key differs) |
| Source | `git archive` of the commit, version checks | none |

## If a step fails

- `release.sh` stops at the first problem and tells which run failed (link). Fix on the branch, push, run again: the draft is replaced, never duplicated.
- The Windows job needs the prefix in `win-prefix/onair-win-prefix.tar.zst` of OnAir-ci. After adding a Windows library to
  `tools/package/cross_deps_windows.sh`, rebuild `~/onair-win-prefix` on the Mac (`tools/package/cross_deps_windows.sh`), then
  `tar --zstd -C ~ -cf onair-win-prefix.tar.zst onair-win-prefix` and upload it again (contents API, file below 100 MB).
- A release can be unpublished by editing it back to draft on the release page; the tag then has to be deleted by hand (**[OWNER]**).
