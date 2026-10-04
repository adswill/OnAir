# Contributing to OnAir

Thank you for helping! Bug reports, test results from your radio and pull requests are all welcome.

## Reporting a problem

Open an issue and say: what you did, what you expected, what happened, your operating system, your radio and the channel's standard (DVB-T2, DVB-T, ATSC, DAB). On Windows, `%APPDATA%\OnAir\onair.log` is useful. A screenshot of the status bar and the constellations helps a lot. If a radio other than the HackRF behaves oddly, say so: those drivers are experimental.

## Building and testing

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=ON
cmake --build build -j
ctest --test-dir build
```

See the README for the packages each system needs, and `ARCHITECTURE.md` for how the code is organised. Windows can be built natively with MSYS2 (UCRT64, `tools/package/make_windows.sh`) or cross-compiled from macOS or Linux (`tools/package/make_windows_cross.sh`).

## Pull requests

- Keep a change focused, and explain **why** in the description. Small pull requests get reviewed faster.
- The build must pass on every system in the CI (Linux x86-64 and arm64, macOS Apple silicon and Intel, Windows). Keep system-specific code behind `#ifdef __APPLE__` / `_WIN32` and in the platform files.
- Add or extend a test when you change the receiver, the error correction or the player logic. The generators in the library make most tests possible without a radio.
- Do not change the decoded output of the receiver unless that is the point of the change. The tests compare it.
- Match the style around you: short comments that say why, no large reformatting of lines you did not change.
- Say how you tested it, and on which system and radio.

## Licence

OnAir is released under the GNU General Public License v3.0 or later. By sending a contribution you agree that it is released under the same licence.
