// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/kris/kris_common.h"
#include <algorithm>
#include <cmath>

namespace dect2::kris {

namespace {

// One axis: m bits, L = 2^m levels. Label = the m bits as an integer (first bit most significant); the label is a Gray code of the level index i
// (level = L - 1 - 2 i, so the first bit 0 means positive and neighbours differ in one bit).
struct Axis {
    int m = 0;
    std::vector<float> level;   // by label, scaled
};

Axis makeAxis(int m, float scale) {
    Axis a;
    a.m = m;
    const int L = 1 << m;
    a.level.assign(static_cast<size_t>(L), 0.f);
    for (int i = 0; i < L; i++) {
        const int gray = i ^ (i >> 1);
        a.level[static_cast<size_t>(gray)] = static_cast<float>(L - 1 - 2 * i) * scale;
    }
    return a;
}

Axis axisFor(Mod mod) {
    const int b = static_cast<int>(mod);
    if (b == 1) return makeAxis(1, 1.f);   // BPSK: one real axis, levels +-1
    const int m = b / 2, L = 1 << m;
    return makeAxis(m, 1.f / std::sqrt(2.f * (static_cast<float>(L) * L - 1.f) / 3.f));
}

}  // namespace

std::vector<std::complex<float>> qamMap(const uint8_t* bits, size_t nBits, Mod mod) {
    const int b = static_cast<int>(mod);
    const bool bpsk = (b == 1);
    const Axis ax = axisFor(mod);
    const int m = ax.m;
    const size_t nSym = (nBits + b - 1) / b;
    std::vector<std::complex<float>> out(nSym);
    auto bit = [&](size_t i) -> unsigned { return i < nBits ? (bits[i] & 1u) : 0u; };
    for (size_t s = 0; s < nSym; s++) {
        unsigned li = 0, lq = 0;
        for (int j = 0; j < m; j++) li = (li << 1) | bit(s * b + j);
        if (!bpsk)
            for (int j = 0; j < m; j++) lq = (lq << 1) | bit(s * b + m + j);
        out[s] = {ax.level[li], bpsk ? 0.f : ax.level[lq]};
    }
    return out;
}

void qamDemapSoft(const std::complex<float>* sym, size_t n, Mod mod, float noiseVar, float* llr) {
    const int b = static_cast<int>(mod);
    const bool bpsk = (b == 1);
    const Axis ax = axisFor(mod);
    const int m = ax.m, L = 1 << m;
    const float inv = 1.f / std::max(noiseVar, 1e-12f);
    for (size_t s = 0; s < n; s++) {
        for (int axis = 0; axis < (bpsk ? 1 : 2); axis++) {
            const float y = axis == 0 ? sym[s].real() : sym[s].imag();
            float best0[10], best1[10];   // smallest squared distance with the bit 0 / 1, per bit of the axis
            for (int j = 0; j < m; j++) best0[j] = best1[j] = 1e30f;
            for (int lab = 0; lab < L; lab++) {
                const float d = y - ax.level[static_cast<size_t>(lab)];
                const float d2 = d * d;
                for (int j = 0; j < m; j++) {
                    float& t = ((lab >> (m - 1 - j)) & 1) ? best1[j] : best0[j];
                    if (d2 < t) t = d2;
                }
            }
            for (int j = 0; j < m; j++) llr[s * b + axis * m + j] = (best1[j] - best0[j]) * inv;
        }
    }
}

uint16_t crc16(const uint8_t* d, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= static_cast<uint16_t>(d[i]) << 8;
        for (int k = 0; k < 8; k++) c = (c & 0x8000) ? static_cast<uint16_t>((c << 1) ^ 0x1021) : static_cast<uint16_t>(c << 1);
    }
    return c;
}

uint32_t crc32(const uint8_t* d, size_t n) {
    static const std::vector<uint32_t> tab = [] {
        std::vector<uint32_t> t(256);
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = tab[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::vector<std::complex<float>> zadoffChu(int root, int length) {
    std::vector<std::complex<float>> x;
    if (length <= 0) return x;
    x.resize(static_cast<size_t>(length));
    const uint64_t N = static_cast<uint64_t>(length), u = static_cast<uint64_t>(root < 0 ? -root : root);
    const double sign = root < 0 ? 1.0 : -1.0;
    for (uint64_t i = 0; i < N; i++) {
        const uint64_t q = (length & 1) ? i * (i + 1) : i * i;
        const uint64_t t = ((u % (2 * N)) * (q % (2 * N))) % (2 * N);   // exact phase in units of pi / N
        const double ang = sign * 3.14159265358979323846 * static_cast<double>(t) / static_cast<double>(N);
        x[i] = {static_cast<float>(std::cos(ang)), static_cast<float>(std::sin(ang))};
    }
    return x;
}

std::vector<float> rrcTaps(float beta, int sps, int spanSymbols) {
    std::vector<float> h;
    if (sps < 1 || spanSymbols < 1) return h;
    const int len = spanSymbols * sps + 1;
    h.resize(static_cast<size_t>(len));
    const double pi = 3.14159265358979323846, b = beta;
    double energy = 0;
    for (int i = 0; i < len; i++) {
        const double t = (static_cast<double>(i) - 0.5 * (len - 1)) / sps;   // in symbol periods
        double v;
        if (std::fabs(t) < 1e-9) {
            v = 1.0 - b + 4.0 * b / pi;
        } else if (b > 0 && std::fabs(std::fabs(4.0 * b * t) - 1.0) < 1e-9) {
            v = (b / std::sqrt(2.0)) * ((1.0 + 2.0 / pi) * std::sin(pi / (4.0 * b)) + (1.0 - 2.0 / pi) * std::cos(pi / (4.0 * b)));
        } else {
            v = (std::sin(pi * t * (1.0 - b)) + 4.0 * b * t * std::cos(pi * t * (1.0 + b))) / (pi * t * (1.0 - (4.0 * b * t) * (4.0 * b * t)));
        }
        h[static_cast<size_t>(i)] = static_cast<float>(v);
        energy += v * v;
    }
    const float norm = static_cast<float>(1.0 / std::sqrt(energy));
    for (auto& v : h) v *= norm;
    return h;
}

}  // namespace dect2::kris
