// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi HaLow (802.11ah) OFDM demodulation pieces (halow_demod.h): FFT window, pilot tracking, equalisation, soft demapping, BCC data field decoding.
#include "dect2/halow/halow_demod.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace dect2::halow {

// FFT of one symbol window, scaled 1 / N.
void Demod::fftWin(const cf32* x, cf32* Y) {
    for (int i = 0; i < N; i++) Y[i] = x[i];
    fft_.forward(Y);
    const float g = 1.f / (float)N;
    for (int i = 0; i < N; i++) Y[i] *= g;
}

namespace {
// Common phase a and phase slope b (radians per tone) from the pilots of all copies; returns false without pilots. resid: mean squared error of the pilots after the fit, in Y units.
bool pilotFit(const cf32* Y, int N, const std::vector<Layout>& lays, int polIdx, const cf32* H, double& a, double& b, double& resid) {
    const int pol = pilotPolarity(polIdx);
    std::complex<double> acc(0, 0);
    struct P { int k; std::complex<double> v; cf32 ref; };
    std::vector<P> pr;
    for (const Layout& l : lays)
        for (size_t i = 0; i < l.pilotK.size(); i++) {
            const int k = l.pilotK[i];
            const cf32 ref((float)((i % 4 == 3 ? -1 : 1) * pol), 0.f);
            const cf32 r = Y[bin(k, N)] * std::conj(H[bin(k, N)]) * ref;
            pr.push_back({k, std::complex<double>(r.real(), r.imag()), ref});
            acc += pr.back().v;
        }
    if (pr.empty()) { a = b = resid = 0; return false; }
    a = std::arg(acc);
    double num = 0, den = 0;
    for (auto& p : pr) { const double th = std::arg(p.v * std::polar(1.0, -a)); num += p.k * th; den += (double)p.k * p.k; }
    b = den > 0 ? num / den : 0;
    double e = 0;
    for (auto& p : pr) {
        const cf32 rot = (cf32)std::polar(1.0, -(a + b * p.k));
        const cf32 d = Y[bin(p.k, N)] * rot - H[bin(p.k, N)] * p.ref;
        e += std::norm(d);
    }
    resid = e / (double)pr.size();
    return true;
}
}

// SIG symbol: pilots give common phase and slope, the copies of the layout are added with their channel weights.
bool Demod::sigSym(const cf32* x, const std::vector<Layout>& lays, int polIdx, const cf32* H, std::vector<float>& re, std::vector<float>& im, double& nsum, int& ncnt) {
    if (lays.empty() || !x || !H) return false;
    Y_.resize((size_t)N);
    cf32* Y = Y_.data();
    fftWin(x, Y);
    double a, b, resid;
    pilotFit(Y, N, lays, polIdx, H, a, b, resid);
    re.assign(lays[0].dataK.size(), 0.f);
    im.assign(lays[0].dataK.size(), 0.f);
    for (const Layout& l : lays)
        for (size_t i = 0; i < l.dataK.size(); i++) {
            const int k = l.dataK[i];
            if (!k) continue;
            const cf32 z = Y[bin(k, N)] * std::conj(H[bin(k, N)]) * (cf32)std::polar(1.0, -(a + b * k));
            re[i] += z.real();
            im[i] += z.imag();
            const float h2 = std::norm(H[bin(k, N)]);
            if (h2 > 1e-12f) {
                // noise of the tone in constellation units: the deviation of the tone from the nearer of the two BPSK / QBPSK points
                const float zr = z.real() / h2, zi = z.imag() / h2;
                const float e1 = (std::fabs(zr) - 1.f) * (std::fabs(zr) - 1.f) + zi * zi, e2 = (std::fabs(zi) - 1.f) * (std::fabs(zi) - 1.f) + zr * zr;
                nsum += (double)std::min(e1, e2); ncnt++;
            }
        }
    return true;
}

// Data symbol: equalised tones and the noise variance of each, from the pilots and the training symbols.
bool Demod::dataSym(const cf32* x, const Layout& lay, int polIdx, const cf32* H, float sigma2Ltf, std::vector<cf32>& z, std::vector<float>& nv) {
    if (!x || !H) return false;
    Y_.resize((size_t)N);
    cf32* Y = Y_.data();
    fftWin(x, Y);
    const std::vector<Layout> lays = {lay};
    double a, b, resid;
    pilotFit(Y, N, lays, polIdx, H, a, b, resid);
    const float s2 = std::max(1.5f * sigma2Ltf, (float)resid);   // noise power of one tone in Y: the training symbols' estimate, or the pilots' when worse
    z.assign(lay.dataK.size(), cf32(0, 0));
    nv.assign(lay.dataK.size(), 1e9f);
    for (size_t i = 0; i < lay.dataK.size(); i++) {
        const int k = lay.dataK[i];
        if (!k) continue;
        const float h2 = std::norm(H[bin(k, N)]);
        if (h2 < 1e-14f) continue;
        z[i] = Y[bin(k, N)] * std::conj(H[bin(k, N)]) * (cf32)std::polar(1.0, -(a + b * k)) * (1.f / h2);
        nv[i] = std::max(s2 / h2 * 0.5f, 1e-7f);
    }
    return true;
}

// Demaps the tones of one symbol and deinterleaves them into coded-bit LLRs in transmit order.
void symbolLlr(const std::vector<cf32>& z, const std::vector<float>& nv, const WidthInfo& wi, int mcs, std::vector<float>& coded) {
    const Mcs& m = mcsInfo(mcs);
    const int nb = m.nbpscs, ncbps = wi.nsd * nb;
    std::vector<float> il((size_t)ncbps, 0.f);
    float l[8];
    for (size_t t = 0; t < z.size() && (int)t < wi.nsd; t++) {
        demapQam(z[t], nv[t], nb, l);
        for (int q = 0; q < nb; q++) il[t * (size_t)nb + (size_t)q] = l[q];
    }
    std::vector<float> raw((size_t)ncbps);
    for (int k = 0; k < ncbps; k++) raw[(size_t)k] = il[(size_t)interleaveJ(wi.ncol, wi.nrow * nb, ncbps, nb, k)];
    if (m.rep) for (int i = 0; i < ncbps / 2; i++) coded.push_back(raw[(size_t)i] + raw[(size_t)(i + ncbps / 2)]);
    else coded.insert(coded.end(), raw.begin(), raw.end());
}

// Depuncture, Viterbi (per BCC encoder), merge and descramble the whole data field into the PSDU bytes.
bool decodeDataField(const std::vector<float>& coded, int w, int mcs, int nsym, int len, std::vector<uint8_t>& psdu) {
    const Mcs& m = mcsInfo(mcs);
    const int nd = dataBitsPerSym(w, mcs, 1), nes = encoders(w, mcs, 1);
    if (nd <= 0 || nsym <= 0) return false;
    const size_t ncbpsEff = (size_t)(m.rep ? info(w).nsd / 2 : info(w).nsd * m.nbpscs);
    if (coded.size() < (size_t)nsym * ncbpsEff) return false;
    std::vector<uint8_t> bits;   // scrambled data bits in order
    if (nes == 1) {
        // decode up to the end of the tail (SERVICE + PSDU + 6 zero bits): the pad bits after it are not zero, so the trellis only ends in state 0 there
        const size_t upto = 2 * (8 + 8 * (size_t)len + 6);
        std::vector<float> d = depuncture(std::vector<float>(coded.begin(), coded.begin() + (ptrdiff_t)((size_t)nsym * ncbpsEff)), m.num, m.den, (size_t)nsym * (size_t)nd * 2);
        if (d.size() > upto) d.resize(upto);
        bits = viterbi(d);
    } else {
        const size_t s = (size_t)std::max(1, m.nbpscs / 2);
        std::vector<std::vector<float>> co((size_t)nes);
        for (size_t at = 0, e = 0; at < (size_t)nsym * ncbpsEff; at += s, e = (e + 1) % (size_t)nes)
            for (size_t i = 0; i < s && at + i < coded.size(); i++) co[e].push_back(coded[at + i]);
        std::vector<std::vector<uint8_t>> st((size_t)nes);
        const size_t per = (size_t)nsym * (size_t)nd / (size_t)nes;
        for (int e = 0; e < nes; e++) st[(size_t)e] = viterbi(depuncture(co[(size_t)e], m.num, m.den, per * 2));
        const size_t total = (size_t)nsym * (size_t)nd - 6 * (size_t)nes;
        for (size_t i = 0; i < total; i++) bits.push_back(st[i % (size_t)nes][i / (size_t)nes]);
    }
    const size_t need = 8 + 8 * (size_t)len;
    if (len < 1 || bits.size() < need + 6) return false;
    const std::vector<uint8_t> sc = scramblerFrom(bits.data(), bits.size());
    psdu.assign((size_t)len, 0);
    for (int i = 0; i < len; i++) for (int b = 0; b < 8; b++) psdu[(size_t)i] |= (uint8_t)((bits[8 + 8 * (size_t)i + (size_t)b] ^ sc[8 + 8 * (size_t)i + (size_t)b]) << b);
    return true;
}

} // namespace dect2::halow
