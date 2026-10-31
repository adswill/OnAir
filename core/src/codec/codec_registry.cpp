#include "dect2/codec/codec_registry.h"
#include <map>
#include <mutex>

namespace dect2::codec {

namespace {
std::mutex gMu;
struct Slot { DecoderFactory f; std::string path; };
std::map<std::string, Slot>& slots() { static std::map<std::string, Slot> m; return m; }
}

const std::vector<CodecInfo>& codecTable() {
    static const std::vector<CodecInfo> t = {
        {"ambe2", "AMBE+2 (3600x2450)", CodecKind::Patented, "DMR, NXDN, dPMR, P25 phase 2", "plug-in onair-codec-ambe2 (for example around mbelib), or libmbe in the codecs folder"},
        {"ambe",  "AMBE (3600x2400)",   CodecKind::Patented, "D-STAR", "plug-in onair-codec-ambe, or libmbe in the codecs folder"},
        {"aeroambe", "AMBE (Aero, 4800x3600)", CodecKind::Patented, "Inmarsat Aero C-channel voice", "plug-in onair-codec-aeroambe (tools/addons/aeroambe, around the mbelib of libaeroambe); gets each 96-bit coded voice frame as received: 12 bytes, low bit first"},
        {"imbe",  "IMBE (7200x4400)",   CodecKind::Patented, "P25 phase 1, ProVoice / EDACS", "plug-in onair-codec-imbe, or libmbe in the codecs folder"},
        {"ysf",   "AMBE+2 (Yaesu)",     CodecKind::Patented, "Yaesu System Fusion (C4FM)", "plug-in onair-codec-ysf"},
        {"iridium", "AMBE-family (Iridium)", CodecKind::Patented, "Iridium voice", "plug-in onair-codec-iridium; gets the 39 byte voice field of each frame as received (unverified without recordings)"},
        {"tetra", "TETRA ACELP",        CodecKind::Patented, "TETRA", "plug-in onair-codec-tetra (unencrypted calls only)"},
        {"tetrapol", "TETRAPOL RP-CELP", CodecKind::Patented, "TETRAPOL", "plug-in onair-codec-tetrapol (clear calls only)"},
        {"",      "Codec 2",            CodecKind::Open,     "M17, FreeDV 700D / 700E / 1600", "built in (libcodec2 comes with OnAir)"},
        {"",      "RADE",               CodecKind::Open,     "FreeDV RADE", "optional librade"},
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

} // namespace dect2::codec
