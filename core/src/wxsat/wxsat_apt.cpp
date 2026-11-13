// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NOAA APT below the FM discriminator: the 2400 Hz amplitude-modulated subcarrier down to 4160 samples a second, the sync A pulse train found by correlation, 2080 samples a line.
// 4.0 line filter (setNoiseFilter, on by default): the noise of one sample is measured on each line's space A (47 words of steady grey after
// sync A; its middle 31), averaged over lines; the two pictures of the line then go through a local Wiener filter over 7 samples with that
// noise: flat stretches are averaged, edges kept, and a clean line (noise near zero) passes unchanged. Before it, a sample more than 4 robust
// sigmas (median absolute deviation of space A) from the median of its 5 neighbours (an FM click) is replaced by that median. Simulated gains: docs/SENSITIVITY-4.0-X2.md.
#include "dect2/wxsat/wxsat_rx.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2::wxsat {

namespace {
constexpr int kLine = 2080, kSync = 39;
constexpr int kSearch = kLine + kSync;
}

struct AptDecoder::Impl {
    double fs = 0, phase = 0, dphase = 0, take = 0, ratio = 1;
    std::vector<std::complex<float>> ring;
    size_t rpos = 0;
    int boxN = 1;
    std::complex<float> boxSum{0, 0};
    std::vector<float> env;
    float syncT[kSync];
    float lastEnv = 0;
    bool lock = false;
    int misses = 0, lineIdx = 0;
    int64_t consumed = 0;   // samples of env already removed
    int64_t expect = 0;     // env position (absolute) of the next line
    uint64_t lines = 0;
    float low = 0, high = 1, level = 0;
    bool noiseFilter = true;
    double noiseVar = -1;   // of one envelope sample, from the space A of the lines
    double noiseRob = -1;   // the same from the median absolute deviation (FM clicks hardly move it)
    std::function<void(int, const uint8_t*)>* cb = nullptr;

    Impl() {
        // sync A: 4 black, 7 cycles 1040 Hz (2 samples black, 2 white), 7 black, as +-1
        const int pat[kSync] = {0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0};
        double mean = 0;
        for (int v : pat) mean += v;
        mean /= kSync;
        for (int i = 0; i < kSync; i++) syncT[i] = (float)(pat[i] - mean);
    }

    void setup(double rate) {
        fs = rate;
        dphase = 2 * math::kPi * 2400.0 / rate;
        boxN = std::max(1, (int)std::lround(rate / 2400.0));
        ratio = rate / 4160.0;
        ring.assign((size_t)boxN, {0, 0});
        clear();
    }
    void clear() {
        rpos = 0; boxSum = {0, 0}; std::fill(ring.begin(), ring.end(), std::complex<float>(0, 0));
        phase = 0; take = 0; env.clear(); lastEnv = 0;
        lock = false; misses = 0; consumed = 0; expect = 0; lineIdx = 0; lines = 0; low = 0; high = 1; level = 0; noiseVar = -1; noiseRob = -1;
    }

    float corr(int at, float& energy) const {
        double mean = 0;
        for (int i = 0; i < kSync; i++) mean += env[(size_t)(at + i)];
        mean /= kSync;
        double dot = 0, e = 0, t2 = 0;
        for (int i = 0; i < kSync; i++) {
            const double v = env[(size_t)(at + i)] - mean;
            dot += v * syncT[i]; e += v * v; t2 += syncT[i] * syncT[i];
        }
        energy = (float)e;
        return e > 0 ? (float)(dot / std::sqrt(e * t2)) : 0.f;
    }

    void emit(int at, int skipped) {
        // white and black from the sync pulses
        float hi = 0, lo = 0;
        int nh = 0, nl = 0;
        for (int i = 0; i < kSync; i++) { const float v = env[(size_t)(at + i)]; if (syncT[i] > 0) { hi += v; nh++; } else { lo += v; nl++; } }
        hi /= std::max(1, nh); lo /= std::max(1, nl);
        const float a = lines < 4 ? 0.5f : 0.1f;
        low += a * (lo - low); high += a * (hi - high);
        const float range = std::max(1e-6f, high - low);
        float line[kLine];
        for (int i = 0; i < kLine; i++) line[i] = env[(size_t)(at + i)];
        if (noiseFilter) {
            double s1 = 0, s2 = 0; int k = 0;
            for (int i = kSync + 8; i < 86 - 8; i++) { const double v = line[i]; s1 += v; s2 += v * v; k++; }
            const double var = std::max(0.0, (s2 - s1 * s1 / k) / (k - 1));
            noiseVar = noiseVar < 0 ? var : noiseVar + 0.1 * (var - noiseVar);
            {
                float sp[86], dv[86]; int m = 0;
                for (int i = kSync + 8; i < 86 - 8; i++) sp[m++] = line[i];
                std::nth_element(sp, sp + m / 2, sp + m);
                const float med = sp[m / 2];
                for (int i = 0; i < m; i++) dv[i] = std::fabs(sp[i] - med);
                std::nth_element(dv, dv + m / 2, dv + m);
                const double r = 1.4826 * dv[m / 2];
                noiseRob = noiseRob < 0 ? r * r : noiseRob + 0.1 * (r * r - noiseRob);
            }
            constexpr int R = 3;
            for (const int seg0 : {86, 1126}) {          // image A and image B, 909 words each
                // FM clicks: a sample more than 4 robust sigmas from the median of the 5 around it is replaced by that median
                const float lim = (float)(4 * std::sqrt(std::max(0.0, noiseRob)));
                float c[909];
                for (int i = 0; i < 909; i++) {
                    float w5[5];
                    for (int d = -2; d <= 2; d++) w5[d + 2] = line[seg0 + std::clamp(i + d, 0, 908)];
                    std::nth_element(w5, w5 + 2, w5 + 5);
                    c[i] = std::fabs(line[seg0 + i] - w5[2]) > lim ? w5[2] : line[seg0 + i];
                }
                std::copy(c, c + 909, line + seg0);
                float f[909];
                for (int i = 0; i < 909; i++) {
                    double a1 = 0, a2 = 0; int n = 0;
                    for (int d = -R; d <= R; d++) { const int j = std::clamp(i + d, 0, 908); const double v = line[seg0 + j]; a1 += v; a2 += v * v; n++; }
                    const double mu = a1 / n, lv = std::max(0.0, a2 / n - mu * mu);
                    const double g = lv > 1e-12 ? std::max(0.0, lv - noiseVar) / lv : 0.0;
                    f[i] = (float)(mu + g * (line[seg0 + i] - mu));
                }
                std::copy(f, f + 909, line + seg0);
            }
        }
        uint8_t px[kLine];
        for (int i = 0; i < kLine; i++) {
            const float v = (line[i] - low) / range * 255.f;
            px[i] = (uint8_t)std::clamp((int)std::lround(v), 0, 255);
        }
        lines++;
        if (cb && *cb) (*cb)(lineIdx, px);
        lineIdx += 1 + skipped;
    }

    void process() {
        for (;;) {
            if (lock) {
                const int base = (int)(expect - consumed);
                if (base + kLine + 16 + kSync > (int)env.size()) return;
                int best = base; float bv = -2, e;
                for (int d = -6; d <= 6; d++) {
                    const int at = base + d;
                    if (at < 0) continue;
                    const float c = corr(at, e);
                    if (c > bv) { bv = c; best = at; }
                }
                if (bv < 0.35f) { if (++misses > 12) { lock = false; continue; } best = base; }
                else misses = 0;
                emit(best, 0);
                expect = consumed + best + kLine;
                const int cut = std::max(0, best + kLine - 8);
                env.erase(env.begin(), env.begin() + cut);
                consumed += cut;
            } else {
                if ((int)env.size() < kSearch * 2 + kSync) return;
                int bestAt = -1;
                float bv = 0.45f, e;
                for (int at = 0; at < kSearch; at++) {
                    const float c = corr(at, e);
                    if (c > bv) { bv = c; bestAt = at; }
                }
                if (bestAt < 0) {
                    env.erase(env.begin(), env.begin() + kLine);
                    consumed += kLine;
                    continue;
                }
                lock = true; misses = 0;
                expect = consumed + bestAt;
            }
        }
    }

    void feed(const float* x, size_t n) {
        if (fs <= 0) return;
        for (size_t i = 0; i < n; i++) {
            const std::complex<float> m = x[i] * std::complex<float>((float)std::cos(phase), (float)-std::sin(phase));
            phase += dphase; if (phase > 2 * math::kPi) phase -= 2 * math::kPi;
            boxSum += m - ring[rpos]; ring[rpos] = m; rpos = (rpos + 1) % ring.size();
            const float v = std::abs(boxSum) * 2.f / (float)boxN;
            level += 0.002f * (v - level);
            take += 1.0;
            if (take >= ratio) {
                take -= ratio;
                const float f = (float)take;
                env.push_back(lastEnv + (1.f - f) * (v - lastEnv));
                if (env.size() >= (size_t)kSearch * 3) process();
            }
            lastEnv = v;
        }
    }
};

AptDecoder::AptDecoder() : p_(std::make_unique<Impl>()) { p_->cb = &onLine; }
AptDecoder::~AptDecoder() = default;
void AptDecoder::configure(double r) { p_->setup(r); }
void AptDecoder::reset() { p_->clear(); }
void AptDecoder::pushAudio(const float* x, size_t n) { p_->feed(x, n); }
bool AptDecoder::locked() const { return p_->lock; }
uint64_t AptDecoder::linesOut() const { return p_->lines; }
float AptDecoder::carrierLevel() const { return p_->level; }
void AptDecoder::setNoiseFilter(bool on) { p_->noiseFilter = on; }

} // namespace dect2::wxsat
