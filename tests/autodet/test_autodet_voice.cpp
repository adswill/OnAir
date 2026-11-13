// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Digital voice (auto)": every voice system's own test signal (the app's generators), moved off the centre, with a sample clock error and
// noise at several SNRs over the signal's width; a confusion matrix, the weakest SNR at which each one is still right, neighbours in one
// capture (DMR next to P25, DMR next to NXDN one 25 kHz channel away), an inverted spectrum, and noise / analogue FM that must not be
// called digital. AUTODET_VERBOSE=1 prints every case.
#include "dect2/autodet/autodet.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/engine/sig_ident.h"
#include "dect2/fusion/fusion_gen.h"
#include "dect2/p25p2/p25p2_gen.h"
#include "dect2/source/source.h"
#include "../common/impair.h"
#include "../common/test_util.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace dect2;
using namespace dect2::autodet;

namespace {

constexpr double kRate = 1e6;
constexpr double kSec = 1.2;
const bool kVerbose = std::getenv("AUTODET_VERBOSE") != nullptr;

struct Sys { int sys; int stdMode; int opt0; double bwHz; };

std::vector<cf32> analogFm(size_t n, double rate, uint32_t seed) {
    std::mt19937 g(seed);
    std::uniform_real_distribution<double> u(0, 1);
    double f[6], a[6], ph[6];
    for (int i = 0; i < 6; i++) { f[i] = 300 + 2700 * u(g); a[i] = 0.3 + 0.7 * u(g); ph[i] = 6.28 * u(g); }
    std::vector<cf32> x(n);
    double phase = 0;
    for (size_t k = 0; k < n; k++) {
        const double t = (double)k / rate;
        const double env = 0.5 + 0.5 * std::sin(2 * M_PI * 3.1 * t);   // syllables
        double m = 0.15 * std::sin(2 * M_PI * 88.5 * t);               // CTCSS
        for (int i = 0; i < 6; i++) m += env * a[i] * std::sin(2 * M_PI * f[i] * t + ph[i]) / 3.5;
        phase += 2 * M_PI * 2500 * m / rate;
        x[k] = cf32((float)(0.3 * std::cos(phase)), (float)(0.3 * std::sin(phase)));
    }
    return x;
}

// the clean signal of a system at kRate, kSec seconds (after a settling stretch)
bool render(const Sys& s, std::vector<cf32>& out, uint32_t seed) {
    const size_t n = (size_t)(kRate * kSec);
    out.clear();
    if (s.sys == VoiceAnalogFm) { out = analogFm(n, kRate, seed); return true; }
    if (s.sys == VoiceP25p2) {
        P25p2GenConfig c; c.rate = kRate; c.snrDb = 80; c.superframes = 6; c.seed = seed;
        c.calls.push_back(P25p2GenCall());
        out = p25p2Signal(c);
        if (out.size() < n) return false;
        out.resize(n);
        return true;
    }
    if (s.sys == VoiceYsf) {
        fusion::GenParams p; p.commFrames = 20; p.seed = seed;
        std::vector<uint8_t> bits;
        if (!fusion::genTransmissionBits(p, bits)) return false;
        out = fusion::modulateC4fm(bits, kRate, 0, 0.3f, 40);
        if (out.size() < n) return false;
        out.resize(n);
        return true;
    }
    SynthConfig sc;
    sc.snrDb = 80; sc.mode = s.stdMode; sc.modeOpt[0] = s.opt0;
    if (s.sys == VoiceDmr) sc.modeOpt[4] = (int)seed;
    auto g = makeModeSynth(s.stdMode, sc, kRate);
    if (!g) return false;
    std::vector<cf32> all((size_t)(kRate * 0.3) + n);
    g->generate(all.data(), all.size());
    out.assign(all.end() - (long)n, all.end());
    return true;
}

// signal power over the stretches where it is on (a TDMA burst, a transmission)
double onPower(const std::vector<cf32>& x) {
    const size_t w = 2000;
    std::vector<double> p;
    for (size_t i = 0; i + w <= x.size(); i += w) { double s = 0; for (size_t k = i; k < i + w; k++) s += std::norm(x[k]); p.push_back(s / w); }
    if (p.empty()) return 0;
    const double mx = *std::max_element(p.begin(), p.end());
    double s = 0; int c = 0;
    for (double v : p) if (v > 0.3 * mx) { s += v; c++; }
    return c ? s / c : 0;
}

void addNoise(std::vector<cf32>& x, double sigPower, double bwHz, double snrDb, std::mt19937& g) {
    const double nvar = sigPower * kRate / bwHz / std::pow(10.0, snrDb / 10);
    std::normal_distribution<double> nd(0, std::sqrt(nvar / 2));
    for (auto& v : x) v += cf32((float)nd(g), (float)nd(g));
}

const VoiceSignal* strongest(const VoiceResult& r) { return r.signals.empty() ? nullptr : &r.signals[0]; }

} // namespace

int main() {
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<Sys> sys = {
        {VoiceDmr, 11, 0, 9000}, {VoiceP25, 52, 0, 8100}, {VoiceP25p2, 182, 0, 8000}, {VoiceNxdn96, 40, 2, 8300}, {VoiceNxdn48, 40, 1, 4200},
        {VoiceDstar, 35, 1, 5500}, {VoiceDpmr, 47, 0, 4200}, {VoiceYsf, 130, 0, 10000}, {VoiceTetra, 48, 0, 22000}, {VoiceM17, 33, 1, 9000},
        {VoiceAnalogFm, 0, 0, 11000},
    };
    // AUTODET_FULL=1: the whole sweep; otherwise four SNRs. Only >= 10 dB must be right; below, a 'sure' must still never be wrong.
    const bool full = std::getenv("AUTODET_FULL") != nullptr;
    const std::vector<double> snrs = full ? std::vector<double>{30, 15, 10, 7, 5, 3, 1} : std::vector<double>{15, 10, 6, 3};
    std::mt19937 g(12345);
    std::uniform_real_distribution<double> uoff(-300e3, 300e3);
    int conf[VoiceCount][VoiceCount] = {};
    int sureWrong = 0, sureRight = 0, total = 0, right = 0;
    std::map<int, double> weakest;
    for (const Sys& s : sys) {
        std::vector<cf32> clean;
        if (!render(s, clean, 3)) { printf("%-20s no generator\n", voiceSysName(s.sys)); CHECK(false, "%s: no test signal", voiceSysName(s.sys)); continue; }
        const double ps = onPower(clean);
        const std::vector<sigid::Region> rg = sigid::findRegions(clean.data(), clean.size(), kRate, 1, 10);
        const double base = rg.empty() ? 0 : rg[0].centreHz;   // a generator may put its signal off the centre
        double worst = 99;
        const std::vector<cf32> clk = impair::clock(clean, 40);   // a 40 ppm radio clock
        for (double snr : snrs) {
            std::vector<cf32> x = clk;
            double off = std::round(uoff(g));
            if (std::fabs(base + off) > 380e3) off = -off;
            impair::shift(x, off, kRate);
            off += base;
            addNoise(x, ps, s.bwHz, snr, g);
            VoiceOptions o;
            const VoiceResult r = detectVoice(x.data(), x.size(), kRate, o);
            const VoiceSignal* v = strongest(r);
            int got = v ? v->sys : VoiceUnknown;
            if (got == VoiceNxdn48 && s.sys == VoiceNxdn96) got = VoiceNxdn48;
            total++;
            const bool ok = got == s.sys && v && std::fabs(v->offHz - off) < 3000;
            right += ok;
            if (ok && snr < worst) worst = snr;
            conf[s.sys][got]++;
            if (v && v->verdict == VerdictSure) { if (ok) sureRight++; else sureWrong++; }
            if (kVerbose || (!ok && snr >= 10))
                printf("  %-20s %4.0f dB off %+7.0f: %s (%s %.0f %%) at %+.0f Hz: %s [%.0f ms]\n", voiceSysName(s.sys), snr, off, v ? voiceSysName(v->sys) : "-",
                       v ? verdictName(v->verdict) : "-", v ? v->confidence * 100 : 0, v ? v->offHz : 0, v ? v->why.c_str() : "", r.ms);
            if (snr >= 10) CHECK(ok, "%s at %.0f dB: got %s", voiceSysName(s.sys), snr, v ? voiceSysName(v->sys) : "nothing");
        }
        weakest[s.sys] = worst;
    }
    printf("\nconfusion matrix (rows: truth, columns: answer; %zu SNRs each)\n%-20s", snrs.size(), "");
    for (int c = 0; c < VoiceCount; c++) printf(" %5.5s", c ? voiceSysName(c) : "?");
    printf("\n");
    for (const Sys& s : sys) {
        printf("%-20s", voiceSysName(s.sys));
        for (int c = 0; c < VoiceCount; c++) printf(" %5d", conf[s.sys][c]);
        printf("   weakest right: %s\n", weakest[s.sys] < 99 ? (std::to_string((int)weakest[s.sys]) + " dB").c_str() : "never");
    }
    printf("right %d of %d; 'sure' answers: %d right, %d wrong\n", right, total, sureRight, sureWrong);
    CHECK(sureWrong == 0, "a 'sure' answer was wrong %d times", sureWrong);

    // ---- neighbours: DMR and P25 200 kHz apart, DMR and NXDN96 25 kHz apart, all at 20 dB
    {
        std::vector<cf32> a, b, c;
        CHECK(render(sys[0], a, 5) && render(sys[1], b, 6) && render(sys[3], c, 7), "render");
        const size_t n = std::min({a.size(), b.size(), c.size()});
        std::vector<cf32> x(n, cf32(0, 0));
        const double pa = onPower(a), pb = onPower(b), pc = onPower(c);
        auto base = [](const std::vector<cf32>& v) { const auto rg = sigid::findRegions(v.data(), v.size(), kRate, 1, 10); return rg.empty() ? 0.0 : rg[0].centreHz; };
        impair::shift(a, -100e3 - base(a), kRate); impair::shift(b, 100e3 - base(b), kRate); impair::shift(c, -75e3 - base(c), kRate);
        for (size_t i = 0; i < n; i++) x[i] = a[i] + b[i] * (float)std::sqrt(pa / pb) + c[i] * (float)std::sqrt(pa / pc);
        addNoise(x, pa, 9000, 20, g);
        const VoiceResult r = detectVoice(x.data(), n, kRate);
        printf("\nneighbours: %s\n", r.headline.c_str());
        bool dmr = false, p25 = false, nx = false;
        for (const VoiceSignal& v : r.signals) {
            if (kVerbose) printf("  %+8.0f Hz %s (%s): %s\n", v.offHz, voiceSysName(v.sys), verdictName(v.verdict), v.why.c_str());
            dmr |= v.sys == VoiceDmr && std::fabs(v.offHz + 100e3) < 3e3;
            p25 |= v.sys == VoiceP25 && std::fabs(v.offHz - 100e3) < 3e3;
            nx |= v.sys == VoiceNxdn96 && std::fabs(v.offHz + 75e3) < 3e3;
        }
        CHECK(dmr && p25 && nx, "neighbours: DMR %d, P25 %d, NXDN %d", dmr, p25, nx);
    }
    // ---- an inverted spectrum (I and Q swapped) is still found, and reported as inverted
    {
        std::vector<cf32> x;
        render(sys[1], x, 9);
        impair::swapIq(x);
        addNoise(x, onPower(x), 8100, 20, g);
        const VoiceResult r = detectVoice(x.data(), x.size(), kRate);
        const VoiceSignal* v = strongest(r);
        CHECK(v && v->sys == VoiceP25 && v->inverted, "inverted P25: %s, inverted %d", v ? voiceSysName(v->sys) : "-", v ? v->inverted : 0);
    }
    // ---- noise only: nothing, or nothing sure
    {
        std::vector<cf32> x((size_t)(kRate * kSec), cf32(0, 0));
        addNoise(x, 1e-3, kRate, 0, g);
        const VoiceResult r = detectVoice(x.data(), x.size(), kRate);
        int sure = 0;
        for (const VoiceSignal& v : r.signals) sure += v.verdict == VerdictSure;
        CHECK(sure == 0, "noise: %d sure answers (%s)", sure, r.headline.c_str());
    }
    printf("time %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return testSummary("autodet_voice");
}
