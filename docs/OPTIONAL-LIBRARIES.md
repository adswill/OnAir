# Optional libraries and codecs

OnAir loads a few libraries at run time that it does not ship: audio codecs, voice codecs, and the drivers of some radios. Without them the rest of
OnAir works; only the feature named in the table stays off. This page lists every one of them, the exact folder to put it in on Windows, macOS and
Linux, the file names OnAir looks for, and where to get it from its own project. **Settings > Advanced > Optional libraries and codecs** and
**Help > Voice codecs** show the same list with *found* or *missing* for each line on your computer.

> **Only download these from the official page of the project or of the radio's maker (the "Official source" column).** A DLL or library from a
> download site, a forum attachment or a "DLL fix" site can be malware: OnAir loads it into its own process, so it runs with your rights. If you are
> not sure, build it yourself from the project's source, or install it with your system's package manager (Homebrew, apt, MSYS2). OnAir never downloads
> any of these for you.

## The folder

Put the file in the **OnAir codecs folder**. It is the same folder for every library in this page:

| OS | Folder (survives an update) | Also searched |
|---|---|---|
| Windows | `%LOCALAPPDATA%\OnAir\codecs\` (usually `C:\Users\<you>\AppData\Local\OnAir\codecs\`) | `codecs\` next to `OnAir.exe` (for example `C:\Program Files\OnAir\codecs\`) |
| macOS | `~/Library/Application Support/OnAir/codecs/` | `codecs/` next to `OnAir.app` |
| Linux | `~/.local/share/onair/codecs/` (or `$XDG_DATA_HOME/onair/codecs/`) | `codecs/` next to the `dect2` program |

The **Open codecs folder** button (in the **?** help of a codec, in **Help > Voice codecs** and in the Settings list) opens the first one and makes it if
it is missing. Then press **Look again**; no restart is needed for most libraries (a radio driver is found the next time the radio list is scanned,
restart OnAir if it does not show up).

Why the codecs folder and not the program folder: the program folder (`C:\Program Files\OnAir\`) is replaced when you update OnAir, and a DLL you put
there can disappear (issue #472). The codecs folder in your user folder is never touched by an update. A DLL next to `OnAir.exe` still works, and the
copies that ship with OnAir there win over a copy in the codecs folder.

Folders are searched in this order, and the first file found wins: the program's own folder (the libraries OnAir ships), your user codecs folder, the
**extra folder** you set in Help > Voice codecs, the folder in the `ONAIR_CODEC_DIR` environment variable, the `codecs` folder next to the program, then
the system (Windows `PATH` and the vendor install folders, macOS Homebrew and MacPorts, Linux `/usr/lib`, `/usr/local/lib` and the multiarch folder).
A library a vendor installer puts in the system (the SDRplay API, PothosSDR, a package manager) is found there and needs no copying.

**Bit width and processor must match OnAir.** OnAir for Windows is 64-bit: use 64-bit DLLs. On a Mac use the build for the processor OnAir runs as
(Apple silicon: arm64; the Intel OnAir under Rosetta: x86_64). If a DLL needs other DLLs (an MSYS2 build often needs `libgcc_s_seh-1.dll`,
`libwinpthread-1.dll`, sometimes `libstdc++-6.dll`), put those in the same folder. Check the licence and the patent situation of a codec in your
country before you install it.

## Sound and codecs

| Library | Used for | File names OnAir looks for | Official source |
|---|---|---|---|
| **libfaad2** | DAB+ HE-AAC stations: SBR and parametric stereo. Without it they play band-limited. | Win `libfaad.dll`, `libfaad2.dll`, `libfaad-2.dll`, `libfaad2-2.dll`, `faad2.dll`, `faad.dll`; macOS `libfaad.dylib`, `libfaad.2.dylib`; Linux `libfaad.so.2`, `libfaad.so` | <https://github.com/knik0/faad2> (source). `brew install faad2`, `apt install libfaad2`, MSYS2 `mingw-w64-x86_64-faad2` |
| **LAME** (MP3 encoder) | Recording to MP3. WAV and FLAC work without it. | Win `libmp3lame.dll`, `libmp3lame-0.dll`, `lame_enc.dll`, `mp3lame.dll`; macOS `libmp3lame.dylib`, `libmp3lame.0.dylib`; Linux `libmp3lame.so.0`, `libmp3lame.so` | <https://lame.sourceforge.io> publishes **source code only**. Windows: build it, or take it from MSYS2 (`mingw-w64-x86_64-lame`, the file is `libmp3lame-0.dll`). macOS `brew install lame`, Linux `apt install libmp3lame0` |
| **libfdk-aac** | xHE-AAC on DRM stations that FFmpeg cannot decode. | Win `libfdk-aac-2.dll`, `libfdk-aac.dll`, `fdk-aac.dll`, `libfdk-aac-1.dll`, `fdk-aac-2.dll` (any `*fdk*aac*.dll`); macOS `libfdk-aac.2.dylib`, `libfdk-aac.dylib`; Linux `libfdk-aac.so.2`, `libfdk-aac.so` | <https://github.com/mstorsjo/fdk-aac> (source). `brew install fdk-aac`, `apt install libfdk-aac2` (non-free / multiverse), MSYS2 `mingw-w64-x86_64-fdk-aac`. Also `DECT2_FDKAAC_LIB` (full path). |
| **mbelib** or **mbelib-neo** | DMR, P25, NXDN, dPMR and D-STAR voice (patented AMBE / IMBE). Calls and data show without it. | mbelib: Win `mbelib.dll`, `libmbe.dll`, `libmbe-1.dll`, `mbe.dll`; macOS `libmbe.dylib`; Linux `libmbe.so.1`. mbelib-neo: `libmbe-neo.dll`, `libmbe-neo.dylib`, `libmbe-neo.so.2` | <https://github.com/lwvmobile/mbelib> and <https://github.com/arancormonk/mbelib-neo> (source; build it yourself, see [CODECS.md](CODECS.md)). Use one of the two |
| **codec2** | FreeDV and M17 speech. It comes with OnAir; add one only if this copy is incomplete. | Win `codec2.dll`, `libcodec2.dll`; macOS `libcodec2.dylib`; Linux `libcodec2.so.1.2` | <https://github.com/drowe67/codec2> |
| **FAAD2 with the HDC patch** | HD Radio sound (HDC is patented). Stations and data show without it. | `libfaad_hdc.dll` / `libfaad_hdc.dylib` / `libfaad_hdc.so` (also `libfaad2-hdc`) | Build it yourself from <https://github.com/theori-io/nrsc5> plus FAAD2 2.11.2, see [CODECS.md](CODECS.md#hd-radio-sound-hdc-build-it-yourself) |
| **DRA+ add-on** | China Digital Radio (CDR) sound. | `onair_dra.dll` / `libonair_dra.dylib` / `libonair_dra.so` | Built by you, see [CODECS.md](CODECS.md#dra-cdr-sound) |
| **FFmpeg `libavcodec` and `libavutil`** | ATSC 3.0 AC-4 sound, DTMB AVS+ and DRA. Everything else uses the FFmpeg inside OnAir. | Win `avcodec-NN.dll` and `avutil-NN.dll`; macOS `libavcodec.NN.dylib`; Linux `libavcodec.so.NN` (put the libavutil, libswresample ... of the same build next to it). Or set a folder in **Settings > Advanced > FFmpeg libraries** | <https://ffmpeg.org> (source) and the builds it links to; an FFmpeg with the AC-4 / AVS+ decoder you built yourself. See [CODECS.md](CODECS.md#ac-4-atsc-30-sound) |
| **Voice codec plug-ins** `onair-codec-<id>` | TETRA, TETRAPOL, Yaesu Fusion, AMBE+2, AMBE and IMBE voice. | `onair-codec-tetra.dll` / `.dylib` / `.so`, and `ambe2`, `ambe`, `imbe`, `ysf`, `tetrapol`, `aeroambe` | Built by you with `tools/codec-builder`, see [CODECS.md](CODECS.md#quick-start-install-the-voice-codecs-codec-setup) |

## Radio drivers

OnAir talks to most radios directly. These radios use the maker's own library, which OnAir finds in the codecs folder (since 3.8; before that only in
the program folder and the system). Some OnAir packages already include the library of an RTL-SDR or Airspy (check the *found* state first); add a library only when your radio is not found or to use a different build.

| Radio | File names OnAir looks for | Official source |
|---|---|---|
| **Fobos SDR** (RigExpert) | Win `fobos_sdr.dll`, `libfobos_sdr.dll`; macOS `libfobos_sdr.0.dylib`; Linux `libfobos_sdr.so.0` | the RigExpert site <https://rigexpert.com> (the software package of the Fobos SDR; its open-source library is called libfobos). Take the 64-bit `fobos_sdr.dll` from it |
| **bladeRF** (Nuand) | Win `bladeRF.dll`, `libbladeRF.dll`; macOS `libbladeRF.2.dylib`; Linux `libbladeRF.so.2` | <https://github.com/Nuand/bladeRF>, <https://www.nuand.com> |
| **HydraSDR** | Win `hydrasdr.dll`, `libhydrasdr.dll`; macOS `libhydrasdr.dylib`; Linux `libhydrasdr.so.1` | the HydraSDR maker's own download page |
| **LimeSDR** (LimeSuite) | Win `LimeSuite.dll`, `libLimeSuite.dll`; macOS `libLimeSuite.dylib`; Linux `libLimeSuite.so.23.11-1` and older | <https://github.com/myriadrf/LimeSuite> |
| **PlutoSDR** (libiio) | Win `libiio.dll`, `iio.dll`; macOS `libiio.0.dylib`; Linux `libiio.so.0` | <https://github.com/analogdevicesinc/libiio> |
| **USRP** (UHD) | Win `uhd.dll`, `libuhd.dll`; macOS `libuhd.dylib`; Linux `libuhd.so.4.x` | <https://github.com/EttusResearch/uhd>, Windows installer <https://files.ettus.com> |
| **SDRplay RSP** | install the SDRplay API with SDRplay's own installer (it also installs the service the radio needs); it puts `sdrplay_api.dll` in `C:\Program Files\SDRplay\API\x64\` and OnAir finds it there (Windows registry); macOS and Linux: `libsdrplay_api.so.3` in `/usr/local/lib`. Nothing to copy into the codecs folder | <https://www.sdrplay.com/api/> |
| **RTL-SDR** (librtlsdr) | Win `rtlsdr.dll`, `librtlsdr.dll`; macOS `librtlsdr.0.dylib`; Linux `librtlsdr.so.0` | <https://github.com/osmocom/rtl-sdr>; RTL-SDR Blog V4 <https://github.com/rtlsdrblog/rtl-sdr-blog> |
| **Airspy R2 / Mini** | Win `airspy.dll`, `libairspy.dll`; macOS `libairspy.0.dylib`; Linux `libairspy.so.0` | <https://github.com/airspy/airspyone_host>, Windows <https://airspy.us/downloads> |
| **Airspy HF+** | Win `airspyhf.dll`, `libairspyhf.dll`; macOS `libairspyhf.0.dylib`; Linux `libairspyhf.so.0` | <https://github.com/airspy/airspyhf>, Windows <https://airspy.us/downloads> |

HackRF, RX888 and several others are used over USB directly and need no library. A Windows driver (WinUSB / Zadig) is a separate matter and is
not a file in this folder.

## Plug-ins (not libraries)

OnAir plug-ins (decoders you add as a native library plus a manifest) go in the **plugins** folder of OnAir's data folder (**View > Plugins > Open
folder**), not in the codecs folder: see [plugins.md](plugins.md). They run native code with OnAir's rights, so install only plug-ins whose source or
author you trust.

## Check what OnAir found

- **Settings > Advanced > Optional libraries and codecs** (and **Help > Voice codecs**) lists every line above: *found* (hover for the file) or *missing*.
- For a radio driver the log says which library was used or why none was: the log line `radio library: <path>` or `radio library not found: <name>`
  (**Settings > Advanced > Save the log**; on Windows `onair.log` in the data folder).
- A DLL that is there but will not load is reported with the reason (a DLL it needs is missing, 32-bit instead of 64-bit, blocked by antivirus).
