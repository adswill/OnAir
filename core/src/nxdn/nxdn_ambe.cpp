// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AMBE through the user-installed mbelib (see nxdn_ambe.h); the loader is shared with the other voice decoders (source/mbe_dyn.h).
#include "dect2/nxdn/nxdn_ambe.h"
#include "dect2/nxdn/nxdn_proto.h"
#include "../source/mbe_dyn.h"
#include "dect2/codec/codec_registry.h"
#include "dect2/voice/vocoder_fec.h"
#include <algorithm>
#include <cstring>

namespace dect2 {

namespace {
using FrameFn = void (DECT2_CALL*)(short* aout, int* errs, int* errs2, char* errStr, char fr[4][24], char d[49], void* cur, void* prev, void* prevEnh, int uvq);
mbe::Loader<FrameFn> gMbe("mbe_processAmbe3600x2450Frame");
} // namespace

bool nxdnAmbeAvailable(std::string* path) { return codec::pluginLoaded("ambe2", path) || gMbe.available(path); }
bool nxdnAmbeRescan() { return gMbe.rescan(); }

struct NxdnAmbe::Impl {
    const mbe::Api<FrameFn>* a = nullptr;
    std::shared_ptr<mbe::State> s = std::make_shared<mbe::State>();
    std::unique_ptr<codec::SpeechCodec> plug;   // the user's codec plug-in "ambe2" (docs/CODECS.md); preferred over mbelib
    voicefec::AmbeProtector prot;               // soft-decision correction and concealment in front of the codec
    int perm[96];
    bool permOk = false;
};

NxdnAmbe::NxdnAmbe() : p_(std::make_unique<Impl>()) {
    p_->plug = codec::pluginVocoder("ambe2");
    if (p_->plug) return;
    const mbe::Api<FrameFn>& a = gMbe.api();
    if (!a.ok) return;
    if (!mbe::initState(a, p_->s)) return;   // on the codec guard: a library that crashes is disabled, OnAir goes on
    p_->a = &a;
}
NxdnAmbe::~NxdnAmbe() = default;
bool NxdnAmbe::ok() const { return p_->plug || p_->a != nullptr; }

void NxdnAmbe::decode(const uint8_t bits[72], int16_t pcm[160]) {
    char fr[4][24];
    nxdn::ambeFrame(bits, fr);
    decodeFr(fr, pcm);
}

void NxdnAmbe::decodeSoft(const uint8_t bits[72], const uint8_t rel[72], int16_t pcm[160]) {
    char fr[4][24];
    nxdn::ambeFrame(bits, fr);
    if (!p_->permOk) { voicefec::permutationOf([](const uint8_t* b, char f[4][24]) { nxdn::ambeFrame(b, f); }, p_->perm); p_->permOk = true; }
    uint8_t r[4][24];
    voicefec::permuteReliability(p_->perm, rel, r);
    decodeFrSoft(fr, r, pcm);
}

void NxdnAmbe::decodeFrSoft(const char fr[4][24], const uint8_t rel[4][24], int16_t pcm[160]) {
    if (!voicefec::enabled()) { decodeFr(fr, pcm); return; }
    char out[4][24];
    const voicefec::FrameInfo fi = p_->prot.process(fr, rel, out);
    decodeFr(out, pcm);
    if (fi.gain != 1.f) for (int i = 0; i < 160; i++) pcm[i] = (int16_t)(pcm[i] * fi.gain);
}

void NxdnAmbe::fecCounts(int* frames, int* repaired, int* concealed) const {
    if (frames) *frames = p_->prot.frames;
    if (repaired) *repaired = p_->prot.repairedFrames;
    if (concealed) *concealed = p_->prot.concealedFrames;
}

void NxdnAmbe::decodeFr(const char frIn[4][24], int16_t pcm[160]) {
    if (!p_->a && !p_->plug) { memset(pcm, 0, 160 * sizeof(int16_t)); return; }
    char fr[4][24];
    std::copy_n(&frIn[0][0], 4 * 24, &fr[0][0]);
    if (p_->plug) {
        const std::vector<int16_t> v = p_->plug->decode(codec::packCodeWords(&fr[0][0], 4, 24));
        memset(pcm, 0, 160 * sizeof(int16_t));
        for (size_t i = 0; i < v.size() && i < 160; i++) pcm[i] = v[i];
        return;
    }
    mbe::decodeFrame(*p_->a, p_->s, &fr[0][0], 4 * 24, pcm);   // guarded: never on the caller's stack, a crash or hang disables the library
}

} // namespace dect2
