// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cell towers, LTE: transmit diversity, OFDM and channel estimation shared by the test signal and the receiver (cell_lte_int.h).
#include "cell_lte_int.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include "dect2/common/mathconst.h"

namespace dect2::cell {

void lteTxdEncode(const std::vector<cf32>& d0, int P, std::vector<std::vector<cf32>>& y) {
    std::vector<cf32> d = d0;
    while (d.size() % (size_t)P) d.push_back(0);
    y.assign((size_t)P, std::vector<cf32>(d.size(), cf32(0, 0)));
    const float s = dect2::math::k1OverSqrt2f;
    if (P == 1) { y[0] = d; return; }
    if (P == 2) {
        for (size_t i = 0; i + 1 < d.size(); i += 2) {
            const cf32 x0 = d[i], x1 = d[i + 1];
            y[0][i] = s * x0; y[1][i] = -s * std::conj(x1);
            y[0][i + 1] = s * x1; y[1][i + 1] = s * std::conj(x0);
        }
        return;
    }
    for (size_t i = 0; i + 3 < d.size(); i += 4) {
        const cf32 x0 = d[i], x1 = d[i + 1], x2 = d[i + 2], x3 = d[i + 3];
        y[0][i] = s * x0; y[2][i] = -s * std::conj(x1);
        y[0][i + 1] = s * x1; y[2][i + 1] = s * std::conj(x0);
        y[1][i + 2] = s * x2; y[3][i + 2] = -s * std::conj(x3);
        y[1][i + 3] = s * x3; y[3][i + 3] = s * std::conj(x2);
    }
}

static inline void llrOf(cf32 z, float sc, std::vector<float>& o) { o.push_back(sc * z.real()); o.push_back(sc * z.imag()); }

void lteTxdDecode(const cf32* r, const std::vector<const cf32*>& h, size_t n, int P, float nv, std::vector<float>& o) {
    nv = std::max(nv, 1e-9f);
    if (P == 1) {
        const float sc = 2.0f * 1.41421356f / nv;
        for (size_t j = 0; j < n; j++) llrOf(r[j] * std::conj(h[0][j]), sc, o);
        return;
    }
    const float sc = 2.0f / nv;
    auto pair = [&](size_t j, int pa, int pb) {
        const cf32 ha = 0.5f * (h[(size_t)pa][j] + h[(size_t)pa][j + 1]), hb = 0.5f * (h[(size_t)pb][j] + h[(size_t)pb][j + 1]);
        const cf32 r0 = r[j], r1 = r[j + 1];
        llrOf(std::conj(ha) * r0 + hb * std::conj(r1), sc, o);
        llrOf(std::conj(ha) * r1 - hb * std::conj(r0), sc, o);
    };
    if (P == 2) {
        for (size_t j = 0; j + 1 < n; j += 2) pair(j, 0, 1);
        if (n % 2) { o.push_back(0); o.push_back(0); }
        return;
    }
    for (size_t j = 0; j + 3 < n; j += 4) { pair(j, 0, 2); pair(j + 2, 1, 3); }
    for (size_t j = n - n % 4; j < n; j++) { o.push_back(0); o.push_back(0); }
}

void lteOfdmMod(const LteGrid& g, int nRb, bool ext, int nfft, std::vector<cf32>& out) {
    Fft f(nfft);
    std::vector<cf32> b((size_t)nfft);
    for (int l = 0; l < g.nSym; l++) {
        std::fill(b.begin(), b.end(), cf32(0, 0));
        for (int k = 0; k < g.nSc; k++) b[(size_t)lteBin(k, nRb, nfft)] = g.at(l, k);
        f.inverse(b.data());
        const int cp = lteCpLen(l, ext, nfft);
        out.insert(out.end(), b.end() - cp, b.end());
        out.insert(out.end(), b.begin(), b.end());
    }
}

void lteOfdmDemod(const cf32* x, int nRb, bool ext, int nfft, LteGrid& g) {
    const int ns = 2 * lteSymbPerSlot(ext);
    g.init(ns, 12 * nRb);
    thread_local Fft* fp = nullptr;
    thread_local int fn = 0;
    if (fn != nfft) { delete fp; fp = new Fft(nfft); fn = nfft; }
    std::vector<cf32> b((size_t)nfft);
    std::vector<cf32> rot((size_t)nfft);
    for (int l = 0; l < ns; l++) {
        const int cp = lteCpLen(l, ext, nfft), off = cp / 4;
        const int s = lteSymStart(l, ext, nfft) + cp - off;
        for (int i = 0; i < nfft; i++) b[(size_t)i] = x[s + i];
        fp->forward(b.data());
        const float inv = 1.0f / (float)nfft;
        for (int k = 0; k < g.nSc; k++) {
            const int bin = lteBin(k, nRb, nfft);
            const int fb = bin < nfft / 2 ? bin : bin - nfft;
            const double a = 2 * M_PI * (double)fb * off / nfft;   // the window was taken off samples early
            g.at(l, k) = b[(size_t)bin] * inv * cf32((float)std::cos(a), (float)std::sin(a));
        }
    }
}

// linear interpolation over the whole axis from samples at positions pos (ascending), extrapolated at both ends
static void interp(const std::vector<int>& pos, const std::vector<cf32>& val, int n, cf32* out, int stride) {
    const size_t m = pos.size();
    if (m == 0) { for (int i = 0; i < n; i++) out[(size_t)i * (size_t)stride] = 0; return; }
    if (m == 1) { for (int i = 0; i < n; i++) out[(size_t)i * (size_t)stride] = val[0]; return; }
    size_t j = 0;
    for (int i = 0; i < n; i++) {
        while (j + 2 < m && pos[j + 1] <= i) j++;
        const float t = (float)(i - pos[j]) / (float)(pos[j + 1] - pos[j]);
        out[(size_t)i * (size_t)stride] = val[j] + t * (val[j + 1] - val[j]);
    }
}

void lteChanEst(const LteGrid& y, const LteCellCfg& c, int sf, int nRbGrid, int, int ports, LteChan& out, int wide, int lsfLo, int lsfHi) {
    (void)nRbGrid;
    const int N = lteSymbPerSlot(c.extCp), nsym = 2 * N, nsc = 12 * c.nRb;
    out.h.assign((size_t)ports, LteGrid());
    double resid = 0, rsrp = 0, rssi = 0; int nres = 0, nrs = 0, nrssi = 0;
    cf32 r[2 * kMaxRb];
    for (int p = 0; p < ports; p++) {
        LteGrid& H = out.h[(size_t)p];
        H.init(nsym, nsc);
        std::vector<int> syms;
        std::vector<std::vector<cf32>> fr;   // per pilot symbol: the estimate on every subcarrier
        for (int lsf = 0; lsf < nsym; lsf++) {
            const int l = lsf % N, ns = 2 * sf + lsf / N;
            const bool has = (p <= 1) ? (l == 0 || l == N - 3) : (l == 1);
            if (!has || lsf < lsfLo || lsf > lsfHi) continue;
            lteCrsSeq(c.pci, ns, l, c.extCp, r);
            std::vector<int> pos;
            std::vector<cf32> ls;
            for (int k = 0; k < nsc; k++) if (lteCrsAt(c, sf, lsf, k, p)) {
                const int m = k / 6;
                pos.push_back(k);
                ls.push_back(y.at(lsf, k) * std::conj(r[m + kMaxRb - c.nRb]));
            }
            std::vector<cf32> sm(ls.size());
            for (size_t i = 0; i < ls.size(); i++) {
                if (i == 0 || i + 1 == ls.size()) sm[i] = ls[i] * 0.5f + (i == 0 ? ls[std::min(i + 1, ls.size() - 1)] : ls[i - 1]) * 0.5f;
                else sm[i] = 0.25f * ls[i - 1] + 0.5f * ls[i] + 0.25f * ls[i + 1];
                if (p == 0 && i > 0 && i + 1 < ls.size()) { resid += std::norm(ls[i] - sm[i]); nres++; }
                if (p == 0) { rsrp += std::norm(ls[i]); nrs++; }
            }
            if (wide > 1 && ls.size() > 1) {   // the mean of +-wide pilots, the delay's phase slope taken out first
                const int n = (int)ls.size();
                cf32 st(0, 0);
                for (int i = 0; i + 1 < n; i++) st += ls[(size_t)i + 1] * std::conj(ls[(size_t)i]);
                const float ph = std::arg(st) / (float)(pos[1] - pos[0]);   // per subcarrier
                std::vector<cf32> dr((size_t)n);
                for (int i = 0; i < n; i++) dr[(size_t)i] = ls[(size_t)i] * std::polar(1.f, -ph * (float)pos[(size_t)i]);
                for (int i = 0; i < n; i++) {
                    cf32 a(0, 0);
                    const int lo = std::max(0, i - wide), hi = std::min(n - 1, i + wide);
                    for (int j = lo; j <= hi; j++) a += dr[(size_t)j];
                    sm[(size_t)i] = a / (float)(hi - lo + 1) * std::polar(1.f, ph * (float)pos[(size_t)i]);
                }
            }
            if (p == 0) { double e = 0; for (int k = 0; k < nsc; k++) e += std::norm(y.at(lsf, k)); rssi += e; nrssi++; }
            std::vector<cf32> full((size_t)nsc);
            interp(pos, sm, nsc, full.data(), 1);
            syms.push_back(lsf);
            fr.push_back(std::move(full));
        }
        if (wide > 1 && fr.size() > 1) {   // the mean over the subframe, the carrier offset's phase drift taken out first
            double sw = 0, sp = 0;
            for (size_t i = 0; i + 1 < fr.size(); i++) {
                cf32 a(0, 0);
                for (int k = 0; k < nsc; k++) a += fr[i + 1][(size_t)k] * std::conj(fr[i][(size_t)k]);
                sw += std::abs(a); sp += std::abs(a) * std::arg(a) / (syms[i + 1] - syms[i]);
            }
            const float ps = sw > 0 ? (float)(sp / sw) : 0.f;   // per symbol
            for (int k = 0; k < nsc; k++) {
                cf32 a(0, 0);
                for (size_t i = 0; i < fr.size(); i++) a += fr[i][(size_t)k] * std::polar(1.f, -ps * (float)syms[i]);
                a /= (float)fr.size();
                for (size_t i = 0; i < fr.size(); i++) fr[i][(size_t)k] = a * std::polar(1.f, ps * (float)syms[i]);
            }
        }
        // time
        std::vector<cf32> col(syms.size());
        std::vector<cf32> o((size_t)nsym);
        for (int k = 0; k < nsc; k++) {
            for (size_t i = 0; i < syms.size(); i++) col[i] = fr[i][(size_t)k];
            interp(syms, col, nsym, o.data(), 1);
            for (int l = 0; l < nsym; l++) H.at(l, k) = o[(size_t)l];
        }
    }
    out.noiseVar = nres ? (float)(resid / nres / 0.375) : 1e-3f;
    out.rsrp = nrs ? (float)std::max(std::max(1e-12, 0.05 * rsrp / nrs), rsrp / nrs - out.noiseVar) : 0.f;   // a weak cell under others: noise estimate above it; floor at -13 dB of the raw power
    out.rssi = nrssi ? (float)(rssi / nrssi) : 0.f;
}

} // namespace dect2::cell
