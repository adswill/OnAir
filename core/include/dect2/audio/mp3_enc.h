// MP3 recording through LAME (libmp3lame), loaded at run time: OnAir does not ship the library (LGPL, patents in some regions), so MP3 is
// offered when it is installed and the WAV / FLAC recorders work without it.
#pragma once
#include "dect2/codec/codec_paths.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace dect2 {

class Mp3Enc {
public:
    // true when libmp3lame can be loaded
    static bool available() { return api().ok; }

    bool begin(FILE* f, uint32_t rate, int kbps, const std::vector<std::string>& tags = {}, int channels = 1) {
        const Api& a = api();
        if (!a.ok || !f) return false;
        ch_ = channels == 2 ? 2 : 1;
        if (ch_ == 2 && !a.encodeInter) return false;   // the caller falls back to mono
        gf_ = a.init();
        if (!gf_) return false;
        a.setRate(gf_, (int)rate);
        a.setCh(gf_, ch_);
        a.setMode(gf_, ch_ == 2 ? 1 : 3);   // joint stereo, or MONO
        a.setBrate(gf_, kbps);
        a.setQuality(gf_, 5);
        if (a.id3Init && a.id3Comment) {
            a.id3Init(gf_);
            for (const std::string& t : tags) a.id3Comment(gf_, t.c_str());
        }
        if (a.initParams(gf_) < 0) { a.close(gf_); gf_ = nullptr; return false; }
        f_ = f;
        return true;
    }
    void write(const int16_t* pcm, size_t n) {
        if (!gf_) return;
        std::vector<unsigned char> out(n * 5 / 4 + 7200);   // n frames; stereo input is interleaved
        const int r = ch_ == 2 ? api().encodeInter(gf_, pcm, (int)n, out.data(), (int)out.size()) : api().encode(gf_, pcm, nullptr, (int)n, out.data(), (int)out.size());
        if (r > 0) fwrite(out.data(), 1, (size_t)r, f_);
    }
    void finish() {
        if (!gf_) return;
        unsigned char out[7200];
        const int r = api().flush(gf_, out, (int)sizeof out);
        if (r > 0) fwrite(out, 1, (size_t)r, f_);
        api().close(gf_);
        gf_ = nullptr;
    }

private:
    struct Api {
        bool ok = false;
        void* (*init)() = nullptr;
        int (*setRate)(void*, int) = nullptr;
        int (*setCh)(void*, int) = nullptr;
        int (*setMode)(void*, int) = nullptr;
        int (*setBrate)(void*, int) = nullptr;
        int (*setQuality)(void*, int) = nullptr;
        int (*initParams)(void*) = nullptr;
        int (*encode)(void*, const short*, const short*, int, unsigned char*, int) = nullptr;
        int (*encodeInter)(void*, const short*, int, unsigned char*, int) = nullptr;
        int (*flush)(void*, unsigned char*, int) = nullptr;
        int (*close)(void*) = nullptr;
        void (*id3Init)(void*) = nullptr;
        void (*id3Comment)(void*, const char*) = nullptr;
    };
    static const Api& api() {
        static const Api a = load();
        return a;
    }
    static Api load() {
        Api a;
        void* h = nullptr;
#if defined(_WIN32)
        // the codecs folders first (the user folder survives an update, the program folder does not: issue #376), then the system search path
        const std::vector<codec::CodecDir> dirs = codec::systemCodecDirs();
        for (const char* n : {"libmp3lame.dll", "libmp3lame-0.dll", "lame_enc.dll", "mp3lame.dll"}) {
            for (const auto& d : dirs) {
                const std::string full = codec::joinPath(d.path, n);
                const int wn = MultiByteToWideChar(CP_UTF8, 0, full.c_str(), -1, nullptr, 0);
                if (wn <= 1) continue;
                std::wstring w((size_t)wn, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, full.c_str(), -1, w.data(), wn);
                if ((h = (void*)LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))) break;
            }
            if (h) break;
        }
        if (!h) for (const char* n : {"libmp3lame.dll", "libmp3lame-0.dll", "lame_enc.dll", "mp3lame.dll"}) { if ((h = (void*)LoadLibraryA(n))) break; }
        auto sym = [&](const char* s) { return h ? (void*)GetProcAddress((HMODULE)h, s) : nullptr; };
#else
        for (const char* n : {"libmp3lame.so.0", "libmp3lame.so", "libmp3lame.0.dylib", "libmp3lame.dylib"}) {
            for (const auto& d : codec::systemCodecDirs()) { if ((h = dlopen(codec::joinPath(d.path, n).c_str(), RTLD_NOW | RTLD_LOCAL))) break; }
            if (h) break;
        }
        if (!h) for (const char* n : {"libmp3lame.so.0", "libmp3lame.so", "libmp3lame.0.dylib", "libmp3lame.dylib", "/opt/homebrew/lib/libmp3lame.dylib", "/usr/local/lib/libmp3lame.dylib"}) { if ((h = dlopen(n, RTLD_NOW | RTLD_LOCAL))) break; }
        auto sym = [&](const char* s) { return h ? dlsym(h, s) : nullptr; };
#endif
#define M(f, s) a.f = reinterpret_cast<decltype(a.f)>(sym(s))
        M(init, "lame_init"); M(setRate, "lame_set_in_samplerate"); M(setCh, "lame_set_num_channels"); M(setMode, "lame_set_mode");
        M(setBrate, "lame_set_brate"); M(setQuality, "lame_set_quality"); M(initParams, "lame_init_params");
        M(encode, "lame_encode_buffer"); M(encodeInter, "lame_encode_buffer_interleaved"); M(flush, "lame_encode_flush"); M(close, "lame_close");
        M(id3Init, "id3tag_init"); M(id3Comment, "id3tag_set_comment");
#undef M
        a.ok = a.init && a.setRate && a.setCh && a.setMode && a.setBrate && a.setQuality && a.initParams && a.encode && a.flush && a.close;
        return a;
    }
    void* gf_ = nullptr;
    FILE* f_ = nullptr;
    int ch_ = 1;
};

}  // namespace dect2
