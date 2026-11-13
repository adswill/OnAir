// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The "?" button next to the sound of each mode whose voice or audio needs a codec that OnAir does not ship (docs/CODECS.md). It says whether
// the codec was found and where, what it is, how to install it on this computer, where to put it, a link to the full page, and "Look again".
#include "../app.h"
#include "dect2/audio/ac4_dec.h"
#include "dect2/tv/avsplus_dec.h"
#include "dect2/cdr/cdr_dra.h"
#include "dect2/hdr/hdr_hdc.h"
#include "dect2/hfdig/hfdig_freedv.h"
#include "dect2/dstar/dstar_ambe.h"
#include "dect2/nxdn/nxdn_ambe.h"
#include "dect2/p25/p25_imbe.h"
#include "dect2/codec/codec_guard.h"
#include "dect2/engine/updater.h"
#include "../codec/speech_codec_ui.h"
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr const char* kDocUrl = "https://github.com/adswill/OnAir/blob/main/docs/CODECS.md";

// mbelib is built from source by the user (patented codec: OnAir ships none). Plain steps; the full text is docs/CODECS.md.
constexpr const char* kMbeMac =
    "Build it yourself from github.com/lwvmobile/mbelib (patent notice: check your local law):\n"
    "  brew install cmake git\n  git clone https://github.com/lwvmobile/mbelib\n  cd mbelib && mkdir build && cd build\n"
    "  cmake .. && make\nThen copy libmbe.dylib into the codecs folder below.\nmbelib-neo (libmbe-neo, github.com/arancormonk/mbelib-neo) works too, under its own name: do not rename it to libmbe.";
constexpr const char* kMbeWin =
    "Build it yourself from github.com/lwvmobile/mbelib (patent notice: check your local law). In MSYS2 MINGW64:\n"
    "  pacman -S --needed git mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake make\n"
    "  git clone https://github.com/lwvmobile/mbelib\n  cd mbelib && mkdir build && cd build\n"
    "  cmake .. -G \"MSYS Makefiles\" -DBUILD_SHARED_LIBS=ON && make\n"
    "Copy the .dll it makes (libmbe.dll) into the codecs folder below. It may also need libgcc_s_seh-1.dll and libwinpthread-1.dll "
    "from C:\\msys64\\mingw64\\bin next to it. Use 64-bit if OnAir is 64-bit.\n"
    "Do not use codec2.dll for this: codec2 is not used by DMR, P25, NXDN or dPMR.\nmbelib-neo (libmbe-neo, github.com/arancormonk/mbelib-neo) works too, under its own name: do not rename it to libmbe.";
constexpr const char* kMbeLinux =
    "Build it yourself from github.com/lwvmobile/mbelib (patent notice: check your local law):\n"
    "  sudo apt install git cmake build-essential\n  git clone https://github.com/lwvmobile/mbelib\n  cd mbelib && mkdir build && cd build\n"
    "  cmake .. && make && sudo make install && sudo ldconfig\n(or copy libmbe.so.1 into the codecs folder below)\nmbelib-neo (libmbe-neo, github.com/arancormonk/mbelib-neo) works too, under its own name: do not rename it to libmbe.";

struct Text {
    const char* title;
    const char* what;
    const char* install;   // for this computer
    bool loadable;         // OnAir loads it when installed (otherwise the text points to another program)
};

Text textOf(CodecHelp c) {
    switch (c) {
    case CodecHelp::Codec2:
        return {"codec2 (FreeDV and M17 voice)",
                "codec2 is the open voice codec of FreeDV and M17. It comes with OnAir (the library libcodec2 sits next to the program), so FreeDV and "
                "M17 speech works with nothing to install. This help only matters when it says \"not found\" above: then this copy of OnAir is "
                "incomplete.",
#if defined(__APPLE__)
                "Normally nothing to do: the library is inside OnAir.app. If it is not found, download the OnAir disk image again and copy "
                "OnAir.app to Applications; or install it yourself with Homebrew (brew.sh):\n  brew install codec2",
#elif defined(_WIN32)
                "Normally nothing to do: libcodec2.dll is in the same folder as OnAir.exe. If it is not found, unpack the whole OnAir zip again "
                "(do not copy OnAir.exe alone) or use the setup program. To use your own codec2 instead, put codec2.dll or libcodec2.dll into the "
                "codecs folder below (MSYS2: pacman -S mingw-w64-x86_64-codec2).",
#else
                "Normally nothing to do: libcodec2 is in the OnAir package (the lib folder next to bin). If it is not found, install the OnAir "
                ".deb or portable package again, or install your distribution's:\n  Debian / Ubuntu:  sudo apt install libcodec2-1.2 (Ubuntu 22.04: "
                "libcodec2-1.0)\n  Fedora:  sudo dnf install codec2",
#endif
                true};
    case CodecHelp::DmrAmbe:
        return {"AMBE+2 (DMR voice)",
                "DMR voice uses AMBE+2, a patented vocoder. OnAir does not ship it. Put a codec plug-in (onair-codec-ambe2, built around a library such as mbelib; "
                "docs/CODECS.md has the ABI and a small example wrapper) or the libmbe library itself in a codecs folder (your user one, one next to OnAir, or the extra folder set in Help > Voice codecs), and OnAir decodes and plays the voice of one "
                "time slot (choose slot 1, slot 2 or the first heard). Encrypted calls are never decoded. Check the patent and radio rules where you live first.",
#if defined(__APPLE__)
                kMbeMac,
#elif defined(_WIN32)
                kMbeWin,
#else
                kMbeLinux,
#endif
                true};
    case CodecHelp::Hdc:
        return {"HDC (HD Radio sound)",
                "HD Radio sound uses HDC, a patented codec (a modified HE-AAC) owned by Xperi. OnAir does not ship it and contains no HDC code: "
                "stations, program data and pictures are shown without it. When you build the HDC-patched FAAD2 yourself (the open-source nrsc5 project "
                "publishes the patch), OnAir loads it and plays the program you select (HD1, HD2 ...). Whether you may use HDC depends on the "
                "patent and licence rules where you live, and listening to radio services is regulated in many countries: check them first. "
                "OnAir has only been tested against a stand-in library, not a real one.",
#if defined(__APPLE__)
                "Build it yourself (full steps in docs/CODECS.md):\n"
                "  brew install cmake git\n  git clone https://github.com/theori-io/nrsc5\n  git clone https://github.com/knik0/faad2\n"
                "  cd faad2 && patch -p1 < ../nrsc5/support/faad2-hdc-support.patch\n  mkdir build && cd build\n"
                "  cmake .. -DBUILD_SHARED_LIBS=ON && make\nCopy the libfaad_hdc library it makes (libfaad_hdc.dylib) into the codecs folder below.",
#elif defined(_WIN32)
                "Build it yourself (full steps in docs/CODECS.md). In MSYS2 MINGW64:\n"
                "  pacman -S --needed git patch mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake make\n"
                "  git clone https://github.com/theori-io/nrsc5\n  git clone https://github.com/knik0/faad2\n"
                "  cd faad2 && patch -p1 < ../nrsc5/support/faad2-hdc-support.patch\n  mkdir build && cd build\n"
                "  cmake .. -G \"MSYS Makefiles\" -DBUILD_SHARED_LIBS=ON && make\n"
                "Copy libfaad_hdc.dll into the codecs folder below. Use 64-bit if OnAir is 64-bit.",
#else
                "Build it yourself (full steps in docs/CODECS.md):\n"
                "  sudo apt install git patch cmake build-essential\n  git clone https://github.com/theori-io/nrsc5\n  git clone https://github.com/knik0/faad2\n"
                "  cd faad2 && patch -p1 < ../nrsc5/support/faad2-hdc-support.patch\n  mkdir build && cd build\n"
                "  cmake .. -DBUILD_SHARED_LIBS=ON && make\nCopy libfaad_hdc.so into the codecs folder below.",
#endif
                true};
    case CodecHelp::Dra:
        return {"DRA+ (CDR sound)",
                "CDR radio and many DTMB TV channels in China (the DRA sound track of a TV service) are coded with DRA / DRA+. OnAir does not include a DRA+ decoder: the codec's patent and licence status is unclear, and the decoder "
                "OnAir was tested with was written by a third party (maliwen2015, github.com/maliwen2015/ffmpeg_cavs_dra, MIT as published there), "
                "so it is an add-on you install yourself, like the HD Radio and voice codecs. Without it the CDR services, signal information and "
                "data still work, and the raw sound can be saved to a .dra file. With the add-on (libonair_dra) in the codecs folder OnAir plays the "
                "service you select, radio or TV (a TV service with several sound tracks lists DRA in the audio box). Whether you may use it depends on the patent and licence rules where you live: check them first.",
#if defined(__APPLE__)
                "Build the add-on yourself (full steps in docs/CODECS.md; the OnAir source has tools/addons/dra):\n"
                "  brew install cmake git python3\n  git clone https://github.com/adswill/OnAir && cd OnAir/tools/addons/dra\n"
                "  python3 extract_dra.py\n  cmake -S . -B build -DDRA_SRC=dra_src && cmake --build build\n"
                "Copy libonair_dra.dylib from the build folder into the codecs folder below.",
#elif defined(_WIN32)
                "Build the add-on yourself (full steps in docs/CODECS.md; the OnAir source has tools/addons/dra). In MSYS2 MINGW64:\n"
                "  pacman -S --needed git python mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake make\n"
                "  git clone https://github.com/adswill/OnAir && cd OnAir/tools/addons/dra\n"
                "  python extract_dra.py\n  cmake -S . -B build -G \"MSYS Makefiles\" -DDRA_SRC=dra_src && cmake --build build\n"
                "Copy onair_dra.dll into the codecs folder below. Use 64-bit if OnAir is 64-bit.",
#else
                "Build the add-on yourself (full steps in docs/CODECS.md; the OnAir source has tools/addons/dra):\n"
                "  sudo apt install git python3 cmake build-essential\n  git clone https://github.com/adswill/OnAir && cd OnAir/tools/addons/dra\n"
                "  python3 extract_dra.py\n  cmake -S . -B build -DDRA_SRC=dra_src && cmake --build build\n"
                "Copy libonair_dra.so from the build folder into the codecs folder below.",
#endif
                true};
    case CodecHelp::AeroAmbe:
        return {"AMBE 4800x3600 (Inmarsat Aero voice)",
                "Aero C channel voice uses an AMBE vocoder, which is patented, so OnAir ships none. OnAir finds the C channels, demodulates them and decodes the "
                "frames (framing and coding as in JAERO), and hands each 12 byte voice frame, as JAERO hands it to libaeroambe, to a codec plug-in you build "
                "yourself. Without it the calls are listed and the coded frames saved (.bits). Unencrypted traffic only. Using the codec may need a patent "
                "licence: check the rules where you live first.",
#if defined(_WIN32)
                "Build the plug-in yourself (full steps in docs/CODECS.md; the OnAir source has tools/addons/aeroambe). In MSYS2 MINGW64:\n"
                "  pacman -S --needed git mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake make\n"
                "  git clone https://github.com/jontio/libaeroambe\n"
                "  cmake -S libaeroambe/mbelib-master -B libaeroambe/mbelib-master/build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 && cmake --build libaeroambe/mbelib-master/build\n"
                "  git clone https://github.com/adswill/OnAir && cd OnAir/tools/addons/aeroambe\n"
                "  cmake -S . -B build -G \"MSYS Makefiles\" -DAEROAMBE_MBELIB=../../../../libaeroambe/mbelib-master && cmake --build build\n"
                "Copy onair-codec-aeroambe.dll into the codecs folder below. Use 64-bit if OnAir is 64-bit.",
#else
                "Build the plug-in yourself (full steps in docs/CODECS.md; the OnAir source has tools/addons/aeroambe):\n"
                "  git clone https://github.com/jontio/libaeroambe\n"
                "  cmake -S libaeroambe/mbelib-master -B libaeroambe/mbelib-master/build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 && cmake --build libaeroambe/mbelib-master/build\n"
                "  git clone https://github.com/adswill/OnAir && cd OnAir/tools/addons/aeroambe\n"
                "  cmake -S . -B build -DAEROAMBE_MBELIB=../../../../libaeroambe/mbelib-master && cmake --build build\n"
                "Copy the library the build made (onair-codec-aeroambe.dylib or .so) into the codecs folder below, then accept the notice once.",
#endif
                true};
    case CodecHelp::IridiumAmbe:
        return {"AMBE (Iridium voice)",
                "Iridium voice uses an AMBE-family vocoder, which is patented, so OnAir ships none. It lists the voice calls and hands each "
                "frame's 39 byte voice field, as received, to a codec plug-in you install yourself. This is not verified against real Iridium "
                "recordings; with no plug-in the calls stay silent. Encrypted or scrambled traffic is never decoded.",
                "Put onair-codec-iridium (.dll, .dylib or .so) in the codecs folder below and accept the notice. You can also save a .bits file "
                "(Save for iridium-toolkit) and use iridium-toolkit (github.com/muccc/iridium-toolkit) with ir77_ambe_decode from osmo-ir77.",
                true};
    case CodecHelp::DstarAmbe:
        return {"mbelib (D-STAR AMBE voice)",
                "D-STAR voice uses AMBE, a patented vocoder. OnAir does not ship it: calls, callsigns, messages and positions are shown without "
                "it. The open mbelib library decodes AMBE; when you install it yourself, OnAir loads it and plays D-STAR voice. Its own notice "
                "says that using it may need a patent licence: check the rules where you live first.",
#if defined(__APPLE__)
                kMbeMac,
#elif defined(_WIN32)
                kMbeWin,
#else
                kMbeLinux,
#endif
                true};
    case CodecHelp::DpmrAmbe:
        return {"mbelib (dPMR AMBE+2 voice)",
                "dPMR voice uses AMBE+2, a patented vocoder. OnAir does not ship it: calls, own and called IDs, colour codes, status and short "
                "messages are shown without it. The open mbelib library decodes AMBE; when you install it yourself, OnAir loads it and plays dPMR "
                "voice. Its own notice says that using it may need a patent licence: check the rules where you live first.",
#if defined(__APPLE__)
                kMbeMac,
#elif defined(_WIN32)
                kMbeWin,
#else
                kMbeLinux,
#endif
                true};
    case CodecHelp::NxdnAmbe:
        return {"mbelib (NXDN AMBE+2 voice)",
                "NXDN voice uses AMBE+2, a patented vocoder. OnAir does not ship it: calls, unit IDs, talkgroups, messages and site information are shown without "
                "it. The open mbelib library decodes AMBE; when you install it yourself, OnAir loads it and plays NXDN voice. Its own notice "
                "says that using it may need a patent licence: check the rules where you live first.",
#if defined(__APPLE__)
                kMbeMac,
#elif defined(_WIN32)
                kMbeWin,
#else
                kMbeLinux,
#endif
                true};
    case CodecHelp::P25Imbe:
        return {"mbelib (P25 IMBE voice)",
                "P25 Phase 1 voice uses IMBE, a patented vocoder. OnAir does not ship it: calls, unit IDs, talkgroups and the trunking system are shown "
                "without it. The open mbelib library decodes IMBE; when you install it yourself, OnAir loads it and plays clear P25 voice. Encrypted "
                "calls are only reported as encrypted: OnAir never decrypts. mbelib's own notice says that using it may need a patent licence, and "
                "listening to radio services is regulated in many countries: check the rules where you live first.",
#if defined(__APPLE__)
                kMbeMac,
#elif defined(_WIN32)
                kMbeWin,
#else
                kMbeLinux,
#endif
                true};
    case CodecHelp::Avs:
        return {"AVS+, AVS2 and AVS3 (TV pictures in China)",
                "DTMB television in China sends its pictures as AVS+ (and some channels as AVS2 or AVS3), Chinese patented video codecs. "
                "OnAir plays them through the FFmpeg built into it and ships no codec of its own. FFmpeg decodes AVS1 in its JiZhun profile, "
                "but not AVS+ (the broadcast profile), and AVS2 and AVS3 only when it was built with libdavs2 or libuavs3d, which the FFmpeg in "
                "OnAir is not. The service list, the sound and the guide work without the picture.",
                "For AVS+ you can build an FFmpeg add-on yourself (press ? next to the AVS+ notice, or see docs/CODECS.md). For AVS2 or AVS3, save the stream (Outputs: file or UDP) and play it "
                "with an FFmpeg build that includes libdavs2 or libuavs3d (ffplay). Check the patent rules where you live first.",
                false};
    case CodecHelp::Ac4:
        return {"AC-4 (ATSC 3.0 sound)",
                "ATSC 3.0 in the US and South Korea (and some DVB services) send their sound as Dolby AC-4, a patented codec. The FFmpeg inside "
                "OnAir has no AC-4 decoder, so without help OnAir plays the picture and any other sound track (AAC, AC-3) but not the AC-4 sound. "
                "If you install a libavcodec built from an FFmpeg that includes an AC-4 decoder, OnAir loads it, decodes the AC-4 sound of the "
                "selected service and plays it, mixed to stereo or 5.1. This is experimental: it has not been tried on a real AC-4 broadcast.",
                "AC-4 audio needs an FFmpeg with the AC-4 decoder. The one public source is the patch in github.com/funnymanva/ffmpeg-with-ac4 "
                "(FFmpeg 6.1 plus an AC-4 decoder; its script builds a static ffmpeg program, but OnAir needs the SHARED libraries: configure "
                "FFmpeg with --enable-shared --disable-static after applying ffmpeg_ac4.patch). Put libavcodec and libavutil (and the "
                "libswresample and other libraries they need) next to each other in the codecs folder below, then press Look again.\n"
                "Licences, honestly: the decoder in that patch (ac4dec.c, Paul B Mahol) is LGPL-2.1-or-later and the build script is MIT, so "
                "building and using it yourself is fine as far as copyright goes. The patents are another matter: Dolby holds patents on AC-4 "
                "and licenses them for products; OnAir ships no AC-4 code and downloads nothing. Whether you may use it is for you to check where you live.",
                true};
    case CodecHelp::AvsPlus:
        return {"AVS+ (TV pictures in China)",
                "DTMB television in China sends its pictures as AVS+, a Chinese patented video codec. The FFmpeg inside OnAir decodes AVS1 only in its "
                "JiZhun profile, not AVS+, and OnAir ships no codec of its own. If you install a libavcodec built from an FFmpeg that has an AVS+ "
                "decoder, OnAir loads it and shows the picture of AVS+ services. The sound, the service list and the guide work without it. "
                "Experimental: it has not been tried on a real AVS+ broadcast.",
                "You build the add-on yourself. The public project github.com/maliwen2015/ffmpeg_cavs_dra (MIT) offers a patch for FFmpeg 7.1.2 "
                "(ffmpeg-7.1.2_cavs_dra.patch) that adds AVS+ and DRA decoding; its page gives no build steps, so these are the ordinary FFmpeg ones: "
                "get the FFmpeg 7.1.2 source, apply the patch (patch -p1 < ffmpeg-7.1.2_cavs_dra.patch), run ./configure --enable-shared --disable-static "
                "--disable-programs --disable-doc, make, and take libavcodec and libavutil (and the libswresample and other libraries they need) from the "
                "build. On Windows build in MSYS2 with the same options; on macOS use Xcode command line tools; on Linux gcc and make. Put the files "
                "next to each other in the codecs folder below, then press Look again. OnAir looks for a decoder named cavs (set DECT2_AVSPLUS_DECODER "
                "if your build names it differently).\n"
                "Licences, honestly: the project is MIT and FFmpeg is LGPL/GPL, so building it yourself is fine as far as copyright goes. AVS+ is covered "
                "by patents (the Chinese AVS group licenses them); OnAir ships and downloads nothing of it. Whether you may use it is for you to check where you live.",
                true};
    }
    return {"", "", "", false};
}

} // namespace

const char* codecNeedMbe() {
    static const std::string t = [] {
        std::string n;
        for (const auto& f : dect2::codecFileNameList(false)) n += (n.empty() ? "" : ", ") + f;
        return "voice: needs mbelib (" + n + ")";
    }();
    return t.c_str();
}

// the plug-in codec id a help topic belongs to (the AMBE / IMBE family), or null
static const char* pluginIdFor(CodecHelp c) {
    return c == CodecHelp::DmrAmbe || c == CodecHelp::NxdnAmbe || c == CodecHelp::DpmrAmbe ? "ambe2" : c == CodecHelp::DstarAmbe ? "ambe" : c == CodecHelp::P25Imbe ? "imbe" : c == CodecHelp::IridiumAmbe ? "iridium" : nullptr;
}

void codecHelpButton(CodecHelp c, const char* label) {
    const Text t = textOf(c);
    // the codec notice must be answerable from the panel itself, not only from inside the collapsed install helper (otherwise an installed plug-in never loads)
    if (const char* pid0 = pluginIdFor(c)) appcodec::codecNoticePrompt(pid0);
    ImGui::PushID((int)c + 7100);
    if (ImGui::SmallButton(label)) ImGui::OpenPopup("codechelp");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How to get the %s codec", t.title);
    if (ImGui::BeginPopup("codechelp")) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextUnformatted(t.title);
        ImGui::Separator();
        if (ImGui::SmallButton("All optional libraries: which, where, official sources")) dect2::openUrl("https://github.com/adswill/OnAir/blob/main/docs/OPTIONAL-LIBRARIES.md");
        // a codec that crashed or hung this run (codec_guard.h): said first; Look again tries it once more
        if (const std::string gm = dect2::codec::guardAllMessages(); !gm.empty()) { ImGui::TextColored(pal::warnAmber(), "%s", gm.c_str()); ImGui::Separator(); }
        appcodec::codecArchLines(true);   // OnAir's processor, Rosetta notice, which architecture to build for
        {   // the AMBE / IMBE family: the shared plug-in install helper first
            const char* pid = pluginIdFor(c);
            if (pid) { appcodec::codecInstallHelper(pid); ImGui::Separator(); }
        }
        if (c == CodecHelp::AvsPlus) {   // the optional AVS+ FFmpeg add-on (same layout as AC-4)
            const dect2::AvsPlusLibInfo li = dect2::avsPlusLibrary();
            if (li.hasDecoder) ImGui::TextColored(pal::okGreen(), "AVS+ decoder found: %s", li.path.c_str());
            else ImGui::TextColored(pal::warnAmber(), "AVS+ add-on not found: AVS+ services play without a picture%s%s", li.message.empty() ? "" : "\n", li.message.c_str());
            ImGui::Spacing();
            ImGui::TextUnformatted(t.what);
            ImGui::Spacing();
            ImGui::TextDisabled("Install");
            ImGui::TextUnformatted(t.install);
            ImGui::Spacing();
            ImGui::TextDisabled("File names OnAir looks for");
            ImGui::TextUnformatted("libavcodec.dylib / libavcodec.NN.dylib (macOS), libavcodec.so.NN (Linux), avcodec-NN.dll (Windows), and the libavutil of the same kind in the same folder");
            ImGui::TextDisabled("Folders, in this order");
            for (const auto& d : dect2::avsPlusSearchFolders()) ImGui::BulletText("%s", d.c_str());
            ImGui::Spacing();
            const auto folders = dect2::codecFolderList();
            if (!folders.empty()) {
                if (ImGui::Button("Open codecs folder")) {
                    std::error_code ec;
                    std::filesystem::create_directories(std::filesystem::path(folders.back()), ec);
                    dect2::openUrl(folders.back());
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n(made if it does not exist)", folders.back().c_str());
                ImGui::SameLine();
            }
            if (ImGui::Button("Look again")) dect2::avsPlusLibrary(true);
            ImGui::SameLine();
            if (ImGui::Button("Full instructions")) dect2::openUrl(kDocUrl);
            appcodec::discordCodecButton();
            ImGui::PopTextWrapPos();
            ImGui::EndPopup();
            ImGui::PopID();
            return;
        }
        if (c == CodecHelp::Ac4) {   // its own layout: the optional FFmpeg, not mbelib or codec2
            const dect2::Ac4LibInfo li = dect2::ac4Library();
            if (li.hasDecoder) ImGui::TextColored(pal::okGreen(), "AC-4 decoder found: %s", li.path.c_str());
            else ImGui::TextColored(pal::warnAmber(), "AC-4 decoder not found: AC-4 sound is left out%s%s", li.message.empty() ? "" : "\n", li.message.c_str());
            ImGui::Spacing();
            ImGui::TextUnformatted(t.what);
            ImGui::Spacing();
            ImGui::TextDisabled("Install");
            ImGui::TextUnformatted(t.install);
            ImGui::Spacing();
            ImGui::TextDisabled("File names OnAir looks for");
            ImGui::TextUnformatted("libavcodec.dylib / libavcodec.NN.dylib (macOS), libavcodec.so.NN (Linux), avcodec-NN.dll (Windows), and the libavutil of the same kind in the same folder");
            ImGui::TextDisabled("Folders, in this order");
            for (const auto& d : dect2::ac4SearchFolders()) ImGui::BulletText("%s", d.c_str());
            ImGui::Spacing();
            if (appcodec::openCodecFolderButtons()) ImGui::SameLine();
            if (ImGui::Button("Look again")) dect2::ac4Library(true);
            ImGui::SameLine();
            if (ImGui::Button("Full instructions")) dect2::openUrl(kDocUrl);
            ImGui::PopTextWrapPos();
            ImGui::EndPopup();
            ImGui::PopID();
            return;
        }
        if (c == CodecHelp::Hdc || c == CodecHelp::Dra) {   // their own layout: a library the user builds (patched FAAD2 / the DRA+ add-on)
            const bool dra = c == CodecHelp::Dra;
            std::string path, problem;
            if (dra ? dect2::cdr::draAvailable(&path, &problem) : dect2::hdr::hdcAvailable(&path, &problem)) ImGui::TextColored(pal::okGreen(), dra ? "DRA+ decoder loaded: %s" : "HDC library found: %s", path.c_str());
            else if (!problem.empty()) ImGui::TextColored(pal::warnAmber(), dra ? "DRA+ add-on found but not usable (%s): %s" : "HDC library found but not usable (%s): %s", path.c_str(), problem.c_str());
            else if (dra) ImGui::TextColored(pal::warnAmber(), "DRA+ decoder add-on not installed: services and data are shown, without sound");
            else ImGui::TextColored(pal::warnAmber(), "HDC sound codec not installed: stations and data are shown, without sound");
            ImGui::Spacing();
            ImGui::TextUnformatted(t.what);
            ImGui::Spacing();
            ImGui::TextDisabled("Install");
            ImGui::TextUnformatted(t.install);
            ImGui::Spacing();
            ImGui::TextDisabled("File names OnAir looks for");
            std::string names;
            for (const auto& n : dra ? dect2::cdr::draFileNames() : dect2::hdr::hdcFileNames()) names += (names.empty() ? "" : ", ") + n;
            ImGui::TextUnformatted(names.c_str());
            ImGui::TextDisabled(dra ? "Folders, in this order (or set DECT2_DRA_PATH to the file)" : "Folders, in this order (or set DECT2_FAAD_HDC_PATH to the file)");
            for (const auto& d : dect2::codecSearchPlaces()) ImGui::BulletText("%s", d.c_str());
            ImGui::Spacing();
            if (appcodec::openCodecFolderButtons()) ImGui::SameLine();
            if (ImGui::Button("Look again")) { if (dra) dect2::cdr::draRescan(); else dect2::hdr::hdcRescan(); }
            ImGui::SameLine();
            if (ImGui::Button("Full instructions")) dect2::openUrl(kDocUrl);
            ImGui::PopTextWrapPos();
            ImGui::EndPopup();
            ImGui::PopID();
            return;
        }
        if (t.loadable) {
            std::string path, ver;
            // DMR is mbelib's AMBE+2 too (it used to fall through to codec2 here and showed codec2's state, #252)
            const bool mbe = c != CodecHelp::Codec2;
            const bool found = c == CodecHelp::P25Imbe ? dect2::p25ImbeAvailable(&path) : c == CodecHelp::NxdnAmbe || c == CodecHelp::DpmrAmbe || c == CodecHelp::DmrAmbe ? dect2::nxdnAmbeAvailable(&path) : mbe ? dect2::dstarAmbeAvailable(&path) : dect2::hfdigFreedvLibrary(&path, &ver);
            if (found) ImGui::TextColored(pal::okGreen(), "%s found: %s", mbe ? "mbelib" : "codec2", path.c_str());
            else ImGui::TextColored(pal::warnAmber(), "%s not found%s", mbe ? "mbelib" : "codec2", mbe ? ": calls and data are shown, without sound" : ": this copy of OnAir came without it");
        } else {
            ImGui::TextColored(pal::warnAmber(), "Not decoded in OnAir");
        }
        ImGui::Spacing();
        ImGui::TextUnformatted(t.what);
        ImGui::Spacing();
        ImGui::TextDisabled(t.loadable ? (c == CodecHelp::Codec2 ? "If it is not found" : "Install") : "What you can do");
        ImGui::TextUnformatted(t.install);
        if (t.loadable) {
            const bool mbe = c != CodecHelp::Codec2;
            ImGui::Spacing();
            ImGui::TextDisabled("Which codec which mode needs");
            ImGui::TextUnformatted("mbelib: DMR, P25, NXDN, dPMR, D-STAR (AMBE / AMBE+2 / IMBE).\ncodec2: FreeDV and M17 only; it does nothing for DMR, P25, NXDN or dPMR.\nTETRA: its own speech codec, needs no library from you.");
            ImGui::Spacing();
            ImGui::TextDisabled("File names OnAir looks for (%s)", mbe ? "mbelib" : "codec2");
            std::string names;
            for (const auto& n : dect2::codecFileNameList(!mbe)) names += (names.empty() ? "" : ", ") + n;
            ImGui::TextUnformatted(names.c_str());
            ImGui::TextDisabled("Folders, in this order");
            for (const auto& d : dect2::codecSearchPlaces()) ImGui::BulletText("%s", d.c_str());
            ImGui::Spacing();
            if (appcodec::openCodecFolderButtons()) ImGui::SameLine();
            if (ImGui::Button("Look again")) { if (c == CodecHelp::P25Imbe) dect2::p25ImbeRescan(); else if (c == CodecHelp::NxdnAmbe || c == CodecHelp::DpmrAmbe || c == CodecHelp::DmrAmbe) dect2::nxdnAmbeRescan(); else if (c == CodecHelp::DstarAmbe) dect2::dstarAmbeRescan(); else dect2::hfdigFreedvRescan(); }
            ImGui::SameLine();
        }
        if (ImGui::Button("Full instructions")) dect2::openUrl(kDocUrl);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kDocUrl);
        if (!t.loadable) appcodec::discordCodecButton();   // the loadable topics got it with the folder buttons above
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
    ImGui::PopID();
}
