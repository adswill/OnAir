// SPDX-License-Identifier: GPL-3.0-or-later
// Dynamic Linux Moonshine/ONNX loader. Libraries may be anywhere below models/.
#include "moonshine_runtime_loader.h"
#include "dect2/common/u8file.h"
#include <algorithm>
#include <cstdint>
#include <dlfcn.h>
#include <filesystem>
#include <mutex>
#include <map>
#include <vector>

namespace dect2 {
namespace {
namespace fs = std::filesystem;
bool libraryName(const std::string& n, const std::string& prefix) {
    return n.rfind(prefix, 0) == 0 && n.find(".so", prefix.size()) != std::string::npos;
}
std::string dlError() { const char* s = dlerror(); return s ? s : "unknown loader error"; }
std::mutex gMutex;
std::string gPinnedOnnx;
void* gPinnedHandle = nullptr;

MoonshineDiscoveredLibraries discover(const std::string& root) {
    MoonshineDiscoveredLibraries result;
    std::error_code ec;
    fs::recursive_directory_iterator it(u8file(root), fs::directory_options::skip_permission_denied, ec), end;
    while (!ec && it != end) {
        auto entry = *it;
        it.increment(ec);
        std::error_code e;
        if (!entry.is_regular_file(e) && !entry.is_symlink(e)) continue;
        const auto name = entry.path().filename().string();
        if (libraryName(name, "libmoonshine")) result.moonshine.push_back(entry.path().string());
        // ONNX providers are plugins, not substitutes for libonnxruntime.so.
        if (libraryName(name, "libonnxruntime") && name.find("_providers_") == std::string::npos)
            result.onnx.push_back(entry.path().string());
    }
    auto clean = [](std::vector<std::string>& v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    };
    clean(result.moonshine); clean(result.onnx);
    // Stable, deterministic ordering, regardless of filesystem iteration order.
    // Never combine libraries from different directories.
    std::map<fs::path, std::pair<std::vector<std::string>, std::vector<std::string>>> dirs;
    for (const auto& path : result.moonshine) dirs[fs::path(path).parent_path()].first.push_back(path);
    for (const auto& path : result.onnx) dirs[fs::path(path).parent_path()].second.push_back(path);
    for (const auto& [dir, pair] : dirs) {
        if (!pair.first.empty() && !pair.second.empty()) {
            // Prefer the unversioned .so entry, if present, then lexicographic order.
            const auto preferred = [](const std::vector<std::string>& v, const char* filename) {
                for (const auto& path : v) if (fs::path(path).filename() == filename) return path;
                return v.front();
            };
            result.selectedMoonshine = preferred(pair.first, "libmoonshine.so");
            result.selectedOnnx = preferred(pair.second, "libonnxruntime.so");
            break;
        }
    }
    return result;
}


// The first two C ABI fields of OrtApiBase are GetApi and GetVersionString.
struct OrtApiBasePrefix {
    const void* (*getApi)(uint32_t);
    const char* (*getVersionString)();
};
using OrtGetApiBaseFn = const OrtApiBasePrefix* (*)();
}

MoonshineDiscoveredLibraries discoverMoonshineLibraries(const std::string& root) { return discover(root); }
MoonshineLibraries::~MoonshineLibraries() { close(); }
void MoonshineLibraries::close() {
    if (moonshine) dlclose(moonshine);
    if (onnx) dlclose(onnx);
    moonshine = nullptr; onnx = nullptr;
    moonshinePath.clear(); onnxPath.clear(); provider.clear();
    moonshineVersion.clear(); onnxVersion.clear();
}
bool MoonshineLibraries::open(const std::string& root) {
    close(); error.clear();
    const auto libraries = discover(root);
    const std::string& m = libraries.selectedMoonshine;
    const std::string& o = libraries.selectedOnnx;
    if (m.empty() || o.empty()) {
        error = "No directory under " + root +
                " contains both libmoonshine*.so* and libonnxruntime*.so*";
        return false;
    }
    std::lock_guard<std::mutex> guard(gMutex);
    if (!gPinnedOnnx.empty() && gPinnedOnnx != o) {
        error = "ONNX Runtime " + gPinnedOnnx + " is already loaded. Restart OnAir to use " + o;
        return false;
    }
    dlerror();
    // The ONNX and Moonshine runtimes may own process-global state and background
    // threads. Keep successfully loaded DSOs mapped when a speech backend is
    // replaced (Moonshine -> Whisper), even after its local dlclose().
    void* ort = dlopen(o.c_str(), RTLD_NOW | RTLD_GLOBAL | RTLD_NODELETE);
    if (!ort) { error = "ONNX: " + dlError(); return false; }
    auto cleanup = [&] { if (ort) dlclose(ort); };
    auto ortApi = reinterpret_cast<OrtGetApiBaseFn>(dlsym(ort, "OrtGetApiBase"));
    if (!ortApi || !ortApi() || !ortApi()->getVersionString) {
        error = "ONNX: missing OrtGetApiBase / GetVersionString";
        cleanup(); return false;
    }
    const char* version = ortApi()->getVersionString();
    const std::string ortVersion = version ? version : "Unknown";

    dlerror();
    void* moon = dlopen(m.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
    if (!moon) { error = "Moonshine: " + dlError(); cleanup(); return false; }
    const char* required[] = {"moonshine_load_transcriber_from_files", "moonshine_free_transcriber",
                             "moonshine_transcribe_without_streaming", "moonshine_error_to_string",
                             "moonshine_get_stt_dependencies", "moonshine_free_buffer"};
    for (const auto* symbol : required) if (!dlsym(moon, symbol)) {
        error = std::string("Moonshine missing C API symbol: ") + symbol;
        dlclose(moon); cleanup(); return false;
    }
    using VersionFn = int32_t (*)();
    auto moonVersionFn = reinterpret_cast<VersionFn>(dlsym(moon, "moonshine_get_version"));
    std::string moonVersion = "Unknown (version symbol missing)";
    if (moonVersionFn) {
        const int32_t v = moonVersionFn();
        if (v >= 0) moonVersion = std::to_string(v / 10000) + "." + std::to_string((v / 100) % 100) + "." + std::to_string(v % 100);
    }
    if (!gPinnedHandle) {
        gPinnedHandle = dlopen(o.c_str(), RTLD_NOW | RTLD_GLOBAL | RTLD_NODELETE);
        if (gPinnedHandle) gPinnedOnnx = o;
    }
    onnx = ort; moonshine = moon;
    onnxPath = o; moonshinePath = m;
    onnxVersion = ortVersion; moonshineVersion = moonVersion;
    provider = "Library default (actual execution device not verified)";
    return true;
}
} // namespace dect2
