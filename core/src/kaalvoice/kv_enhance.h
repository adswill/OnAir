// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL voice codec 4: decoder-side speech enhancer (receiver only, the bit stream does not change). See docs/KAAL-ENHANCE.md.
//
// Runs inside the decoder on the raw core synthesis and the decoder's side information (LPC, pitch lag and gains, pulse energy, the
// high-band envelope, frame kinds). Stages (Config::enhanceStages): pitch and formant post-filter with tilt compensation, energy
// restoration of the sparse pulse excitation, high-band harmonic extension + envelope smoothing, comfort-noise shaping, de-clicking
// after losses and an optional Wiener noise reduction. Deterministic: only + - * / sqrt and the codec's own elementary functions.
#pragma once
#include "kv_core.h"
#include <memory>

namespace dect2 {
namespace kaalvoice {
namespace detail {

// Whether the enhancer runs for this configuration (Config::enhance -1 = auto: below 140 bits per 20 ms, i.e. 3.2 / 4.8 / 6.0).
bool enhanceActive(const Config& c, const Prof& P);
// Extra decoder delay of the enhancer (the noise reduction stage), samples at the output rate.
int enhanceDelay(const Config& c, const Prof& P);

// Per-tier strengths (exposed for the benchmark).
struct EnhTier {
    int minBits20;
    double gn, gd;       // formant post-filter A(z/gn) / A(z/gd)
    double ltp;          // pitch post-filter strength
    double tilt;         // tilt compensation factor
    double fill;         // energy restoration: noise power relative to the pulse power, at zero voicing
    double bwe;          // high band: largest harmonic share
    double restoreDb;    // energy restoration: level the waveform-matching gains lose on average (low band, dB)
};
const EnhTier& enhTierOf(const Prof& P);

class Enhancer {
public:
    enum Kind { Speech, Concealed, Comfort };
    Enhancer(const Prof& P, uint32_t stages, bool postfilter);
    void reset();
    uint32_t stages() const { return st_; }
    // Speech or concealed frame: lb holds the raw core synthesis (core postfilter off), N8 samples; replaced by the enhanced signal.
    void lowBand(const CoreDecoder& core, bool lost, float* lb);
    // Both sides restarted after comfort noise: clear the pitch history.
    void restart();
    // Comfort-noise frame: lb holds the comfort noise; level smoothing.
    void comfortLow(float* lb);
    // High band from the per-block band levels (dB per bin); core == nullptr for comfort noise (noise only). hb: nb * 80 samples.
    void highBand(const CoreDecoder* core, const double (*L)[kHbBands], int nb, float* hb);
    // Final output frame (output rate, int16 scale): de-click and noise reduction.
    void output(float* x, int n, Kind k, bool afterLoss, int lostRun);
    double noiseDb() const { return noiseDb_; }   // background estimate of the low band (dB per sample)
private:
    void postfilter(const SubInfo& si, const float* in, float* out, int L, double w, bool lost);
    const Prof& P_;
    const EnhTier& T_;
    uint32_t st_;
    bool pf_;
    Rng rng_, hbRng_;
    // low band
    float nfSyn_[kP];                    // synthesis memory of the restoration noise
    float pfRes_[kPitMax + 2 + kMaxN8];  // post-filter residual history
    float pfFir_[kP], pfIir_[kP];
    float pfTilt_;
    double pfAgc_;
    int lostRun_;
    double cngE_, cngG_;                 // smoothed comfort-noise energy, last comfort-noise gain
    // high band
    Up2 exUp_;                           // excitation 8 -> 16 kHz (same delay as the output's low band)
    float hbOla_[kHbM];
    double hbLs_[kHbBands];
    bool hbHave_;
    // output
    float last_[2];                      // last output sample, mean |step| of the last 1 ms
    double env_;                         // output RMS of the recent past
    double noiseDb_;
    // noise reduction (optional)
    struct Nr;
    std::unique_ptr<Nr> nr_;
public:
    ~Enhancer();
    Enhancer(const Enhancer&) = delete;
    Enhancer& operator=(const Enhancer&) = delete;
};

}  // namespace detail
}  // namespace kaalvoice
}  // namespace dect2
