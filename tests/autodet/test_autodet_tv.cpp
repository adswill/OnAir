// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "TV (auto)": every TV standard's own generator (DVB-T 2K / 8K with each guard, DVB-T2 1K..32K incl. the extended 32K of a real mux, 7 and
// 6 MHz channels, ISDB-T 6 / 8 MHz, ATSC 1.0, ATSC 3.0, DTMB PN420 / PN945, DVB-S, DVB-S2 QPSK / 8PSK, DVB-C 64 / 256-QAM, analogue B/G, I, M),
// off the centre, with a sample clock error and noise at several SNRs over the signal's width: the standard and the parameters must come out,
// (--quick: one case per standard; AUTODET_FULL=1: SNRs from 25 down to -3 dB)
// a "sure" answer must never be wrong. A confusion matrix and the weakest SNR that is still right are printed. AUTODET_VERBOSE=1: every case.
#include "dect2/atsc/atsc_gen.h"
#include "dect2/autodet/autodet.h"
#include "dect2/dsp/resampler.h"
#include "dect2/dvbc/dvbc_rx.h"
#include "dect2/dvbt/dvbt_gen.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/source/source.h"
#include "dect2/t2/t2gen.h"
#include "../common/impair.h"
#include "../common/test_util.h"
#include <chrono>
#include <functional>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace dect2;
using namespace dect2::autodet;

namespace {

const bool kVerbose = std::getenv("AUTODET_VERBOSE") != nullptr;

struct Case {
    std::string label;
    int truth;
    double rate;            // the capture's rate
    double bwHz;            // nominal occupied width (for the SNR)
    double chanMhz = 0;     // expected raster (0: not checked)
    int fftK = 0;           // expected FFT size (0: not checked)
    double guard = 0;       // expected guard (0: not checked)
    double rs = 0;          // expected symbol rate (0: not checked)
    bool sat = false;       // the capture comes from an LNB
    std::function<bool(std::vector<cf32>&, size_t)> gen;
};

std::vector<cf32> resample(const std::vector<cf32>& x, double from, double to) {
    RationalResampler r;
    if (!r.configure(from, to)) return {};
    std::vector<cf32> y;
    r.process(x.data(), x.size(), y);
    return y;
}

// DVB-T2 at the channel width: the generator's samples are at 64/7 MHz for 8 MHz; the same samples at chan * 8/7 MHz are that channel
Case t2Case(const char* label, double chan, int s2, int gi, bool ext, int pp, double guard, int fftK) {
    Case c; c.label = label; c.truth = TvDvbt2; c.chanMhz = chan; c.fftK = fftK; c.guard = guard;
    const double nat = chan == 1.7 ? 131e6 / 71 : chan * 8e6 / 7;
    c.rate = nat * 35 / 32; c.bwHz = chan * 1e6 * (fftK >= 16 ? 0.97 : 0.95);
    c.gen = [=](std::vector<cf32>& out, size_t n) {
        TxParams p; p.s2field1 = s2; p.giIdx = gi; p.ext = ext; p.pp = pp;
        T2Generator g(p);
        std::vector<cf32> a, f;
        while (a.size() < (size_t)((double)n * 32 / 35) + 100000 + g.frameLength()) { f.clear(); g.nextFrame(f); a.insert(a.end(), f.begin(), f.end()); }
        // start somewhere inside a frame, as a radio would
        a.erase(a.begin(), a.begin() + (long)(g.frameLength() / 3));
        out = resample(a, nat, nat * 35 / 32);
        if (out.size() < n) return false;
        out.resize(n);
        return true;
    };
    return c;
}

Case tCase(const char* label, double chan, int mode, int guardIdx, double guard) {
    Case c; c.label = label; c.truth = TvDvbt; c.chanMhz = chan; c.fftK = mode ? 8 : 2; c.guard = guard;
    const double nat = chan * 8e6 / 7;
    c.rate = nat * 35 / 32; c.bwHz = chan * 1e6 * 0.95;
    c.gen = [=](std::vector<cf32>& out, size_t n) {
        dvbt::Params p; p.mode = mode; p.guard = guardIdx;
        dvbt::Generator g(p, dvbt::testTsSource());
        std::vector<cf32> a, s;
        while (a.size() < (size_t)((double)n * 32 / 35) + 50000) { s.clear(); g.nextSymbol(s); a.insert(a.end(), s.begin(), s.end()); }
        out = resample(a, nat, nat * 35 / 32);
        if (out.size() < n) return false;
        out.resize(n);
        return true;
    };
    return c;
}

Case synthCase(const char* label, int truth, int stdMode, double rate, double bwHz, std::vector<int> opt, std::vector<double> val = {}) {
    Case c; c.label = label; c.truth = truth; c.rate = rate; c.bwHz = bwHz;
    c.gen = [=](std::vector<cf32>& out, size_t n) {
        SynthConfig sc; sc.snrDb = 80; sc.mode = stdMode;
        for (size_t i = 0; i < opt.size() && i < 8; i++) sc.modeOpt[i] = opt[i];
        for (size_t i = 0; i < val.size() && i < 4; i++) sc.modeVal[i] = val[i];
        if (stdMode == 6 && !opt.empty()) sc.isdbtBwMhz = opt[0];
        auto g = makeModeSynth(stdMode, sc, rate);
        if (!g) return false;
        std::vector<cf32> a((size_t)(rate * 0.15) + n);
        g->generate(a.data(), a.size());
        out.assign(a.end() - (long)n, a.end());
        return true;
    };
    return c;
}

// DVB-C with a transport stream of random payload, as a real multiplex looks after its scrambling (the built-in test stream repeats,
// which puts a comb of lines into the spectrum no real channel has)
Case dvbcCase(const char* label, int qamOpt, double rs) {
    Case c; c.label = label; c.truth = TvDvbc; c.rate = 10e6; c.bwHz = rs * 1.15; c.rs = rs;
    c.gen = [=](std::vector<cf32>& out, size_t n) {
        SynthConfig sc; sc.snrDb = 80; sc.mode = 42; sc.modeOpt[0] = qamOpt; sc.modeVal[0] = rs;
        auto rng = std::make_shared<std::mt19937>(99);
        auto g = makeDvbcSynth(sc, 10e6, [rng](uint8_t* p) { p[0] = 0x47; for (int i = 1; i < 188; i++) p[i] = (uint8_t)((*rng)() & 0xFF); p[1] &= 0x1F; });
        if (!g) return false;
        std::vector<cf32> a((size_t)(1.5e6) + n);
        g->generate(a.data(), a.size());
        out.assign(a.end() - (long)n, a.end());
        return true;
    };
    return c;
}

Case atscCase() {
    Case c; c.label = "ATSC 8-VSB"; c.truth = TvAtsc; c.rate = 8e6; c.bwHz = 5.38e6; c.chanMhz = 6;
    c.gen = [](std::vector<cf32>& out, size_t n) {
        atsc::ChannelConfig cc;
        atsc::Generator g(dvbt::testTsSource(), cc, 8e6);
        out.clear();
        g.generate(n, out);
        return out.size() >= n;
    };
    return c;
}

double power(const std::vector<cf32>& x) { double p = 0; for (const auto& v : x) p += std::norm(v); return x.empty() ? 0 : p / (double)x.size(); }

} // namespace

int main(int argc, char** argv) {
    const bool quick = argc > 1 && std::string(argv[1]) == "--quick";   // one case per standard, two SNRs (the quick CI tier)
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<Case> cs;
    cs.push_back(tCase("DVB-T 8K 1/4 8 MHz", 8, 1, 3, 0.25));
    cs.push_back(tCase("DVB-T 2K 1/32 8 MHz", 8, 0, 0, 1.0 / 32));
    cs.push_back(tCase("DVB-T 8K 1/16 7 MHz", 7, 1, 1, 1.0 / 16));
    cs.push_back(tCase("DVB-T 2K 1/8 6 MHz", 6, 0, 2, 1.0 / 8));
    cs.push_back(t2Case("DVB-T2 32K ext 1/8 PP8 (real mux)", 8, 5, 2, true, 7, 0.125, 32));
    cs.push_back(t2Case("DVB-T2 8K 1/128 8 MHz", 8, 1, 4, false, 0, 1.0 / 128, 8));
    cs.push_back(t2Case("DVB-T2 16K 19/256 7 MHz", 7, 4, 6, false, 3, 19.0 / 256, 16));
    cs.push_back(t2Case("DVB-T2 2K 1/4 8 MHz", 8, 0, 3, false, 0, 0.25, 2));
    cs.push_back(t2Case("DVB-T2 1K 1/16 1.7 MHz", 1.7, 3, 1, false, 3, 1.0 / 16, 1));
    { Case c = synthCase("ISDB-T 6 MHz", TvIsdbt, 6, 8e6, 5.57e6, {6}); c.chanMhz = 6; cs.push_back(c); }
    { Case c = synthCase("ISDB-T 8 MHz", TvIsdbt, 6, 10e6, 7.43e6, {8}); c.chanMhz = 8; cs.push_back(c); }
    cs.push_back(atscCase());
    { Case c = synthCase("ATSC 3.0 (6.144 MHz synth)", TvAtsc3, 5, 8e6, 5.18e6, {}); c.chanMhz = 6; c.fftK = 8; cs.push_back(c); }
    { Case c = synthCase("DTMB PN945 8 MHz", TvDtmb, 9, 10e6, 7.56e6, {0}); c.chanMhz = 8; cs.push_back(c); }
    { Case c = synthCase("DTMB PN420 8 MHz", TvDtmb, 9, 10e6, 7.56e6, {2}); c.chanMhz = 8; cs.push_back(c); }
    { Case c = synthCase("DVB-S2 QPSK 5 Msym/s", TvDvbs, 8, 10e6, 6.75e6, {0, 0}, {5e6}); c.rs = 5e6; c.sat = true; cs.push_back(c); }
    { Case c = synthCase("DVB-S2 8PSK 4 Msym/s r0.20", TvDvbs, 8, 10e6, 4.8e6, {0, 1, 0, 2}, {4e6}); c.rs = 4e6; c.sat = true; cs.push_back(c); }
    { Case c = synthCase("DVB-S QPSK 3 Msym/s", TvDvbs, 8, 10e6, 4.05e6, {1}, {3e6}); c.rs = 3e6; c.sat = true; cs.push_back(c); }
    cs.push_back(dvbcCase("DVB-C 64-QAM 6.952 Msym/s", 0, 6.952e6));
    cs.push_back(dvbcCase("DVB-C 256-QAM 6.9 Msym/s", 4, 6.9e6));
    cs.push_back(dvbcCase("DVB-C 16-QAM 5.0 Msym/s", 1, 5.0e6));
    { Case c = synthCase("Analogue B/G PAL", TvAnalog, 10, 10e6, 5e6, {0}); cs.push_back(c); }
    { Case c = synthCase("Analogue I PAL", TvAnalog, 10, 10e6, 5.5e6, {2}); cs.push_back(c); }
    { Case c = synthCase("Analogue M NTSC", TvAnalog, 10, 10e6, 4.2e6, {4}); cs.push_back(c); }

    // AUTODET_FULL=1: the whole sweep (the slow tier); otherwise three SNRs. Only >= 12 dB must be right; below, a 'sure' must still never be wrong.
    const bool full = std::getenv("AUTODET_FULL") != nullptr;
    const std::vector<double> snrs = full ? std::vector<double>{25, 12, 9, 6, 3, 0, -3} : quick ? std::vector<double>{12, 3} : std::vector<double>{12, 6, 3};
    if (quick) {
        std::vector<Case> one;
        bool seen[TvCount] = {};
        for (const Case& c : cs) if (!seen[c.truth]) { seen[c.truth] = true; one.push_back(c); }
        cs.swap(one);
    }
    std::mt19937 g(777);
    std::uniform_real_distribution<double> u(-1, 1);
    int conf[TvCount][TvCount] = {};
    int total = 0, right = 0, sureWrong = 0, paramWrong = 0;
    for (const Case& c : cs) {
        const size_t n = (size_t)(c.rate * tvCaptureSec());
        std::vector<cf32> clean;
        if (!c.gen(clean, n)) { CHECK(false, "%s: no test signal", c.label.c_str()); continue; }
        const double ps = power(clean);
        const std::vector<cf32> clk = impair::clock(clean, 20);   // a 20 ppm radio clock
        double worst = 99;
        for (double snr : snrs) {
            std::vector<cf32> x = clk;
            const double room = std::max(0.0, (c.rate - c.bwHz) / 2 - 0.3e6);
            const double off = std::round(u(g) * room * 0.7);
            impair::shift(x, off, c.rate);
            const double nvar = ps * c.rate / c.bwHz / std::pow(10.0, snr / 10);
            std::normal_distribution<double> nd(0, std::sqrt(nvar / 2));
            for (auto& v : x) v += cf32((float)nd(g), (float)nd(g));
            TvOptions o; o.satellite = c.sat; o.rfHz = c.sat ? 1500e6 : 600e6;
            const TvResult r = detectTv(x.data(), x.size(), c.rate, o);
            total++;
            bool ok = r.std == c.truth;
            bool pok = true;
            if (ok && c.chanMhz > 0 && r.p.chanMhz != c.chanMhz) pok = false;
            if (ok && c.fftK > 0 && r.p.fftK != c.fftK) pok = false;
            if (ok && c.guard > 0 && std::fabs(r.p.guard - c.guard) > 1e-6) pok = false;
            if (ok && c.rs > 0 && std::fabs(r.p.symRate - c.rs) / c.rs > 0.01) pok = false;
            if (ok && !pok) paramWrong++;
            ok = ok && pok;
            right += ok;
            if (ok && snr < worst) worst = snr;
            conf[c.truth][r.std]++;
            if (r.verdict == VerdictSure && !ok) sureWrong++;
            if (kVerbose || (!ok && snr >= 12)) {
                printf("  %-34s %3.0f dB off %+5.2f MHz: %s [%.0f ms]\n", c.label.c_str(), snr, off / 1e6, r.headline.c_str(), r.ms);
                if (kVerbose || !ok) for (const auto& e : r.evidence) printf("      %s\n", e.c_str());
            }
            if (snr >= 12) CHECK(ok, "%s at %.0f dB: %s", c.label.c_str(), snr, r.headline.c_str());
        }
        printf("%-36s weakest right: %s\n", c.label.c_str(), worst < 99 ? (std::to_string((int)worst) + " dB").c_str() : "never");
    }
    printf("\nconfusion matrix (rows: truth, columns: answer)\n%-12s", "");
    for (int cc = 0; cc < TvCount; cc++) printf(" %6.6s", cc ? tvStdName(cc) : "?");
    printf("\n");
    for (int t = 1; t < TvCount; t++) {
        printf("%-12.12s", tvStdName(t));
        for (int cc = 0; cc < TvCount; cc++) printf(" %6d", conf[t][cc]);
        printf("\n");
    }
    printf("right %d of %d (standard right but a parameter wrong: %d); 'sure' and wrong: %d\n", right, total, paramWrong, sureWrong);
    CHECK(sureWrong == 0, "'sure' answers wrong: %d", sureWrong);
    // noise only: no TV standard claimed
    {
        std::vector<cf32> x((size_t)(10e6 * tvCaptureSec()));
        std::normal_distribution<double> nd(0, 0.05);
        for (auto& v : x) v = cf32((float)nd(g), (float)nd(g));
        const TvResult r = detectTv(x.data(), x.size(), 10e6);
        CHECK(r.std == TvUnknown || r.verdict != VerdictSure, "noise: %s", r.headline.c_str());
        printf("noise only: %s\n", r.headline.c_str());
    }
    printf("time %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return testSummary("autodet_tv");
}
