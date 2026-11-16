// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Manual notches (dsp/notch_bank.h): a strong carrier at +7 kHz drops by 40 dB or more while a weak wanted signal at -3 kHz changes by less than 0.5 dB;
// the RF-fixed kind keeps cancelling the same carrier after a retune, the follow kind keeps its offset; width limits, the stored text, an empty or
// disabled list leaves the samples bit for bit.
#include "dect2/dsp/notch_bank.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
using namespace dect2;
using cf32 = std::complex<float>;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static const double kRate = 1e6;
struct Gen {
    std::mt19937 rng{11};
    std::normal_distribution<float> nd{0.f, 0.0005f};
    double ph[2] = {0, 0};
    // a carrier of amplitude ca at ch Hz and a wanted tone of amplitude wa at wh Hz, plus a little noise
    void block(std::vector<cf32>& x, size_t n, double ch, double ca, double wh, double wa) {
        x.resize(n);
        const double w0 = 2 * M_PI * ch / kRate, w1 = 2 * M_PI * wh / kRate;
        for (size_t i = 0; i < n; i++) {
            const double a = ph[0] + w0 * (double)i, b = ph[1] + w1 * (double)i;
            x[i] = cf32(nd(rng) + (float)(ca * std::cos(a) + wa * std::cos(b)), nd(rng) + (float)(ca * std::sin(a) + wa * std::sin(b)));
        }
        ph[0] = std::fmod(ph[0] + w0 * (double)n, 2 * M_PI); ph[1] = std::fmod(ph[1] + w1 * (double)n, 2 * M_PI);
    }
};
static double toneDb(const std::vector<cf32>& x, size_t from, double hz) {
    std::complex<double> s = 0;
    const double w = 2 * M_PI * hz / kRate;
    const size_t N = x.size() - from;
    double wsum = 0;
    for (size_t i = from; i < x.size(); i++) {   // a Hann window: a strong carrier 10 kHz away must not leak into the weak tone's bin
        const double h = 0.5 - 0.5 * std::cos(2 * M_PI * ((double)(i - from) + 0.5) / (double)N);
        s += h * std::complex<double>(x[i].real(), x[i].imag()) * std::polar(1.0, -w * (double)i); wsum += h;
    }
    const double m = std::abs(s) / wsum;
    return 20 * std::log10(m + 1e-30);
}
// run `sec` seconds through the bank in 4096-sample blocks; returns the output
static std::vector<cf32> run(NotchBank* nb, Gen& g, double sec, double centre, double ch, double ca, double wh, double wa) {
    std::vector<cf32> out, blk;
    const size_t total = (size_t)(sec * kRate);
    for (size_t done = 0; done < total; done += 4096) {
        g.block(blk, 4096, ch, ca, wh, wa);
        if (nb) nb->process(blk.data(), blk.size(), kRate, centre);
        out.insert(out.end(), blk.begin(), blk.end());
    }
    return out;
}

int main() {
    const double centre = 404.0e6;
    {   // the owner's case: carrier +7 kHz (strong), wanted -3 kHz (weak); RF-fixed notch at 404.007 MHz
        Gen g1, g2;
        auto ref = run(nullptr, g1, 0.5, centre, 7e3, 1.0, -3e3, 0.01);
        NotchBank nb; Notch n; n.rfHz = centre + 7e3; nb.set({n});
        auto out = run(&nb, g2, 0.5, centre, 7e3, 1.0, -3e3, 0.01);
        const size_t from = out.size() / 2;
        const double c0 = toneDb(ref, from, 7e3), c1 = toneDb(out, from, 7e3), w0 = toneDb(ref, from, -3e3), w1 = toneDb(out, from, -3e3);
        printf("carrier %.1f -> %.1f dB (cut %.1f), wanted %.2f -> %.2f dB\n", c0, c1, c0 - c1, w0, w1);
        CHECK(c0 - c1 >= 40, "carrier cut only %.1f dB", c0 - c1);
        CHECK(std::fabs(w1 - w0) < 0.5, "wanted changed by %.3f dB", w1 - w0);
        // a carrier 30 Hz off the notch still falls by 40 dB with the widest default? (two sections: error / width squared)
        Gen g3; NotchBank nb2; Notch n2; n2.rfHz = centre + 7e3 + 10; n2.widthHz = 200; nb2.set({n2});
        auto o2 = run(&nb2, g3, 0.5, centre, 7e3, 1.0, -3e3, 0.01);
        CHECK(c0 - toneDb(o2, from, 7e3) >= 30, "a notch 10 Hz off cuts only %.1f dB", c0 - toneDb(o2, from, 7e3));
    }
    {   // the follow kind: same offset from the centre, whatever the centre is
        Gen g; NotchBank nb; Notch n; n.follow = true; n.offsetHz = 7e3; nb.set({n});
        run(&nb, g, 0.2, centre, 7e3, 1.0, -3e3, 0.01);
        Gen g2; auto o = run(&nb, g2, 0.4, centre + 5e3, 7e3, 1.0, -3e3, 0.01);   // retuned: the artefact is still at +7 kHz
        Gen g3; auto ref = run(nullptr, g3, 0.4, centre + 5e3, 7e3, 1.0, -3e3, 0.01);
        const size_t from = o.size() / 2;
        CHECK(toneDb(ref, from, 7e3) - toneDb(o, from, 7e3) >= 40, "follow kind lost its offset after a retune");
    }
    {   // the RF-fixed kind: after a retune of +2 kHz the real carrier is at +5 kHz and is still cut; a tone left at +7 kHz is not
        Gen g; NotchBank nb; Notch n; n.rfHz = centre + 7e3; nb.set({n});
        run(&nb, g, 0.2, centre, 7e3, 1.0, -3e3, 0.01);
        const double c2 = centre + 2e3;
        Gen g2; auto o = run(&nb, g2, 0.4, c2, 5e3, 1.0, -3e3, 0.01);
        Gen g3; auto ref = run(nullptr, g3, 0.4, c2, 5e3, 1.0, -3e3, 0.01);
        const size_t from = o.size() / 2;
        CHECK(toneDb(ref, from, 5e3) - toneDb(o, from, 5e3) >= 40, "RF-fixed notch did not follow the carrier after a retune: %.1f dB", toneDb(ref, from, 5e3) - toneDb(o, from, 5e3));
        Gen g4; NotchBank nb2; nb2.set({n});
        run(&nb2, g4, 0.2, centre, 7e3, 1.0, -3e3, 0.01);
        Gen g5; auto o2 = run(&nb2, g5, 0.4, c2, 7e3, 1.0, -3e3, 0.01);   // a signal that moved with the tuner at +7 kHz: no longer under the notch
        Gen g6; auto ref2 = run(nullptr, g6, 0.4, c2, 7e3, 1.0, -3e3, 0.01);
        CHECK(std::fabs(toneDb(ref2, from, 7e3) - toneDb(o2, from, 7e3)) < 0.5, "the RF-fixed notch cut a tone that is no longer at its RF frequency");
    }
    {   // outside the capture: nothing happens; width limits; two notches; disabled
        Gen g; NotchBank nb; Notch n; n.rfHz = centre + 3e6; nb.set({n});
        Gen g2; auto ref = run(nullptr, g, 0.05, centre, 7e3, 1.0, -3e3, 0.01);
        auto o = run(&nb, g2, 0.05, centre, 7e3, 1.0, -3e3, 0.01);
        CHECK(ref.size() == o.size() && !std::memcmp(ref.data(), o.data(), ref.size() * sizeof(cf32)), "a notch outside the capture changed samples");
        CHECK(NotchBank::clampWidth(1) == 25 && NotchBank::clampWidth(1e6) == 5000 && NotchBank::clampWidth(300) == 300, "width clamp");
        Notch a, b; a.rfHz = centre + 7e3; b.rfHz = centre - 3e3; b.widthHz = 5000;
        Gen g3; NotchBank two; two.set({a, b});
        auto o3 = run(&two, g3, 0.3, centre, 7e3, 1.0, -3e3, 0.01);
        Gen g4; auto r3 = run(nullptr, g4, 0.3, centre, 7e3, 1.0, -3e3, 0.01);
        CHECK(toneDb(r3, o3.size() / 2, -3e3) - toneDb(o3, o3.size() / 2, -3e3) >= 40, "second notch");
    }
    {   // an empty list and an all-disabled list are bit-exact
        Gen g, g2; NotchBank nb;
        auto ref = run(nullptr, g, 0.05, centre, 7e3, 1.0, -3e3, 0.01);
        auto o = run(&nb, g2, 0.05, centre, 7e3, 1.0, -3e3, 0.01);
        CHECK(!std::memcmp(ref.data(), o.data(), ref.size() * sizeof(cf32)), "empty list is not bit-exact");
        Notch n; n.rfHz = centre + 7e3; n.enabled = false; nb.set({n});
        Gen g3; auto o3 = run(&nb, g3, 0.05, centre, 7e3, 1.0, -3e3, 0.01);
        CHECK(!std::memcmp(ref.data(), o3.data(), ref.size() * sizeof(cf32)), "a disabled notch changed samples");
        CHECK(!nb.active(), "a disabled list is active");
    }
    {   // the stored text
        Notch a, b; a.rfHz = 404007000.5; a.widthHz = 150; b.follow = true; b.offsetHz = -2000; b.widthHz = 60; b.enabled = false;
        const auto t = NotchBank::toText({a, b});
        const auto v = NotchBank::fromText(t);
        CHECK(v.size() == 2 && !v[0].follow && std::fabs(v[0].rfHz - a.rfHz) < 0.01 && v[0].widthHz == 150 && v[1].follow && v[1].offsetHz == -2000 && !v[1].enabled && v[1].widthHz == 60, "text round trip: %s", t.c_str());
        CHECK(NotchBank::fromText("").empty() && NotchBank::fromText("9;1,2").empty() && NotchBank::fromText("1;junk").empty(), "bad text");
    }
    printf(fails ? "notch_bank: %d FAILED\n" : "notch_bank: all passed\n", fails);
    return fails ? 1 : 0;
}
