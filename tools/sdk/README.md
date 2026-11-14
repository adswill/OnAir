# OnAir's cross SDKs

One clang compiler (LLVM 23) and the same libraries for every platform OnAir is released for. Each SDK is a self-contained tarball for a Linux x86-64 machine:

| SDK | Builds OnAir for | Installer (`--target package`) |
|---|---|---|
| `sdk_windows` | Windows x64 | `OnAir-<v>-windows-x64-setup.exe` and `.zip` |
| `sdk_linux_amd64` | Linux x86-64 (Ubuntu 26.04 and newer) | `onair_<v>_amd64.deb` |
| `sdk_linux_arm64` | Linux arm64, Raspberry Pi (Ubuntu 26.04 and newer) | `onair_<v>_arm64.deb` |
| `sdk_macos_amd64` | macOS Intel | `OnAir-<v>-macos-x86_64.dmg` |
| `sdk_macos_arm64` | macOS Apple silicon | `OnAir-<v>-macos-arm64.dmg` |

This folder holds the scripts that make them. [peterdewitte/OnAir.sdk](https://github.com/peterdewitte/OnAir.sdk) has a guided `build_sdk.sh` (questions, and a Docker build for other systems) that runs these scripts.

## What you need

- **Making an SDK:** Ubuntu 26.04 on x86-64 (also in WSL2 or a container). CMake checks it when it configures, and lists the `apt` packages that are missing.
- **Using an SDK:** any Linux x86-64 with `cmake ninja-build pkgconf git python3` (Ubuntu: `sudo apt install cmake ninja-build pkgconf git python3`).
- **Network:** the first build downloads about 3 GB: LLVM, Rust, Ubuntu, MSYS2 and conda-forge packages, the libraries' sources and, for macOS, Apple's macOS SDK (from Apple's software update catalog, no Apple ID). Every download is checked against the lock files.
- **Disk:** about 3 GB for the cache, 0.8 GB per SDK tarball, 3 GB per unpacked SDK, and up to 40 GB for an OnAir build with all its tests.
- **Time, measured with 8 to 14 CPUs:** an SDK takes about 1 minute when its libraries are in the cache and 5 to 6 minutes when they are built, plus the first downloads. OnAir with an SDK: about 20 minutes for the installer (`--target package`), about an hour for everything with the tests.

## 1. Make an SDK

```bash
cmake -S tools/sdk -B build-sdk -G Ninja
cmake --build build-sdk --target sdk_windows     # build-sdk/sdk_windows.tar.gz
cmake --build build-sdk --target sdk_all         # all five
```

There is one target per SDK: `sdk_windows`, `sdk_linux_amd64`, `sdk_linux_arm64`, `sdk_macos_amd64`, `sdk_macos_arm64`. One SDK is made at a time.

- **Inputs:** every file in `common/`, `sources/`, `windows/`, `linux/` and `macos/` (scripts, lock files, patches, licence texts), the scripts in this folder, the options and the versions of the Ubuntu host packages that build parts of it. `cached_tarball.sh` makes a sha256 key of all of them on every build.
- **A change** to any input makes that SDK again from scratch.
- **No change:** nothing happens ("up to date"). An SDK made earlier with the same key (for example after switching branch and back) is taken from the cache in seconds.
- **The cache** is `build-sdk/cache` (`-DONAIR_SDK_CACHE=<folder>` to keep it elsewhere, for example to share it between checkouts or keep it in CI): the downloads, each library built once per version and options, and the two newest SDKs per platform. Deleting the build folder deletes it.
- `-DONAIR_SDK_KEEP_WORK=ON` keeps the unpacked SDK and the libraries' build folders, to see why a build failed.

## 2. Build OnAir with an SDK

Unpack the SDK, source its environment script, then use plain CMake. The environment script sets the compiler, the sysroot and the CMake toolchain file for that platform, and prints the SDK's parts and licences (`ONAIR_SDK_QUIET=1` leaves that out).

```bash
mkdir -p ~/sdk && tar xzf build-sdk/sdk_windows.tar.gz -C ~/sdk
. ~/sdk/environment_windows.sh                       # the "." matters: it sets up this shell
cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows                          # OnAir, its tools and its tests
cmake --build build-windows --target package         # the installer, in build-windows/packages
```

The same commands work for every SDK: replace `windows` by the SDK's name (`linux_amd64`, `linux_arm64`, `macos_amd64`, `macos_arm64`). For `linux_arm64` add `-DRPI_CPU=generic` to the configure command so the program runs on any 64-bit ARM; without it the build is tuned for the Raspberry Pi 5. `--target package` builds only what the installer needs, so on its own it is quicker than the full build.

Use one build folder per SDK, and never a build folder that was configured without an SDK: CMake keeps the compiler of the first configure.

## All releases in one go

`build_all_releases.sh` does steps 1 and 2 for every platform: the `sdk_<platform>` targets, then each SDK's `--target package`. The installers go to `build-releases/installers/`, with `SHA256SUMS`; a failed build leaves its log in `build-releases/logs/`.

```bash
tools/sdk/build_all_releases.sh                              # on a terminal it asks; otherwise all five platforms
tools/sdk/build_all_releases.sh --windows --linux_amd64      # only these
tools/sdk/build_all_releases.sh --docker                     # in an Ubuntu 26.04 container: only Docker needed, on any system
```

`--sdk-only` stops after the SDKs, `-o DIR` writes elsewhere, `--cache DIR` keeps the SDK cache elsewhere, `-j N` limits OnAir's build, `--keep-build-artif` keeps the build folders and `--dry-run` prints the commands. `--help` lists them all.

## 3. Run the tests

- **Linux x86-64:** the tests run on the build machine: `ctest --test-dir build-linux_amd64 -j8 --output-on-failure -LE "timing|slow"`. That is the fast tier CI runs on a pull request; `-L slow` (hours) runs nightly, and `-L timing` measures speed and needs an otherwise idle machine.
- **Windows:** the tests are `.exe` files in the build folder, with the DLLs they need next to them. Copy the source tree and the build folder to a Windows machine at the same absolute path (`/home/u/OnAir` becomes `C:\home\u\OnAir`) and run `ctest --test-dir <build folder>` there, or run one test program by hand.
- **Linux arm64, macOS:** copy the build folder to such a machine in the same way, or test the installer there.

## For scripts, CI and AI assistants

These steps need no questions and no terminal. Points that are easy to miss:

- **One shell:** the environment script only changes the shell it is sourced in. When each command runs in a new shell (CI steps, an AI assistant's tool calls), source it in the same command line as `cmake`: `. ~/sdk/environment_linux_amd64.sh && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build`.
- **Configure once per folder:** a build folder made with one SDK cannot be switched to another SDK or to the system compiler; use a new folder.
- **Parallel jobs:** `cmake --build` uses all CPUs by default; on a shared machine limit it, for example `cmake --build build -- -j8` (Ninja). The SDK build uses all CPUs the process may run on.
- **Exit codes:** every step stops with a non-zero exit code and an `error:` line when something is wrong: a missing host package, a download whose checksum does not match, a missing licence, FFmpeg built with GPL options, a Windows build without camera support, a macOS app that loads a library from outside itself.
- **OnAir.sdk's `build_sdk.sh`** asks its questions only on a terminal. In a script, name the SDKs: `./build_sdk.sh --windows --linux_amd64`; `--build-onair` also builds the installers, `--onair <checkout>` uses a local OnAir checkout instead of cloning one.
- **Long steps:** the first SDK build (downloads and libraries) and a full OnAir build each take tens of minutes: run them in the background and follow the log, rather than waiting on a short timeout.

## Troubleshooting

| Message | What to do |
|---|---|
| `this machine lacks host tools to make the SDK` | install the `apt` packages it lists, then configure again |
| `The SDKs are made on Ubuntu 26.04 x86-64` | use Ubuntu 26.04 (WSL2, a container), or OnAir.sdk's `build_sdk.sh --docker` |
| `apt-get download ... failed (no longer in the archive?)` | a pinned Ubuntu package is gone: keep the cache folder, or update the lock files (`linux/fetch_sysroot.sh --update`) and commit them |
| macOS: Apple's Command Line Tools package is no longer in the catalog | Apple removed that version: keep `cache/apple`, or update `macos/sysroot-*.lock` (`macos/fetch_sysroot.sh --update`) |
| a build failed halfway | build again with `-DONAIR_SDK_KEEP_WORK=ON` and read the library's build folder in `cache/work` |
| a Windows test exits with `0xc0000135` | a DLL is missing next to the `.exe`: copy the whole build folder, not only the program |

## What is here

| | |
|---|---|
| `CMakeLists.txt`, `cached_tarball.sh` | the CMake targets and the key and cache |
| `build_all_releases.sh`, `Dockerfile` | every SDK and every installer in one go, natively or in Docker |
| `make_tarball.sh` | one SDK, from scratch, into `sdk_<platform>.tar.gz` |
| `check_host.sh` | the Ubuntu packages the build needs |
| `common/make_sdk.sh` | the SDK's layout: native tools, target sysroots, licences (`LICENSES.txt`) |
| `common/`, `windows/`, `linux/`, `macos/` | the compiler, the packaging tools and each platform's system libraries, pinned by lock files |
| `sources/` | OnAir's own libraries (FFmpeg, SoapySDR, Hamlib, ...) built from pinned sources with the SDK's clang |

The Windows and Linux SDKs may be shared, with the source of their GPL and LGPL parts. The macOS SDKs hold Apple's macOS SDK: do not publish them.
