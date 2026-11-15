// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Help > Hardware: the media list (every FFmpeg codec, format and capture device OnAir asks for, and the media add-ons), the minimum
// requirements and how this computer measures up, the USB radio drivers, and the plain text that "Copy to clipboard" gives. Pure data and
// logic (no ImGui, no FFmpeg): the window is app/panels/hardware_ui.cpp, the FFmpeg lookups are dect2/tv/media_probe.h. Tested in tests/app/test_hardware_logic.cpp.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace hwinfo {

// ---- media ---------------------------------------------------------------------------------------------------------------------------

enum class MediaKind { VideoEncoder, AudioEncoder, Decoder, Muxer, Demuxer, Capture, AddOn };

// the operating systems a row applies to (h264_mf is Windows only, h264_videotoolbox macOS only, ...)
enum OsMask : unsigned { kWin = 1, kMac = 2, kLinux = 4, kAllOs = kWin | kMac | kLinux };

struct Media {
    MediaKind kind;
    const char* name;      // the FFmpeg name (encoder, decoder, muxer, demuxer); for Capture and AddOn a fixed key, see media_probe.h
    const char* label;     // shown in the window
    const char* usedFor;   // where OnAir uses it
    unsigned os;           // OsMask
    bool hardware;         // a graphics-card / OS encoder: its tick means "it encodes on this computer", not only "it is in FFmpeg"
    bool optional;         // OnAir works without it (an alternative exists, or it is an add-on the user installs)
};

// Everything the code asks FFmpeg for by name or by codec id (grep avcodec_find_*, av_find_input_format, avformat_alloc_output_context2),
// plus the camera input and the add-ons loaded at run time. Order: as shown, grouped by kind.
inline const std::vector<Media>& mediaTable() {
    static const std::vector<Media> t = {
        // video encoders: the digital video generator and the TV transmitters (core/src/tv/videogen_params.cpp), the ATSC 3.0 test content
        {MediaKind::VideoEncoder, "mpeg2video", "MPEG-2 video", "Video generator, DVB-T / ATSC transmit", kAllOs, false, false},
        {MediaKind::VideoEncoder, "libx264", "H.264 (x264)", "Video generator, ATSC 3.0 test content", kAllOs, false, false},
        {MediaKind::VideoEncoder, "libopenh264", "H.264 (OpenH264)", "Video generator (when x264 is missing)", kAllOs, false, true},
        {MediaKind::VideoEncoder, "h264_nvenc", "H.264 NVIDIA NVENC", "Video generator, hardware", kWin | kLinux, true, true},
        {MediaKind::VideoEncoder, "h264_qsv", "H.264 Intel Quick Sync", "Video generator, hardware", kWin | kLinux, true, true},
        {MediaKind::VideoEncoder, "h264_amf", "H.264 AMD AMF", "Video generator, hardware", kWin | kLinux, true, true},
        {MediaKind::VideoEncoder, "h264_vaapi", "H.264 VA-API", "Video generator, hardware", kLinux, true, true},
        {MediaKind::VideoEncoder, "h264_mf", "H.264 Media Foundation", "Video generator, Windows encoder", kWin, true, true},
        {MediaKind::VideoEncoder, "h264_videotoolbox", "H.264 VideoToolbox", "Video generator, hardware", kMac, true, true},
        {MediaKind::VideoEncoder, "libx265", "H.265 / HEVC (x265)", "Video generator, ATSC 3.0 test content", kAllOs, false, false},
        {MediaKind::VideoEncoder, "hevc_nvenc", "H.265 NVIDIA NVENC", "Video generator, hardware", kWin | kLinux, true, true},
        {MediaKind::VideoEncoder, "hevc_qsv", "H.265 Intel Quick Sync", "Video generator, hardware", kWin | kLinux, true, true},
        {MediaKind::VideoEncoder, "hevc_amf", "H.265 AMD AMF", "Video generator, hardware", kWin | kLinux, true, true},
        {MediaKind::VideoEncoder, "hevc_vaapi", "H.265 VA-API", "Video generator, hardware", kLinux, true, true},
        {MediaKind::VideoEncoder, "hevc_mf", "H.265 Media Foundation", "Video generator (needs the HEVC Video Extensions)", kWin, true, true},
        {MediaKind::VideoEncoder, "hevc_videotoolbox", "H.265 VideoToolbox", "Video generator, hardware", kMac, true, true},
        // audio encoders
        {MediaKind::AudioEncoder, "aac", "AAC", "Video generator sound, DAB+ / DRM test signals", kAllOs, false, false},
        {MediaKind::AudioEncoder, "mp2", "MPEG-1 Layer II", "Video generator sound, DAB test signal", kAllOs, false, false},
        {MediaKind::AudioEncoder, "ac3", "AC-3 (Dolby Digital)", "Video generator sound", kAllOs, false, true},
        // decoders: the TV player, DAB / DAB+ / DRM sound, the DAB slideshow, audio and video files
        {MediaKind::Decoder, "mpeg2video", "MPEG-2 video", "TV player (DVB-T, DVB-S, ATSC)", kAllOs, false, false},
        {MediaKind::Decoder, "h264", "H.264 / AVC", "TV player (DVB-T2, DVB-S2, ISDB-T)", kAllOs, false, false},
        {MediaKind::Decoder, "hevc", "H.265 / HEVC", "TV player (DVB-T2 HD, ATSC 3.0)", kAllOs, false, false},
        {MediaKind::Decoder, "cavs", "AVS (Chinese AVS1)", "TV player (DTMB)", kAllOs, false, true},
        {MediaKind::Decoder, "aac", "AAC / HE-AAC", "DAB+, DRM, TV sound", kAllOs, false, false},
        {MediaKind::Decoder, "mp2", "MPEG-1 Layer II", "DAB, TV sound", kAllOs, false, false},
        {MediaKind::Decoder, "mp3", "MP3", "Audio files, HLS sound", kAllOs, false, false},
        {MediaKind::Decoder, "ac3", "AC-3 (Dolby Digital)", "TV sound", kAllOs, false, false},
        {MediaKind::Decoder, "eac3", "E-AC-3 (Dolby Digital Plus)", "TV sound (DVB-T2, ATSC)", kAllOs, false, false},
        {MediaKind::Decoder, "dvbsub", "DVB subtitles", "TV player subtitles", kAllOs, false, false},
        {MediaKind::Decoder, "mjpeg", "JPEG pictures", "DAB slideshow", kAllOs, false, false},
        {MediaKind::Decoder, "gif", "GIF pictures", "DAB slideshow", kAllOs, false, true},
        {MediaKind::Decoder, "bmp", "BMP pictures", "DAB slideshow", kAllOs, false, true},
        // containers
        {MediaKind::Muxer, "mpegts", "MPEG transport stream (write)", "Video generator, TV transmit, recordings", kAllOs, false, false},
        {MediaKind::Muxer, "mp4", "MP4 (write)", "ATSC 3.0 test content, DAB+ recordings", kAllOs, false, false},
        {MediaKind::Muxer, "hls", "HLS (write)", "TV to the browser (web share)", kAllOs, false, true},
        {MediaKind::Demuxer, "mpegts", "MPEG transport stream (read)", "TV player, TS files", kAllOs, false, false},
        {MediaKind::Demuxer, "mov", "MP4 / MOV (read)", "ATSC 3.0 (ROUTE / DASH), video files", kAllOs, false, false},
        {MediaKind::Demuxer, "flv", "FLV (read)", "Video generator RTMP input (OBS Studio)", kAllOs, false, true},
        // camera and capture-card input (libavdevice v4l2 / dshow, AVFoundation on macOS)
        {MediaKind::Capture, "camera", "Cameras and capture cards", "Video generator, analog TV transmit", kAllOs, false, true},
        // add-ons the user installs (patented codecs: OnAir never ships them; Help > Voice codecs)
        {MediaKind::AddOn, "ac4", "AC-4 sound (add-on)", "ATSC 3.0 and DVB sound", kAllOs, false, true},
        {MediaKind::AddOn, "avsplus", "AVS+ video (add-on)", "DTMB TV in China", kAllOs, false, true},
        {MediaKind::AddOn, "dra", "DRA / DRA+ sound (add-on)", "CDR radio, DTMB TV sound", kAllOs, false, true},
    };
    return t;
}

inline const char* kindTitle(MediaKind k) {
    switch (k) {
    case MediaKind::VideoEncoder: return "Video encoders";
    case MediaKind::AudioEncoder: return "Audio encoders";
    case MediaKind::Decoder: return "Decoders";
    case MediaKind::Muxer: return "Containers (write)";
    case MediaKind::Demuxer: return "Containers (read)";
    case MediaKind::Capture: return "Capture";
    case MediaKind::AddOn: return "Add-ons";
    }
    return "";
}

// the rows for this operating system (os: one OsMask bit)
inline std::vector<const Media*> mediaFor(unsigned os) {
    std::vector<const Media*> r;
    for (const auto& m : mediaTable()) if (m.os & os) r.push_back(&m);
    return r;
}

struct MediaStatus {
    bool ok = false;
    std::string note;   // why not (a hardware encoder's error, "not in this FFmpeg"), or where an add-on was loaded from
};

// ---- system facts and requirements -----------------------------------------------------------------------------------------------------

enum class OsFamily { Windows, MacOS, Linux, Other };

struct Facts {
    OsFamily os = OsFamily::Other;
    std::string osText;                     // "Windows 10.0 build 22631", "Ubuntu 26.04 LTS (kernel 7.0.0)" (diag_ui.cpp osName)
    int osMajor = 0, osMinor = 0, osBuild = 0;   // Windows 10.0.22631, macOS 15.1; 0 when unknown (Linux: unused)
    bool is64bit = false;
    std::string arch;                       // "x86-64", "arm64", ...
    std::string cpuText;                    // brand string with the thread count
    int threads = 0;
    bool x86 = false, avx2 = false, fma = false;   // x86 only (x86-64 always has SSE2, the release baseline)
    bool neon = false;                      // arm only
    bool avx2Build = false;                 // built with DECT2_REQUIRES_AVX2: AVX2 + FMA is then a must
    uint64_t ramBytes = 0;                  // 0 = unknown
    std::string gpuText;                    // GL_RENDERER (or "Metal")
    std::string glVersionText;              // GL_VERSION
    int glMajor = 0, glMinor = 0;           // parsed from GL_VERSION; 0 = unknown
    bool gles = false, metal = false;
    std::string gpuComputeText;             // the GPU compute device for error correction, or why there is none
    bool gpuCompute = false;
};

enum class Verdict { Pass, Warn, Fail, Unknown };

struct Check {
    std::string item;          // "Operating system", "Processor threads", ...
    std::string minimum;       // what OnAir needs to start and work
    std::string recommended;   // what TV decoding and the video generator want
    std::string found;         // this computer
    Verdict verdict = Verdict::Unknown;
};

// "3.3 Mesa 24.0", "4.6.0 NVIDIA 550.54", "OpenGL ES 3.2 Mesa ..." -> major / minor (false when no number is found)
inline bool parseGlVersion(const std::string& s, int& major, int& minor, bool& es) {
    es = s.find("OpenGL ES") != std::string::npos;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] < '0' || s[i] > '9') continue;
        int a = 0, b = 0;
        size_t j = i;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9') a = a * 10 + (s[j++] - '0');
        if (j >= s.size() || s[j] != '.' || j + 1 >= s.size() || s[j + 1] < '0' || s[j + 1] > '9') { i = j; continue; }
        j++;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9') b = b * 10 + (s[j++] - '0');
        major = a; minor = b;
        return true;
    }
    return false;
}

// the numbers of diag_ui.cpp osName(): "Windows 10.0 build 22631" -> 10, 0, 22631; "macOS 15.1" -> 15, 1, 0. Other texts: the family only.
inline OsFamily parseOsText(const std::string& s, int& major, int& minor, int& build) {
    major = minor = build = 0;
    const auto num = [&s](size_t& i) { int v = 0; while (i < s.size() && s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0'); return v; };
    OsFamily fam = OsFamily::Linux;
    size_t i = 0;
    if (s.rfind("Windows", 0) == 0) { fam = OsFamily::Windows; i = 7; }
    else if (s.rfind("macOS", 0) == 0) { fam = OsFamily::MacOS; i = 5; }
    else return s.empty() ? OsFamily::Other : OsFamily::Linux;
    while (i < s.size() && s[i] == ' ') i++;
    if (i >= s.size() || s[i] < '0' || s[i] > '9') return fam;
    major = num(i);
    if (i < s.size() && s[i] == '.') { i++; minor = num(i); }
    const size_t b = s.find("build ", i);
    if (b != std::string::npos) { size_t j = b + 6; build = num(j); }
    return fam;
}

inline std::string gbText(uint64_t bytes) {
    std::string b(32, '\0');
    const int n = std::snprintf(b.data(), b.size(), "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    b.resize(n > 0 ? std::min((size_t)n, b.size() - 1) : 0);
    return b;
}

// OnAir's minimum and recommended computer, from docs/INSTALL.md (Windows 10 / 11 64-bit, macOS 15, Linux x86-64 / arm64), the start-up
// checks (OpenGL 3.0 or OpenGL ES 3.0: app/main_window.cpp, AVX2 builds: app/main.cpp) and the release baseline (plain x86-64, SSE2; the
// AVX2 + FMA fast paths picked at run time, dect2/dsp/simd.h). RAM, threads and the GPU are what DVB-T2 / ATSC 3.0 decoding and HD video encoding want.
inline constexpr int kMinThreads = 2, kRecThreads = 4;
inline constexpr uint64_t kGiB = 1024ull * 1024ull * 1024ull;
inline constexpr uint64_t kMinRam = 4 * kGiB, kRecRam = 8 * kGiB;
// a "4 GB" computer reports a little less (memory kept by the firmware and the graphics): 90 % of the figure counts
inline constexpr uint64_t ramSlack(uint64_t v) { return v / 10 * 9; }

inline std::vector<Check> evaluate(const Facts& f) {
    std::vector<Check> r;
    {   // operating system
        Check c{"Operating system", "Windows 10 64-bit, macOS 15, Linux 64-bit", "Windows 11, macOS 15 or later, a current Linux", f.osText, Verdict::Unknown};
        if (f.os == OsFamily::Windows) c.verdict = f.osMajor == 0 ? Verdict::Unknown : f.osMajor >= 10 ? (f.osBuild >= 22000 ? Verdict::Pass : Verdict::Warn) : Verdict::Fail;
        else if (f.os == OsFamily::MacOS) c.verdict = f.osMajor == 0 ? Verdict::Unknown : f.osMajor >= 15 ? Verdict::Pass : Verdict::Fail;
        else if (f.os == OsFamily::Linux) c.verdict = Verdict::Pass;
        if (f.os == OsFamily::Windows && f.osBuild > 0 && f.osBuild < 22000 && f.osMajor >= 10) c.found += " (Windows 10)";
        r.push_back(c);
    }
    r.push_back(Check{"64-bit", "64-bit processor and system", "x86-64 or arm64", f.arch.empty() ? (f.is64bit ? "64-bit" : "32-bit") : f.arch,
                      f.is64bit ? Verdict::Pass : Verdict::Fail});
    {   // threads
        Check c{"Processor threads", std::to_string(kMinThreads), std::to_string(kRecThreads) + " or more", f.threads > 0 ? std::to_string(f.threads) : "unknown",
                f.threads <= 0 ? Verdict::Unknown : f.threads < kMinThreads ? Verdict::Fail : f.threads < kRecThreads ? Verdict::Warn : Verdict::Pass};
        r.push_back(c);
    }
    if (f.x86) {   // instruction set
        // AVX2 + FMA: the hot loops (resamplers, FFT, ATSC / DTMB filters) have a second copy that dispatch picks at run time (dect2/dsp/simd.h)
        const bool fast = f.avx2 && f.fma;
        const std::string found = fast ? "SSE2, AVX2 + FMA" : f.avx2 ? "SSE2, AVX2, no FMA" : "SSE2, no AVX2 + FMA";
        Check c{"Processor instructions", f.avx2Build ? "AVX2 + FMA (this build)" : "x86-64 (SSE2)", "AVX2 + FMA (Intel / AMD from about 2013)", found, Verdict::Pass};
        if (f.avx2Build) c.verdict = fast ? Verdict::Pass : Verdict::Fail;
        else c.verdict = fast ? Verdict::Pass : Verdict::Warn;
        r.push_back(c);
    } else if (!f.arch.empty() && f.arch.find("arm") != std::string::npos) {
        r.push_back(Check{"Processor instructions", "NEON", "NEON (arm64)", f.neon ? "NEON" : "no NEON", f.neon ? Verdict::Pass : Verdict::Fail});
    }
    {   // memory
        Check c{"Memory (RAM)", std::to_string(kMinRam / kGiB) + " GB", std::to_string(kRecRam / kGiB) + " GB or more", f.ramBytes ? gbText(f.ramBytes) : "unknown",
                Verdict::Unknown};
        if (f.ramBytes) c.verdict = f.ramBytes < ramSlack(kMinRam) ? Verdict::Fail : f.ramBytes < ramSlack(kRecRam) ? Verdict::Warn : Verdict::Pass;
        r.push_back(c);
    }
    {   // graphics
        Check c{"Graphics", "OpenGL 3.0 or OpenGL ES 3.0 (macOS: Metal)", "OpenGL 3.2 or later", f.metal ? "Metal" : f.glVersionText.empty() ? "unknown" : (f.gles ? "" : "OpenGL ") + f.glVersionText,
                Verdict::Unknown};
        if (f.metal) c.verdict = Verdict::Pass;
        else if (f.glMajor > 0) {
            const int v = f.glMajor * 10 + f.glMinor;
            c.verdict = v < 30 ? Verdict::Fail : (!f.gles && v >= 32) ? Verdict::Pass : Verdict::Warn;
        }
        r.push_back(c);
    }
    r.push_back(Check{"GPU computing", "not needed (the CPU does all)", "Vulkan, Direct3D 11 or Metal: faster TV error correction",
                      f.gpuComputeText.empty() ? "unknown" : f.gpuComputeText, f.gpuCompute ? Verdict::Pass : Verdict::Warn});
    return r;
}

// the worst verdict of the checks: Fail when one fails, Warn when one only warns, else Pass (Unknown counts as Warn)
inline Verdict overall(const std::vector<Check>& cs) {
    Verdict w = Verdict::Pass;
    for (const auto& c : cs) {
        if (c.verdict == Verdict::Fail) return Verdict::Fail;
        if (c.verdict == Verdict::Warn || c.verdict == Verdict::Unknown) w = Verdict::Warn;
    }
    return w;
}

inline const char* verdictWord(Verdict v) {
    switch (v) {
    case Verdict::Pass: return "OK";
    case Verdict::Warn: return "below recommended";
    case Verdict::Fail: return "below minimum";
    case Verdict::Unknown: return "unknown";
    }
    return "";
}

inline const char* overallText(Verdict v) {
    switch (v) {
    case Verdict::Pass: return "This computer meets the recommended requirements.";
    case Verdict::Warn: return "This computer meets the minimum requirements; some parts are below the recommended ones.";
    case Verdict::Fail: return "This computer is below the minimum requirements: OnAir may not start or may be too slow.";
    case Verdict::Unknown: return "";
    }
    return "";
}

// ---- USB radio drivers ----------------------------------------------------------------------------------------------------------------

// What a USB radio needs before OnAir can open it: the operating system part (WinUSB on Windows; udev rules and the kernel driver
// blacklists on Linux; nothing on macOS), the driver library of that radio (Settings > Advanced > Optional libraries), and per radio that is
// plugged in whether something is known to be wrong. The lines are built in hardware_ui.cpp; the radio matching is here (tested).
enum class UsbGroup { System, Library, Radio };

struct UsbLine {
    UsbGroup group = UsbGroup::System;
    std::string name;       // "WinUSB", "librtlsdr (RTL-SDR)", "RTL-SDR (0bda:2838)"
    std::string usedFor;
    bool ok = false;
    bool optional = false;  // only needed for some radios
    std::string note;       // where it was found, or what to do
};

inline const char* usbGroupTitle(UsbGroup g) {
    switch (g) {
    case UsbGroup::System: return "Operating system";
    case UsbGroup::Library: return "Radio driver libraries";
    case UsbGroup::Radio: return "Radios plugged in";
    }
    return "";
}

struct UsbRadioSeen {
    std::string radio;   // the name dect2/source/usb_diag.h gives ("RTL-SDR", "HackRF Jawbreaker", ...)
    uint16_t vid = 0, pid = 0;
    std::string problem; // found by the bus scan (Windows: no WinUSB driver)
};

inline std::string usbIdText(uint16_t vid, uint16_t pid) {
    static const char* const hex = "0123456789abcdef";
    std::string s = "0000:0000";
    for (int i = 0; i < 4; i++) { s[3 - i] = hex[(vid >> (4 * i)) & 15]; s[8 - i] = hex[(pid >> (4 * i)) & 15]; }
    return s;
}

// One line per radio on the bus: a cross with the reason when the scan or a hint (usbRadioHints, sdrplayApiHints, ...) names a problem.
// A hint belongs to the radio whose name it holds (the longest name that fits, so "HackRF Jawbreaker" does not count for "HackRF");
// a hint no plugged-in radio fits (an AOR I/Q output, an Aaronia SPECTRAN) gets a line of its own.
inline std::vector<UsbLine> usbRadioLines(const std::vector<UsbRadioSeen>& radios, const std::vector<std::string>& hints) {
    std::vector<UsbLine> out;
    for (const auto& r : radios) {
        UsbLine l;
        l.group = UsbGroup::Radio;
        l.name = r.radio + " (" + usbIdText(r.vid, r.pid) + ")";
        l.usedFor = "plugged in";
        l.ok = r.problem.empty();
        l.note = r.problem;
        out.push_back(l);
    }
    for (const auto& h : hints) {
        size_t best = 0;
        for (const auto& r : radios)
            if (r.radio.size() > best && h.find(r.radio) != std::string::npos) best = r.radio.size();
        bool used = false;
        for (size_t i = 0; best > 0 && i < radios.size(); i++) {
            if (radios[i].radio.size() != best || h.find(radios[i].radio) == std::string::npos) continue;
            UsbLine& l = out[i];
            l.ok = false;
            if (l.note.find(h) == std::string::npos) l.note += (l.note.empty() ? "" : "; ") + h;
            used = true;
        }
        if (!used) {
            UsbLine l;
            l.group = UsbGroup::Radio;
            l.name = "USB device";
            l.usedFor = "plugged in";
            l.note = h;
            out.push_back(l);
        }
    }
    return out;
}

// ---- the text for the clipboard --------------------------------------------------------------------------------------------------------

struct MediaLine { const Media* m; MediaStatus s; };

// The USB radio drivers part of the text, also copied alone by the button of that section
inline std::string usbReportText(const std::vector<UsbLine>& usb) {
    std::string t;
    t += "USB radio drivers ([x] works, [ ] missing)\n";
    bool anyRadio = false;
    for (UsbGroup g : {UsbGroup::System, UsbGroup::Library, UsbGroup::Radio}) {
        t += std::string("  ") + usbGroupTitle(g) + "\n";
        for (const auto& l : usb) {
            if (l.group != g) continue;
            anyRadio = anyRadio || g == UsbGroup::Radio;
            t += std::string("    [") + (l.ok ? "x" : " ") + "] " + l.name;
            if (!l.note.empty()) t += ": " + l.note;
            t += "\n";
        }
    }
    if (!anyRadio) t += "    none of the radios OnAir knows\n";
    return t;
}

// Plain text, one line per item: [x] works, [ ] missing. English only (it goes into bug reports).
inline std::string reportText(const std::string& version, const Facts& f, const std::vector<Check>& checks, const std::vector<MediaLine>& media,
                              const std::string& ffmpegVersion, const std::vector<UsbLine>& usb) {
    std::string t = "OnAir " + version + " - hardware\n\n";
    t += "System\n";
    t += "  OS:        " + f.osText + "\n";
    t += "  Processor: " + f.cpuText + (f.arch.empty() ? "" : ", " + f.arch) + "\n";
    t += "  Memory:    " + (f.ramBytes ? gbText(f.ramBytes) : std::string("unknown")) + "\n";
    t += "  Graphics:  " + f.gpuText + (f.glVersionText.empty() ? "" : " (" + f.glVersionText + ")") + "\n";
    t += "  GPU compute: " + f.gpuComputeText + "\n";
    t += "  FFmpeg:    " + ffmpegVersion + "\n\n";
    t += "Requirements: ";
    t += overallText(overall(checks));
    t += "\n";
    for (const auto& c : checks)
        t += "  [" + std::string(c.verdict == Verdict::Pass ? "x" : c.verdict == Verdict::Fail ? "!" : "~") + "] " + c.item + ": " + c.found +
             " (minimum " + c.minimum + "; recommended " + c.recommended + ") - " + verdictWord(c.verdict) + "\n";
    t += "\nMedia support ([x] works, [ ] missing)\n";
    int lastKind = -1;
    for (const auto& l : media) {
        if ((int)l.m->kind != lastKind) { lastKind = (int)l.m->kind; t += std::string("  ") + kindTitle(l.m->kind) + "\n"; }
        t += std::string("    [") + (l.s.ok ? "x" : " ") + "] " + l.m->label + " (" + l.m->name + ")";
        if (!l.s.note.empty()) t += ": " + l.s.note;
        t += "\n";
    }
    t += "\n" + usbReportText(usb);
    return t;
}

} // namespace hwinfo
