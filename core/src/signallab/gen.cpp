// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal Lab: synthetic "unknown" signals with a known truth, and the evaluation harness that runs the pipeline on many of them and
// counts how often each stage gets the truth back. None of the generated signals is a real protocol.
#include "dect2/signallab/signallab.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <sstream>

namespace dect2::signallab {

namespace {
// the standard library's distributions differ between implementations; these do not, so a seed gives the same signal everywhere
double u01(std::mt19937& r) { return ((double)(r() >> 5) + 0.5) / 134217728.0; }
int uint(std::mt19937& r, int a, int b) { return a + (int)(r() % (uint32_t)(b - a + 1)); }
float gauss(std::mt19937& r) { return (float)(std::sqrt(-2.0 * std::log(u01(r))) * std::cos(2 * M_PI * u01(r))); }
int grayInv(int g) { int k = 0; for (; g; g >>= 1) k ^= g; return k; }
int bitsPerSym(Mod m, int order) { if (m == Mod::QAM) return 4; int n = 0; while ((1 << n) < order) n++; return std::max(1, n); }

double rrcPulse(double t, double a) {
    if (std::fabs(t) < 1e-9) return 1 - a + 4 * a / M_PI;
    if (a > 0 && std::fabs(std::fabs(t) - 1 / (4 * a)) < 1e-6)
        return a / std::sqrt(2.0) * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * a)) + (1 - 2 / M_PI) * std::cos(M_PI / (4 * a)));
    return (std::sin(M_PI * t * (1 - a)) + 4 * a * t * std::cos(M_PI * t * (1 + a))) / (M_PI * t * (1 - 16 * a * a * t * t));
}

// one burst (or the whole continuous stream) of symbols to samples, unit power
std::vector<cf> modulate(const GenSpec& g, const std::vector<int>& sym, std::mt19937& rng) {
    const double T = (1 + g.clockPpm * 1e-6) / g.baud, sps = g.fs * T;
    const size_t n = (size_t)std::ceil((double)sym.size() * sps);
    std::vector<cf> y(n);
    const int M = g.mod == Mod::QAM ? 16 : g.order;
    const double ph0 = 2 * M_PI * u01(rng);
    if (g.mod == Mod::FSK || g.mod == Mod::MSK || g.mod == Mod::GMSK) {
        const double h = g.mod == Mod::FSK ? g.h : 0.5;
        const double kappa = M_PI * 0.3 * std::sqrt(2 / std::log(2.0));
        double ph = ph0;
        for (size_t i = 0; i < n; i++) {
            const double tau = (double)i / sps;   // in symbols
            double lev = 0;
            if (g.mod == Mod::GMSK) {
                const long k0 = (long)std::floor(tau);
                for (long k = k0 - 3; k <= k0 + 3; k++) {
                    if (k < 0 || k >= (long)sym.size()) continue;
                    const double d = tau - (double)k - 0.5;
                    lev += (2.0 * sym[(size_t)k] - (M - 1)) * 0.5 * (std::erf(kappa * (d + 0.5)) - std::erf(kappa * (d - 0.5)));
                }
            } else {
                const size_t k = std::min(sym.size() - 1, (size_t)tau);
                lev = 2.0 * sym[k] - (M - 1);
            }
            ph += 2 * M_PI * lev * h * g.baud / 2 / g.fs;
            y[i] = std::polar(1.f, (float)ph);
        }
    } else if (g.mod == Mod::OOK || g.mod == Mod::ASK) {
        std::vector<float> a(n);
        for (size_t i = 0; i < n; i++) { const size_t k = std::min(sym.size() - 1, (size_t)((double)i / sps)); a[i] = g.mod == Mod::OOK ? (float)sym[k] : (float)(0.4 + 0.6 * sym[k] / std::max(1, M - 1)); }
        const int L = std::max(1, (int)(0.1 * sps));
        double acc = 0;
        for (size_t i = 0; i < n; i++) { acc += a[i]; if (i >= (size_t)L) acc -= a[i - L]; y[i] = std::polar((float)(acc / L), (float)ph0); }
    } else {
        std::vector<cf> pts(sym.size());
        for (size_t k = 0; k < sym.size(); k++) {
            if (g.mod == Mod::QAM) pts[k] = cf((float)(2 * (sym[k] / 4) - 3), (float)(2 * (sym[k] % 4) - 3)) / std::sqrt(10.f);
            else { const double r = M == 4 ? M_PI / 4 : 0.0; pts[k] = std::polar(1.f, (float)(r + 2 * M_PI * sym[k] / M)); }
        }
        const cf rot = std::polar(1.f, (float)ph0);
        for (size_t i = 0; i < n; i++) {
            const double tau = (double)i / sps;
            const long k0 = (long)std::lround(tau);
            cf acc(0, 0);
            for (long k = k0 - 6; k <= k0 + 6; k++) if (k >= 0 && k < (long)sym.size()) acc += pts[(size_t)k] * (float)rrcPulse(tau - (double)k, g.rolloff);
            y[i] = acc * rot;
        }
        double p = 0;
        for (const cf& v : y) p += std::norm(v);
        p /= (double)n;
        for (cf& v : y) v /= (float)std::sqrt(p);
    }
    return y;
}

Bits bytesToBits(const std::vector<uint8_t>& b) { Bits o; for (uint8_t v : b) for (int i = 7; i >= 0; i--) o.push_back((uint8_t)((v >> i) & 1)); return o; }
}

std::vector<cf> generate(const GenSpec& g, GenTruth* truth) {
    std::mt19937 rng(g.seed * 2654435761u + 12345);
    auto byte = [](std::mt19937& r) { return uint(r, 0, 255); };
    const int bps = bitsPerSym(g.mod, g.order);
    const int M = g.mod == Mod::QAM ? 16 : g.order;
    // the data-level preamble that comes out alternating on the air for this line code
    Bits pre;
    const bool multi = g.scrambler >= 0 && !scramblers()[(size_t)g.scrambler].additive;
    if (bps == 1) {
        for (int i = 0; i < g.preambleBits; i++) {
            uint8_t b = (uint8_t)((i & 1) ^ 1);
            if (!multi) {
                if (g.line == LineCode::NRZI || g.line == LineCode::Differential || g.line == LineCode::Miller) b = 1;
                else if (g.line == LineCode::Manchester || g.line == LineCode::DiffManchester) b = 0;
            }
            pre.push_back(b);
        }
        if (g.line == LineCode::Manchester || g.line == LineCode::DiffManchester || g.line == LineCode::Miller) pre.resize(pre.size() / 2);
    } else {
        const int a = 0, b = g.mod == Mod::PSK ? M / 2 : M - 1;
        const int ga = a ^ (a >> 1), gb = b ^ (b >> 1);
        for (int s = 0; s < g.preambleBits / bps; s++) { const int v = (s & 1) ? gb : ga; for (int i = bps - 1; i >= 0; i--) pre.push_back((uint8_t)((v >> i) & 1)); }
    }
    GenTruth t;
    t.sync = g.sync;
    const uint8_t counter0 = (uint8_t)byte(rng), ident = (uint8_t)byte(rng);
    std::vector<Bits> air;
    for (int f = 0; f < g.frames; f++) {
        std::vector<uint8_t> pl(std::max(2, g.payloadBytes));
        pl[0] = (uint8_t)(counter0 + f);
        pl[1] = ident;
        for (size_t i = 2; i < pl.size(); i++) pl[i] = (uint8_t)byte(rng);
        if (g.crcAlgo >= 0) {
            const CheckAlgo& c = checkAlgos()[(size_t)g.crcAlgo];
            const uint32_t v = checksum(c, pl.data(), pl.size());
            const int nb = c.width / 8;
            for (int k = 0; k < nb; k++) pl.push_back(c.refout ? (uint8_t)(v >> (8 * k)) : (uint8_t)(v >> (8 * (nb - 1 - k))));
        }
        Bits body = bytesToBits(pl);
        t.payloads.push_back(body);
        t.frameBits = body.size();
        if (g.scrambler >= 0 && scramblers()[(size_t)g.scrambler].additive) body = scramble(body, scramblers()[(size_t)g.scrambler]);
        Bits data = pre;
        data.insert(data.end(), g.sync.begin(), g.sync.end());
        data.insert(data.end(), body.begin(), body.end());
        air.push_back(data);
    }
    // multiplicative scrambling and the line code over the whole transmission (bursts: per burst)
    auto finish = [&](const Bits& d) {
        Bits b = multi ? scramble(d, scramblers()[(size_t)g.scrambler]) : d;
        b = encodeLine(b, g.line);
        // a few trailing symbols so the last bits are not cut by the ramp-down
        for (int i = 0; i < 4 * bps; i++) b.push_back((uint8_t)(i & 1));
        std::vector<int> sym;
        for (size_t i = 0; i + (size_t)bps <= b.size(); i += (size_t)bps) {
            int v = 0;
            for (int k = 0; k < bps; k++) v = (v << 1) | b[i + (size_t)k];
            if (g.mod == Mod::QAM) sym.push_back(grayInv(v >> 2) * 4 + grayInv(v & 3));
            else sym.push_back(grayInv(v));
        }
        return sym;
    };
    std::vector<cf> out;
    const size_t gap = (size_t)(g.gapSec * g.fs);
    const size_t lead = std::max<size_t>(gap / 2, (size_t)(0.002 * g.fs));
    out.resize(lead, cf(0, 0));
    if (g.gapSec <= 0) {
        Bits all;
        for (const Bits& d : air) all.insert(all.end(), d.begin(), d.end());
        const auto w = modulate(g, finish(all), rng);
        out.insert(out.end(), w.begin(), w.end());
    } else {
        for (const Bits& d : air) {
            const auto w = modulate(g, finish(d), rng);
            out.insert(out.end(), w.begin(), w.end());
            out.insert(out.end(), gap, cf(0, 0));
        }
    }
    out.insert(out.end(), lead, cf(0, 0));
    // carrier offset and noise (SNR in a bandwidth equal to the symbol rate)
    auto N = [](std::mt19937& r) { return gauss(r); };
    const double sig2 = std::pow(10.0, -g.snrDb / 10) * g.fs / g.baud;
    const float sd = (float)std::sqrt(sig2 / 2);
    const double w = 2 * M_PI * g.offsetHz / g.fs;
    for (size_t i = 0; i < out.size(); i++) {
        const float ni = N(rng) * sd, nq = N(rng) * sd;   // two statements: the order of arguments is not fixed
        out[i] = out[i] * std::polar(1.f, (float)std::fmod(w * (double)i, 2 * M_PI)) + cf(ni, nq);
    }
    if (truth) *truth = t;
    return out;
}

GenSpec randomSpec(uint32_t seed) {
    std::mt19937 rng(seed * 7919u + 17);
    auto U = [&](double a, double b) { return a + (b - a) * u01(rng); };
    auto I = [&](int a, int b) { return uint(rng, a, b); };
    GenSpec g;
    g.seed = seed;
    const int pick = I(0, 99);
    if (pick < 25) { g.mod = Mod::FSK; g.order = 2; g.h = U(0.8, 2.0); }
    else if (pick < 35) { g.mod = Mod::FSK; g.order = 4; g.h = U(0.6, 1.0); }
    else if (pick < 45) { g.mod = Mod::GMSK; g.order = 2; g.h = 0.5; }
    else if (pick < 65) { g.mod = Mod::OOK; g.order = 2; }
    else if (pick < 80) { g.mod = Mod::PSK; g.order = 2; }
    else if (pick < 95) { g.mod = Mod::PSK; g.order = 4; }
    else { g.mod = Mod::PSK; g.order = 8; }
    const double rates[] = {48000, 250000, 1000000};
    g.fs = rates[I(0, 2)];
    g.baud = std::round(g.fs / U(8, 32));
    double bw = g.mod == Mod::FSK ? (g.order - 1) * g.h * g.baud + 2 * g.baud : g.mod == Mod::PSK ? 1.35 * g.baud : g.mod == Mod::OOK ? 3 * g.baud : 1.5 * g.baud;
    g.offsetHz = std::round(U(-1, 1) * std::max(0.0, g.fs / 2 - bw) * 0.6);
    g.snrDb = U(14, 28);
    g.clockPpm = U(-50, 50);
    const int lc = I(0, 99);
    if (g.mod == Mod::OOK) g.line = lc < 40 ? LineCode::NRZ : lc < 80 ? LineCode::Manchester : lc < 90 ? LineCode::Miller : LineCode::DiffManchester;
    else if (g.mod == Mod::PSK && g.order == 2) g.line = lc < 60 ? LineCode::NRZ : LineCode::Differential;
    else if (g.order == 2) g.line = lc < 50 ? LineCode::NRZ : lc < 75 ? LineCode::NRZI : LineCode::Manchester;
    else g.line = LineCode::NRZ;
    const int sc = I(0, 99);
    if (g.order == 2 && (g.mod == Mod::FSK || g.mod == Mod::GMSK) && (g.line == LineCode::NRZ || g.line == LineCode::NRZI)) g.scrambler = sc < 15 ? 0 : sc < 30 ? 5 : -1;
    else g.scrambler = sc < 10 ? 5 : -1;
    g.preambleBits = 2 * I(12, 32);
    const int sl = I(0, 1) ? 16 : 32;
    for (;;) {
        g.sync.clear();
        for (int i = 0; i < sl; i++) g.sync.push_back((uint8_t)I(0, 1));
        int tr = 0;
        for (int i = 1; i < sl; i++) tr += g.sync[(size_t)i] != g.sync[(size_t)i - 1];
        if (tr >= sl / 4 && tr <= sl * 3 / 4 && g.sync[0] == g.sync[1]) break;   // starts by breaking the alternation, so the preamble end is clear
    }
    g.payloadBytes = I(6, 24);
    g.crcAlgo = I(0, 99) < 85 ? I(0, (int)checkAlgos().size() - 1) : -1;
    g.frames = I(6, 14);
    g.gapSec = I(0, 99) < 20 ? 0 : U(g.mod == Mod::OOK ? 0.015 : 0.008, 0.03);
    return g;
}

EvalReport evaluate(int cases, uint32_t seed, bool verbose) {
    EvalReport r;
    r.stages = {{"bandwidth"}, {"carrier offset"}, {"bursts"}, {"modulation family"}, {"modulation order"}, {"symbol rate (1 %)"},
                {"line code"}, {"sync word"}, {"frame length"}, {"frames recovered (80 %)"}, {"checksum algorithm"}, {"no false checksum"}};
    std::ostringstream log;
    for (int c = 0; c < cases; c++) {
        const GenSpec g = randomSpec(seed + (uint32_t)c);
        GenTruth t;
        const std::vector<cf> x = generate(g, &t);
        const Analysis a = analyse(x, g.fs);
        auto mark = [&](size_t i, bool ok) { r.stages[i].tested++; r.stages[i].ok += ok; return ok; };
        const double bw = g.mod == Mod::FSK ? (g.order - 1) * g.h * g.baud + g.baud : g.mod == Mod::PSK ? 1.35 * g.baud : g.mod == Mod::GMSK ? 1.0 * g.baud : 2 * g.baud;
        const bool okBw = mark(0, a.channel.bw99Hz > 0.4 * bw && a.channel.bw99Hz < 2.5 * bw);
        const bool okOff = mark(1, std::fabs(a.channel.offsetHz - g.offsetHz) < 0.15 * bw);
        const bool okBursts = mark(2, g.gapSec <= 0 ? a.channel.continuous : a.channel.bursts.size() == (size_t)g.frames);
        auto fam = [](Mod m) { return m == Mod::MSK ? Mod::GMSK : m; };
        const bool okMod = mark(3, fam(a.usedMod) == fam(g.mod));
        bool okOrd = okMod;
        if (okMod && g.mod != Mod::OOK && g.mod != Mod::GMSK) okOrd = a.usedOrder == g.order;
        mark(4, okOrd);
        const bool okRate = mark(5, a.rate.baud > 0 && std::fabs(a.rate.baud / g.baud - 1) < 0.01);
        auto lc = [](LineCode l) { return l == LineCode::Differential ? LineCode::NRZI : l == LineCode::ManchesterInv ? LineCode::Manchester : l; };
        const bool okLine = mark(6, lc(a.line.code) == lc(g.line) && (g.scrambler < 0) == (a.line.scrambler < 0) && (g.scrambler < 0 || a.line.scrambler == g.scrambler));
        bool okSync = false;
        const Bits& fs = a.frame.sync.pattern;
        if (fs.size() >= 8 && fs.size() <= t.sync.size()) okSync = std::equal(fs.begin(), fs.end(), t.sync.end() - (long)fs.size());
        if (!okSync && fs.size() >= 8) { const Bits iv = invert(fs); if (fs.size() <= t.sync.size()) okSync = std::equal(iv.begin(), iv.end(), t.sync.end() - (long)iv.size()); }
        mark(7, okSync);
        const bool okLen = mark(8, a.frame.frameBits == t.frameBits);
        size_t found = 0;
        for (const Bits& p : t.payloads) {
            for (const Bits& f : a.frame.frames) {
                if (f.size() < p.size()) continue;
                if (std::equal(p.begin(), p.end(), f.begin())) { found++; break; }
            }
        }
        const bool okFr = mark(9, found * 10 >= t.payloads.size() * 8);
        bool okCk = true;
        if (g.crcAlgo >= 0) {
            okCk = !a.checks.empty() && a.checks[0].algo == g.crcAlgo;
            // a different catalogue entry giving the same values on these frames is just as right
            if (!okCk && !a.checks.empty()) {
                const CheckAlgo& x1 = checkAlgos()[(size_t)a.checks[0].algo], &x2 = checkAlgos()[(size_t)g.crcAlgo];
                okCk = x1.check == x2.check && x1.width == x2.width;
            }
            mark(10, okCk);
        } else mark(11, a.checks.empty());
        if (verbose) {
            char b[400];
            snprintf(b, sizeof b, "case %3d %-4s-%d %8.0f Bd fs %7.0f %-12s sc %2d crc %2d gap %4.1f ms snr %4.1f | %s%s%s%s%s%s%s%s%s%s | got %s-%d %.1f Bd %s\n", c,
                     modName(g.mod), g.order, g.baud, g.fs, lineCodeName(g.line), g.scrambler, g.crcAlgo, g.gapSec * 1000, g.snrDb,
                     okBw ? "B" : "b", okOff ? "O" : "o", okBursts ? "U" : "u", okMod ? "M" : "m", okOrd ? "D" : "d", okRate ? "R" : "r", okLine ? "L" : "l",
                     okSync ? "S" : "s", okLen ? "N" : "n", okFr ? "F" : "f", modName(a.usedMod), a.usedOrder, a.rate.baud, okCk ? "ck-ok" : "ck-BAD");
            log << b;
        }
    }
    r.cases = cases;
    std::ostringstream o;
    o << log.str();
    o << "Signal Lab evaluation: " << cases << " synthetic signals (seed " << seed << ")\n";
    for (const EvalStage& s : r.stages) {
        char b[120];
        snprintf(b, sizeof b, "  %-26s %4d / %-4d  %5.1f %%\n", s.name, s.ok, s.tested, s.tested ? 100.0 * s.ok / s.tested : 0.0);
        o << b;
    }
    r.text = o.str();
    return r;
}

} // namespace dect2::signallab
