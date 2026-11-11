# Contributing to OnAir

Thank you for helping! Bug reports, test results from your radio and pull requests are all welcome.

## Reporting a problem

Open an issue and say: what you did, what you expected, what happened, your operating system, your radio and the channel's standard (DVB-T2, DVB-T, ATSC, DAB). On Windows, `%APPDATA%\OnAir\onair.log` is useful. A screenshot of the status bar and the constellations helps a lot. If a radio other than the HackRF behaves oddly, say so: those drivers are experimental.

## Building and testing

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=ON
cmake --build build -j "$(getconf _NPROCESSORS_ONLN)"   # one job per core (a bare -j starts hundreds and can run out of memory)
ctest --test-dir build
```

Rust is required (cargo and rustc 1.70 or newer; `rustup target add <triple>` for a cross build): the protocol, FEC, file and network layers listed in `ARCHITECTURE.md` are Rust and have no C++ twin. See the README for the packages each system needs, and `ARCHITECTURE.md` for how the code is organised. Windows can be built natively with MSYS2 (UCRT64, `tools/package/make_windows.sh`) or cross-compiled from macOS or Linux (`tools/package/make_windows_cross.sh`).

## Pull requests

- Keep a change focused, and explain **why** in the description. Small pull requests get reviewed faster.
- The build must pass on every system in the CI (Linux x86-64 and arm64, macOS Apple silicon and Intel, Windows). Keep system-specific code behind `#ifdef __APPLE__` / `_WIN32` and in the platform files.
- Add or extend a test when you change the receiver, the error correction or the player logic. The generators in the library make most tests possible without a radio.
- Do not change the decoded output of the receiver unless that is the point of the change. The tests compare it. The Rust-backed layers have golden-vector tests (`tests/<mode>/test_<mode>_proto_rs.cpp`): when a change of a Rust function changes the digest on purpose, update the value in the test and say why in the commit message. Do not leave a replaced C++ implementation behind an `#ifdef`; delete it (`rust/README.md` says how to add a module).
- Match the style around you: short comments that say why, no large reformatting of lines you did not change.
- Say how you tested it, and on which system and radio.

## Threads

Name every thread you create: `dect2::namedThread("onair-xxx", fn, args...)` instead of `std::thread`, or `dect2::setThreadName(...)` inside the thread function (`core/include/dect2/common/thread_name.h`). Short stable names, `onair-<what>`, at most 15 characters.

## Licence

OnAir is released under the GNU General Public License v3.0 or later. By sending a contribution you agree that it is released under the same licence, including the attribution term in [ADDITIONAL-TERMS.md](ADDITIONAL-TERMS.md) (GPL section 7(b)). This is "inbound = outbound": you keep the copyright of your contribution, it is covered by the holder line "OnAir contributors" (see REUSE.toml and docs/PROVENANCE.md), and there is no copyright assignment. If your contribution contains code or data from another project, say so in the pull request with its licence; it needs a REUSE.toml entry and a THIRD_PARTY.md row.

What to keep when copying OnAir code is in [NOTICE.md](NOTICE.md); the name and logo policy is in [TRADEMARKS.md](TRADEMARKS.md).
