# Voice and audio codecs

> **DMR, P25, NXDN, dPMR and D-STAR voice is silent until you install mbelib.** The calls, IDs and data are shown without it. Its vocoders are patented, so OnAir does not include it: put `libmbe` / `mbelib` in the codecs folder (the **?** button next to the sound has an **Open codecs folder** button), then press **Look again**.

> **CDR (China Digital Radio) sound is silent until you install the DRA+ add-on.** The services, signal information and data work without it. See [DRA+ (CDR sound)](#dra-cdr-sound).

> **HD Radio sound is silent until you install the HDC codec.** The stations, program data and pictures are shown without it. See [HD Radio sound](#hd-radio-sound-hdc-build-it-yourself).

OnAir never ships a patented codec. You install the ones you are allowed to use yourself, and OnAir loads them. The one open codec, **codec2** (FreeDV and M17 speech), comes with OnAir: nothing to install. **Whether you may
install or use them is your own responsibility: follow the patent and radio-listening law where you live.**

Every mode that needs a codec has a small **?** button. It shows whether the library was found and where, the exact file names OnAir
looks for, the exact folders it searches, an **Open codecs folder** button (it makes the folder if it is missing) and **Look again**
(searches again, no restart).

## Which mode needs which codec

| Mode | Codec | Library you provide |
|---|---|---|
| DMR, P25 Phase 1 and 2, NXDN, dPMR | AMBE+2 / IMBE (patented) | **mbelib** (not included: you install it yourself; voice is silent until you do) |
| D-STAR | AMBE (patented) | **mbelib** |
| HD Radio (NRSC-5, FM and AM) | HDC (patented, owned by Xperi) | a **patched FAAD2** (`libfaad_hdc`) that you build yourself (see below); sound is silent until you do |
| CDR (China Digital Radio) | DRA+ (patent and licence status not verified) | the **DRA+ add-on** (`libonair_dra`) that you build yourself (see [DRA+ (CDR sound)](#dra-cdr-sound)); sound is silent until you do |
| FreeDV 700D / 700E / 1600, M17 | codec2 (open, LGPL) | **codec2**: comes with OnAir (the library `libcodec2` next to the program) |
| FreeDV RADE | RADE neural modem (open, BSD) | **librade** (not codec2): optional; without it the RADE slot stays dark |
| Yaesu System Fusion (WIP) | AMBE+2 class (proprietary, DVSI) | a speech codec **plug-in with the id `ysf`** that you install yourself (see [Speech codec plug-ins](#speech-codec-plug-ins-voice-modes)); callsigns and data are shown without it, no sound |
| TETRA | its own speech codec | nothing from you: it is not mbelib and not codec2 |
| TETRA voice (unencrypted calls only) | TETRA ACELP | a **speech codec plug-in** `onair-codec-tetra` that you build yourself, for example around **mbelib-neo** (see [Speech codec plug-ins](#speech-codec-plug-ins-voice-modes)); the cell, network and call information work without it |
| TETRAPOL voice (clear calls only) | RP-CELP (PAS 0001-7, patented) | a **speech codec plug-in** `onair-codec-tetrapol` that you install yourself (see [TETRAPOL voice](#tetrapol-voice-id-tetrapol)); cells, frame sync and the call table work without it |
| Yaesu System Fusion (C4FM) voice | AMBE+2 (patented) | a **speech codec plug-in** with the codec id **`ysf`** (`onair-codec-ysf`) that you build yourself; callsigns, call mode and slow data work without it |
| KA modes (KAAL, KAPP, KAFT, KAVB, KAIT, KAWS) | their own codecs (unpatented, written for OnAir) | nothing: nothing to install |
| DAB, DRM, TV, FM | MP2, AAC, H.264 ... | nothing (FFmpeg is inside OnAir) |
| ATSC 3.0 (US, South Korea) | Dolby AC-4 (patented) | optional **libavcodec + libavutil from an FFmpeg with an AC-4 decoder** (see below); without it the AC-4 sound is left out, picture and AAC / AC-3 sound still play |

**codec2 does not help DMR, P25, NXDN or dPMR.** Putting `codec2.dll` or `libcodec2.dll` in the codecs folder only matters for FreeDV
and M17. If a digital-voice mode says "mbelib not found", you need **mbelib**, a different library.

## File names and search order (the same on every mode)

OnAir tries these file names. Folders are searched in this order; inside each folder every name is tried before the next folder.

1. The `DECT2_MBELIB_PATH` / `DECT2_CODEC2_PATH` / `DECT2_FAAD_HDC_PATH` / `DECT2_DRA_PATH` environment variable (a full path), if set.
2. The OnAir **codecs folders** (below).
3. The libraries that ship with OnAir: the program's own folder (Windows: next to `OnAir.exe`; Linux: next to `dect2`, and `../lib/onair` and `../lib` of it, which is where the `.deb` and the portable package put codec2; macOS: inside the app, `Contents/Frameworks` and `Contents/Resources`). A copy you put in a codecs folder (step 2) wins over the shipped one.
4. The system: Windows `PATH`; macOS `/opt/homebrew/lib`, `/usr/local/lib`, `/opt/local/lib`; Linux `/usr/local/lib`, `/usr/lib`, `/usr/lib64`, the multiarch folder.

| Library | Windows | macOS | Linux |
|---|---|---|---|
| mbelib | `mbelib.dll`, `libmbe.dll`, `libmbe-1.dll`, `mbe.dll` | `libmbe.1.3.dylib`, `libmbe.1.dylib`, `libmbe.dylib` | `libmbe.so.1.3`, `libmbe.so.1`, `libmbe.so` |
| HDC (patched FAAD2) | `libfaad_hdc.dll`, `faad_hdc.dll`, `libfaad2-hdc.dll`, `faad2-hdc.dll` | `libfaad_hdc.dylib`, `libfaad_hdc.2.dylib`, `libfaad2-hdc.dylib` | `libfaad_hdc.so`, `libfaad_hdc.so.2`, `libfaad2-hdc.so` (any `libfaad_hdc.so.*`) |
| DRA+ add-on | `onair_dra.dll`, `libonair_dra.dll` | `libonair_dra.dylib` | `libonair_dra.so` |
| codec2 | `codec2.dll`, `libcodec2.dll` | `libcodec2.1.2.dylib` ... `libcodec2.dylib` | `libcodec2.so.1.2` ... `libcodec2.so` (any `libcodec2.so.*`) |

### The OnAir codecs folders

| OS | Next to the program | In your user folder |
|---|---|---|
| Windows | `codecs\` next to `OnAir.exe` | `%LOCALAPPDATA%\OnAir\codecs\` |
| macOS | `codecs/` next to `OnAir.app` | `~/Library/Application Support/OnAir/codecs/` |
| Linux | `codecs/` next to the `dect2` program | `~/.local/share/onair/codecs/` (or `$XDG_DATA_HOME/onair/codecs/`) |

Every codec loader (the voice plug-ins and all the libraries: mbelib, codec2, HDC, DRA+, AC-4, AVS) searches **all** of these, in this order,
and the first folder that has the file wins:

1. your user folder (table above, right column)
2. the **extra folder** you type in **Help > Voice codecs** (saved in your settings)
3. the folder in the `ONAIR_CODEC_DIR` environment variable, if set
4. the `codecs` folder next to the program (table above, left column; on macOS also `OnAir.app/Contents/Resources/codecs`)

Duplicates are dropped (on Windows, `C:/Radio/Onair` and `c:\radio\onair\` are the same folder). If you installed OnAir in a folder of your own
(for example `C:\Radio\SDRApps\Onair`), a `codecs` folder next to `OnAir.exe` works for every plug-in, and so does the `%LOCALAPPDATA%` one.
If the same plug-in file is in two folders, **Help > Voice codecs** shows which folder each plug-in was found in and warns about the copy that
is ignored. Its **Open codecs folder** button opens your user folder; **Open folder next to OnAir** and **Open extra folder** open the others.

The **?** popup prints the full paths on your computer. A DLL must match OnAir: 64-bit with 64-bit. If it needs other DLLs
(for example `libgcc_s_seh-1.dll`, `libwinpthread-1.dll`), put them in the same folder.

## mbelib (DMR, P25, NXDN, dPMR, D-STAR voice): build it yourself

mbelib decodes AMBE/IMBE, which are patented in some countries (the project's own notice says so). OnAir does not ship, download or
link to binaries of it. Upstream project: <https://github.com/szechyjs/mbelib> (a maintained fork: <https://github.com/lwvmobile/mbelib>). OnAir loads it at run time and contains none of its source. Check your local law first.

**Windows (MSYS2 / MinGW).** Install MSYS2 (msys2.org), open the "MSYS2 MINGW64" shell, paste:

```
pacman -S --needed git mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake make
git clone https://github.com/lwvmobile/mbelib
cd mbelib && mkdir build && cd build
cmake .. -G "MSYS Makefiles" -DBUILD_SHARED_LIBS=ON && make
```

Copy the resulting `libmbe.dll` (look in the `build` folder) and, if OnAir says it does not load, `libgcc_s_seh-1.dll` and
`libwinpthread-1.dll` from `C:\msys64\mingw64\bin`, into `%LOCALAPPDATA%\OnAir\codecs\`. Press **Look again**.

**macOS.**

```
brew install cmake git
git clone https://github.com/lwvmobile/mbelib
cd mbelib && mkdir build && cd build
cmake .. && make
mkdir -p "$HOME/Library/Application Support/OnAir/codecs"
cp libmbe*.dylib "$HOME/Library/Application Support/OnAir/codecs/"
```

**Linux.**

```
sudo apt install git cmake build-essential     # or your distribution's equivalent
git clone https://github.com/lwvmobile/mbelib
cd mbelib && mkdir build && cd build
cmake .. && make && sudo make install && sudo ldconfig
```

(or copy `libmbe.so*` into `~/.local/share/onair/codecs/`). If the build gives a static library only, add `-DBUILD_SHARED_LIBS=ON`.

Then open the mode and look at its **?** button: it says `mbelib found: <path>`. Encrypted P25/NXDN/DMR calls are only reported as
encrypted; OnAir never decrypts. DMR voice is played through mbelib or through the `ambe2` plug-in (see below).

## codec2 (FreeDV, M17)

codec2 is open software (LGPL-2.1) and **comes with OnAir**: FreeDV and M17 speech works after you download OnAir, with nothing to install.
It is only for FreeDV and M17.

- Where it is: Windows `libcodec2.dll` next to `OnAir.exe`; macOS `OnAir.app/Contents/Frameworks/libcodec2.1.2.dylib`; Linux the `.deb` puts it in
  `/usr/lib/onair/`, the portable package in `lib/` (next to `bin/`).
- It is a **separate shared library** that OnAir loads at run time, not part of the OnAir program, as the LGPL asks for. The source is in
  `third_party/codec2` (the unmodified 1.2.0 release, see `third_party/codec2/README-OnAir.md`), and every package carries the licence text
  (`licenses/codec2/COPYING`) and a notice.
- **Use your own build instead:** put your codec2 (`libcodec2.so.1.2`, `libcodec2.1.2.dylib`, `codec2.dll` or `libcodec2.dll`) in the codecs folder
  (the **Open codecs folder** button of the **?** popup), or set `DECT2_CODEC2_PATH` to its full path. Those are searched before the shipped copy.
- If the **?** button says `codec2 not found`, this copy of OnAir is incomplete (for example only `OnAir.exe` was copied out of the zip):
  unpack or install the whole package again. Or install codec2 yourself: macOS `brew install codec2`; Linux `sudo apt install libcodec2-1.2`
  (Ubuntu 22.04: `libcodec2-1.0`) or `sudo dnf install codec2`; Windows `libcodec2.dll` from a FreeDV install or MSYS2
  (`pacman -S mingw-w64-x86_64-codec2`, with the DLLs it needs) into the codecs folder.
- Building OnAir yourself: the codec2 library is built with it (`cmake --build build`, file `libcodec2*` next to the programs in the build folder).
  `-DDECT2_BUNDLE_CODEC2=OFF` leaves it out; OnAir then uses an installed codec2 as before.

Open HF digital and pick FreeDV (or M17): the **?** button says `codec2 found: <path>`.

## HD Radio sound (HDC): build it yourself

HD Radio (NRSC-5) sound is coded with **HDC**, a modified HE-AAC. HDC is a proprietary codec owned by Xperi (HD Radio) and covered by patents.
**OnAir does not include it, does not download it and contains no HDC or FAAD2 code.** Without it OnAir still shows the stations, the program
list, the program data (title, artist), the pictures and the other data services; the sound stays silent and the status says
*HDC sound codec not installed*. Whether you may build and use it depends on the patent and licence rules of your country; check them first,
as with mbelib. Listening to radio services is regulated in many countries as well.

The open-source **nrsc5** project (<https://github.com/theori-io/nrsc5>) decodes HDC with FAAD2 plus a patch of its own
(`support/faad2-hdc-support.patch`). If you build that patched FAAD2 as a shared library, OnAir loads it and plays the program you select on
the right (HD1, HD2, HD3 ...), with the volume and mute of the program. Steps (about five minutes; use FAAD2 **2.11.2**, newer versions break the patch):

The patch matches FAAD2 **2.11.2**; with a newer FAAD2 some hunks fail and the build stops with `call to undeclared function 'IS_HDC'` (issue #222). The `--branch 2.11.2` in the steps below keeps it working.

**macOS.**

```
brew install cmake git
git clone https://github.com/theori-io/nrsc5
git clone --branch 2.11.2 https://github.com/knik0/faad2   # 2.11.2: the nrsc5 patch does not apply to newer FAAD2
cd faad2
patch -p1 < ../nrsc5/support/faad2-hdc-support.patch
mkdir build && cd build
cmake .. -DBUILD_SHARED_LIBS=ON && make
mkdir -p "$HOME/Library/Application Support/OnAir/codecs"
cp libfaad_hdc*.dylib "$HOME/Library/Application Support/OnAir/codecs/"
```

**Linux.**

```
sudo apt install git patch cmake build-essential     # or your distribution's equivalent
git clone https://github.com/theori-io/nrsc5
git clone --branch 2.11.2 https://github.com/knik0/faad2   # 2.11.2: the nrsc5 patch does not apply to newer FAAD2
cd faad2
patch -p1 < ../nrsc5/support/faad2-hdc-support.patch
mkdir build && cd build
cmake .. -DBUILD_SHARED_LIBS=ON && make
mkdir -p ~/.local/share/onair/codecs
cp libfaad_hdc.so* ~/.local/share/onair/codecs/
```

**Windows (MSYS2 / MinGW).** In the "MSYS2 MINGW64" shell:

```
pacman -S --needed git patch mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake make
git clone https://github.com/theori-io/nrsc5
git clone --branch 2.11.2 https://github.com/knik0/faad2   # 2.11.2: the nrsc5 patch does not apply to newer FAAD2
cd faad2
patch -p1 < ../nrsc5/support/faad2-hdc-support.patch
mkdir build && cd build
cmake .. -G "MSYS Makefiles" -DBUILD_SHARED_LIBS=ON && make
```

Copy `libfaad_hdc.dll` (look in the `build` folder) into `%LOCALAPPDATA%\OnAir\codecs\`, together with `libgcc_s_seh-1.dll` and
`libwinpthread-1.dll` from `C:\msys64\mingw64\bin` if OnAir says it does not load. 64-bit with 64-bit.

Then open **HD Radio** and press **Look again** in the **?** popup next to the sound (no restart needed): it says
`HDC library found: <path>`. Pick a program in the list on the right to hear it.

- **The library must be the patched build**: a plain FAAD2 (`libfaad`) has no `NeAACDecInitHDC` entry point and OnAir refuses it with that
  reason. The patch is written for the faad2 releases around 2.10 and 2.11; if it does not apply to the newest source, check out the 2.10.x or 2.11.x tag
  (`git checkout 2.10.1` in the faad2 folder) and apply it again.
- OnAir checks the FAAD2 version (2.7 to 2.11) and the layout of the library's frame information when it loads, and refuses anything else with a plain
  message instead of risking a crash. Set `DECT2_FAAD_HDC_PATH` to the full path of the file to use one that is not in a codecs folder.
- The sound is 44.1 kHz stereo, played with the same volume, mute and automatic level control as the other modes. A missing or damaged packet is
  silence for that packet, and the decoder starts again after it; three decode errors in a row also restart it.
- Only the main stream of each program is played (the digital "core"); the hybrid enhancement streams are ignored, as in nrsc5.
- OnAir has been tested against a stand-in library, not against a real patched FAAD2 and a real station; if the real library behaves differently
  (a different file name, or a version the check refuses), please open an issue with the message the **?** popup shows.

## DRA+ (CDR sound)

**CDR sound is silent until you install the DRA+ add-on.** CDR (China Digital Radio) codes its sound with DRA+. OnAir does not include a DRA+
decoder and contains no DRA+ decoder source: whether DRA / DRA+ is covered by patents, and where the decoder we tested with came from, has not
been verified. So it is handled like mbelib and HDC: an add-on that you obtain and build yourself, which OnAir loads when it is found.

**What works without it:** everything but the sound. The multiplex, the services and their names, signal quality, the plots and all data are shown;
you can pick a service and use **Save stream** to write its raw DRA+ sound to a `.dra` file (see the README, "Saving DRA sound"). When you select
a service with sound and the add-on is missing, the CDR panel shows an amber **!** notice with a **How to install** button, and the log says so once.
With the add-on loaded the line turns into a green *DRA+ decoder loaded*.

**What the add-on is:** one small shared library, `libonair_dra`, made from the open-source DRA decoder in maliwen2015's repository
<https://github.com/maliwen2015/ffmpeg_cavs_dra> (MIT as published there; it sits in the file `ffmpeg-7.1.2_cavs_dra.patch`). It exports five C
functions (`DRADecCreate`, `DRADecDestroy`, `DRADecGetFrameInfo`, `DRADecSendData`, `DRADecRecvFrame`); OnAir feeds it one DRA frame at a time and
plays the 16-bit PCM it returns. It reads no files and uses no network. OnAir includes only the loader and the build files in `tools/addons/dra`;
the decoder source is **not** in this repository and no prebuilt library is ever shipped, installed or put in a package.

**Where to put it** (the exact file name matters):

| OS | File name | Codecs folder (the **?** popup has an **Open codecs folder** button) |
|---|---|---|
| macOS | `libonair_dra.dylib` | `~/Library/Application Support/OnAir/codecs/` (or a `codecs` folder next to `OnAir.app`) |
| Linux | `libonair_dra.so` | `~/.local/share/onair/codecs/` (or `$XDG_DATA_HOME/onair/codecs/`; or a `codecs` folder next to the program) |
| Windows | `onair_dra.dll` (or `libonair_dra.dll`) | `%LOCALAPPDATA%\OnAir\codecs\` (or a `codecs` folder next to `OnAir.exe`) |

Or set `DECT2_DRA_PATH` to the full path of the file. Press **Look again** in the **?** popup: no restart is needed.

**Build it** (about a minute; you need git, Python 3, CMake and a C compiler). The build files are in the OnAir source, `tools/addons/dra`:

macOS:
```
brew install cmake git python3
git clone https://github.com/adswill/OnAir && cd OnAir/tools/addons/dra
python3 extract_dra.py
cmake -S . -B build -DDRA_SRC=dra_src && cmake --build build
mkdir -p ~/Library/Application\ Support/OnAir/codecs && cp build/libonair_dra.dylib ~/Library/Application\ Support/OnAir/codecs/
```
Linux:
```
sudo apt install git python3 cmake build-essential
git clone https://github.com/adswill/OnAir && cd OnAir/tools/addons/dra
python3 extract_dra.py
cmake -S . -B build -DDRA_SRC=dra_src && cmake --build build
mkdir -p ~/.local/share/onair/codecs && cp build/libonair_dra.so ~/.local/share/onair/codecs/
```
Windows (MSYS2 MINGW64 shell; use the 64-bit one if OnAir is 64-bit):
```
pacman -S --needed git python mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake make
git clone https://github.com/adswill/OnAir && cd OnAir/tools/addons/dra
python extract_dra.py
cmake -S . -B build -G "MSYS Makefiles" -DDRA_SRC=dra_src && cmake --build build
mkdir -p "$LOCALAPPDATA/OnAir/codecs" && cp build/onair_dra.dll "$LOCALAPPDATA/OnAir/codecs/"
```
`extract_dra.py` downloads the patch from the repository above and writes `dradec.c` and `dradec.h` (it also removes the bandwidth-extension
rate doubling, which made the sound play at twice the speed; it refuses to run if the upstream file is not the version it knows). Instead of
downloading, you can point it at a copy of the patch you already have: `python3 extract_dra.py ffmpeg-7.1.2_cavs_dra.patch`. The library may need
`libgcc_s_seh-1.dll` and `libwinpthread-1.dll` from `C:\msys64\mingw64\bin` next to it on Windows.

Whether you may build and use it depends on the patent and licence rules of your country; check them first. OnAir has been tested with a stand-in
library in its automated tests and with the decoder built from the source above on recordings; there is no guarantee for other builds.

## AVS+, AVS2, AVS3 and DRA (DTMB television in China)

DTMB channels in China send their pictures as AVS+ (stream type 0x42; some as AVS2, 0xD2, or AVS3, 0xD4) and some send their sound as
DRA. These are Chinese patented codecs, and OnAir ships none of them: it plays what the FFmpeg built into it can decode.

- AVS1 in its JiZhun profile: FFmpeg's `cavs` decoder plays it (the macOS and Windows downloads; the Linux packages carry a smaller
  FFmpeg without it).
- AVS+ (the broadcast profile, 0x48): FFmpeg's `cavs` decoder refuses it ("only support JiZhun profile", FFmpeg 8 and 9). OnAir lists
  the service as TV, plays its sound, and says in the player that there is no picture, unless you install the AVS+ add-on (below).
- AVS2 and AVS3: FFmpeg decodes them only when it is built with libdavs2 or libuavs3d, which the FFmpeg in OnAir is not. Save the stream
  (Outputs: file or UDP) and play it with an FFmpeg build that has them (ffplay).
- DRA sound: FFmpeg has no DRA decoder, so OnAir plays it through the same **DRA add-on** as CDR (see [DRA+ (CDR sound)](#dra-cdr-sound)). A DTMB service
  whose sound is DRA (stream type 0x14, or private data with the registration descriptor "DRA1") lists DRA in the **audio** box next to its language;
  pick the language you want. With the add-on installed the player cuts the stream into DRA frames, feeds them to the add-on and plays the PCM in step
  with the picture (the PES times drive the same clock as any other sound). Without it the player shows an amber notice, "DRA audio: install the DRA
  add-on to hear this service", with a **?** button to the install steps (also in Help > Voice codecs); a second sound track in another format plays.
  Status: tested with a synthetic stream and a stand-in add-on only; not tried on a real DTMB broadcast or with the real decoder. The stream type 0x14
  is from memory of the Chinese standard (GY/T 220.1) and not verified.

**The AVS+ add-on (optional, you build it).** If you put `libavcodec` and `libavutil` (shared libraries) from an FFmpeg that has an AVS+ decoder
into the codecs folder (or the folder in `DECT2_AVSPLUS_DIR`), OnAir loads them at run time (the same way as the AC-4 add-on, so the FFmpeg may
be of another major version than OnAir's; libavcodec 59 to 64, FFmpeg 5 to 9, are accepted) and decodes the video packets of a stream whose sequence header says
AVS+ (profile 0x48, PMT stream type 0x42) with it. The service list shows an **AVS+** badge on the video; without the add-on the TV panel says
"install the AVS+ add-on to see the picture" with a **?** that opens the instructions, and the same row is on Help > Voice codecs. An add-on put
into the folder while OnAir runs is picked up within about three seconds (or press **Look again**); the picture starts at the next sequence
header. OnAir looks for a decoder named `cavs` in your library (set `DECT2_AVSPLUS_DECODER` if your build names it differently).

*Where it comes from.* The only public source known to this project is <https://github.com/maliwen2015/ffmpeg_cavs_dra> (MIT; "patches FFmpeg to
support AVS+ and DRA decoding"): a patch `ffmpeg-7.1.2_cavs_dra.patch` for FFmpeg 7.1.2. Its page has no build instructions (and ships
`ffmpeg.zip` / `ffplay.zip` archives that OnAir neither downloads nor uses). Build it yourself with the ordinary FFmpeg steps:

1. Get the FFmpeg 7.1.2 source (ffmpeg.org) and apply the patch from its top folder: `patch -p1 < ffmpeg-7.1.2_cavs_dra.patch`
   (check the patch's own paths; use `-p0` if it was made without a prefix).
2. Configure shared libraries only: `./configure --enable-shared --disable-static --disable-programs --disable-doc`, then `make -j4`.
   Linux: gcc, make, nasm. macOS: Xcode command line tools and `brew install nasm`. Windows: MSYS2 (MinGW-w64 gcc, make, nasm), and add
   `--arch=x86_64 --target-os=mingw32` if configure does not guess it.
3. Take `libavcodec` and `libavutil` from the build (`libavcodec.62.dylib`-style names on macOS, `libavcodec.so.61` on Linux, `avcodec-61.dll`
   on Windows) together with the libraries they depend on (`libswresample`, ...) and put them side by side in the codecs folder. Press
   **Look again**.

**Not verified.** Tested here with a stock decoder standing in for the add-on (loading, calling, picture times, hot pickup) and with a synthetic
transport stream; **never with a real AVS+ broadcast or the real add-on**. Open points: whether the patched decoder is named `cavs`, whether its
pictures are 4:2:0 8 bit (anything else is refused with a message), interlaced and aspect-ratio signalling (the stream's own aspect is used).
Licences: the project is MIT and FFmpeg LGPL, so building it yourself is fine for copyright; AVS+ is covered by patents (check where you live).
OnAir ships and downloads nothing of it.

Details and the external-player route (save the TS / stream UDP, copy the VLC / mpv command): [AVSPLUS.md](AVSPLUS.md).

The player panel has a **?** button next to these messages. A channel that signals a CA system (a CA descriptor, free_CA_mode) but sends
its streams in the clear is not shown as scrambled: OnAir goes by the scrambling bits of the stream packets.

## AMBE (Inmarsat Aero voice)

Aero C channel voice uses an AMBE vocoder. OnAir decodes the P channel data only and does not tune voice channels. Separate programs,
for example JAERO (<https://github.com/jontio/JAERO>), work on C channels.

## AMBE variant (Iridium voice)

OnAir ships no Iridium vocoder. Voice slots are grouped into calls (Iridium > Calls: time, frequency, direction, length, satellite and
beam when a broadcast frame on that frequency named them, status) with a Listen selector and one WAV recording per call in the recordings
folder. Sound needs a plug-in `onair-codec-iridium` (id `iridium`, ABI in `core/include/dect2/codec/speech_codec.h`) that you install
yourself. Each call to `onair_speech_decode` gets the 39 bytes (312 bits) of the voice field exactly as received (over-the-air bit order,
MSB first; no de-scrambling or de-interleaving by OnAir) and returns PCM. **Unverified:** this has only been tested with synthetic
bursts and a stub plug-in, never a real Iridium recording; whether the voice field needs further de-interleaving or FEC before the vocoder
is for the plug-in. No public description defines an "encrypted" marker, so none is shown: frames that fail their checks or that the
plug-in refuses are listed as "not readable" and stay silent, and OnAir never decodes encrypted traffic or works on keys.

Alternatively, **Save for iridium-toolkit** writes a `.bits` file. Then:

1. Get iridium-toolkit: <https://github.com/muccc/iridium-toolkit>
2. `iridium-parser.py output.bits > output.parsed`
3. Install the `ir77_ambe_decode` program from osmo-ir77 (<http://git.osmocom.org/osmo-ir77>), as the toolkit's README explains, and
   use its `stats-voc.py` or `voc-cluster.py` to play the voice.

## AAC, xHE-AAC and the other FFmpeg codecs

DAB/DAB+ (MP2, AAC), DRM (AAC, xHE-AAC) and the TV modes decode with FFmpeg, which is built into every OnAir download (FFmpeg 8.0).
Nothing to install, with one exception: the SBR and parametric stereo of DAB+ HE-AAC (HE-AAC and HE-AAC v2 stations). FFmpeg cannot apply
them to the 960-sample frames of DAB+ ("SBR with 960 frame length"), so without help those stations play the core only: band-limited, and
HE-AAC v2 in mono. OnAir uses faad2 for them when it is installed (Linux: the `libfaad2` package; macOS: `brew install faad2`; Windows: a
`libfaad.dll`, `libfaad2.dll` or `libfaad-2.dll` beside the program, for example the one that ships with QT-DAB). It is found at run time and not shipped; the DAB player panel says which decoder is working and whether SBR
and PS are applied. Some xHE-AAC tools (harmonic SBR, PVC, inter-TES, MPEG Surround 212) are not in FFmpeg yet; DRM stations that use
them stay silent and the DRM view says so.

### xHE-AAC for DRM (optional libfdk-aac)

Some DRM stations (for example the CNR-1 xHE-AAC broadcast on 13.825 MHz) use tools that FFmpeg's USAC decoder lacks. OnAir can use
**libfdk-aac** for them. It is never shipped or linked: you install it yourself, OnAir finds it at run time and the DRM view says
"(libfdk-aac)" next to the audio format when it is used. Without it the DRM view says which library to install.
Install: Linux `libfdk-aac2` (Debian/Ubuntu, may need non-free/multiverse) or `fdk-aac`; macOS `brew install fdk-aac`; Windows a
`libfdk-aac-2.dll` (or any `*fdk*aac*.dll`) in the OnAir **codecs folder** (see below; press **Look again** or restart OnAir) or beside the program. A different file can be named in the environment variable `DECT2_FDKAAC_LIB`.
The DRM view's **Audio** section has a line **xHE-AAC decoder** that says, in plain words, one of three things: *loaded* (with the file
it loaded), *not installed* (with the exact file name and the codecs folder path, and an **Open codecs folder** button), or *found but
not loaded* with the reason: on Windows "a DLL it needs is missing" (an MSYS2/MinGW build also needs `libgcc_s_seh-1.dll`,
`libwinpthread-1.dll` and sometimes `libstdc++-6.dll` in the same folder) or "wrong kind of DLL" (a 32-bit build; OnAir is 64-bit).
**Look again** loads it without restarting OnAir.
Check the licence and your local patent situation before installing it. Status: the frames and the stream configuration follow ETSI ES 201 980;
the decoding with a real libfdk-aac against a real DRM xHE-AAC recording has not been verified yet.

## AC-4 (ATSC 3.0 sound)

ATSC 3.0 broadcasts in the US and South Korea (and a few DVB services) carry their sound as Dolby AC-4. FFmpeg, including the one inside
OnAir, has **no AC-4 decoder** (it knows the codec id but no decoder: `ffmpeg -h decoder=ac4` says so). OnAir therefore leaves AC-4 sound out
and says so in the ATSC 3.0 tab and the TV tab, with a **?** card. Picture and any AAC or AC-3 sound track of the service play as usual.

**Optional support.** If you provide `libavcodec` and `libavutil` (shared libraries) from an FFmpeg that contains a decoder named `ac4`, OnAir
loads them at run time from the codecs folders (or the folder in `DECT2_AC4_DIR`), decodes the AC-4 access units of the selected service,
mixes them to stereo or 5.1 and plays them (internally: decoded with your library, encoded as AC-3 by OnAir's own FFmpeg, put in the
transport stream). The libraries are used through a small, stable interface, so an FFmpeg of another major version than OnAir's works.
Put them, and the libraries they need (`libswresample` and others), together in the codecs folder, then press **Look again**.

**Where such an FFmpeg comes from.** The only public AC-4 decoder known to this project is the patch
`ffmpeg_ac4.patch` in <https://github.com/funnymanva/ffmpeg-with-ac4> (FFmpeg 6.1 plus a 5,900 line decoder, `libavcodec/ac4dec.c`, by Paul B Mahol, 2019).
Its `build-ffmpeg` script builds a **static** `ffmpeg` program with `--disable-shared`, which does not give OnAir what it needs: apply the patch
to FFmpeg 6.1 yourself and configure it with `--enable-shared --disable-static`. The patch does not build with FFmpeg 7 or newer (it uses the
removed `avctx->channels`). OnAir's test of this path used a stock FFmpeg with the AC-3 decoder standing in for the AC-4 one; **it has not been
tried on a real AC-4 broadcast**, and nobody has checked how complete the patch's decoder is (OnAir uses its first presentation, its default).

**Licences.** The patch's decoder carries the LGPL-2.1-or-later header of FFmpeg, and the build script is MIT-licensed (it is derived from
markus-perl/ffmpeg-build-script): for copyright you may build and use them yourself. Copyright is not the whole story: AC-4 is a Dolby codec covered
by patents, licensed to makers of products (and Dolby's AC-4 specification is ETSI TS 103 190, public, but implementing it is not a patent licence).
OnAir ships no AC-4 code, no FFmpeg patch and downloads nothing; the build script above also pulls in GPL and non-free libraries if you ask it to
(`--enable-gpl-and-non-free`), which would make that binary unredistributable. Whether you may use it where you live is your responsibility.

## Speech codec plug-ins (voice modes)

A voice mode that needs a speech codec OnAir does not ship (TETRA voice with the id `tetra`, TETRAPOL voice with the id `tetrapol`, Yaesu Fusion voice with the id `ysf`, later Iridium voice) loads it from the codecs folders (see "The OnAir codecs folders" above: your user folder first, then the extra folder,
`ONAIR_CODEC_DIR`, and the `codecs` folder next to the program): `onair-codec-<id>.dylib` / `.so` / `.dll` (user folder: macOS
`~/Library/Application Support/OnAir/codecs/`, Linux `~/.local/share/onair/codecs/`, Windows `%LOCALAPPDATA%\OnAir\codecs\`). Without it the
mode says "Codec not installed" and links here. The first time, OnAir shows a legal notice: you are responsible for following the laws
where you live on listening to radio traffic and for the codec's licence; OnAir does not include or distribute the codec. The codec loads only
after you accept it once. The plug-in's C interface is in `core/include/dect2/codec/speech_codec.h`.

TETRA voice: OnAir removes the radio channel coding (ETSI EN 300 395-2 clause 5.5) and gives the plug-in one speech frame per call of `onair_speech_decode`: 137 bytes, one bit per byte (0 or 1), byte i is speech frame bit B(i+1) of table 4 of that standard (the output order of the source encoder). Two frames per burst, 30 ms each, in order. Frames whose CRC fails are dropped, not passed on. Encrypted calls, and calls whose set-up OnAir did not hear saying "clear", are never decoded and never reach the plug-in.
Yaesu Fusion (id `ysf`) hands the plug-in one voice frame per call, as it was sent: 9 bytes (the 72 channel bits of V/D mode 1), 7 bytes (the 49 voice
bits of V/D mode 2 after the error correction, MSB first) or 18 bytes (the 144 channel bits of voice FR). The bit order inside the V/D mode 1 and voice FR frames is not in
Yaesu's public specification, so the plug-in has to do that part itself.

Recommended source for a TETRA plug-in: mbelib-neo (https://github.com/arancormonk/mbelib-neo), GPL-2.0-or-later. It has a TETRA full-rate speech decoder (`mbe_tetraDecodeFrame`) that takes the same 137 bytes, one bit per byte, B(1)..B(137), so a small wrapper exporting the four `onair_speech_*` functions is enough. Its authors have not cleared the patent status of the TETRA codec, so check that you may use it where you live. You build it yourself; OnAir does not include it.

Yaesu Fusion voice (id `ysf`): OnAir decodes the C4FM frames, removes the channel coding and gives the plug-in the AMBE+2 voice bits of each frame through the same four `onair_speech_*` functions. The decoder behind it is yours to supply, for example from mbelib-neo (it handles IMBE, AMBE and AMBE+2). The same rules apply: you build it, OnAir does not include it, and a legal notice is shown once. Without the plug-in the mode shows the callsigns and data and says "Codec not installed".

### TETRAPOL voice (id `tetrapol`)

OnAir removes the TETRAPOL channel coding (PAS 0001-2 clause 6: the transmission differential, scrambling, UHF precoding, interleaving and the
protected part's code) and calls `onair_speech_decode` once per 20 ms voice frame with **16 bytes**:

| Bytes | Content |
|---|---|
| 0 .. 14 | the 120 RP-CELP bits of the frame, bit k (k = 0 .. 119) in byte k / 8 under the mask `0x80 >> (k % 8)` (most significant bit first). k = 0 .. 19 are `voice1` (the protected bits), k = 20 .. 119 `voice2`, in the order they are in the frame: the PAS 0001-7 bit numbering 0 .. 119 |
| 15 | flags: bit 0 set when the frame is damaged (the CRC-3 of the protected part failed); the other bits are 0 |

The plug-in answers with 160 samples of 8 kHz PCM (another rate is resampled). A damaged frame should be concealed (repeat or fade the last
frame). The plug-in's own sample rate comes from `onair_speech_sample_rate`.

**Only clear calls reach the plug-in.** A frame is handed over only when the call's own signalling, read in the clear on the same channel
(`D_CALL_START` or `D_CALL_CONNECT`), carried a key reference with key index 0 (a clear call, PAS 0001-3-2). Encrypted calls (any other
key index) and calls whose state OnAir did not hear (tuned in after the start) are never decoded and never reach the plug-in: no key is
used, guessed or stored. Most TETRAPOL networks encrypt their traffic, so most calls stay silent and show "encrypted (not decoded)".

**Listen to** (clear calls / nothing) and **Record calls** (one WAV per clear call in the recordings folder, the date and time in the file
name, no group or channel number) are in the TETRAPOL panel. The call table is shown on the screen only; it is never saved, exported or
logged. There is no free RP-CELP decoder that we know of, and OnAir does not include one; the panel's **Install helper** shows the exact file
name it looks for and the codecs folder.

### AMBE+2, AMBE and IMBE plug-ins (DMR, NXDN, dPMR, D-STAR, P25)

Three more plug-in ids, one registry (`core/include/dect2/codec/codec_registry.h`), the same four `onair_speech_*` functions and 160 samples of 8 kHz PCM per call:

| Id (file `onair-codec-<id>.dylib` / `.so` / `.dll`) | Modes | Bytes handed to `onair_speech_decode` |
|---|---|---|
| `ambe2` | DMR, NXDN, dPMR (AMBE+2 3600x2450) | 12: the four de-interleaved code words C0..C3 of mbelib, 24 bits each, row by row, most significant bit first |
| `ambe` | D-STAR (AMBE 3600x2400) | 12, the same layout |
| `imbe` | P25 phase 1 (IMBE 7200x4400) | 23: the eight code words c0..c7, 23 bits each, row by row, MSB first (184 bits) |

OnAir does the de-interleaving; the plug-in only has to expand the bits back into mbelib's `char fr[4][24]` (or `fr[8][23]`) and call `mbe_processAmbe3600x2450Frame` / `mbe_processAmbe3600x2400Frame` / `mbe_processImbe7200x4400Frame`, then return the 160 samples. A small reference wrapper is in `tools/codec-plugin-example/mbelib_wrapper.c` (compile it yourself against your own mbelib; no codec code is in this repository).
If no plug-in is installed OnAir still loads a plain `libmbe` from the codecs folder as before; a plug-in wins when both exist. Encrypted calls are not decoded by default: DMR calls flagged private in their service options show `ENC` and play nothing. You can decrypt a call **only with a key you import yourself** — OnAir has no automatic decrypter, key search or key recovery. See **[docs/KEYS.md](KEYS.md)** for the Keys window, the ciphers supported per protocol (P25 ADP/DES/AES and the NXDN scrambler are applied; DMR vendor privacy framing is not public and is not implemented), and how keys are stored encrypted at rest.
DMR plays one time slot at a time: the **Listen to** box in the DMR panel chooses slot 1, slot 2 or the first slot heard.

**Voice monitor.** Every digital voice mode (DMR, P25, NXDN, dPMR, D-STAR, Yaesu System Fusion, TETRA, M17) has the same strip at the top of its
tab: locked / searching, codec ready / missing (with an **Install codec...** button that opens the helper above), encrypted, hold and recording,
and a now-talking card with the talkgroup, the source, the colour code / NAC / RAN, the slot and an error meter, with **Hold** and **Mute**.
**Help > Voice monitor** (or the strip's button) opens the calls, a searchable call history, the allow / mute / priority lists, names for
talkgroups, hold, the hang time, the mode's own volume, a squelch on the bit error rate, the output device and per-call recording (a WAV and a
JSON file of the details per call, in the app data folder's `voice-recordings`). One call is played at a time per mode, so a call sent on two
slots or carriers is heard once. Encrypted calls are listed and never decoded or recorded. TETRA can also be told to decode only the 25 kHz
channel at the dial frequency, one timeslot or one carrier.

**Install helper.** The DMR panel (and the **?** button of NXDN, dPMR, D-STAR and P25) shows whether the plug-in is loaded (with its path) or not found (with the exact file name for your OS), the codecs folder, and three buttons: **Open codecs folder** (made if missing), **Build steps** (this page) and **Rescan for plug-in** (looks again without a restart). OnAir downloads nothing for you. Upstream mbelib: <https://github.com/lwvmobile/mbelib>.

## Nothing patented is bundled

OnAir never includes, downloads or links a patented codec. That covers mbelib / mbelib-neo (IMBE, AMBE, AMBE+2 and the TETRA codec), the HD Radio HDC decoder (the patched FAAD2), DRA+, the AC-4 decoder, the Iridium and Inmarsat voice codecs and the TETRAPOL RP-CELP codec: you build or install them yourself, they load from your user codecs folder, and the voice stays silent until you do. The **KA modes** (KAAL, KAPP, KAFT, KAVB, KAIT and KAWS) use their own codecs, written for OnAir and not covered by third-party patents as far as is known (KAAL voice uses its own MDCT codec, KAIT its own wavelet picture codec), so they need nothing from you. KAVB has no video or audio codec yet.

## Legal note

Some of these codecs are covered by patents in some countries. Whether you install and use them is your choice and your
responsibility; check the rules where you live. OnAir does not ship them, does not download them, and contains no code from them.
