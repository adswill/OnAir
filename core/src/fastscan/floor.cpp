// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/fastscan/floor.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {
namespace fastscan {

void DynamicFloor::configure(size_t bins, const FloorParams& p) {
    p_ = p;
    floor_.assign(bins, -150.f);
    last_.assign(bins, 0.0);
    seen_.assign(bins, 0);
}

void DynamicFloor::update(size_t first, const float* x, size_t n, double now, float seedMax) {
    if (first >= floor_.size()) return;
    n = std::min(n, floor_.size() - first);
    // a majority of this hop's bins far above their floors: the level of the hop changed, shift it as one
    tmp_.clear();
    for (size_t i = 0; i < n; i++) if (seen_[first + i]) tmp_.push_back(x[i] - floor_[first + i]);
    float shift = 0;
    if (tmp_.size() >= 16 && tmp_.size() * 2 >= n) {
        const size_t k = tmp_.size() / 2;
        std::nth_element(tmp_.begin(), tmp_.begin() + k, tmp_.end());
        if (std::fabs(tmp_[k]) > p_.jumpDb) shift = tmp_[k];
    }
    for (size_t i = 0; i < n; i++) {
        const size_t b = first + i;
        if (!seen_[b]) { floor_[b] = std::min(x[i] - 1.0f, seedMax); seen_[b] = 1; last_[b] = now; continue; }
        const double dt = std::min(p_.maxDtSec, std::max(0.0, now - last_[b]));
        last_[b] = now;
        float f = floor_[b] + shift;
        const double d = x[i] - f;
        if (d < -p_.fastFallDb) f += (float)(d * 0.5);
        else if (d < 0) f += (float)std::max(d, -p_.releaseDbPerSec * dt);
        else if (d <= p_.outlierDb) f += (float)std::min({d, p_.maxStepDb, p_.attackDbPerSec * dt});
        floor_[b] = f;
    }
}

// ---- persistence
namespace { const uint32_t kMagic = 0x464c4f31u; }   // "FLO1"
bool DynamicFloor::save(const std::string& path, double startHz, double stepHz) const {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint64_t n = floor_.size();
    bool ok = fwrite(&kMagic, 4, 1, f) == 1 && fwrite(&startHz, 8, 1, f) == 1 && fwrite(&stepHz, 8, 1, f) == 1 && fwrite(&n, 8, 1, f) == 1;
    std::vector<int16_t> q(n);
    for (size_t i = 0; i < n; i++) q[i] = seen_[i] ? (int16_t)std::lround(std::min(300.0, std::max(-300.0, (double)floor_[i])) * 100.0) : INT16_MIN;
    ok = ok && fwrite(q.data(), 2, n, f) == n;
    return fclose(f) == 0 && ok;
}
bool DynamicFloor::load(const std::string& path, double startHz, double stepHz) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint32_t m = 0; double s = 0, st = 0; uint64_t n = 0;
    bool ok = fread(&m, 4, 1, f) == 1 && fread(&s, 8, 1, f) == 1 && fread(&st, 8, 1, f) == 1 && fread(&n, 8, 1, f) == 1;
    ok = ok && m == kMagic && std::fabs(s - startHz) < 1.0 && std::fabs(st - stepHz) < 1e-3 * std::max(1.0, stepHz) && n == floor_.size();
    std::vector<int16_t> q(ok ? n : 0);
    ok = ok && fread(q.data(), 2, n, f) == n;
    fclose(f);
    if (!ok) return false;
    for (size_t i = 0; i < n; i++) if (q[i] != INT16_MIN) { floor_[i] = q[i] / 100.f; seen_[i] = 1; }
    return true;
}

// ---- hop calibration
void HopCalibration::configure(size_t n) { prof_.assign(n, 0.f); tmp_.assign(n, 0.f); n_ = 0; }

void HopCalibration::apply(const float* in, float* out) {
    const size_t n = prof_.size();
    if (!n) return;
    // the hop's own level: the median of the bins with the profile taken out (signals are a minority)
    for (size_t i = 0; i < n; i++) tmp_[i] = in[i] - prof_[i];
    std::nth_element(tmp_.begin(), tmp_.begin() + n / 2, tmp_.end());
    const float level = tmp_[n / 2];
    // stochastic median of (bin - level) per position: big steps while the profile is new, then small; the step is bounded, so a signal that
    // sits at one position of many hops moves the profile by at most the step
    const float step = std::max(0.05f, 3.0f / (float)(n_ + 1));
    for (size_t i = 0; i < n; i++) {
        const float r = in[i] - level;
        prof_[i] += r > prof_[i] ? step : (r < prof_[i] ? -step : 0.f);
    }
    n_++;
    for (size_t i = 0; i < n; i++) out[i] = in[i] - prof_[i];
}

}  // namespace fastscan
}  // namespace dect2
