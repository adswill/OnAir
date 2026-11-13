// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// General receiver, AM: the synchronous detector (both sidebands, upper, lower) against the envelope detector, and the noise blanker.
//   selective fading (two paths 0.1 - 0.3 ms apart, the notch sweeping through the carrier): harmonic distortion of a 1 kHz tone
//   a station 5 kHz above, 10 dB stronger: the lower sideband detector leaves its sideband out
//   impulse noise (60 us every 7 ms, 25 dB above the carrier): the blanker; and that neither costs anything on a clean or noisy station
#include "dect2/general/general_rx.h"
#include "dect2/general/general_tel.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include "dect2/common/mathconst.h"
using namespace dect2;
#include "../common/test_util.h"

using dect2::math::kPi;
constexpr double kRate = 250000;

struct Sig {
    double cnrDb = 40;           // carrier to noise in 9 kHz
    double depth = 0.5;          // the 1 kHz tone
    double prog = 0.3;           // a 400 Hz tone besides (programme), 0 = none
    double offsetHz = 0;         // the station off the dial
    double echoMs = 0;           // second path, 0.95 of the first, drifting 0.3 Hz against it
    double adjKhz = 0;           // a station this far above, 10 dB stronger, its 2.3 kHz tone keyed at 3 Hz
    double impDb = -100;         // impulses
};

static std::vector<cf32> make(const Sig& s, double secs) {
    const size_t n = (size_t)(secs * kRate);
    std::vector<cf32> x(n);
    std::mt19937 rng(11);
    std::normal_distribution<float> nd(0, 1);
    const double amp = 0.2, nsd = amp * std::sqrt(1.0 / std::pow(10, s.cnrDb / 10) * kRate / 9000 / 2);
    const int d = (int)std::lround(s.echoMs * 1e-3 * kRate);
    auto am = [&](double t) { return std::polar((float)(amp * (1 + s.depth * std::sin(2 * kPi * 1000 * t) + s.prog * std::sin(2 * kPi * 400 * t + 1))), (float)(2 * kPi * s.offsetHz * t)); };
    for (size_t i = 0; i < n; i++) {
        const double t = i / kRate;
        cf32 v = am(t);
        if (d > 0 && i >= (size_t)d) v += am((double)(i - d) / kRate) * std::polar(0.95f, (float)(2 * kPi * 0.3 * t));
        if (s.adjKhz > 0) v += std::polar((float)(amp * 3.16 * (1 + 0.7 * std::sin(2 * kPi * 2300 * t) * std::sin(2 * kPi * 3 * t))), (float)(2 * kPi * s.adjKhz * 1e3 * t));
        v += cf32((float)(nsd * nd(rng)), (float)(nsd * nd(rng)));
        x[i] = v;
    }
    if (s.impDb > -50) {
        const float a = (float)(amp * std::pow(10, s.impDb / 20));
        for (size_t i = 0; i < n; i += (size_t)(0.007 * kRate)) for (size_t k = 0; k < (size_t)(60e-6 * kRate) && i + k < n; k++) x[i + k] += std::polar(a, (float)(rng() % 628) / 100.f);
    }
    return x;
}

static std::vector<float> demod(const std::vector<cf32>& x, int det, bool nb = false, bool autoQ = false, std::string* status = nullptr) {
    GeneralReceiver r;
    r.configure(kRate);
    r.setSignalOffset(0);
    r.setSilent(true);
    r.setDemod(GenAm);
    r.setWidthHz(9000);
    r.setSquelch(false, false, -100);
    r.setAmDetector(det);
    r.setNoiseBlanker(nb, 12);
    r.setAmAuto(autoQ);
    std::vector<float> a;
    r.setAudioTap([&](const float* p, size_t n) { a.insert(a.end(), p, p + n); });
    uint64_t seq = 0;
    for (size_t i = 0; i < x.size(); i += 25000) {
        r.feed(x.data() + i, std::min<size_t>(25000, x.size() - i));
        GeneralTelemetry t;
        if (r.telemetry(t, seq)) { seq = t.seq; if (status) *status = t.amStatus; }
    }
    return std::vector<float>(a.begin() + std::min(a.size(), (size_t)(2 * kGeneralAudioRate)), a.end());   // after 2 s
}

// power of the tone at f (least squares over the whole record), and the total power with the mean taken out
static double tonePow(const std::vector<float>& a, double f) {
    double c = 0, s = 0;
    for (size_t i = 0; i < a.size(); i++) { const double w = 2 * kPi * f * (double)i / kGeneralAudioRate; c += a[i] * std::cos(w); s += a[i] * std::sin(w); }
    c *= 2.0 / a.size(); s *= 2.0 / a.size();
    return 0.5 * (c * c + s * s);
}
static double acPow(const std::vector<float>& a) {
    double m = 0, p = 0;
    for (float v : a) m += v;
    m /= std::max<size_t>(1, a.size());
    for (float v : a) p += (v - m) * (v - m);
    return p / std::max<size_t>(1, a.size());
}
// SINAD of the 1 kHz tone (the 400 Hz programme tone is not counted as noise)
static double sinad(const std::vector<float>& a) {
    const double p1 = tonePow(a, 1000), p4 = tonePow(a, 400);
    return 10 * std::log10(p1 / std::max(1e-15, acPow(a) - p1 - p4));
}
static double thd(const std::vector<float>& a) { return 10 * std::log10(tonePow(a, 1000) / std::max(1e-15, tonePow(a, 2000) + tonePow(a, 3000))); }

static const char* const kDet[4] = {"envelope", "sync", "sync upper", "sync lower"};

int main() {
    {   // clean and noisy: the synchronous detectors give the same level as the envelope one and no worse noise (both sidebands)
        for (double cnr : {40.0, 20.0, 8.0}) {
            Sig s; s.cnrDb = cnr;
            const auto x = make(s, 8);
            double v[4];
            for (int d = 0; d < 4; d++) {
                const auto a = demod(x, d);
                v[d] = sinad(a);
                if (cnr == 40.0) {
                    const double amp = std::sqrt(2 * tonePow(a, 1000));
                    CHECK(std::fabs(amp - 0.25) < 0.03, "%s: tone level %.3f (wanted 0.25)", kDet[d], amp);
                }
            }
            printf("AWGN %4.0f dB   SINAD envelope %5.1f  sync %5.1f  upper %5.1f  lower %5.1f\n", cnr, v[0], v[1], v[2], v[3]);
            CHECK(v[1] > v[0] - 1.0, "%.0f dB: sync %.1f dB against envelope %.1f dB", cnr, v[1], v[0]);
        }
    }
    {   // a station 150 Hz off the dial: the PLL pulls in
        Sig s; s.offsetHz = 150; s.cnrDb = 30;
        const double e = sinad(demod(make(s, 8), 0)), y = sinad(demod(make(s, 8), 1));
        printf("150 Hz off      SINAD envelope %5.1f  sync %5.1f\n", e, y);
        CHECK(y > e - 1.0, "150 Hz off: sync %.1f dB against envelope %.1f dB", y, e);
    }
    for (double ms : {0.1, 0.2, 0.3}) {   // selective fading: distortion
        Sig s; s.echoMs = ms; s.depth = 0.8; s.prog = 0; s.cnrDb = 35;
        const auto x = make(s, 12);
        double v[4];
        for (int d = 0; d < 4; d++) v[d] = thd(demod(x, d));
        printf("fading %.1f ms   1 kHz over its harmonics: envelope %5.1f  sync %5.1f  upper %5.1f  lower %5.1f dB\n", ms, v[0], v[1], v[2], v[3]);
        CHECK(v[1] > v[0] + 15 && v[2] > v[0] + 15 && v[3] > v[0] + 15, "fading %.1f ms: synchronous not clearly cleaner (%.1f %.1f %.1f against %.1f)", ms, v[1], v[2], v[3], v[0]);
    }
    {   // adjacent station 5 kHz above
        Sig s; s.adjKhz = 5; s.cnrDb = 35;
        const auto x = make(s, 8);
        double v[4];
        for (int d = 0; d < 4; d++) v[d] = sinad(demod(x, d));
        printf("+5 kHz station  SINAD envelope %5.1f  sync %5.1f  upper %5.1f  lower %5.1f\n", v[0], v[1], v[2], v[3]);
        CHECK(v[3] > v[0] + 15, "adjacent: lower sideband %.1f dB against envelope %.1f dB", v[3], v[0]);
    }
    {   // impulse noise and the blanker
        Sig s; s.impDb = 25; s.cnrDb = 35;
        const auto x = make(s, 8);
        const double off = sinad(demod(x, 0)), on = sinad(demod(x, 0, true)), onSync = sinad(demod(x, 1, true));
        Sig c; c.cnrDb = 20;
        const auto y = make(c, 8);
        const double cleanOff = sinad(demod(y, 0)), cleanOn = sinad(demod(y, 0, true));
        printf("impulses +25 dB SINAD blanker off %5.1f  on %5.1f (sync %5.1f) | no impulses, 20 dB: off %5.1f on %5.1f\n", off, on, onSync, cleanOff, cleanOn);
        CHECK(on > off + 10, "blanker: %.1f dB against %.1f dB without", on, off);
        CHECK(cleanOn > cleanOff - 0.5, "blanker on a clean station: %.1f dB against %.1f dB", cleanOn, cleanOff);
    }
    {   // Auto quality against the envelope detector: noise, a whistle, impulses (the 1 kHz tone with the 400 Hz programme tone)
        struct A { const char* name; Sig s; double minGain; };
        Sig awgn; awgn.cnrDb = 10;
        Sig wh; wh.cnrDb = 30; wh.adjKhz = 0;
        Sig imp; imp.cnrDb = 30; imp.impDb = 25;
        const A cases[] = {{"AWGN 10 dB", awgn, 2.0}, {"impulses +25 dB", imp, 10.0}};
        for (const A& c : cases) {
            const auto x = make(c.s, 10);
            std::string st;
            const double e = sinad(demod(x, 0)), a = sinad(demod(x, 0, false, true, &st));
            printf("Auto quality, %-16s SINAD envelope %5.1f  auto %5.1f  (%s)\n", c.name, e, a, st.c_str());
            CHECK(a > e + c.minGain, "Auto quality %s: %.1f dB against %.1f dB", c.name, a, e);
        }
        {   // clean: Auto must not spoil it
            Sig s; s.cnrDb = 40;
            const auto x = make(s, 8);
            const double e = sinad(demod(x, 0)), a = sinad(demod(x, 0, false, true));
            printf("Auto quality, clean 40 dB  SINAD envelope %5.1f  auto %5.1f\n", e, a);
            CHECK(a > e - 1.5, "Auto quality clean: %.1f dB against %.1f dB", a, e);
        }
    }
    return fails ? 1 : 0;
}
