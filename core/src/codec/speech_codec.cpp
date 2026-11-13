// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Loader for user-installed speech codecs (see core/include/dect2/codec/speech_codec.h and docs/CODECS.md).
#include "dect2/codec/speech_codec.h"
#include "dect2/codec/codec_guard.h"
#include "dect2/codec/codec_arch.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace dect2::codec {

namespace {

constexpr const char* kDocUrl = "https://github.com/adswill/OnAir/blob/main/docs/CODECS.md";
constexpr size_t kMaxSamplesPerFrame = 16000;   // a bit over one second at 16 kHz: no coded frame is longer

struct Api {
    void* (*open)() = nullptr;
    void (*close)(void*) = nullptr;
    int (*rate)() = nullptr;
    int (*decode)(void*, const uint8_t*, size_t, int16_t*, size_t) = nullptr;
};

// Opens the library; on failure err gets the system's own reason (GetLastError text / dlerror).
void* openLibrary(const std::string& path, std::string& err) {
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (n <= 0) { err = "the file path is not valid text"; return nullptr; }
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
    // The altered search path makes Windows look for the plug-in's own dependent DLLs (a codec library shipped next to it) in the
    // plug-in's folder first. The path must be absolute for that.
    HMODULE m = LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        const DWORD code = GetLastError();
        char buf[512] = {0};
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buf, sizeof buf - 1, nullptr);
        std::string t = buf;
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ' || t.back() == '.')) t.pop_back();
        err = "the file would not load, Windows error " + std::to_string(code) + (t.empty() ? "" : ": " + t) +
              (code == 126 ? " (a DLL the plug-in needs is missing: put it in the same folder)" : code == 193 ? " (wrong bit width: use a 64-bit plug-in)" : "");
        return nullptr;
    }
    return reinterpret_cast<void*>(m);
#else
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* e = dlerror();
        const std::string why = e ? e : "unknown error";
        // the wrong processor (an arm64 plug-in in the Intel OnAir under Rosetta): said in plain words first
        const std::string arch = archMismatchMessage(why);
        err = arch.empty() ? "the file would not load: " + why : arch;   // the loader's own text lists every path it tried: too long to show
        if (runningUnderRosetta()) err += " " + rosettaNotice();
    }
    return h;
#endif
}

void closeLibrary(void* lib) {
    if (!lib) return;
#ifdef _WIN32
    FreeLibrary(reinterpret_cast<HMODULE>(lib));
#else
    dlclose(lib);
#endif
}

void* symbol(void* lib, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

// Decoder over a plug-in: owns the library handle and the codec state. Every call into the plug-in goes through the codec guard
// (codec_guard.h): a plug-in built for another library version (mbelib-neo for an mbelib wrapper) that crashes or hangs is disabled
// and OnAir goes on. Its state and library are then never touched again (not closed: that would call into it).
class PluginCodec final : public SpeechCodec {
public:
    PluginCodec(Api api, void* state, void* lib, std::string id, int rate) : api_(api), state_(state), lib_(lib), id_(std::move(id)), rate_(rate) {}
    ~PluginCodec() override {
        if (guardDisabled(id_)) return;   // it crashed or hangs: leave it alone
        Api a = api_;
        void* s = state_;
        if (s) runGuarded(id_, [a, s] { a.close(s); });
        if (!guardDisabled(id_)) closeLibrary(lib_);
    }
    int sampleRate() const override { return rate_; }
    std::vector<int16_t> decode(const std::vector<uint8_t>& coded) override {
        // the buffers belong to the call (a hung call may outlive this decoder); the PCM buffer is larger than the plug-in is told
        struct Call { std::vector<uint8_t> in; std::vector<int16_t> pcm; int n = 0; };
        auto c = std::make_shared<Call>();
        c->in = coded;
        c->pcm.assign(kMaxSamplesPerFrame + 4096, 0);
        Api a = api_;
        void* s = state_;
        if (runGuarded(id_, [a, s, c] { c->n = a.decode(s, c->in.data(), c->in.size(), c->pcm.data(), kMaxSamplesPerFrame); }) != GuardResult::Ok) return {};
        if (c->n <= 0) return {};
        c->pcm.resize(std::min(static_cast<size_t>(c->n), kMaxSamplesPerFrame));   // a count larger than the buffer is not believed
        return c->pcm;
    }
private:
    Api api_;
    void* state_;
    void* lib_;
    std::string id_;
    int rate_;
};

std::unique_ptr<SpeechCodec> openSharedLibrary(const std::string& path, std::string& err) {
    void* lib = openLibrary(path, err);
    if (!lib) return nullptr;
    Api a;
    a.open = reinterpret_cast<decltype(a.open)>(symbol(lib, "onair_speech_open"));
    a.close = reinterpret_cast<decltype(a.close)>(symbol(lib, "onair_speech_close"));
    a.rate = reinterpret_cast<decltype(a.rate)>(symbol(lib, "onair_speech_sample_rate"));
    a.decode = reinterpret_cast<decltype(a.decode)>(symbol(lib, "onair_speech_decode"));
    if (!a.open || !a.close || !a.rate || !a.decode) {
        std::string miss;
        if (!a.open) miss += " onair_speech_open";
        if (!a.close) miss += " onair_speech_close";
        if (!a.rate) miss += " onair_speech_sample_rate";
        if (!a.decode) miss += " onair_speech_decode";
        closeLibrary(lib);
        err = "it does not export the codec functions (missing:" + miss + ")";
        return nullptr;
    }
    // optional handshake: a plug-in that exports onair_speech_abi() must answer the version this OnAir speaks (kSpeechAbi). Plug-ins
    // built before it was added do not export it and load as before.
    if (auto abi = reinterpret_cast<int (*)()>(symbol(lib, "onair_speech_abi"))) {
        const int v = abi();
        if (v != kSpeechAbi) {
            closeLibrary(lib);
            err = "it was built for codec interface version " + std::to_string(v) + ", this OnAir uses version " + std::to_string(kSpeechAbi) + ": build it again from docs/CODECS.md";
            return nullptr;
        }
    }
    // the plug-in's id from its file name (onair-codec-<id>.<ext>): the codec guard keeps it apart from the other codecs
    std::string id = std::filesystem::path(path).stem().string();
    if (id.rfind("onair-codec-", 0) == 0) id = id.substr(12);
    struct Start { void* state = nullptr; int rate = 0; };
    auto st = std::make_shared<Start>();   // owned by the call: one that hangs may finish after this function returned
    if (runGuarded(id, [a, st] { st->state = a.open(); st->rate = st->state ? a.rate() : 0; }) != GuardResult::Ok) {
        err = guardMessage(id);   // crashed or hangs while starting: left loaded and alone
        return nullptr;
    }
    if (!st->state) { closeLibrary(lib); err = "its decoder would not start"; return nullptr; }
    const int rate = st->rate >= 4000 && st->rate <= 96000 ? st->rate : 8000;   // a rate no speech codec has is not believed
    return std::make_unique<PluginCodec>(a, st->state, lib, id, rate);
}

bool validId(const std::string& id) {
    if (id.empty()) return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    return true;
}

} // namespace

std::string speechCodecNotice() {
    return "Legal notice. OnAir does not include, download or distribute this speech codec. You install it yourself, and you are "
           "responsible for it: for following the laws where you live on receiving and listening to radio transmissions, and for the "
           "licence and patent terms of the codec. Install only a codec you are allowed to use.";
}

std::string speechCodecFileName(const std::string& id) {
#ifdef _WIN32
    const char* ext = ".dll";
#elif defined(__APPLE__)
    const char* ext = ".dylib";
#else
    const char* ext = ".so";
#endif
    return "onair-codec-" + id + ext;
}

CodecLookup loadSpeechCodec(const std::string& id, const std::string& folder, bool noticeAccepted,
                            std::unique_ptr<SpeechCodec>& out, const CodecOpener& opener) {
    CodecLookup r;
    out.reset();
    if (!validId(id)) {
        r.status = CodecStatus::LoadFailed;
        r.message = "Codec id \"" + id + "\" is not valid.";
        return r;
    }
    const std::string file = speechCodecFileName(id);
    const std::filesystem::path path = std::filesystem::path(folder) / file;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        r.status = CodecStatus::NotInstalled;
        r.message = "Codec not installed: this mode needs the " + id + " speech codec. OnAir does not include it. Install it yourself "
                    "(see " + std::string(kDocUrl) + ") and put the plug-in file " + file + " in " + folder + ".";
        return r;
    }
    std::error_code ac;
    const std::filesystem::path abs = std::filesystem::absolute(path, ac);
    r.path = ac ? path.string() : abs.string();
    r.folder = folder;
    if (!noticeAccepted) {
        r.status = CodecStatus::NoticeRequired;
        r.message = "Accept the codec notice to load the " + id + " speech codec.";
        return r;
    }
    std::string err;
    std::unique_ptr<SpeechCodec> codec = opener ? opener(r.path, err) : openSharedLibrary(r.path, err);
    if (!codec) {
        r.status = CodecStatus::LoadFailed;
        r.message = "The " + id + " codec at " + r.path + " could not be loaded: " + err + ".";
        return r;
    }
    out = std::move(codec);
    r.status = CodecStatus::Ready;
    r.message = "Codec loaded: " + id + ".";
    return r;
}

CodecLookup loadSpeechCodecInDirs(const std::string& id, const std::vector<std::string>& folders, bool noticeAccepted,
                                  std::unique_ptr<SpeechCodec>& out, const CodecOpener& opener) {
    out.reset();
    if (!validId(id) || folders.empty()) return loadSpeechCodec(id, folders.empty() ? std::string() : folders[0], noticeAccepted, out, opener);
    const std::string file = speechCodecFileName(id);
    std::vector<std::string> hits;
    for (const auto& f : folders) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(std::filesystem::path(f) / file, ec)) hits.push_back(f);
    }
    if (hits.empty()) {
        CodecLookup r;
        std::string list;
        for (const auto& f : folders) list += (list.empty() ? "" : ", ") + f;
        r.status = CodecStatus::NotInstalled;
        r.message = "Codec not installed: this mode needs the " + id + " speech codec. OnAir does not include it. Install it yourself "
                    "(see " + std::string(kDocUrl) + ") and put the plug-in file " + file + " in one of these folders: " + list + ".";
        return r;
    }
    CodecLookup first;
    for (size_t i = 0; i < hits.size(); ++i) {
        CodecLookup r = loadSpeechCodec(id, hits[i], noticeAccepted, out, opener);
        if (r.status == CodecStatus::Ready || r.status == CodecStatus::NoticeRequired) {
            for (size_t j = 0; j < hits.size(); ++j) if (j != i) r.shadowed.push_back(hits[j]);
            return r;
        }
        if (i == 0) first = r;
    }
    for (size_t j = 1; j < hits.size(); ++j) first.shadowed.push_back(hits[j]);
    return first;   // none would load: say why the first one failed
}

} // namespace dect2::codec
