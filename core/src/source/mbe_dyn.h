// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The user-installed mbelib, loaded at run time (patented voice codecs are never shipped, docs/CODECS.md): the part that the AMBE (D-STAR, NXDN) and IMBE (P25) decoders share.
// Only two functions of mbelib.h (1.3) are needed: mbe_initMbeParms and one frame function per codec. mbe_parms is a plain struct of a few kilobytes at most; it is kept in a
// larger buffer of our own (MbeState), so its layout is never needed.
#pragma once
#include "native_common.h"
#include "dect2/codec/codec_guard.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2::mbe {

// mbelib-neo (github.com/arancormonk/mbelib-neo, the library is libmbe-neo) keeps the function names of mbelib 1.3 but changed the frame
// functions: (short* out, mbe_process_result* result, const char fr[..][..], char d[..], cur, prev, prevEnh) instead of
// (short* out, int* errs, int* errs2, char* errStr, fr, d, cur, prev, prevEnh, int uvquality). Called with the old arguments it writes
// its result over the caller's stack and the decoder state over the frame bits, which took OnAir down (Discord report, 2026-10-23). It
// is told apart by the functions only it has (mbe_versionString, mbe_initProcessResult) and called the way it expects.
using NeoFrame = int (DECT2_CALL*)(short* aout, void* result, const char* fr, char* d, void* cur, void* prev, void* prevEnh);

// The sound is taken from the float frame functions (the same name with "f": mbe_processAmbe3600x2450Framef ..., in mbelib 1.3 and in mbelib-neo)
// and scaled here. The short ones multiply by 7 and clip hard at +-0.95 of full scale (mbe_floattoshort), which loud speech reached: a gain of 4
// leaves it about 5 dB of headroom (measured on P25 Phase 2 voice of the NSW PSN). A library without them: the short ones, as before.
constexpr float kFloatGain = 4.f;

template <class Frame>
struct Api {
    native::DynLib lib;
    bool ok = false;
    bool neo = false;   // the library is mbelib-neo (frame functions with its own arguments)
    bool fl = false;    // frame / neoFrame are the float functions (out is float*)
    void (DECT2_CALL* init)(void* cur, void* prev, void* prevEnh) = nullptr;
    Frame frame = nullptr;
    NeoFrame neoFrame = nullptr;
};

// One per codec (a file-scope object): finds the library once, on first use, and again when asked (the "Look again" button). The Api found stays for the life of the program,
// the decoders hold the one they started with.
template <class Frame>
class Loader {
public:
    explicit Loader(const char* frameSymbol) : sym_(frameSymbol) {}
    const Api<Frame>& api() {
        if (const Api<Frame>* a = api_.load(std::memory_order_acquire)) return *a;
        std::lock_guard<std::mutex> lk(mu_);
        if (!api_.load()) api_.store(scan(), std::memory_order_release);
        return *api_.load();
    }
    bool available(std::string* path) {
        const Api<Frame>& a = api();
        if (a.ok && path) *path = a.lib.path;
        return a.ok && !codec::guardDisabled("mbelib");   // a library that crashed or hung this run is not offered again
    }
    bool rescan() {
        codec::guardReset("mbelib");   // Look again: a disabled library is tried again (a new decoder takes it)
        if (api().ok) return true;
        std::lock_guard<std::mutex> lk(mu_);
        api_.store(scan(), std::memory_order_release);
        return api_.load()->ok;
    }

private:
    const Api<Frame>* scan() {
        Api<Frame>* r = new Api<Frame>;
        std::vector<std::string> names;
        if (const char* e = getenv("DECT2_MBELIB_PATH")) if (*e) names.push_back(e);
        if (names.empty()) {
            names = native::codecLibNames(DECT2_MBE_STEM, DECT2_MBE_SO, DECT2_MBE_WIN);
            for (auto& n : native::codecLibNames(DECT2_MBE_NEO_STEM, DECT2_MBE_NEO_SO, DECT2_MBE_NEO_WIN)) names.push_back(n);   // libmbe-neo
        }
        if (r->lib.open(names)) {
            void* v = nullptr;
            r->neo = r->lib.opt(v, "mbe_versionString") || r->lib.opt(v, "mbe_initProcessResult");
            const std::string fsym = std::string(sym_) + "f";
            r->fl = r->neo ? r->lib.opt(r->neoFrame, fsym.c_str()) : r->lib.opt(r->frame, fsym.c_str());
            r->ok = r->lib.get(r->init, "mbe_initMbeParms") && (r->fl || (r->neo ? r->lib.get(r->neoFrame, sym_) : r->lib.get(r->frame, sym_)));
            if (r->ok) fprintf(stderr, "voice codec library: %s (%s%s)\n", r->lib.path.c_str(), r->neo ? "mbelib-neo" : "mbelib", r->fl ? ", float" : "");
        }
        return r;
    }
    const char* sym_;
    std::mutex mu_;
    std::atomic<const Api<Frame>*> api_{nullptr};
};

// The state of one decoder: the current, previous and enhanced parameter blocks (16 kB each; mbelib's mbe_parms is about 1.4 kB,
// mbelib-neo's about 2.9 kB). Shared with the guarded calls: a call that hangs keeps it alive after the decoder is gone.
struct State {
    std::vector<double> cur, prev, enh;
    State() : cur(2048, 0.0), prev(2048, 0.0), enh(2048, 0.0) {}
};

// Every buffer a frame call gets, on the heap and larger than it should need: a library that writes more than it should (another
// version) writes into these, not over the caller's stack.
struct Scratch {
    short out[160 + 512] = {};
    float fout[160 + 512] = {};
    int e1 = 0, e2 = 0;
    int result[64] = {};       // mbelib-neo's mbe_process_result (5 ints) and room to spare
    char err[512] = {};
    char fr[8 * 24 + 256] = {};
    char d[128 + 256] = {};
};

using ClassicFrame = void (DECT2_CALL*)(short* aout, int* errs, int* errs2, char* errStr, char* fr, char* d, void* cur, void* prev, void* prevEnh, int uvq);
using ClassicFrameF = void (DECT2_CALL*)(float* aout, int* errs, int* errs2, char* errStr, char* fr, char* d, void* cur, void* prev, void* prevEnh, int uvq);
using NeoFrameF = int (DECT2_CALL*)(float* aout, void* result, const char* fr, char* d, void* cur, void* prev, void* prevEnh);

// mbe_initMbeParms on the codec guard (codec_guard.h). False: the library crashed or hangs (it is then disabled).
template <class Frame>
bool initState(const Api<Frame>& a, const std::shared_ptr<State>& st) {
    const Api<Frame>* ap = &a;   // the Api stays for the life of the program
    std::shared_ptr<State> s = st;
    return codec::runGuarded("mbelib", [ap, s] { ap->init(s->cur.data(), s->prev.data(), s->enh.data()); }) == codec::GuardResult::Ok;
}

// One frame (frBytes of code word bits: 4x24 AMBE, 8x23 IMBE) into 160 samples of 8 kHz PCM, on the codec guard. False: silence (the
// library crashed, hangs or was disabled before).
template <class Frame>
bool decodeFrame(const Api<Frame>& a, const std::shared_ptr<State>& st, const char* fr, size_t frBytes, int16_t pcm[160]) {
    auto sc = std::make_shared<Scratch>();
    std::memcpy(sc->fr, fr, std::min(frBytes, sizeof sc->fr));
    const Api<Frame>* ap = &a;
    std::shared_ptr<State> s = st;
    const codec::GuardResult g = codec::runGuarded("mbelib", [ap, s, sc] {
        if (ap->fl && ap->neo) reinterpret_cast<NeoFrameF>(ap->neoFrame)(sc->fout, sc->result, sc->fr, sc->d, s->cur.data(), s->prev.data(), s->enh.data());
        else if (ap->fl) reinterpret_cast<ClassicFrameF>(ap->frame)(sc->fout, &sc->e1, &sc->e2, sc->err, sc->fr, sc->d, s->cur.data(), s->prev.data(), s->enh.data(), 3);
        else if (ap->neo) ap->neoFrame(sc->out, sc->result, sc->fr, sc->d, s->cur.data(), s->prev.data(), s->enh.data());
        else reinterpret_cast<ClassicFrame>(ap->frame)(sc->out, &sc->e1, &sc->e2, sc->err, sc->fr, sc->d, s->cur.data(), s->prev.data(), s->enh.data(), 3);
    });
    if (g != codec::GuardResult::Ok) { std::memset(pcm, 0, 160 * sizeof(int16_t)); return false; }
    if (ap->fl)
        for (int i = 0; i < 160; i++) {
            const float v = sc->fout[i] * kFloatGain;
            pcm[i] = (int16_t)(v == v ? std::max(-32767.f, std::min(32767.f, v)) : 0.f);   // NaN: silence
        }
    else for (int i = 0; i < 160; i++) pcm[i] = sc->out[i];
    return true;
}

} // namespace dect2::mbe
