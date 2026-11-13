// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared by the KAAL frame version 4 tests: a signal maker (FrameTx with random voice payloads), a channel (noise by Es/N0, carrier offset,
// sample-clock offset, fractional delay, flat Rayleigh fading with Doppler, two-ray multipath, phase noise) and a slot scorer.
#pragma once
#include "dect2/kaal/kaal_v4.h"
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace kv4 {
using namespace dect2::kaal::v4;
using cf = std::complex<float>;
constexpr double kPi = 3.14159265358979323846;

struct Chan {
    double esn0Db = 99;        // per symbol (16 ksym/s), noise over the whole 48 kHz band
    double cfoHz = 0;
    double ppm = 0;            // receiver sample clock offset
    double delay = 0;          // extra fractional delay in samples (plus lead samples of noise)
    int lead = 0;              // noise-only samples before the signal
    double dopplerHz = 0;      // > 0: flat Rayleigh fading (sum of sinusoids), mean power 1
    bool rician = false;       // with a line-of-sight component (K = 6 dB)
    double echoDb = -99;       // second ray level (dB), 0 = equal
    double echoSamples = 0;    // second ray delay in samples (fractional ok)
    double phaseNoise = 0;     // random-walk phase, rad rms per sample
    uint32_t seed = 1;
};

// sinc interpolation of x at fractional position t (Blackman window +-8)
inline cf interpAt(const std::vector<cf>& x, double t) {
    const int64_t i0 = (int64_t)std::floor(t);
    cf acc(0, 0);
    for (int k = -7; k <= 8; k++) {
        const int64_t j = i0 + k;
        if (j < 0 || j >= (int64_t)x.size()) continue;
        const double d = t - (double)j;
        const double s = std::fabs(d) < 1e-12 ? 1.0 : std::sin(kPi * d) / (kPi * d);
        const double w = 0.42 + 0.5 * std::cos(kPi * d / 8.5) + 0.08 * std::cos(2 * kPi * d / 8.5);
        acc += x[(size_t)j] * (float)(s * w);
    }
    return acc;
}

// signal power per sample measured over the whole signal
inline double powerOf(const std::vector<cf>& x) { double p = 0; for (auto& v : x) p += std::norm(v); return p / std::max<size_t>(1, x.size()); }

inline std::vector<cf> channel(const std::vector<cf>& in, const Chan& c) {
    std::mt19937 rng(c.seed);
    std::normal_distribution<double> nd(0, 1);
    const double ps = powerOf(in);
    // multipath
    std::vector<cf> x = in;
    if (c.echoDb > -90) {
        const float a = (float)std::pow(10.0, c.echoDb / 20.0);
        const cf ph = std::polar(a, 1.1f);
        for (size_t i = 0; i < x.size(); i++) x[i] += ph * interpAt(in, (double)i - c.echoSamples);
        const double p2 = powerOf(x);
        for (auto& v : x) v *= (float)std::sqrt(ps / p2);
    }
    // fading
    if (c.dopplerHz > 0) {
        const int N = 16;
        std::uniform_real_distribution<double> ud(0, 2 * kPi);
        std::vector<double> th(N), ph(N), ph2(N);
        for (int k = 0; k < N; k++) { th[k] = ud(rng); ph[k] = ud(rng); ph2[k] = ud(rng); }
        const double K = c.rician ? std::pow(10.0, 0.6) : 0.0;
        const double sLos = std::sqrt(K / (K + 1)), sSc = std::sqrt(1 / (K + 1));
        for (size_t i = 0; i < x.size(); i++) {
            const double t = (double)i / kFs;
            double re = 0, im = 0;
            for (int k = 0; k < N; k++) {
                const double w = 2 * kPi * c.dopplerHz * std::cos(th[k]);
                re += std::cos(w * t + ph[k]); im += std::sin(w * t + ph2[k]);
            }
            const cf g((float)(sSc * re / std::sqrt((double)N) + sLos), (float)(sSc * im / std::sqrt((double)N)));
            x[i] *= g;
        }
    }
    // carrier offset, phase noise
    double phn = 0;
    for (size_t i = 0; i < x.size(); i++) {
        phn += c.phaseNoise * nd(rng);
        x[i] *= std::polar(1.f, (float)(2 * kPi * c.cfoHz * (double)i / kFs + phn));
    }
    // receiver clock offset and delay: y[m] = x(m * (1 + ppm) - delay)
    std::vector<cf> y;
    if (c.ppm != 0 || c.delay != 0) {
        const double r = 1.0 + c.ppm * 1e-6;
        const size_t n = (size_t)((double)x.size() / r);
        y.resize(n);
        for (size_t m = 0; m < n; m++) y[m] = interpAt(x, (double)m * r - c.delay);
    } else y = x;
    // lead noise samples + AWGN at the Es/N0
    std::vector<cf> out(c.lead, cf(0, 0));
    out.insert(out.end(), y.begin(), y.end());
    if (c.esn0Db < 90) {
        const double nvar = ps * kSps / std::pow(10.0, c.esn0Db / 10.0);
        const double s = std::sqrt(nvar / 2);
        for (auto& v : out) v += cf((float)(s * nd(rng)), (float)(s * nd(rng)));
    }
    return out;
}

// transmitter with random voice payloads: profiles per slot (kIdle = data), nSf superframes
struct Sent {
    std::vector<std::array<std::array<uint8_t, 40>, kSlots>> pay;   // per superframe, per slot (voice only)
    std::vector<std::array<Profile, kSlots>> prof;
};
inline std::vector<cf> makeSignal(const Profile prof[kSlots], int nSf, Sent& sent, uint32_t seed, const std::string& text = "",
                                  bool beacon = false, bool robustData = false, int textEverySf = 0) {
    FrameTx tx;
    tx.configure(kFs);
    tx.setDataRobust(robustData);
    for (int s = 0; s < kSlots; s++) tx.setSlotProfile(s, prof[s]);
    if (beacon) {
        dect2::kaal::BeaconFields f;
        f.callsign = "LY2ABC"; f.grid = "KO24pq"; f.lat = 54.68; f.lon = 25.28; f.powerDbm = 37;
        tx.setBeacon(f);
    }
    if (!text.empty()) tx.queueText(text);
    std::mt19937 rng(seed);
    std::vector<cf> out;
    for (int n = 0; n < nSf; n++) {
        std::array<std::array<uint8_t, 40>, kSlots> p{};
        for (int s = 0; s < kSlots; s++) {
            if (!profileVoice(prof[s])) continue;
            for (auto& b : p[s]) b = (uint8_t)rng();
            const int nb = payloadBits(prof[s]);
            for (int i = nb; i < 320; i++) p[s][i >> 3] &= (uint8_t)~(0x80 >> (i & 7));
            tx.pushVoice(s, p[s].data());
        }
        if (textEverySf > 0 && n % textEverySf == 0 && !text.empty()) tx.queueText(text);
        std::array<Profile, kSlots> pr;
        for (int s = 0; s < kSlots; s++) pr[s] = prof[s];
        sent.pay.push_back(p);
        sent.prof.push_back(pr);
        tx.superframe(out);
    }
    return out;
}

// Runs the receiver and matches the decoded voice payloads to the sent ones by content (superframe index unknown to the receiver).
struct Score {
    int sent[kSlots] = {}, good[kSlots] = {}, wrong[kSlots] = {};
    double lockS = -1;
    std::vector<std::string> texts;
    int beacons = 0;
    float esn0 = 0;
    double cfo = 0;
    double ppm = 0;
    int saves = 0;
};
inline Score runRx(const std::vector<cf>& sig, const Sent& sent, int settleSf = 0, bool iterative = true, bool dd = true,
                   double inRate = kFs, bool eq = true) {
    FrameRx rx;
    rx.configure(inRate);
    rx.setIterative(iterative);
    rx.setDdTracking(dd);
    rx.setEqualiser(eq);
    // feed in uneven chunks
    size_t pos = 0, ch = 777;
    std::vector<RxSuperframe> got;
    while (pos < sig.size()) {
        const size_t n = std::min(ch, sig.size() - pos);
        rx.feed(sig.data() + pos, n);
        pos += n;
        ch = ch == 777 ? 4096 : ch == 4096 ? 333 : 777;
        RxSuperframe r;
        while (rx.popSuperframe(r)) got.push_back(r);
    }
    Score sc;
    std::map<std::string, int> index[kSlots];   // payload -> count sent
    // the last superframe cannot be decoded (its end is the next superframe's chirp); superframes before settleSf are matched but not counted
    for (size_t n = 0; n + 1 < sent.pay.size(); n++)
        for (int s = 0; s < kSlots; s++) {
            if (!profileVoice(sent.prof[n][s])) continue;
            index[s][std::string((const char*)sent.pay[n][s].data(), 40)]++;
            if (n >= (size_t)settleSf) sc.sent[s]++;
        }
    for (auto& r : got)
        for (int s = 0; s < kSlots; s++) {
            if (!r.slot[s].ok || !profileVoice(r.slot[s].profile)) continue;
            auto it = index[s].find(std::string((const char*)r.slot[s].bits, 40));
            if (it != index[s].end() && it->second > 0) { it->second--; sc.good[s]++; }
            else sc.wrong[s]++;
        }
    std::string t;
    while (rx.pollText(t)) sc.texts.push_back(t);
    dect2::kaal::BeaconFields f;
    while (rx.pollBeacon(f)) sc.beacons++;
    const RxStatsV4 st = rx.stats();
    sc.lockS = st.lockTimeS;
    sc.esn0 = st.esn0Db;
    sc.cfo = st.cfoHz;
    sc.ppm = st.clockPpm;
    sc.saves = st.iterativeSaves;
    for (int s = 0; s < kSlots; s++) sc.good[s] = std::min(sc.good[s], sc.sent[s]);
    return sc;
}

}  // namespace kv4
