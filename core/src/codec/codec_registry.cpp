// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/codec/codec_registry.h"
#include <map>
#include <mutex>

namespace dect2::codec {

namespace {
std::mutex gMu;
struct Slot { DecoderFactory f; std::string path; };
std::map<std::string, Slot>& slots() { static std::map<std::string, Slot> m; return m; }
std::map<std::string, CodecLookup>& lookups() { static std::map<std::string, CodecLookup> m; return m; }
}

const std::vector<CodecInfo>& codecTable() {
    static const std::vector<CodecInfo> t = {
        {"ambe2", "AMBE+2 (3600x2450)", CodecKind::Patented, "DMR, NXDN, dPMR, P25 phase 2", "plug-in onair-codec-ambe2 (for example around mbelib), or libmbe in the codecs folder"},
        {"ambe",  "AMBE (3600x2400)",   CodecKind::Patented, "D-STAR", "plug-in onair-codec-ambe, or libmbe in the codecs folder"},
        {"aeroambe", "AMBE 4800x3600 (Inmarsat Aero)", CodecKind::Patented, "Inmarsat Aero C channel voice", "plug-in onair-codec-aeroambe (build tools/addons/aeroambe against libaeroambe's mbelib; docs/CODECS.md)"},
        {"imbe",  "IMBE (7200x4400)",   CodecKind::Patented, "P25 phase 1, ProVoice / EDACS", "plug-in onair-codec-imbe, or libmbe in the codecs folder"},
        {"ysf",   "AMBE+2 (Yaesu)",     CodecKind::Patented, "Yaesu System Fusion (C4FM)", "plug-in onair-codec-ysf"},
        {"iridium", "AMBE-family (Iridium)", CodecKind::Patented, "Iridium voice", "plug-in onair-codec-iridium; gets the 39 byte voice field of each frame, first bit received = least significant bit (iridium-toolkit .dfs order; unverified without recordings)"},
        {"tetra", "TETRA ACELP",        CodecKind::Patented, "TETRA", "plug-in onair-codec-tetra (unencrypted calls only)"},
        {"tetrapol", "TETRAPOL RP-CELP", CodecKind::Patented, "TETRAPOL", "plug-in onair-codec-tetrapol (clear calls only)"},
        {"",      "Codec 2",            CodecKind::Open,     "M17, FreeDV 700D / 700E / 1600", "built in (libcodec2 comes with OnAir)"},
        {"",      "KA codecs",          CodecKind::Open,     "KAAL, KAPP, KAFT, KAVB, KAIT, KAWS", "built in"},
        {"",      "DECT and satellite voice", CodecKind::Unsupported, "DECT, other satellite voice", "no voice decoder in OnAir (signalling and data only)"},
    };
    return t;
}

const CodecInfo* findCodec(const std::string& id) {
    if (id.empty()) return nullptr;
    for (const auto& c : codecTable()) if (id == c.id) return &c;
    return nullptr;
}

size_t pluginFrameBytes(const std::string& id) {
    if (id == "ambe2" || id == "ambe" || id == "aeroambe") return 12;
    if (id == "imbe") return 23;
    if (id == "iridium") return 39;
    if (id == "tetrapol") return 16;
    return 0;
}

std::vector<uint8_t> packCodeWords(const char* bits, int rows, int cols) {
    const int n = rows * cols;
    std::vector<uint8_t> out(static_cast<size_t>((n + 7) / 8), 0);
    for (int i = 0; i < n; i++) if (bits[i] & 1) out[static_cast<size_t>(i / 8)] |= static_cast<uint8_t>(0x80 >> (i % 8));
    return out;
}

void setPluginFactory(const std::string& id, DecoderFactory f, const std::string& path) {
    std::lock_guard<std::mutex> lk(gMu);
    if (f) slots()[id] = {std::move(f), path}; else slots().erase(id);
}

bool pluginLoaded(const std::string& id, std::string* path) {
    std::lock_guard<std::mutex> lk(gMu);
    auto it = slots().find(id);
    if (it == slots().end()) return false;
    if (path) *path = it->second.path;
    return true;
}

std::unique_ptr<SpeechCodec> pluginVocoder(const std::string& id) {
    DecoderFactory f;
    { std::lock_guard<std::mutex> lk(gMu); auto it = slots().find(id); if (it == slots().end()) return nullptr; f = it->second.f; }
    return f();
}

std::string pluginStatusLine(const std::string& id, const std::string& folder) {
    std::string p;
    if (pluginLoaded(id, &p)) return "Codec plug-in loaded: " + p;
    return "Codec plug-in not found. OnAir looks for " + speechCodecFileName(id) + " in " + folder + " (Windows onair-codec-" + id + ".dll, macOS .dylib, Linux .so).";
}

void notePluginLookup(const std::string& id, const CodecLookup& r) {
    std::lock_guard<std::mutex> lk(gMu);
    lookups()[id] = r;
}

bool lastPluginLookup(const std::string& id, CodecLookup& out) {
    std::lock_guard<std::mutex> lk(gMu);
    const auto it = lookups().find(id);
    if (it == lookups().end()) return false;
    out = it->second;
    return true;
}

std::vector<std::string> registerAcceptedPlugins(const std::vector<std::string>& folders, const std::function<bool(const std::string&)>& accepted,
                                                 const PluginLookupLog& onLookup, const CodecOpener& opener) {
    std::vector<std::string> added;
    for (const CodecInfo& c : codecTable()) {
        if (c.kind != CodecKind::Patented || !*c.id || pluginLoaded(c.id)) continue;
        const std::string id = c.id;
        std::unique_ptr<SpeechCodec> probe;
        const CodecLookup r = loadSpeechCodecInDirs(id, folders, accepted && accepted(id), probe, opener);
        CodecLookup before;
        const bool seen = lastPluginLookup(id, before);
        notePluginLookup(id, r);
        if (onLookup && (!seen || before.status != r.status || before.message != r.message)) onLookup(id, r);
        if (r.status != CodecStatus::Ready) continue;
        probe.reset();
        // every decoder (one per slot / speaker) is a new instance of the plug-in, from the folder it was found in
        setPluginFactory(id, [id, folder = r.folder, opener]() {
            std::unique_ptr<SpeechCodec> d;
            loadSpeechCodec(id, folder, true, d, opener);
            return d;
        }, r.path);
        added.push_back(id);
    }
    return added;
}

} // namespace dect2::codec
