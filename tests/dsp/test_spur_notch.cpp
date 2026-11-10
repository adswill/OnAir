// Adaptive spur notch (dsp/spur_notch.h): a line is cut by 25 dB or more only when the same baseband place was a line under another radio centre (a real signal
// moves with the tuner), inside the channel or outside it, with the wanted carrier untouched; persistence alone cuts nothing; a signal that moved with the tuner is
// never cut; a clean capture comes out bit for bit. Then every mode of the registry with its own channel width, rate and tune offset (the engine's rule).
#include "dect2/dsp/spur_notch.h"
#include "dect2/engine/mode_tuning.h"
#include "../common/test_long.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <set>
#include <tuple>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
using cf32 = std::complex<float>;

struct Gen {
    std::mt19937 rng{7};
    std::normal_distribution<float> nd{0.f, 0.01f};
    struct Tone { double hz; double amp; double ph = 0; };
    std::vector<Tone> tones;
    double rate;
    explicit Gen(double r) : rate(r) {}
    void block(std::vector<cf32>& x, size_t n) {
        x.resize(n);
        for (size_t i = 0; i < n; i++) x[i] = cf32(nd(rng), nd(rng));
        for (auto& t : tones) {
            const double w = 2 * M_PI * t.hz / rate;
            for (size_t i = 0; i < n; i++) x[i] += (float)t.amp * cf32((float)std::cos(t.ph + w * (double)i), (float)std::sin(t.ph + w * (double)i));
            t.ph = std::fmod(t.ph + w * (double)n, 2 * M_PI);
        }
    }
};
// power of tone hz in the last `len` samples of a stretch
static double tonePower(const std::vector<cf32>& x, double hz, double rate) {
    std::complex<double> s = 0;
    for (size_t i = 0; i < x.size(); i++) s += std::complex<double>(x[i]) * std::polar(1.0, -2 * M_PI * hz / rate * (double)i);
    return std::norm(s / (double)x.size());
}
static double db(double p) { return 10 * std::log10(std::max(p, 1e-30)); }

// feeds `sec` seconds, returns the last second's output
static std::vector<cf32> feed(SpurNotch& sn, Gen& g, double sec) {
    const size_t total = (size_t)(sec * g.rate), blk = 65536;
    std::vector<cf32> x, tail;
    size_t done = 0;
    const size_t keep = (size_t)g.rate;
    while (done < total) {
        const size_t n = std::min(blk, total - done);
        g.block(x, n);
        sn.process(x.data(), n);
        done += n;
        if (done + keep > total) tail.insert(tail.end(), x.begin(), x.end());
    }
    return tail;
}

// the radio's spurs seen for 2 s under one centre, then a retune: the lines are in memory (a real signal would have moved)
static void trained(SpurNotch& sn, Gen& g, double chOff, double bw, double alt = 0, const std::vector<Gen::Tone>* only = nullptr) {
    sn.setChannel(chOff, bw, 100e6, alt);
    if (only) { Gen t(g.rate); t.tones = *only; feed(sn, t, 2.5); } else feed(sn, g, 2.5);
    sn.setChannel(chOff, bw, 100.0371e6, alt);
}

static void basic() {
    const double r = 2.4e6;
    {   // persistence alone cuts nothing
        SpurNotch sn; sn.configure(r); sn.setChannel(0, 100e3, 100e6);
        Gen g(r); g.tones = {{600e3, 0.02}};
        const auto y = feed(sn, g, 6.0);
        CHECK(sn.activeCount() == 0 && db(0.02 * 0.02) - db(tonePower(y, 600e3, r)) < 0.5, "a line was cut without a retune (%d active)", sn.activeCount());
    }
    {   // outside the channel, proved by a retune
        SpurNotch sn; sn.configure(r);
        Gen g(r); g.tones = {{600e3, 0.02}, {20e3, 0.02}};   // the spur and a wanted carrier in the channel
        const std::vector<Gen::Tone> spursOnly = {{600e3, 0.02}};   // the carrier was elsewhere under the other centre: it moved with the tuner
        trained(sn, g, 0, 100e3, 0, &spursOnly);
        const auto y = feed(sn, g, 4.5);
        const double spur0 = db(0.02 * 0.02), spur1 = db(tonePower(y, 600e3, r));
        const double w1 = db(tonePower(y, 20e3, r));
        CHECK(spur0 - spur1 >= 25, "out-of-channel spur reduced by %.1f dB", spur0 - spur1);
        CHECK(std::fabs(w1 - db(0.02 * 0.02)) < 0.1, "wanted carrier changed by %.2f dB", w1 - db(0.02 * 0.02));
        CHECK(sn.activeCount() == 1, "%d lines cut", sn.activeCount());
    }
    {   // inside the channel: kept until the same place is a line under another centre
        SpurNotch sn; sn.configure(r); sn.setChannel(0, 100e3, 100e6);
        Gen g(r); g.tones = {{30e3, 0.02}};
        auto y = feed(sn, g, 4.5);
        CHECK(db(0.02 * 0.02) - db(tonePower(y, 30e3, r)) < 1.0, "in-channel line cut before it was proved (%.1f dB)", db(0.02 * 0.02) - db(tonePower(y, 30e3, r)));
        sn.setChannel(0, 100e3, 101e6);   // retuned by 1 MHz; the line stays at +30 kHz: the radio's own
        y = feed(sn, g, 4.5);
        CHECK(db(0.02 * 0.02) - db(tonePower(y, 30e3, r)) >= 25, "instrumental in-channel line reduced by %.1f dB", db(0.02 * 0.02) - db(tonePower(y, 30e3, r)));
    }
    {   // a signal that moves with the tuner is never taken
        SpurNotch sn; sn.configure(r); sn.setChannel(0, 100e3, 100e6);
        Gen g(r); g.tones = {{30e3, 0.02}};
        feed(sn, g, 4.5);
        sn.setChannel(0, 100e3, 100.01e6);   // retuned by 10 kHz: the signal is now at +20 kHz
        g.tones = {{20e3, 0.02}};
        const auto y = feed(sn, g, 4.5);
        CHECK(db(0.02 * 0.02) - db(tonePower(y, 20e3, r)) < 1.0, "a moving in-channel signal was cut (%.1f dB)", db(0.02 * 0.02) - db(tonePower(y, 20e3, r)));
    }
    {   // a wide channel and no retune: nothing cut
        SpurNotch sn; sn.configure(r); sn.setChannel(0, r, 100e6);
        Gen g(r); g.tones = {{600e3, 0.02}};
        const auto y = feed(sn, g, 4.5);
        CHECK(db(0.02 * 0.02) - db(tonePower(y, 600e3, r)) < 0.5, "wide channel: a line was cut");
        CHECK(sn.activeCount() == 0, "wide channel: %d lines active", sn.activeCount());
    }
    {   // clean capture: bit for bit
        SpurNotch sn; sn.configure(r); sn.setChannel(0, 100e3, 100e6);
        Gen g(r), h(r);
        std::vector<cf32> a, b;
        bool same = true;
        for (int k = 0; k < 40; k++) {
            g.block(a, 65536); h.block(b, 65536);
            sn.process(a.data(), a.size());
            if (a != b) same = false;
        }
        CHECK(same, "a capture without lines was changed");
    }
    {   // a keyed signal in the channel (on for 0.3 s every 1 s) is never taken (it is protected, and it is no proved spur)
        SpurNotch sn; sn.configure(r); sn.setChannel(0, 100e3, 100e6);
        Gen g(r);
        std::vector<cf32> x;
        for (int s = 0; s < 8; s++) {
            g.tones = {{30e3, 0.02}};
            feed(sn, g, 0.3);
            g.tones.clear();
            feed(sn, g, 0.7);
        }
        CHECK(sn.activeCount() == 0, "a keyed signal was cut (%d)", sn.activeCount());
    }
    {   // two lines
        SpurNotch sn; sn.configure(r);
        Gen g(r); g.tones = {{-500e3, 0.03}, {700e3, 0.02}};
        trained(sn, g, 0, 100e3);
        const auto y = feed(sn, g, 4.5);
        CHECK(db(0.03 * 0.03) - db(tonePower(y, -500e3, r)) >= 25 && db(0.02 * 0.02) - db(tonePower(y, 700e3, r)) >= 25, "two lines: %.1f / %.1f dB", db(0.03 * 0.03) - db(tonePower(y, -500e3, r)), db(0.02 * 0.02) - db(tonePower(y, 700e3, r)));
    }
}

// every mode with its channel, rate and tune offset, as the engine feeds the notch
static void everyMode() {
    std::set<std::tuple<double, double, double>> seen;
    std::set<std::tuple<double, bool, double, bool, int>> classes;
    int modes = 0, runs = 0, cut = 0, wide = 0;
    for (int std_ = 0; std_ < 1200; std_++) {
        const ModeTuning* mt = modeTuning(std_);
        if (!mt) continue;
        modes++;
        const double r = std::min(std::max(mt->sampleRate, 1e6), 3.2e6);
        double bw = mt->bandwidthMhz * 1e6;
        const double off = mt->tuneOffsetHz;
        if (bw > r / 3) bw = r;
        if (!seen.insert({r, bw, off}).second) continue;
        const double chOff = off != 0 ? -off : 0.0;
        SpurNotch sn; sn.configure(r);
        const double half = bw / 2 + 3 * sn.binHz();
        // a spur place outside every protected window, if there is one
        double spur = 1e18;
        for (double f : {0.43, -0.43, 0.31, -0.31, 0.17, -0.17}) {
            const double h = f * r;
            if (std::fabs(h - chOff) > half + 0.02 * r && (off == 0 || std::fabs(h - off) > half + 0.02 * r) && std::fabs(h) > 0.05 * r) { spur = h; break; }
        }
        // by default one set-up per class (rate, wide or not, spur place, tune offset or not, channel width within a factor 4); the long run every distinct set-up
        if (!testLong() && !classes.insert({r, bw >= r, spur, off != 0, (int)std::floor(std::log2(std::max(bw, 1.0)) / 2)}).second) continue;
        runs++;
        Gen g(r);
        const double wantedHz = bw < r ? chOff + 0.1 * bw : 0.0;
        g.tones = {{wantedHz, 0.02}};
        if (spur < 1e17) g.tones.push_back({spur, 0.02});
        // the spur is in memory (seen under another centre), the wanted carrier was not there: it moved with the tuner
        { Gen tr(r); if (spur < 1e17) tr.tones = {{spur, 0.02}}; sn.setChannel(chOff, bw, 100e6, off != 0 ? off : 0.0); feed(sn, tr, 2.5); sn.setChannel(chOff, bw, 100.0371e6, off != 0 ? off : 0.0); }
        const auto y = feed(sn, g, 4.0);
        const double w = db(tonePower(y, wantedHz, r)) - db(0.02 * 0.02);
        CHECK(std::fabs(w) < 0.2, "mode %d (%s): the wanted carrier moved by %.2f dB (rate %.0f, bw %.0f, off %.0f)", std_, mt->id, w, r, bw, off);
        if (spur < 1e17) {
            const double red = db(0.02 * 0.02) - db(tonePower(y, spur, r));
            CHECK(red >= 20, "mode %d (%s): the spur at %.0f Hz was reduced by only %.1f dB (rate %.0f, bw %.0f)", std_, mt->id, spur, red, r, bw);
            cut++;
        } else wide++;
    }
    printf("spur_notch: %d modes, %d distinct channel set-ups, spur cut in %d, wide/protected in %d\n", modes, runs, cut, wide);
}

int main() {
    basic();
    everyMode();
    printf(fails ? "spur_notch: %d FAILED\n" : "spur_notch: all passed\n", fails);
    return fails ? 1 : 0;
}
