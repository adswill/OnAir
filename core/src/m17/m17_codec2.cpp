// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Codec 2 through libcodec2, loaded at run time: the one shipped with OnAir, or one the user installed (see m17_codec2.h); only the few functions of codec2.h (release 1.2) that are needed.
#include "dect2/m17/m17_codec2.h"
#include "../source/native_common.h"
#include <cstdlib>
#include <cstring>
#include <vector>

namespace dect2 {

namespace {
struct CODEC2;   // opaque
constexpr int CODEC2_MODE_3200 = 0, CODEC2_MODE_1600 = 2;

struct Api {
    native::DynLib lib;
    bool ok = false;
    CODEC2* (DECT2_CALL* create)(int) = nullptr;
    void (DECT2_CALL* destroy)(CODEC2*) = nullptr;
    void (DECT2_CALL* encode)(CODEC2*, unsigned char*, short*) = nullptr;
    void (DECT2_CALL* decode)(CODEC2*, short*, const unsigned char*) = nullptr;
    int (DECT2_CALL* spf)(CODEC2*) = nullptr;
    int (DECT2_CALL* bpf)(CODEC2*) = nullptr;
};

const Api& api() {
    static const Api a = [] {
        Api r;
        std::vector<std::string> names;
        if (const char* e = getenv("DECT2_CODEC2_PATH")) if (*e) names.push_back(e);
        if (names.empty()) names = native::codecLibNames(DECT2_CODEC2_STEM, DECT2_CODEC2_SO, DECT2_CODEC2_WIN);
        if (!r.lib.open(names)) return r;
        native::DynLib& l = r.lib;
        r.ok = l.get(r.create, "codec2_create") && l.get(r.destroy, "codec2_destroy") && l.get(r.encode, "codec2_encode") &&
               l.get(r.decode, "codec2_decode") && l.get(r.spf, "codec2_samples_per_frame") && l.get(r.bpf, "codec2_bytes_per_frame");
        return r;
    }();
    return a;
}
} // namespace

bool m17Codec2Available(std::string* path) {
    const Api& a = api();
    if (a.ok && path) *path = a.lib.path;
    return a.ok;
}

const char* m17Codec2InstallHint() {
#if defined(__APPLE__)
    return "This copy of OnAir came without its codec2 library: download the OnAir disk image again, or install it with Homebrew: brew install codec2";
#elif defined(_WIN32)
    return "This copy of OnAir came without libcodec2.dll: unpack the whole OnAir zip again (it belongs next to OnAir.exe), or install OnAir with its setup program.";
#else
    return "This copy of OnAir came without its codec2 library: install the OnAir package again, or install your distribution's codec2 package (libcodec2).";
#endif
}

struct M17Codec2::Impl {
    CODEC2* c = nullptr;
    int spf = 0;
};

M17Codec2::M17Codec2(int rate) : p_(std::make_unique<Impl>()) {
    const Api& a = api();
    if (!a.ok) return;
    p_->c = a.create(rate == 1600 ? CODEC2_MODE_1600 : CODEC2_MODE_3200);
    if (p_->c && a.bpf(p_->c) != 8) { a.destroy(p_->c); p_->c = nullptr; }   // both M17 rates use 8-byte frames
    if (p_->c) p_->spf = a.spf(p_->c);
}
M17Codec2::~M17Codec2() { if (p_->c) api().destroy(p_->c); }
bool M17Codec2::ok() const { return p_->c != nullptr; }
int M17Codec2::samplesPerFrame() const { return p_->spf; }
void M17Codec2::decode(const uint8_t bits[8], int16_t* pcm) {
    if (!p_->c) { memset(pcm, 0, sizeof(int16_t) * (size_t)p_->spf); return; }
    api().decode(p_->c, pcm, bits);
}
void M17Codec2::encode(const int16_t* pcm, uint8_t bits[8]) {
    if (!p_->c) { memset(bits, 0, 8); return; }
    std::vector<short> b(pcm, pcm + p_->spf);
    api().encode(p_->c, bits, b.data());
}

} // namespace dect2
