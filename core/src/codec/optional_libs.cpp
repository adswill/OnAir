// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/codec/optional_libs.h"
#include "dect2/codec/codec_paths.h"
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace dect2::codec {

namespace {

std::string lowerOf(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

struct Def {
    const char* id; const char* name; const char* group; const char* usedFor;
    const char* patWin; const char* patMac; const char* patLinux;   // '|' separated patterns ('*' = any run)
    const char* source;
};

// Where a file for each kind goes: the OnAir codecs folder of this OS (the user one survives an update)
#if defined(_WIN32)
const char* kWhere = "%LOCALAPPDATA%\\OnAir\\codecs\\  (or a codecs folder next to OnAir.exe)";
#elif defined(__APPLE__)
const char* kWhere = "~/Library/Application Support/OnAir/codecs/";
#else
const char* kWhere = "~/.local/share/onair/codecs/";
#endif

const char* const G1 = "Sound and codecs";
const char* const G2 = "Radio drivers";
const char* const G3 = "Plug-ins";

const Def kDefs[] = {
    {"faad2", "libfaad2 (DAB+ HE-AAC)", G1, "SBR and parametric stereo on DAB+ HE-AAC stations; without it they play band-limited",
     "libfaad.dll|libfaad2.dll|libfaad-2.dll|libfaad2-2.dll|faad2.dll|faad.dll", "libfaad.*dylib", "libfaad.so*", "https://github.com/knik0/faad2"},
    {"lame", "LAME (MP3 recording)", G1, "recording to MP3; WAV and FLAC work without it",
     "libmp3lame.dll|libmp3lame-0.dll|lame_enc.dll|mp3lame.dll", "libmp3lame*.dylib", "libmp3lame.so*", "https://lame.sourceforge.io (source code only)"},
    {"fdkaac", "libfdk-aac (xHE-AAC for DRM)", G1, "DRM stations that use xHE-AAC tools FFmpeg lacks stay silent without it",
     "libfdk-aac-2.dll|libfdk-aac.dll|fdk-aac.dll|libfdk-aac-1.dll|fdk-aac-2.dll", "libfdk-aac*.dylib", "libfdk-aac.so*", "https://github.com/mstorsjo/fdk-aac"},
    {"mbe", "mbelib (DMR, P25, NXDN, dPMR, D-STAR voice)", G1, "digital voice is silent without it (or a codec plug-in below); calls and data still show",
     "mbelib.dll|libmbe.dll|libmbe-1.dll|mbe.dll", "libmbe.*dylib", "libmbe.so*", "https://github.com/lwvmobile/mbelib"},
    {"mbeneo", "mbelib-neo (DMR, P25, NXDN, dPMR, D-STAR voice)", G1, "the same voice as mbelib, from the mbelib-neo fork (use one of the two)",
     "libmbe-neo.dll|mbe-neo.dll|libmbe-neo-2.dll", "libmbe-neo*.dylib", "libmbe-neo.so*", "https://github.com/arancormonk/mbelib-neo"},
    {"codec2", "codec2 (FreeDV, M17 speech)", G1, "comes with OnAir; only needed if this copy is incomplete or you want your own build",
     "codec2.dll|libcodec2.dll", "libcodec2*.dylib", "libcodec2.so*", "https://github.com/drowe67/codec2"},
    {"hdc", "FAAD2 with the HDC patch (HD Radio sound)", G1, "HD Radio sound; stations and data show without it",
     "libfaad_hdc.dll|faad_hdc.dll|libfaad2-hdc.dll|faad2-hdc.dll", "libfaad_hdc*.dylib|libfaad2-hdc*.dylib", "libfaad_hdc.so*|libfaad2-hdc.so*", "https://github.com/theori-io/nrsc5 (build it yourself, see docs/CODECS.md)"},
    {"dra", "DRA+ add-on (CDR sound)", G1, "China Digital Radio sound; services and data show without it",
     "onair_dra.dll|libonair_dra.dll", "libonair_dra*.dylib", "libonair_dra.so*", "built by you (docs/CODECS.md)"},
    {"ffmpeg", "FFmpeg libavcodec and libavutil (AC-4 sound, AVS+ and DRA TV)", G1, "ATSC 3.0 AC-4 sound and DTMB AVS+/DRA picture or sound; everything else uses the FFmpeg inside OnAir",
     "avcodec-*.dll", "libavcodec*.dylib", "libavcodec.so*", "https://ffmpeg.org (source) and the builds it links to"},
    {"codecplugins", "Voice codec plug-ins (onair-codec-<id>)", G3, "TETRA, TETRAPOL, Yaesu Fusion, AMBE and IMBE voice through your own plug-in",
     "onair-codec-*.dll", "onair-codec-*.dylib", "onair-codec-*.so", "built by you (docs/CODECS.md, tools/codec-builder)"},
    {"rtlsdr", "librtlsdr (RTL-SDR)", G2, "RTL-SDR dongles; some OnAir packages include it, add one if the radio is not found",
     "rtlsdr.dll|librtlsdr.dll", "librtlsdr*.dylib", "librtlsdr.so*", "https://github.com/osmocom/rtl-sdr (RTL-SDR Blog V4: https://github.com/rtlsdrblog/rtl-sdr-blog)"},
    {"airspy", "libairspy (Airspy R2 / Mini)", G2, "Airspy R2 and Mini; some OnAir packages include it",
     "airspy.dll|libairspy.dll", "libairspy.*dylib", "libairspy.so*", "https://github.com/airspy/airspyone_host (Windows: https://airspy.us/downloads)"},
    {"airspyhf", "libairspyhf (Airspy HF+)", G2, "Airspy HF+ Discovery and Dual; some OnAir packages include it",
     "airspyhf.dll|libairspyhf.dll", "libairspyhf*.dylib", "libairspyhf.so*", "https://github.com/airspy/airspyhf (Windows: https://airspy.us/downloads)"},
    {"fobos", "fobos_sdr (RigExpert Fobos SDR)", G2, "the Fobos SDR; not shipped",
     "fobos_sdr.dll|libfobos_sdr.dll", "libfobos_sdr*.dylib", "libfobos_sdr.so*", "https://rigexpert.com (the Fobos SDR software package; open-source library: libfobos)"},
    {"bladerf", "libbladeRF (Nuand bladeRF)", G2, "bladeRF boards; not shipped",
     "bladeRF.dll|libbladeRF.dll", "libbladeRF*.dylib", "libbladeRF.so*", "https://github.com/Nuand/bladeRF and https://www.nuand.com"},
    {"hydrasdr", "libhydrasdr (HydraSDR)", G2, "HydraSDR radios; not shipped",
     "hydrasdr.dll|libhydrasdr.dll", "libhydrasdr*.dylib", "libhydrasdr.so*", "the HydraSDR maker's own download page"},
    {"limesuite", "LimeSuite (LimeSDR)", G2, "LimeSDR and other Lime radios; not shipped",
     "libLimeSuite.dll|LimeSuite.dll", "libLimeSuite*.dylib", "libLimeSuite.so*", "https://github.com/myriadrf/LimeSuite"},
    {"iio", "libiio (PlutoSDR, ADALM-Pluto)", G2, "PlutoSDR and other Analog Devices IIO radios; not shipped",
     "libiio.dll|iio.dll", "libiio*.dylib", "libiio.so*", "https://github.com/analogdevicesinc/libiio"},
    {"uhd", "UHD (Ettus USRP)", G2, "USRP radios; not shipped",
     "uhd.dll|libuhd.dll", "libuhd*.dylib", "libuhd.so*", "https://github.com/EttusResearch/uhd (Windows installer: https://files.ettus.com)"},
    {"hamlib", "Hamlib (CAT rig control)", G2, "Source > Add Hamlib rig: a transceiver or receiver controlled over CAT or rigctld. The Windows package loads it at run time; the Linux and macOS packages include it",
     "libhamlib-4.dll|hamlib-4.dll|libhamlib.dll|hamlib.dll", "libhamlib*.dylib", "libhamlib.so*", "https://github.com/Hamlib/Hamlib/releases (Windows: hamlib-w64-<version>.zip, the DLLs of its bin folder)"},
    {"sdrplay", "SDRplay API (RSP radios)", G2, "RSP1, RSP2, RSPdx, RSPduo: install the SDRplay API; its own installer puts the library where OnAir finds it",
     "sdrplay_api.dll", "libsdrplay_api.so*", "libsdrplay_api.so*", "https://www.sdrplay.com/api/"},
};

std::vector<std::string> splitBar(const char* s) {
    std::vector<std::string> v;
    std::string cur;
    for (const char* p = s; ; ++p) {
        if (*p == '|' || *p == 0) { if (!cur.empty()) v.push_back(cur); cur.clear(); if (!*p) break; }
        else cur += *p;
    }
    return v;
}

// File names in each folder, listed once per call
using Listing = std::map<std::string, std::vector<std::string>>;
const std::vector<std::string>& listDir(Listing& cache, const std::string& dir) {
    auto it = cache.find(dir);
    if (it != cache.end()) return it->second;
    std::vector<std::string>& v = cache[dir];
    try {
        std::error_code ec;
        for (std::filesystem::directory_iterator d(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(dir.data()), dir.size())), ec), e; !ec && d != e; d.increment(ec)) {
            std::error_code e2;
            if (d->is_directory(e2)) continue;
            const std::u8string n = d->path().filename().u8string();
            v.emplace_back(reinterpret_cast<const char*>(n.data()), n.size());
        }
    } catch (const std::exception&) {}
    return v;
}

std::vector<std::string> searchDirs() {
    std::vector<std::string> v;
    auto add = [&](std::string d) {
        if (d.empty()) return;
        for (const auto& e : v) if (samePath(e, d)) return;
        v.push_back(d);
    };
    for (const auto& d : systemCodecDirs()) add(d.path);
    const std::string exe = programDir();
    if (!exe.empty()) {
        add(exe);
#if defined(__APPLE__)
        add(exe + "/../Frameworks");
        add(exe + "/../Resources");
#elif !defined(_WIN32)
        add(exe + "/../lib/onair");
        add(exe + "/../lib");
#endif
    }
#if defined(_WIN32)
    if (const char* pf = getenv("ProgramFiles")) for (const char* sub : {"\\PothosSDR\\bin", "\\UHD\\bin", "\\LimeSuite\\bin", "\\SDRplay\\API\\x64"}) add(std::string(pf) + sub);
    if (const char* p = getenv("PATH")) {
        std::string cur;
        for (const char* c = p; ; ++c) {
            if (*c == ';' || *c == 0) { add(cur); cur.clear(); if (!*c) break; }
            else cur += *c;
        }
    }
#elif defined(__APPLE__)
    for (const char* d : {"/opt/homebrew/lib", "/usr/local/lib", "/opt/local/lib"}) add(d);
#else
    for (const char* d : {"/usr/local/lib", "/usr/local/lib64", "/usr/lib", "/usr/lib64", "/lib", "/lib64"}) add(d);
    try {
        std::error_code ec;
        for (std::filesystem::directory_iterator d("/usr/lib", ec), e; !ec && d != e; d.increment(ec)) {
            const std::string n = d->path().filename().string();
            if (n.size() > 10 && n.compare(n.size() - 10, 10, "-linux-gnu") == 0) add("/usr/lib/" + n);
            else if (n.size() > 13 && n.compare(n.size() - 13, 13, "-linux-gnueabi") == 0) add("/usr/lib/" + n);
        }
    } catch (const std::exception&) {}
#endif
    return v;
}

} // namespace

bool optionalNameMatches(const std::string& pattern, const std::string& name) {
    const std::string p = lowerOf(pattern), n = lowerOf(name);
    size_t pi = 0, ni = 0, star = std::string::npos, mark = 0;
    while (ni < n.size()) {
        if (pi < p.size() && p[pi] == '*') { star = pi++; mark = ni; }
        else if (pi < p.size() && p[pi] == n[ni]) { ++pi; ++ni; }
        else if (star != std::string::npos) { pi = star + 1; ni = ++mark; }
        else return false;
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

std::vector<OptionalLib> optionalLibraries() {
    std::vector<OptionalLib> out;
    const std::vector<std::string> dirs = searchDirs();
    Listing cache;
    for (const Def& d : kDefs) {
        OptionalLib o;
        o.id = d.id; o.name = d.name; o.group = d.group; o.usedFor = d.usedFor; o.source = d.source;
        o.where = kWhere;
#if defined(_WIN32)
        const std::vector<std::string> pats = splitBar(d.patWin);
#elif defined(__APPLE__)
        const std::vector<std::string> pats = splitBar(d.patMac);
#else
        const std::vector<std::string> pats = splitBar(d.patLinux);
#endif
        for (size_t i = 0; i < pats.size(); ++i) { if (i) o.files += ", "; o.files += pats[i]; }
        for (const auto& dir : dirs) {
            for (const auto& f : listDir(cache, dir)) {
                for (const auto& p : pats) if (optionalNameMatches(p, f)) { o.found = true; o.foundPath = joinPath(dir, f); break; }
                if (o.found) break;
            }
            if (o.found) break;
        }
        out.push_back(std::move(o));
    }
    return out;
}

} // namespace dect2::codec
