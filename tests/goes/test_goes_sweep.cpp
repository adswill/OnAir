// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GOES HRIT receiver under real-world impairments the clean loopback test does not cover: roll-off 0.3 and 0.5 (the generator and the
// receiver may disagree), I/Q swapped (spectrum inverted) and negated signals, +-100 kHz carrier offsets, +-30 ppm clock, low Es/N0,
// every sample rate an SDR gives, a deep fade in the middle and a burst of strong interference. Every case must rebuild every file.
#include "dect2/goes/goes_gen.h"
#include "dect2/goes/goes_rx.h"
#include "dect2/goes/goes_tel.h"
#include <cstdio>
#include <cstdlib>
using namespace dect2;
#include "../common/test_util.h"

struct Case {
    const char* name;
    double rate, snr, cfo, ppm, rolloff;
    bool swapIq, negate, invG2;
    double seconds, fadeDb;      // fadeDb > 0: the middle third is attenuated by that much
    bool conventional = false;   // Reed-Solomon symbols in the conventional basis instead of the CCSDS dual basis
    double driftHzPerS = 0;      // carrier drift (a warming LNB oscillator, Doppler)
    double toneDb = -200;        // a continuous interferer, this many dB above the signal
    double iqGain = 1, iqSkewDeg = 0;   // I/Q imbalance of the radio
    double toneHz = 300e3;      // where the interferer is
    bool nrzm = false;          // differentially (NRZ-M) coded frame bits, as the real GOES-19 HRIT sends them (found in a 2026 recording)
};

static bool run(const Case& c) {
    GoesGenConfig g;
    g.rate = c.rate; g.snrDb = c.snr; g.cfoHz = c.cfo; g.sroPpm = c.ppm; g.rolloff = c.rolloff; g.fillFrames = 12; g.seed = 9; g.invertG2 = c.invG2; g.dualBasis = !c.conventional; g.nrzm = c.nrzm;
    GoesGenerator gen(g);
    GoesReceiver rx;
    rx.configure(c.rate);
    std::vector<std::vector<uint8_t>> files;
    rx.setFileCallback([&](const std::vector<uint8_t>& f, int, int) { files.push_back(f); });
    std::vector<cf32> buf(65536);
    GoesTelemetry t;
    uint64_t seq = 0;
    const double total = c.seconds * c.rate;
    double nIdx = 0;
    const double toneAmp = 0.2 * std::pow(10.0, c.toneDb / 20);
    for (double done = 0; done < total; done += (double)buf.size()) {
        gen.generate(buf.data(), buf.size());
        const bool fade = c.fadeDb > 0 && done > total / 3 && done < 2 * total / 3;
        const float fg = fade ? (float)std::pow(10.0, -c.fadeDb / 20) : 1.f;
        for (auto& v : buf) {
            if (fade) v *= fg;
            if (c.driftHzPerS != 0) { const double t = nIdx / c.rate; const double ph = 3.141592653589793 * c.driftHzPerS * t * t; v *= cf32((float)std::cos(ph), (float)std::sin(ph)); }
            if (c.toneDb > -100) { const double ph = 2 * 3.141592653589793 * c.toneHz * nIdx / c.rate; v += cf32((float)(toneAmp * std::cos(ph)), (float)(toneAmp * std::sin(ph))); }
            if (c.iqGain != 1 || c.iqSkewDeg != 0) { const double sk = c.iqSkewDeg * 3.141592653589793 / 180; v = cf32(v.real(), (float)(c.iqGain * (v.imag() * std::cos(sk) + v.real() * std::sin(sk)))); }
            nIdx += 1;
            if (c.swapIq) v = std::conj(v);
            if (c.negate) v = -v;
        }
        rx.feed(buf.data(), buf.size());
        if (rx.telemetry(t, seq)) seq = t.seq;
    }
    const std::vector<GoesTestFile> tf = goesTestFiles();
    size_t exact = 0;
    for (const auto& f : files) for (const auto& x : tf) if (f == x.file) { exact++; break; }
    // which of the test files came back at least once
    size_t kinds = 0;
    for (const auto& x : tf) for (const auto& f : files) if (f == x.file) { kinds++; break; }
    // a file with a lost packet inside is still handed over, flagged in fileErrors: only those may differ from what was sent
    const bool ok = kinds == tf.size() && files.size() - exact <= t.fileErrors;
    printf("  %-58s %s  state %d lock %d frames %llu ok %llu bad, files %zu (%zu/%zu kinds, %zu exact), cfo %.0f\n", c.name, ok ? "PASS" : "FAIL",
           t.state, t.carrierLock, (unsigned long long)(t.blocksOk + t.blocksBad), (unsigned long long)t.blocksOk, files.size(), kinds, tf.size(), exact, t.cfoHz);
    return ok;
}

int main() {
    const bool all = getenv("GOES_SWEEP_ALL") != nullptr;
    //          name                                   rate   snr  cfo    ppm  alpha swap  neg   g2inv  sec  fade
    const Case cases[] = {
        {"2.4M a0.5 base",                              2.4e6, 8,   0,     0,   0.5, false, false, true, 2.0, 0},
        {"2.4M a0.3 (generator) vs receiver",           2.4e6, 8,   0,     0,   0.3, false, false, true, 2.0, 0},
        {"2.4M a0.3 Es/N0 4 dB",                        2.4e6, 4,   20e3,  10,  0.3, false, false, true, 2.5, 0},
        {"2.4M I/Q swapped",                            2.4e6, 8,   30e3,  0,   0.3, true,  false, true, 2.0, 0},
        {"2.4M negated",                                2.4e6, 8,   -30e3, 0,   0.3, false, true,  true, 2.0, 0},
        {"2.4M +100 kHz",                               2.4e6, 8,   100e3, 30,  0.3, false, false, true, 2.0, 0},
        {"2.4M -100 kHz -30 ppm",                       2.4e6, 8,   -100e3,-30, 0.3, false, false, true, 2.0, 0},
        {"2.0M a0.3 +40 kHz",                           2.0e6, 8,   40e3,  0,   0.3, false, false, true, 2.0, 0},
        {"3.0M a0.3 -40 kHz",                           3.0e6, 8,   -40e3, 0,   0.3, false, false, true, 2.0, 0},
        {"2.048M a0.3 +60 kHz",                         2.048e6, 8, 60e3,  0,   0.3, false, false, true, 2.0, 0},
        {"2.4M Es/N0 3 dB",                             2.4e6, 3,   0,     0,   0.3, false, false, true, 3.0, 0},
        {"2.4M Es/N0 4 dB, 15 dB fade 0.5 s",           2.4e6, 6,   10e3,  0,   0.3, false, false, true, 4.0, 15},
        {"2.4M Es/N0 8 dB, 40 dB fade (signal lost)",   2.4e6, 8,   10e3,  0,   0.3, false, false, true, 5.0, 40},
        {"4M a0.3 -80 kHz",                             4.0e6, 6,   -80e3, 20,  0.3, false, false, true, 2.0, 0},
        {"2.4M G2 plain, swapped, -50 kHz",             2.4e6, 6,   -50e3, 0,   0.3, true,  false, false, 2.0, 0},
        {"2.4M conventional-basis Reed-Solomon",        2.4e6, 8,   20e3,  0,   0.3, false, false, true, 2.0, 0, true},
        {"2.4M conventional RS, Es/N0 4 dB, swapped",   2.4e6, 4,   -20e3, 0,   0.3, true,  false, true, 2.5, 0, true},
        {"2.4M NRZ-M coded frames",                      2.4e6, 8,   0,     0,   0.3, false, false, true, 2.0, 0, false, 0, -200, 1, 0, 300e3, true},
        {"2.4M NRZ-M, Es/N0 4 dB, -440 Hz, 2 ppm",       2.4e6, 4,   -440,  2,   0.3, false, false, true, 3.0, 0, false, 0, -200, 1, 0, 300e3, true},
        {"2.4M NRZ-M, G2 plain, swapped, negated, +45 kHz", 2.4e6, 6, 45e3, 0,  0.3, true,  true,  false, 2.5, 0, false, 0, -200, 1, 0, 300e3, true},
        {"2.4M NRZ-M conventional-basis RS, 30 kHz",     2.4e6, 6,   30e3,  0,   0.3, false, false, true, 2.5, 0, true, 0, -200, 1, 0, 300e3, true},
    };
    const Case more[] = {
        {"2.4M Es/N0 2 dB",                             2.4e6, 2,   15e3,  5,   0.3, false, false, true, 5.0, 0},
        {"2.4M Es/N0 1 dB",                             2.4e6, 1,   -15e3, 5,   0.3, false, false, true, 8.0, 0},
        {"2.4M drift 5 kHz/s from +20 kHz, 6 dB",       2.4e6, 6,   20e3,  0,   0.3, false, false, true, 4.0, 0, false, 5e3},
        {"2.4M drift -20 kHz/s, 6 dB",                  2.4e6, 6,   0,     0,   0.3, false, false, true, 4.0, 0, false, -20e3},
        {"2.4M tone 0 dB inside the band (+300 kHz)",   2.4e6, 8,   0,     0,   0.3, false, false, true, 3.0, 0, false, 0, 0, 1, 0, 300e3},
        {"2.4M tone 0 dB inside the band (-250 kHz)",  2.4e6, 10,  0,     0,   0.3, false, false, true, 4.0, 0, false, 0, 0, 1, 0, -250e3},
        {"2.4M tone +15 dB outside the band (+950 kHz)", 2.4e6, 8,  0,     0,   0.3, false, false, true, 3.0, 0, false, 0, 15, 1, 0, 950e3},
        {"2.4M I/Q imbalance 0.8 and 8 degrees",        2.4e6, 8,   10e3,  0,   0.3, false, false, true, 2.5, 0, false, 0, -200, 0.8, 8},
    };
    int bad = 0, n = 0;
    for (const auto& c : more) { n++; if (!run(c)) bad++; }
    for (const auto& c : cases) { n++; if (!run(c)) bad++; }
    (void)all;
    printf("goes sweep: %d of %d cases failed\n", bad, n);
    return bad ? 1 : 0;
}
