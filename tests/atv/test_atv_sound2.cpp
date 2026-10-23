// Analogue TV second sound: NICAM-728 (ETSI EN 300 163) and A2 / Zweikanalton. The standard's own numbers first (scrambler start, frame structure
// through a transmitter and receiver of ours), then both systems alone with carrier offsets, noise and other sample rates, then a whole TV signal.
#include "dect2/atv/atv_gen.h"
#include "dect2/atv/atv_rx.h"
#include "dect2/atv/atv_sound2.h"
#include "../common/jobs.h"
#include <random>
#include <atomic>
#include <cmath>
#include <cstdio>

using namespace dect2;
using testjobs::jprintf;
static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL: " __VA_ARGS__); jprintf("\n"); fails++; } } while (0)

// amplitude of the tone f in x (48 kHz), over the seconds after `skip`
static double toneAmp(const std::vector<float>& x, double f, double skip = 0.5) {
    const size_t a = (size_t)(skip * 48000);
    if (x.size() < a + 4800) return 0;
    double re = 0, im = 0;
    const size_t n = x.size() - a;
    for (size_t i = 0; i < n; i++) { const double ph = 2 * M_PI * f * (double)i / 48000.0; re += x[a + i] * std::cos(ph); im += x[a + i] * std::sin(ph); }
    return 2 * std::hypot(re, im) / (double)n;
}
[[maybe_unused]] static double rms(const std::vector<float>& x, double skip = 0.5) {
    const size_t a = (size_t)(skip * 48000);
    if (x.size() < a + 100) return 0;
    double s = 0;
    for (size_t i = a; i < x.size(); i++) s += (double)x[i] * x[i];
    return std::sqrt(s / (double)(x.size() - a));
}
static double db(double v) { return 20 * std::log10(std::max(v, 1e-12)); }

static void noise(std::vector<cf32>& x, double sigma, uint64_t seed) {
    std::mt19937_64 g(seed);
    std::normal_distribution<float> d(0.f, (float)(sigma / std::sqrt(2.0)));
    for (auto& v : x) v += cf32(d(g), d(g));
}

// ---- NICAM alone
struct NicamCase { const char* name; double rate, cfo, snrDb; bool wide; int mode; int chan; };
static void nicamCase(const NicamCase& k) {
    NicamTxConfig tc; tc.rate = k.rate; tc.wide = k.wide; tc.mode = k.mode; tc.level = 0.04;
    NicamTx tx(tc);
    NicamRx rx;
    rx.configure(k.rate);
    const double fc = 3.8e6;                                   // where a channel's NICAM carrier is, from the centre of the input
    std::vector<float> l, r;
    const size_t chunk = 20000;
    const size_t total = (size_t)(1.5 * k.rate);
    std::vector<cf32> buf;
    for (size_t done = 0; done < total; done += chunk) {
        buf.assign(chunk, cf32(0, 0));
        tx.add(buf.data(), chunk, fc + k.cfo);
        if (k.snrDb < 100) noise(buf, std::sqrt(0.0016 * (k.rate / 364e3) / std::pow(10.0, k.snrDb / 10)), 7 + done);
        rx.process(buf.data(), chunk, fc, k.wide, k.chan, l, r);
    }
    const NicamStatus st = rx.status();
    const double lA = toneAmp(l, 1000), lB = toneAmp(l, 1500), rA = toneAmp(r, 1000), rB = toneAmp(r, 1500);
    jprintf("NICAM %-34s locked %d mode %d frames %llu parity bad %llu/%llu  L 1k %.1f dB 1.5k %.1f dB  R 1k %.1f dB 1.5k %.1f dB\n", k.name, (int)st.locked, st.mode,
            (unsigned long long)st.frames, (unsigned long long)st.parityBad, (unsigned long long)st.samples, db(lA), db(lB), db(rA), db(rB));
    CHECK(st.locked && rx.audioValid(), "NICAM %s: not locked", k.name);
    CHECK(st.mode == (k.mode == 0 ? 0 : 1), "NICAM %s: mode %d", k.name, st.mode);
    if (k.mode == 0) {
        CHECK(lA > 0.05 && rB > 0.05, "NICAM %s: the tones are missing (%.3f %.3f)", k.name, lA, rB);
        CHECK(db(lA) - db(lB) > 30 && db(rB) - db(rA) > 30, "NICAM %s: channels mixed (%.1f / %.1f dB)", k.name, db(lA) - db(lB), db(rB) - db(rA));
        CHECK(lA > 0.05 && std::fabs(db(lA) - db(rB)) < 3.0, "NICAM %s: levels differ", k.name);
    } else {
        const double want = k.chan == 0 ? 1000 : 1500;
        const double a = toneAmp(l, want), o = toneAmp(l, k.chan == 0 ? 1500 : 1000);
        CHECK(a > 0.05 && db(a) - db(o) > 30, "NICAM %s: wrong channel (%.1f dB over the other)", k.name, db(a) - db(o));
        CHECK(std::fabs(db(toneAmp(r, want)) - db(a)) < 1.0, "NICAM %s: the two sides differ", k.name);
    }
    if (k.snrDb >= 30) CHECK(st.parityBad == 0, "NICAM %s: %llu parity errors", k.name, (unsigned long long)st.parityBad);
    else CHECK(st.errorRate < 0.1f, "NICAM %s: error rate %.3f", k.name, st.errorRate);
}

// ---- A2 alone
struct A2Case { const char* name; double rate, cfo, snrDb; int mode; };
static void a2Case(const A2Case& k) {
    A2TxConfig tc; tc.rate = k.rate; tc.mode = k.mode; tc.withFirst = true; tc.level1 = 0.05; tc.level2 = 0.05;
    A2Tx tx(tc);
    A2Rx rx;
    rx.configure(k.rate);
    const double f1 = 2.75e6;                                  // the first sound carrier from the centre
    std::vector<float> l, r;
    const size_t chunk = 20000, total = (size_t)(2.0 * k.rate);
    std::vector<cf32> buf;
    for (size_t done = 0; done < total; done += chunk) {
        buf.assign(chunk, cf32(0, 0));
        tx.add(buf.data(), chunk, f1 + k.cfo);
        if (k.snrDb < 100) noise(buf, std::sqrt(0.0025 * (k.rate / 150e3) / std::pow(10.0, k.snrDb / 10)), 11 + done);
        rx.process(buf.data(), chunk, f1, 50, 50, l, r);
    }
    const A2Status st = rx.status();
    const double lA = toneAmp(l, 1000), lB = toneAmp(l, 1500), rA = toneAmp(r, 1000), rB = toneAmp(r, 1500);
    jprintf("A2 %-36s carrier2 %d mode %d pilot %.1f dB  L 1k %.1f dB 1.5k %.1f dB  R 1k %.1f dB 1.5k %.1f dB\n", k.name, (int)st.carrier2, st.mode, st.pilotDb, db(lA), db(lB),
            db(rA), db(rB));
    CHECK(st.carrier2 && st.mode == k.mode, "A2 %s: carrier2 %d mode %d (want %d)", k.name, (int)st.carrier2, st.mode, k.mode);
    if (k.mode == 1 || k.mode == 2) {
        CHECK(lA > 0.03 && rB > 0.03, "A2 %s: tones missing (%.3f %.3f)", k.name, lA, rB);
        CHECK(db(lA) - db(lB) > 25 && db(rB) - db(rA) > 25, "A2 %s: channels mixed (%.1f / %.1f dB)", k.name, db(lA) - db(lB), db(rB) - db(rA));
    } else {
        CHECK(lA > 0.03 && std::fabs(db(lA) - db(rA)) < 1.0, "A2 %s: mono %.3f %.3f", k.name, lA, rA);
    }
}

int main() {
    // the energy dispersal sequence starts 0000 0111 1011 1110 0010 (EN 300 163, 4.1.3)
    {
        uint8_t s[720];
        nicam::scrambler(s);
        const char* want = "00000111101111100010";
        bool ok = true;
        for (int i = 0; i < 20; i++) ok = ok && (s[i] == (uint8_t)(want[i] - '0'));
        CHECK(ok, "scrambler does not start 0000 0111 1011 1110 0010");
        // the interleaving of the standard: the first transmitted bits are 25, 69, 113, ... (bit j of the sound block = 24 + j + 1)
        CHECK(nicam::deinterleavedIndex(0) == 0 && nicam::deinterleavedIndex(1) == 44 && nicam::deinterleavedIndex(2) == 88 && nicam::deinterleavedIndex(16) == 1 &&
              nicam::deinterleavedIndex(703) == 703, "interleaving");
        // the 14 bit to 10 bit companding ranges
        CHECK(nicam::shiftForPeak(8191) == 4 && nicam::shiftForPeak(511) == 0 && nicam::shiftForPeak(512) == 1 && nicam::shiftForPeak(4095) == 3 && nicam::shiftForPeak(4096) == 4, "shift");
        CHECK(nicam::scaleFactorForPeak(8000) == 7 && nicam::scaleFactorForPeak(3000) == 6 && nicam::scaleFactorForPeak(1500) == 5 && nicam::scaleFactorForPeak(700) == 3, "scale factors");
        // emphasis: the standard's curve, de-emphasis against pre-emphasis; 400 Hz is 9.5 dB below 2 kHz
        auto gain = [](double f) {
            nicam::Emphasis e; double peak = 0;
            for (int i = 0; i < 6400; i++) { const double y = nicam::preEmphasis(e, (float)std::sin(2 * M_PI * f * i / 32000.0)); if (i > 3200) peak = std::max(peak, std::fabs(y)); }
            return peak;
        };
        CHECK(std::fabs(db(gain(2000)) - 0) < 0.3, "pre-emphasis at 2 kHz %.2f dB", db(gain(2000)));
        CHECK(std::fabs(db(gain(400)) + 9.5) < 1.0, "pre-emphasis at 400 Hz %.2f dB (the standard: 9.5 below 2 kHz)", db(gain(400)));
    }
    const NicamCase nc[] = {
        {"stereo 10 Msps clean", 10e6, 0, 200, false, 0, 0},
        {"stereo 10 Msps +3 kHz", 10e6, 3e3, 200, false, 0, 0},
        {"stereo 10 Msps -8 kHz", 10e6, -8e3, 200, false, 0, 0},
        {"stereo 10 Msps C/N 25 dB", 10e6, 0, 25, false, 0, 0},
        {"stereo 10 Msps C/N 17 dB", 10e6, 1e3, 17, false, 0, 0},
        {"stereo 8 Msps", 8e6, 0, 200, false, 0, 0},
        {"stereo 16 Msps", 16e6, 1e3, 200, false, 0, 0},
        {"stereo 20 Msps", 20e6, 0, 200, false, 0, 0},
        {"stereo 12.5 Msps", 12.5e6, -2e3, 40, false, 0, 0},
        {"system I (100% roll-off) 10 Msps", 10e6, 0, 200, true, 0, 0},
        {"dual channel 1", 10e6, 0, 200, false, 1, 0},
        {"dual channel 2", 10e6, 500, 200, false, 1, 1},
    };
    for (const auto& k : nc) nicamCase(k);
    const A2Case ac[] = {
        {"stereo 10 Msps", 10e6, 0, 200, 1},
        {"stereo 10 Msps +2 kHz", 10e6, 2e3, 200, 1},
        {"stereo 16 Msps", 16e6, 0, 200, 1},
        {"stereo 10 Msps C/N 30 dB", 10e6, 0, 30, 1},
        {"dual 10 Msps", 10e6, 0, 200, 2},
        {"mono (pilot, no tone) 10 Msps", 10e6, 0, 200, 0},
    };
    for (const auto& k : ac) a2Case(k);

    // ---- the whole receiver: a PAL B/G signal with A2 and with NICAM
    struct Full { const char* name; int sys, sound2; const char* want; bool stereo; double cfo; double cnr; };
    const Full fl[] = {
        {"PAL B/G with A2 stereo", kAtvG, 1, "A2 stereo", true, 0, 40},
        {"PAL B/G with A2 stereo, carrier +40 kHz", kAtvG, 1, "A2 stereo", true, 40e3, 40},
        {"PAL B/G with A2 dual", kAtvG, 2, "A2 dual, sound A", false, 0, 40},
        {"PAL B/G with A2 mono pilot", kAtvG, 3, "FM mono (A2 pilot)", false, 0, 40},
        {"PAL B/G with NICAM stereo", kAtvG, 4, "NICAM stereo", true, 0, 40},
        {"PAL B/G with NICAM stereo, +30 kHz, C/N 35 dB", kAtvG, 4, "NICAM stereo", true, 30e3, 35},
        {"PAL D/K with NICAM stereo", kAtvDK, 4, "NICAM stereo", true, 0, 40},
        {"PAL I with NICAM stereo", kAtvI, 4, "NICAM stereo", true, 0, 40},
        {"PAL B/G with NICAM dual", kAtvG, 5, "NICAM dual mono, channel 1", false, 0, 40},
        {"PAL D/K with A2 stereo", kAtvDK, 1, "A2 stereo", true, 0, 40},
    };
    for (const auto& f : fl) {
        AtvGenConfig g; g.sys = f.sys; g.colour = kAtvPal; g.cnrDb = f.cnr; g.rate = 10e6; g.sound = 4; g.sound2 = f.sound2; g.cfoHz = f.cfo;
        AtvGenerator gen(g);
        AtvReceiver rx;
        rx.setSilent(true);
        std::vector<float> l, r;
        rx.setAudioTap([&](const float* a, const float* b, size_t n) { l.insert(l.end(), a, a + n); r.insert(r.end(), b, b + n); });
        rx.configure(g.rate);
        std::vector<cf32> buf;
        std::string sys;
        AtvTelemetry t;
        uint64_t seq = 0;
        const size_t chunk = 65536, total = (size_t)(5.0 * g.rate);
        for (size_t done = 0; done < total; done += chunk) {
            buf.resize(chunk);
            gen.generate(buf.data(), chunk);
            rx.feed(buf.data(), chunk);
            if (rx.telemetry(t, seq)) { seq = t.seq; if (done > (size_t)(3.5 * g.rate)) sys = t.soundSystem; }
        }
        const std::vector<float> l2(l.begin() + (long)std::min<size_t>(l.size(), 48000 * 3), l.end()), r2(r.begin() + (long)std::min<size_t>(r.size(), 48000 * 3), r.end());
        const double lA = toneAmp(l2, 1000, 0), lB = toneAmp(l2, 1500, 0), rA = toneAmp(r2, 1000, 0), rB = toneAmp(r2, 1500, 0);
        jprintf("TV %-48s state %d sound '%s'  L 1k %.1f 1.5k %.1f dB  R 1k %.1f 1.5k %.1f dB\n", f.name, t.state, sys.c_str(), db(lA), db(lB), db(rA), db(rB));
        CHECK(t.state == 2, "%s: picture not locked", f.name);
        CHECK(sys == f.want, "%s: sound system '%s' (want '%s')", f.name, sys.c_str(), f.want);
        if (f.stereo) CHECK(lA > 0.03 && rB > 0.03 && db(lA) - db(lB) > 20 && db(rB) - db(rA) > 20, "%s: stereo tones (L %.1f/%.1f R %.1f/%.1f dB)", f.name, db(lA), db(lB), db(rA), db(rB));
    }
    return fails ? 1 : 0;
}
