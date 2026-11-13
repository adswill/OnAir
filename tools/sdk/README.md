# OnAir's cross SDKs

One clang compiler and the same libraries for every platform OnAir is released for. Each SDK is a self-contained tarball for a Linux x86-64 machine: `sdk_windows`, `sdk_linux_amd64`, `sdk_linux_arm64` (Raspberry Pi), `sdk_macos_amd64` and `sdk_macos_arm64`. This folder holds the scripts that make them; [peterdewitte/OnAir.sdk](https://github.com/peterdewitte/OnAir.sdk) has the guided `build_sdk.sh` (with Docker) and the documentation.

## Make an SDK with CMake

On Ubuntu 26.04 x86-64 (CMake checks it and says which `apt` packages are missing):

```bash
cmake -S tools/sdk -B build-sdk -G Ninja
cmake --build build-sdk --target sdk_windows     # build-sdk/sdk_windows.tar.gz
cmake --build build-sdk --target sdk_all         # all five
```

There is one target per SDK: `sdk_windows`, `sdk_linux_amd64`, `sdk_linux_arm64`, `sdk_macos_amd64`, `sdk_macos_arm64`.

- **Inputs:** every file in `common/`, `sources/`, `windows/`, `linux/` and `macos/` (scripts, lock files, patches, licence texts), the scripts in this folder, the options and the versions of the Ubuntu host packages that build parts of it. `cached_tarball.sh` makes a sha256 key of all of them on every build.
- **A change** to any input makes that SDK again from scratch.
- **No change:** nothing happens. An SDK made earlier with the same key (for example after switching branch and back) is taken from the cache in seconds.
- **The cache** is `build-sdk/cache` (`-DONAIR_SDK_CACHE=<folder>` to keep it elsewhere): the downloads (about 3 GB), each library built once per version and options, and the two newest SDKs per platform. Deleting the build folder deletes it.
- **Time:** from scratch with an empty cache, about 40 minutes for all five, plus the downloads. With the libraries in the cache, 5 minutes per SDK.

One SDK is made at a time.

`-DONAIR_SDK_KEEP_WORK=ON` keeps the unpacked SDK and the libraries' build folders, to see why a build failed.

## Use an SDK

```bash
tar xzf build-sdk/sdk_windows.tar.gz
. ./environment_windows.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target package    # the installer, in build/packages
```

## What is here

| | |
|---|---|
| `CMakeLists.txt`, `cached_tarball.sh` | the CMake targets and the key and cache |
| `make_tarball.sh` | one SDK, from scratch, into `sdk_<platform>.tar.gz` |
| `check_host.sh` | the Ubuntu packages the build needs |
| `common/make_sdk.sh` | the SDK's layout: native tools, target sysroots, licences (`LICENSES.txt`) |
| `common/`, `windows/`, `linux/`, `macos/` | the compiler, the packaging tools and each platform's system libraries, pinned by lock files |
| `sources/` | OnAir's own libraries (FFmpeg, SoapySDR, Hamlib, ...) built from pinned sources with the SDK's clang |

The Windows and Linux SDKs may be shared, with the source of their GPL and LGPL parts. The macOS SDKs hold Apple's macOS SDK: do not publish them.
