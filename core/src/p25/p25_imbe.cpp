// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// IMBE through the user-installed mbelib (see p25_imbe.h); the loader is shared with the other voice decoders (source/mbe_dyn.h).
#include "dect2/p25/p25_imbe.h"
#include "../source/mbe_dyn.h"
#include "dect2/codec/codec_registry.h"
#include <cstring>

namespace dect2 {

namespace {
using FrameFn = void (DECT2_CALL*)(short* aout, int* errs, int* errs2, char* errStr, char fr[8][23], char d[88], void* cur, void* prev, void* prevEnh, int uvq);
mbe::Loader<FrameFn> gMbe("mbe_processImbe7200x4400Frame");
} // namespace

bool p25ImbeAvailable(std::string* path) { return codec::pluginLoaded("imbe", path) || gMbe.available(path); }
bool p25ImbeRescan() { return gMbe.rescan(); }

struct P25Imbe::Impl {
    const mbe::Api<FrameFn>* a = nullptr;
    std::shared_ptr<mbe::State> s = std::make_shared<mbe::State>();
    std::unique_ptr<codec::SpeechCodec> plug;   // the user's codec plug-in "imbe" (docs/CODECS.md); preferred over mbelib
};

P25Imbe::P25Imbe() : p_(std::make_unique<Impl>()) {
    p_->plug = codec::pluginVocoder("imbe");
    if (p_->plug) return;
    const mbe::Api<FrameFn>& a = gMbe.api();
    if (!a.ok) return;
    if (!mbe::initState(a, p_->s)) return;   // on the codec guard: a library that crashes is disabled, OnAir goes on
    p_->a = &a;
}
P25Imbe::~P25Imbe() = default;
bool P25Imbe::ok() const { return p_->plug || p_->a != nullptr; }

void P25Imbe::decode(const char frIn[8][23], int16_t pcm[160]) {
    if (!p_->a && !p_->plug) { memset(pcm, 0, 160 * sizeof(int16_t)); return; }
    if (p_->plug) {
        const std::vector<int16_t> v = p_->plug->decode(codec::packCodeWords(&frIn[0][0], 8, 23));
        memset(pcm, 0, 160 * sizeof(int16_t));
        for (size_t i = 0; i < v.size() && i < 160; i++) pcm[i] = v[i];
        return;
    }
    char fr[8][23];
    memcpy(fr, frIn, sizeof fr);
    mbe::decodeFrame(*p_->a, p_->s, &fr[0][0], 8 * 23, pcm);   // guarded: never on the caller's stack, a crash or hang disables the library
}

} // namespace dect2
