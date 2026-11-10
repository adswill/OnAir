# USERLIBS (3.8): optional libraries documented, listed in the app, codecs folder for radio drivers

Issue #472 (adswill/OnAir). Branch `feat38-userlibs`.

## What changed
- **Search path:** the radio-driver loader (`libNames` in `core/src/source/native_common.h`: rtlsdr, airspy, airspyhf, bladeRF, fobos_sdr, hydrasdr, LimeSuite, libiio, UHD) only looked in the program folder and the system. It now also looks in the OnAir codecs folders (user folder, extra folder, `ONAIR_CODEC_DIR`, `codecs` next to the program), after the program folder on Windows, first on Linux, next to Frameworks on macOS. So `fobos_sdr.dll` in `%LOCALAPPDATA%\OnAir\codecs\` works and survives an update; the old place still works. The SDRplay API stays installer-only (it needs its service).
- **Already searched in the codecs folders before:** libfaad2 (DAB+), LAME (MP3), libfdk-aac, mbelib / mbelib-neo, codec2, HDC FAAD2, DRA+, FFmpeg add-ons (AC-4, AVS+), voice codec plug-ins.
- **Complete list** from grepping LoadLibrary / dlopen: also found but not user-added: Windows system DLLs (avrt, setupapi, d3d11, dxgi, vulkan-1), libasound, libsecret, MoltenVK/Vulkan; those are not in the list. OnAir plug-ins load from the plugins folder (named in the doc).
- **Doc:** `docs/OPTIONAL-LIBRARIES.md` (folders per OS, file names, official sources, malware warning), linked from `docs/CODECS.md`, `README.md`, the **?** codec help popup and the Voice codecs window.
- **In the app:** `Settings > Advanced > Optional libraries and codecs` and a section in **Help > Voice codecs**, from the new `dect2::codec::optionalLibraries()` (`core/src/codec/optional_libs.cpp`): one line each, found or missing, tooltip with file names, folder, official source.
- **Test:** `tests/codec/test_optional_libs.cpp` (matcher; a fake `fobos_sdr` in an isolated codecs folder is reported found).

## Validated / not validated
- Only syntax-checked locally (battery rule); build and the new test run in the one CI run.
- The Windows search order change has not been run on Windows. Not verified: the exact vendor pages for HydraSDR and Fobos (the list names the maker's own site, not a deep link); LAME has no official Windows binaries (source only), the doc says so.

## Draft reply for #472 (not posted)
> hey, thanks for the nudge. made a proper list: docs/OPTIONAL-LIBRARIES.md has every optional lib and codec (libfaad2, LAME mp3, libfdk-aac, mbelib, the codec plugins, and the radio dlls like fobos_sdr.dll), the exact folder on Windows/macOS/Linux, the file names OnAir looks for, the official place to get each one, and a warning about dlls from random sites. the same list is in Settings > Advanced with found/missing per line.
> also you were right it makes sense for fobos_sdr.dll to go in the codecs folder, so radio dlls are now searched there too (C:\Users\<you>\AppData\Local\OnAir\codecs\) and an update won't touch it. the old place next to OnAir.exe still works. coming in 3.8.
