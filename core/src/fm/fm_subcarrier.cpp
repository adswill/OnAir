// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/fm/fm_subcarrier.h"
#include "dect2/fm/fm_rds.h"
#include "dect2/dsp/resampler.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <mutex>

namespace dect2 {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kScaDev = 6000;   // nominal SCA deviation, Hz
using cd = std::complex<float>;

struct CFir {
    std::vector<float> h; std::vector<cd> b; size_t p = 0;
    void design(double cut, int n) {
        h.resize((size_t)n); double s = 0;
        for (int i = 0; i < n; i++) {
            double t = i - (n - 1) / 2.0, v = t == 0 ? 2 * cut : std::sin(2 * kPi * cut * t) / (kPi * t);
            v *= 0.42 - 0.5 * std::cos(2 * kPi * i / (n - 1)) + 0.08 * std::cos(4 * kPi * i / (n - 1));
            h[(size_t)i] = (float)v; s += v;
        }
        for (auto& v : h) v = (float)(v / s);
        b.assign((size_t)n, 0); p = 0;
    }
    void push(cd v) { b[p] = v; p = (p + 1) % b.size(); }
    cd out() const { cd a = 0; size_t k = p; for (float t : h) { a += t * b[k]; k = (k + 1) % b.size(); } return a; }
    void reset() { std::fill(b.begin(), b.end(), cd(0)); p = 0; }
};

struct Rds2Chan {
    double f = 0, ph = 0, w = 0;
    CFir lp; int dec = 1, cnt = 0;
    RationalResampler rs; bool rsOk = false;
    std::vector<cf32> in, out;
    RdsDecoder rds;
    double costasPh = 0, costasFr = 0;
    cf32 mf[16]; int mfPos = 0, idx = 0, since = 0, best = 0;
    float energy[16] = {}, symAvg = 0.f;
    int prev = 0;
    std::vector<std::string> last;
    void configure(double fm, double fc) {
        f = fc; w = -2 * kPi * fc / fm;
        dec = std::max(1, (int)std::floor(fm / 38000.0));
        lp.design(3200.0 / fm, 4 * dec * 8 + 1);
        rsOk = rs.configure(fm / dec, 19000);
        rds.setGroupTap([this](const uint16_t b[4], const uint8_t e[4]) {
            last.push_back(rdsHexGroup(b, e));
            if (last.size() > 8) last.erase(last.begin());
        });
        reset();
    }
    void reset() {
        ph = 0; cnt = 0; lp.reset(); if (rsOk) rs.reset(); in.clear(); out.clear(); rds.reset(); costasPh = costasFr = 0; mfPos = idx = since = best = 0; std::fill(energy, energy + 16, 0.f); symAvg = 0; prev = 0; for (auto& m : mf) m = 0; last.clear();
    }
    void sample(cf32 z) {
        // Costas loop, updated at the symbol rate: wide until blocks decode (a few Hz of clock error at 76 kHz)
        const cf32 r = z * cf32((float)std::cos(-costasPh), (float)std::sin(-costasPh));
        costasPh += costasFr;
        if (costasPh > kPi) costasPh -= 2 * kPi; else if (costasPh < -kPi) costasPh += 2 * kPi;
        mf[mfPos] = r; mfPos = (mfPos + 1) & 15;
        static float tpl[16]; static bool init = false;
        if (!init) { for (int k = 0; k < 16; k++) tpl[k] = (float)std::sin(2 * kPi * (k + 0.5) / 16); init = true; }
        cf32 y(0, 0);
        for (int k = 0; k < 16; k++) y += mf[(mfPos + k) & 15] * tpl[k];
        const int phN = idx & 15;
        energy[phN] += (std::norm(y) - energy[phN]) * 0.01f;
        int b = 0;
        for (int k = 1; k < 16; k++) if (energy[k] > energy[b]) b = k;
        if (energy[b] > 1.15f * energy[best]) best = b;
        idx++; since++;
        if (since < 8 || ((phN - best + 16) & 15) >= 3) return;
        since = 0;
        symAvg += (std::fabs(y.real()) - symAvg) * 0.02f;
        const int s = y.real() >= 0 ? 1 : 0;
        const double e = std::max(-1.0, std::min(1.0, (double)(s ? y.imag() : -y.imag()) / std::max(symAvg, 1e-9f)));
        const double bn = rds.synced ? 5.0 : 20.0, wn = 2 * bn / (0.707 + 1 / (4 * 0.707)), wT = wn / 1187.5;
        costasPh += 2 * 0.707 * wT * e;
        costasFr += wT * wT * e / 16;
        costasFr = std::max(-2 * kPi * 25 / 19000.0, std::min(2 * kPi * 25 / 19000.0, costasFr));
        rds.pushBit(s ^ prev, std::fabs(y.real()));
        prev = s;
    }
    void feed(const float* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            const cd m = cd(x[i] * (float)std::cos(ph), x[i] * (float)std::sin(ph));
            ph = std::fmod(ph + w, 2 * kPi);
            lp.push(m);
            if (++cnt >= dec) { cnt = 0; in.push_back(cf32(lp.out())); }
        }
        if (rsOk && !in.empty()) { out.clear(); rs.process(in.data(), in.size(), out); in.clear(); for (const cf32& z : out) sample(z); }
    }
};

struct ScaChan {
    double f = 0, ph = 0, w = 0, fm = 0;
    CFir lp; int dec = 1, cnt = 0;
    cd prev{1, 0};
    double powBand = 0, powRef = 0;
    double presT = 0; bool present = false; float snr = 0;
    double deem = 0, deemA = 0, outPos = 0, outStep = 1; float o1 = 0;
    std::vector<float> audio;
    void configure(double rate, double fc) {
        fm = rate; f = fc; w = -2 * kPi * fc / rate;
        dec = std::max(1, (int)std::round(rate / 48000.0));
        lp.design(9000.0 / rate, dec * 16 + 1);
        const double fa = rate / dec;
        deemA = 1.0 - std::exp(-1.0 / (150e-6 * fa));
        outStep = fa / 48000.0;
        reset();
    }
    void reset() { ph = 0; cnt = 0; lp.reset(); prev = {1, 0}; powBand = powRef = 0; presT = 0; present = false; snr = 0; deem = 0; outPos = 0; o1 = 0; audio.clear(); }
    void feed(const float* x, size_t n, double refPow, bool listen) {
        const double fa = fm / dec;
        for (size_t i = 0; i < n; i++) {
            const cd m = cd(x[i] * (float)std::cos(ph), x[i] * (float)std::sin(ph));
            ph = std::fmod(ph + w, 2 * kPi);
            lp.push(m);
            if (++cnt < dec) continue;
            cnt = 0;
            const cd v = lp.out();
            powBand += 0.003 * (std::norm(v) - powBand);
            const float d = std::arg(v * std::conj(prev)) / (2 * (float)kPi) * (float)fa / (float)kScaDev; prev = v;
            deem += deemA * (d - deem);
            if (listen) {
                outPos += 1.0;
                while (outPos >= outStep) { outPos -= outStep; const float fr = (float)(1.0 - outPos); audio.push_back(o1 + fr * ((float)deem - o1)); }
                o1 = (float)deem;
            }
        }
        const double sn = 10 * std::log10(std::max(1e-12, powBand) / std::max(1e-12, refPow));
        snr = (float)sn;
        presT = std::clamp(presT + (sn > 12 ? 1 : -1) * (double)n / fm, 0.0, 1.0);
        if (presT > 0.5) present = true; else if (presT < 0.1) present = false;
    }
};
}   // namespace

struct FmSubcarriers::Impl {
    double fm = 0;
    Rds2Chan r[3]; ScaChan s[2];
    CFir ref; double refMix = 0, refW = 0, refPow = 1e-9;
    int refDec = 1, refCnt = 0;
    int listen = -1;
    mutable std::mutex mu;
};

FmSubcarriers::FmSubcarriers() : p_(new Impl) {}
FmSubcarriers::~FmSubcarriers() = default;
void FmSubcarriers::configure(double fm) {
    Impl& I = *p_; I.fm = fm;
    static const double rf[3] = {66500, 71250, 76000}; static const double sf[2] = {67000, 92000};
    for (int i = 0; i < 3; i++) I.r[i].configure(fm, rf[i]);
    for (int i = 0; i < 2; i++) I.s[i].configure(fm, sf[i]);
    // the noise reference: the band above everything a station sends (the discriminator noise rises with f^2, so it gives a floor to compare with)
    I.refDec = I.s[0].dec; I.ref.design(9000.0 / fm, I.refDec * 16 + 1); I.refW = -2 * kPi * 108000.0 / fm;
}
void FmSubcarriers::reset() { Impl& I = *p_; for (auto& c : I.r) c.reset(); for (auto& c : I.s) c.reset(); I.ref.reset(); I.refMix = 0; I.refCnt = 0; I.refPow = 1e-9; }
void FmSubcarriers::feed(const float* x, size_t n) {
    Impl& I = *p_;
    if (I.fm <= 0) return;
    for (size_t i = 0; i < n; i++) {
        const cd m = cd(x[i] * (float)std::cos(I.refMix), x[i] * (float)std::sin(I.refMix));
        I.refMix = std::fmod(I.refMix + I.refW, 2 * kPi);
        I.ref.push(m);
        if (++I.refCnt >= I.refDec) { I.refCnt = 0; I.refPow += 0.003 * (std::norm(I.ref.out()) - I.refPow); }
    }
    for (auto& c : I.r) c.feed(x, n);
    std::lock_guard<std::mutex> lk(I.mu);
    for (int i = 0; i < 2; i++) I.s[i].feed(x, n, I.refPow, I.listen == i);
}
FmSubcarrierStatus FmSubcarriers::status() const {
    const Impl& I = *p_;
    FmSubcarrierStatus st;
    for (int i = 0; i < 3; i++) { const Rds2Chan& c = I.r[i]; st.rds2[i].carrierHz = c.f; st.rds2[i].synced = c.rds.synced; st.rds2[i].okPct = c.rds.okPct; st.rds2[i].groups = c.rds.groups; st.rds2[i].lastGroups = c.last; }
    std::lock_guard<std::mutex> lk(I.mu);
    for (int i = 0; i < 2; i++) { const ScaChan& c = I.s[i]; st.sca[i].carrierHz = c.f; st.sca[i].present = c.present; st.sca[i].snrDb = c.snr; }
    return st;
}
void FmSubcarriers::setScaListen(int ch) { std::lock_guard<std::mutex> lk(p_->mu); p_->listen = ch; }
int FmSubcarriers::scaListen() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->listen; }
void FmSubcarriers::takeScaAudio(std::vector<float>& out) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->listen >= 0 && p_->listen < 2) { auto& a = p_->s[p_->listen].audio; out.insert(out.end(), a.begin(), a.end()); a.clear(); }
}

} // namespace dect2
