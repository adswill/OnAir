# Voice codec build helper

Optional scripts that build the voice codec plug-ins (TETRA, AMBE+2, AMBE, IMBE, Yaesu Fusion) **on your own computer**, from the upstream source, and put them where OnAir looks for them. They live in `tools/codec-builder/` in the OnAir repository.

## Legal note, honestly

- These codecs (TETRA ACELP, IMBE, AMBE, AMBE+2) may be covered by patents in some countries. Whether you build and use them is your choice and your responsibility. Check the rules where you live, including the rules on listening to radio traffic. OnAir never decodes encrypted calls.
- OnAir the app never runs these scripts, never downloads anything for you and never links a codec. The OnAir project hosts no codec binary and no codec source.
- The scripts download the source of **mbelib-neo** (<https://github.com/arancormonk/mbelib-neo>, GPL-2.0-or-later) from its authors, on your computer, only when you run the script and type `yes`. The commit is pinned in the script and checked after the download.
- The small wrapper (`onair_codec_wrapper.c`) is OnAir's own code and contains no codec.

## What you get

| id | Plug-in file | For |
|---|---|---|
| `tetra` | `onair-codec-tetra` | TETRA voice (unencrypted calls) |
| `ambe2` | `onair-codec-ambe2` | DMR, NXDN, dPMR |
| `ambe` | `onair-codec-ambe` | D-STAR |
| `imbe` | `onair-codec-imbe` | P25 phase 1 |
| `ysf` | `onair-codec-ysf` | Yaesu Fusion, 7-byte V/D mode 2 frames only; the other Yaesu frame types need Yaesu's undocumented bit order and are refused by the wrapper |

The extension is `.dll` on Windows, `.dylib` on macOS and `.so` on Linux. The files are copied to the codecs folder: Windows `%LOCALAPPDATA%\OnAir\codecs`, macOS `~/Library/Application Support/OnAir/codecs`, Linux `$XDG_DATA_HOME/onair/codecs` (default `~/.local/share/onair/codecs`).

## macOS

1. Install the tools once: `xcode-select --install` and `brew install cmake git`.
2. Get the OnAir repository (or just the `tools/codec-builder` folder) and open Terminal in it.
3. Run `bash tools/codec-builder/build-codecs.sh`.
4. Type the ids you want (or `all`), read the notice, type `yes`.

## Linux

1. Install git, cmake and a C compiler, for example `sudo apt install git cmake build-essential`.
2. Run `bash tools/codec-builder/build-codecs.sh` and answer the prompts as above.

## Windows

1. Install MSYS2 from <https://www.msys2.org>. Open the **MSYS2 MINGW64** shell and run `pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-make git`. The script installs nothing itself.
2. Get the `tools\codec-builder` folder from the OnAir repository.
3. In PowerShell run `powershell -ExecutionPolicy Bypass -File tools\codec-builder\build-codecs.ps1` and answer the prompts.

## After the build

Open OnAir, **Help > Voice codecs**, press **Rescan**, and accept the notice for each codec. Without a plug-in the voice stays silent and the mode says "Codec not installed".

If a step fails the script prints the end of the log and the work folder path; nothing in OnAir is changed. The work folder can be deleted afterwards.
