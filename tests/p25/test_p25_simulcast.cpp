// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// P25 Phase 1 control channel from a simulcast system: two transmitters send the same symbols, one delayed (delay spread), a little weaker and a few Hz
// away in carrier. Sent as C4FM and as CQPSK (the linear simulcast modulation: pi/4 differential QPSK with the same phase steps per symbol, root raised
// cosine pulses, so amplitude dips at the transitions that a C4FM signal does not have), received by the FM discriminator receiver. Measured: share of
// the control messages decoded. The CQPSK signal is a synthetic stand-in built from the published description (the step per dibit equals the C4FM
// frequency step; the pulse shape is a guess), not a recording of a real LSM transmitter. P25_SIMULCAST_TABLE=1 prints the numbers.
#include "dect2/p25/p25_gen.h"
#include "dect2/p25/p25_proto.h"
#include "dect2/p25/p25_rx.h"
#include "dect2/p25/p25_tel.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

namespace {

constexpr double kBaud = 4800;

std::vector<cf32> modulate(const std::vector<int8_t>& sym, double rate, bool cqpsk, double alpha) {
    const int sps = (int)std::lround(rate / kBaud);
    const double pi = 3.14159265358979323846;
    std::vector<double> g;
    const int span = 6;
    for (int k = -span * sps; k <= span * sps; k++) {
        const double t = (double)k / sps;
        double v;
        if (std::fabs(t) < 1e-9) v = 1.0 - alpha + 4 * alpha / pi;
        else if (std::fabs(std::fabs(t) - 1 / (4 * alpha)) < 1e-9) v = alpha / std::sqrt(2.0) * ((1 + 2 / pi) * std::sin(pi / (4 * alpha)) + (1 - 2 / pi) * std::cos(pi / (4 * alpha)));
        else v = (std::sin(pi * t * (1 - alpha)) + 4 * alpha * t * std::cos(pi * t * (1 + alpha))) / (pi * t * (1 - 16 * alpha * alpha * t * t));
        g.push_back(v);
    }
    const size_t n = (sym.size() + 2 * span) * (size_t)sps;
    std::vector<cf32> y(n, cf32(0, 0));
    if (cqpsk) {
        double ph = 0;
        for (size_t k = 0; k < sym.size(); k++) {
            ph += sym[k] * pi / 4;   // +-3 / +-1 -> +-3pi/4 / +-pi/4: the same turn per symbol as the C4FM deviation
            const cf32 s((float)std::cos(ph), (float)std::sin(ph));
            const size_t c = (k + span) * (size_t)sps;
            for (size_t j = 0; j < g.size(); j++) y[c + j - (size_t)(span * sps)] += s * (float)g[j];
        }
    } else {
        // C4FM: frequency pulses (raised cosine 0.2) as the generator does, integrated to phase
        std::vector<double> f(n, 0.0);
        for (size_t k = 0; k < sym.size(); k++) {
            const size_t c = (k + span) * (size_t)sps;
            for (int j = -span * sps; j <= span * sps; j++) {
                const double t = (double)j / sps, a = 0.2;
                const double sinc = std::fabs(t) < 1e-9 ? 1.0 : std::sin(pi * t) / (pi * t);
                const double d = 1 - 4 * a * a * t * t;
                const double cc = std::fabs(d) < 1e-9 ? pi / 4 : std::cos(pi * a * t) / d;
                f[(size_t)((long)c + j)] += sym[k] * 600.0 * sinc * cc;
            }
        }
        double ph = 0;
        for (size_t i = 0; i < n; i++) { ph += 2 * pi * f[i] / rate; y[i] = cf32((float)std::cos(ph), (float)std::sin(ph)); }
    }
    double pw = 0;
    for (auto& v : y) pw += std::norm(v);
    const float sc = (float)(0.05 / std::sqrt(pw / (double)n));
    for (auto& v : y) v *= sc;
    return y;
}

struct Res { double pct; int blocksOk, sent; };

Res run(bool cqpsk, double delayUs, double ratioDb, double dfHz, double snrDb) {
    const double rate = 240000;
    P25GenTx tx;
    tx.kind = P25GenTx::Control;
    tx.durSec = 12;
    std::vector<int8_t> sym;
    p25TxSymbols(tx, sym);
    sym.insert(sym.begin(), 48, 3);   // a few symbols of lead-in
    const std::vector<cf32> a = modulate(sym, rate, cqpsk, 0.2);
    const size_t dly = (size_t)std::llround(delayUs * 1e-6 * rate);
    const double g = std::pow(10.0, ratioDb / 20);
    std::mt19937 rng(5);
    std::normal_distribution<float> nd(0.f, 1.f);
    const double sigma = std::sqrt(0.05 * 0.05 / std::pow(10.0, snrDb / 10) * rate / 12500 / 2);
    std::vector<cf32> x(a.size() + 24000, cf32(0, 0));
    for (size_t i = 0; i < x.size(); i++) {
        cf32 v(0, 0);
        if (i >= 12000 && i - 12000 < a.size()) v += a[i - 12000];
        if (delayUs >= 0 && i >= 12000 + dly && i - 12000 - dly < a.size()) {
            const double ph = 2 * 3.14159265358979323846 * dfHz * (double)i / rate;
            v += (float)g * a[i - 12000 - dly] * cf32((float)std::cos(ph), (float)std::sin(ph));
        }
        v += cf32(sigma * nd(rng), sigma * nd(rng));
        x[i] = v;
    }
    P25Receiver rx;
    rx.setSilent(true);
    rx.configure(rate);
    std::set<uint64_t> seen;
    uint64_t lastSeq = 0;
    for (size_t i = 0; i < x.size(); i += 12000) {
        rx.feed(x.data() + i, std::min<size_t>(12000, x.size() - i));
        P25Telemetry t;
        if (rx.telemetry(t, lastSeq)) { lastSeq = t.seq; for (const auto& r : t.tsbks) seen.insert(r.seq); }
    }
    const int sent = (int)std::lround(tx.durSec / (360.0 / kBaud)) * 3;
    return {100.0 * (double)seen.size() / std::max(1, sent), (int)seen.size(), sent};
}

} // namespace

int main() {
    const bool table = getenv("P25_SIMULCAST_TABLE") != nullptr;
    if (table) printf("%-8s %9s %9s %7s %8s\n", "signal", "delay us", "2nd dB", "dfHz", "blocks%");
    struct Row { double delay, ratio, df, snr; };
    const Row rows[] = {{-1, 0, 0, 25}, {0, -3, 5, 25}, {10, -3, 5, 25}, {20, -3, 5, 25}, {40, -3, 5, 25}, {80, -3, 5, 25}, {40, -10, 5, 25}, {40, 0, 5, 25}, {20, -3, 0.5, 25}};
    for (bool cq : {false, true})
        for (const Row& r : rows) {
            const Res x = run(cq, r.delay, r.ratio, r.df, r.snr);
            if (table) printf("%-8s %9.0f %9.1f %7.1f %7.1f%%  (%d of ~%d)\n", cq ? "CQPSK" : "C4FM", r.delay, r.ratio, r.df, x.pct, x.blocksOk, x.sent);
            if (!cq) CHECK(x.pct > 85 || r.delay >= 40 || r.ratio >= 0, "C4FM, %.0f us %.1f dB: %.1f %%", r.delay, r.ratio, x.pct);
        }
    return testSummary("p25 simulcast");
}
