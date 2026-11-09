# OnAir for Android

The OnAir receiver on a phone: the same C++/Rust decoder core as the desktop program (built as `libonair.so`), with a Kotlin +
Jetpack Compose user interface in the OnAir look (dark theme, OnAir colours, monospaced type). **Receive only**: nothing is
transmitted and nothing is sent to any server.

This folder is the Android project. It builds from a normal checkout of the OnAir repository: the native library is compiled from
the repository's own `core/` and `rust/`. How the app is put together (threads, state, service, how modes plug in) is in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Layout

```
android/
  settings.gradle.kts, build.gradle.kts, gradle.properties, gradlew, gradle/   Gradle project (AGP 8.7, Kotlin 2.0, Gradle 8.11)
  app/                      the app module (io.github.adswill.onair)
    build.gradle.kts        version from ../CMakeLists.txt, signing from outside git, debug/release variants, R8
    proguard-rules.pro      R8 rules (keeps the JNI class and native methods)
    src/main/java/io/github/adswill/onair/
      OnAirNative.kt        the JNI declarations (must match native/src/jni_onair.cpp; tools/check_jni.py checks)
      engine/               OnAirCore (typed JNI wrapper), ReceiverEngine (receiver thread + serialised command queue)
      data/                 ReceiverRepository (single source of truth), ReceiverState
      model/                typed models: Tuning, DeviceCaps, Telemetry, SpectrumFrame, SourceKind ...
      settings/             Settings + SettingsStore (DataStore, migrates the 0.2.x SharedPreferences)
      service/              ReceiverService (foreground service of a receive session)
      diag/                 AppLog (structured log), CrashLog (crash guard, crash log, share)
      ui/                   ReceiverViewModel, Waterfall, Permissions, the shell (AppShell, AppLayout, nav/), the screens
                            (receiver/, panels/, modes/, radio/, settings/, log/, about/, onboarding/), UiPrefs, Previews
      *.kt                  Theme (OnAir palette, fonts, shapes), OnAirIcons, Widgets, ResultsTab, AircraftMap and the generated
                            mode list (Modes.kt)
    src/main/res/           strings, generated modes.xml, theme, icons, fonts (Inter, JetBrains Mono: OFL), FileProvider paths
    src/test/               JVM unit tests of the UI logic (./gradlew testDebugUnitTest)
    src/main/jniLibs/       libonair.so is placed here by tools/build_native.sh (gitignored)
  native/                   the JNI library: CMakeLists.txt (adds the repository root as the core) + src/ (bridge, results JSON,
                            stand-ins for libusb/libhackrf/FFmpeg entry points)
  tools/
    build_native.sh         builds and strips libonair.so for arm64-v8a
    gen_modes.py            regenerates Modes.kt and res/values/modes.xml from the desktop mode table (app/ui/mode_table.cpp; stable indices in mode_order.txt)
    check_jni.py            compares OnAirNative.kt with jni_onair.cpp
  docs/                     ARCHITECTURE.md, STYLE.md (desktop look -> Compose tokens), the 0.2.x plan and mode audit
```

## Build

Needs:

- **JDK 21** (`java -version`), Android SDK with platform 35 (Android Studio or the command-line tools; set `ANDROID_HOME` or put
  `sdk.dir=...` in `android/local.properties`, which is gitignored).
- **NDK 27.2.12479018** and CMake ≥ 3.22 (the SDK's `cmake;3.31.5` is used when present):
  `sdkmanager "ndk;27.2.12479018" "cmake;3.31.5"`.
- **Rust stable** on `PATH` (rustup) with the Android target: `rustup target add aarch64-linux-android`. The core's Rust workspace
  has no external crates and builds offline.
- `make`, and `pkg-config` with the libhackrf, libusb-1.0 and FFmpeg development headers of the build machine (macOS:
  `brew install pkg-config hackrf libusb ffmpeg`). Only their headers are used; the Android library brings its own stand-ins.

Steps, from the repository root:

```sh
android/tools/build_native.sh          # libonair.so -> android/app/src/main/jniLibs/arm64-v8a/ (ONAIR_SRC defaults to this checkout)
cd android
./gradlew assembleDebug                # app/build/outputs/apk/debug/app-debug.apk  (id io.github.adswill.onair.debug)
./gradlew assembleRelease              # app/build/outputs/apk/release/app-release.apk (signed when a key is supplied)
```

`build_native.sh` options (environment): `ONAIR_SRC` (another checkout or worktree to build the core from), `ANDROID_NDK`,
`NDK_VERSION`, `CMAKE`, `JOBS`, `BUILD_TYPE`. The native build directory is `android/native/build/` (gitignored).

A release build stops when `libonair.so` is missing; a debug build only warns and starts without the receiver (useful for UI work).

**Version.** `versionName` comes from `project(OnAir VERSION x.y.z)` in the repository's `CMakeLists.txt`; `versionCode` is
`(major*10000 + minor*100 + patch)*100 + beta` (99 for a stable release), e.g. 0.2.9 → 20999, 0.3.0-beta.1 → 30001.
Override with `-PonairVersionName=0.3.0-beta.2` (or env `ONAIR_VERSION`, which the CI sets from the tag) and `-PonairVersionCode=N`.

## Signing

The signing key **never goes into git** (`android/.gitignore` ignores `keystore.properties`, `*.jks`, `*.keystore`). The maintainer
supplies it in one of two ways:

1. **A properties file outside git.** Either `android/keystore.properties` (ignored), or any path given with
   `-PonairKeystoreProperties=/path/to/keystore.properties` or env `ONAIR_KEYSTORE_PROPERTIES`:

   ```properties
   # a relative storeFile resolves against this file's folder
   storeFile=/absolute/path/onair-release-permanent.jks
   storePassword=...
   keyAlias=onair
   keyPassword=...
   ```

2. **Environment variables** (what the CI uses): `ONAIR_STORE_FILE`, `ONAIR_STORE_PASSWORD`, `ONAIR_KEY_ALIAS`, `ONAIR_KEY_PASSWORD`.

Without either, `assembleRelease` gives an unsigned APK. Every update must be signed with the same key as the installed app
(the permanent key of 0.2.7 onwards); keep the keystore and its password backed up offline. Debug builds use the SDK's debug key and
install next to the release app (`.debug` id suffix).

**GitHub Actions** (`.github/workflows/android.yml`): add these repository secrets (Settings → Secrets and variables → Actions):

| Secret | Value |
| --- | --- |
| `ANDROID_KEYSTORE_BASE64` | `base64 -i onair-release-permanent.jks` (one line) |
| `ANDROID_KEYSTORE_PASSWORD` | the store password |
| `ANDROID_KEY_ALIAS` | `onair` |
| `ANDROID_KEY_PASSWORD` | the key password (if empty, the store password is used) |

The workflow builds the APK on version tags (`v*`) and by hand (Actions → Android → Run workflow) and keeps it as a workflow artifact.
It does not create or change releases; attaching the APK to a release stays a manual step. A push to an `android-*` branch that
touches `android/` runs a quick Kotlin-only check (no native library).

## Using the app

Sources: the built-in **test signal**, **rtl_tcp** (a USB RTL-SDR served by a driver app on the phone or over the network),
**SpyServer**, and **IQ files** picked with the system file picker (cs8, cu8, cf32, ci16, cu16). The first start runs a short setup
(radio, permissions, interests, lessons). Pick a source on the Radio tab, a mode on the Modes tab (search, favourites, filters), press
play. Tap the spectrum to tune, pinch to zoom, double tap to see the whole capture; drag tunes in centre tuning and moves the view in
normal tuning (the chip on the spectrum switches). Tap the frequency digits to type one; − and + step. Receiving goes on with the
screen off; the notification has a Stop button. Settings > Log shows the app and core log and shares it (with the crash log, if any).
Links `onair://tune?freq=100.5M&mode=FM%20broadcast` open the receiver there.

USB radios (RTL-SDR always; HackRF and Airspy when built with `LIBUSB_SRC` etc.), network radios (rtl_tcp, SpyServer, KiwiSDR, TCI,
SDR++ server) and the phone's sound input: [docs/USB.md](docs/USB.md). SDRplay radios cannot be used on Android.

Limits today: arm64-v8a only, minimum Android 9 (API 28), patented voice codecs are not included (user-installed, see the desktop
docs). JVM unit tests: `./gradlew testDebugUnitTest`; JNI declarations: `python3 tools/check_jni.py`.
