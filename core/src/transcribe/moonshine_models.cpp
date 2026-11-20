// SPDX-License-Identifier: GPL-3.0-or-later
// Moonshine model downloads: native catalog -> HTTPS curl -> CRC32C -> atomic publish.
// No Python, shell, hardcoded CDN filename lists or user-supplied URLs required.
#include "dect2/transcribe/moonshine_models.h"
#if defined(__linux__) && !defined(__ANDROID__)
#include "moonshine_runtime_loader.h"
#include "dect2/common/thread_name.h"
#include "dect2/common/u8file.h"
#include "dect2/engine/updater.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <regex>
#include <vector>
#include <dlfcn.h>

namespace dect2 {
namespace {
namespace fs = std::filesystem;
struct Option { const char* name; const char* value; };
using ManifestFn = int32_t (*)(const char*, const Option*, uint64_t, char**);
using FreeFn = void (*)(void*);
struct Asset { std::string name, url, checksum; uint64_t size = 0; };

// The official manifest has a flat object for each file. We intentionally
// reject malformed/unexpected fields rather than interpreting arbitrary JSON.
std::string field(const std::string& object, const char* key) {
    std::smatch m;
    const std::regex re(std::string("\\\"") + key + "\\\"\\s*:\\s*\\\"([^\\\"\\\\]*)\\\"");
    return std::regex_search(object, m, re) ? m[1].str() : std::string();
}
uint64_t sizeField(const std::string& object) {
    std::smatch m;
    static const std::regex re("\\\"size\\\"\\s*:\\s*([0-9]+)");
    if (!std::regex_search(object, m, re)) return 0;
    try { return std::stoull(m[1].str()); } catch (...) { return 0; }
}
bool parseManifest(const std::string& json, std::vector<Asset>& assets, std::string& why) {
    if (json.size() > 256 * 1024 || json.find("\"groups\"") == std::string::npos) { why = "invalid Moonshine dependency manifest"; return false; }
    // Each file object contains name/url/size/checksum/checksum_type, and no nested structures.
    const std::regex object("\\{[^\\{\\}]*\\}");
    for (auto it = std::sregex_iterator(json.begin(), json.end(), object); it != std::sregex_iterator(); ++it) {
        const std::string o = it->str();
        if (o.find("\"name\"") == std::string::npos || o.find("\"url\"") == std::string::npos) continue;
        Asset a{field(o, "name"), field(o, "url"), field(o, "checksum"), sizeField(o)};
        // Untrusted manifest fields cannot choose file paths or download hosts.
        if (a.name.empty() || a.name == "." || a.name == ".." || a.name.find_first_of("/\\:") != std::string::npos ||
            a.url.rfind("https://download.moonshine.ai/", 0) != 0 ||
            a.size == 0 || a.size > 1024ull * 1024 * 1024 ||
            field(o, "checksum_type") != "crc32c" || a.checksum.empty()) {
            why = "unsupported or unsafe Moonshine model manifest entry"; return false;
        }
        if (std::any_of(assets.begin(), assets.end(), [&](const Asset& old) { return old.name == a.name; })) { why = "duplicate Moonshine model filename"; return false; }
        assets.push_back(std::move(a));
    }
    if (assets.empty() || assets.size() > 64) { why = "Moonshine manifest contains no usable files"; return false; }
    return true;
}

// CRC32C Castagnoli; encoded as big-endian four bytes in base64 by Moonshine CDN.
std::string crc32cBase64(const std::string& path) {
    std::ifstream in(u8file(path), std::ios::binary);
    if (!in) return {};
    uint32_t crc = ~uint32_t(0);
    char buf[16384];
    while (in.read(buf, sizeof(buf)) || in.gcount()) {
        for (std::streamsize j = 0; j < in.gcount(); ++j) {
            crc ^= static_cast<unsigned char>(buf[j]);
            for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1u)));
        }
    }
    if (in.bad()) return {};
    crc = ~crc;
    const unsigned char b[4] = {static_cast<unsigned char>(crc >> 24), static_cast<unsigned char>(crc >> 16), static_cast<unsigned char>(crc >> 8), static_cast<unsigned char>(crc)};
    const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    s += abc[b[0] >> 2]; s += abc[((b[0] & 3) << 4) | (b[1] >> 4)];
    s += abc[((b[1] & 15) << 2) | (b[2] >> 6)]; s += abc[b[2] & 63];
    s += abc[b[3] >> 2]; s += abc[(b[3] & 3) << 4]; s += "==";
    return s;
}

} // namespace

MoonshineModelDownloader::~MoonshineModelDownloader() { cancel_ = true; if (thread_.joinable()) thread_.join(); }
std::string MoonshineModelDownloader::error() const { std::lock_guard<std::mutex> lk(mu_); return error_; }
std::string MoonshineModelDownloader::modelName() const { std::lock_guard<std::mutex> lk(mu_); return name_; }
bool MoonshineModelDownloader::start(const std::string& root, const std::string& language, int arch, const std::string& name) {
    if (state_ == State::Downloading || state_ == State::Verifying) return false;
    if (name.rfind("moonshine-", 0) != 0 || name.find_first_of("/\\:") != std::string::npos || (language != "en" && language != "de") || arch < 2 || arch > 5) return false;
    if (thread_.joinable()) thread_.join();
    cancel_ = false; done_ = 0; total_ = 0;
    { std::lock_guard<std::mutex> lk(mu_); error_.clear(); name_ = name; }
    state_ = State::Downloading;
    thread_ = namedThread("onair-moon-dl", [this, root, language, arch, name] { run(root, language, arch, name); });
    return true;
}
void MoonshineModelDownloader::run(std::string root, std::string language, int arch, std::string name) {
    std::error_code ec;
    const fs::path target = u8file(root + "/" + name);
    const fs::path staging = u8file(root + "/." + name + ".download");
    auto fail = [&](const std::string& msg) { fs::remove_all(staging, ec); { std::lock_guard<std::mutex> lk(mu_); error_ = msg; } state_ = State::Failed; };
    MoonshineLibraries libraries;
    if (!libraries.open(root)) { fail("Moonshine/ONNX Runtime unavailable: " + libraries.error); return; }
    void* library = libraries.moonshine;
    const auto get = reinterpret_cast<ManifestFn>(dlsym(library, "moonshine_get_stt_dependencies"));
    const auto release = reinterpret_cast<FreeFn>(dlsym(library, "moonshine_free_buffer"));
    if (!get || !release) { fail("Moonshine runtime does not expose the model dependency API"); return; }
    const std::string archText = std::to_string(arch);
    const Option opt{"model_arch", archText.c_str()};
    char* raw = nullptr;
    const int32_t rc = get(language.c_str(), &opt, 1, &raw);
    std::string manifest = raw ? std::string(raw) : std::string();
    if (raw) release(raw);
    if (rc != 0) { fail("Moonshine does not publish this model/language combination"); return; }
    std::vector<Asset> assets; std::string why;
    if (!parseManifest(manifest, assets, why)) { fail(why); return; }
    uint64_t total = 0;
    for (const auto& a : assets) total += a.size;
    total_ = total;
    fs::create_directories(u8file(root), ec);
    if (ec) { fail("cannot create models folder: " + ec.message()); return; }
    if (fs::exists(target, ec)) { fail("model folder already exists; remove or rename it to reinstall"); return; }
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    if (ec) { fail("cannot create temporary model folder: " + ec.message()); return; }
    uint64_t finished = 0;
    for (const auto& a : assets) {
        if (cancel_) { fail("cancelled"); return; }
        const std::string out = (staging / u8file(a.name)).string();
        const int code = curlDownloadFile(a.url, out, a.size + 4096, 1800, cancel_, [&] {
            std::error_code e;
            const auto sz = fs::file_size(u8file(out), e);
            if (!e) done_ = finished + std::min(uint64_t(sz), a.size);
        });
        if (code != 0) { fail(curlDownloadError(code)); return; }
        state_ = State::Verifying;
        if (fs::file_size(u8file(out), ec) != a.size || ec || crc32cBase64(out) != a.checksum) {
            fail("Moonshine asset failed size/CRC32C verification: " + a.name); return;
        }
        finished += a.size; done_ = finished;
        state_ = State::Downloading;
    }
    if (cancel_) { fail("cancelled"); return; }
    // Publish the folder only when every asset is checked; the scanner never sees partial models.
    fs::rename(staging, target, ec);
    if (ec) { fail("cannot finish model installation: " + ec.message()); return; }
    state_ = State::Done;
}
} // namespace dect2

#else
// Other platforms keep the Transcripts UI linkable, with downloads unavailable.
namespace dect2 {
MoonshineModelDownloader::~MoonshineModelDownloader() { cancel_ = true; if (thread_.joinable()) thread_.join(); }
std::string MoonshineModelDownloader::error() const { return "Moonshine downloads are supported on Linux only"; }
std::string MoonshineModelDownloader::modelName() const { return {}; }
bool MoonshineModelDownloader::start(const std::string&, const std::string&, int, const std::string&) { return false; }
void MoonshineModelDownloader::run(std::string, std::string, int, std::string) {}
}
#endif
