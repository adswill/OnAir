// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal workbench: measurements on synthetic recordings: peak finder accuracy, bandwidth, occupied bandwidth, SNR, symbol rate, bursts.
#include "dect2/wb/wb_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/wb/wb_measure.h"
#include "wb_test.h"
#include <random>
using namespace dect2::wb;
using dect2::cf32;

int main() {
    const double rate = 200000;
    {   // three tones: frequency to a few Hz, level to 1 dB
        const size_t N = 1 << 20;
        std::vector<cf32> x(N);
        const double f[3] = {-50300.0, 12500.0, 71234.0}, a[3] = {0.5, 0.2, 0.05};
        for (size_t i = 0; i < N; i++) {
            cf32 v(0, 0);
            for (int k = 0; k < 3; k++) v += (float)a[k] * cf32((float)std::cos(2 * M_PI * f[k] * (double)i / rate), (float)std::sin(2 * M_PI * f[k] * (double)i / rate));
            x[i] = v;
        }
        addNoise(x, 1e-5, 3);
        auto src = Source::fromSamples(x, rate);
        MeasureOptions o; o.fftSize = 16384; o.ident = false; o.symbolRate = false; o.bursts = false;
        Measurement m;
        CHECK(measure(*src, 0, N, 0, 0, o, m) && m.valid, "measure failed");
        CHECK(m.peaks.size() >= 3, "peaks %zu", m.peaks.size());
        for (int k = 0; k < 3 && k < (int)m.peaks.size(); k++) {
            int order[3] = {0, 1, 2};   // strongest first: 0.5, 0.2, 0.05
            const Peak& p = m.peaks[(size_t)k];
            printf("  peak %d: %.2f Hz (want %.2f), %.2f dB (want %.2f)\n", k, p.freqHz, f[order[k]], p.levelDb, 20 * std::log10(a[order[k]]));
            CHECK(std::fabs(p.freqHz - f[order[k]]) < 3.0, "peak %d at %.2f, want %.2f", k, p.freqHz, f[order[k]]);
            CHECK(std::fabs(p.levelDb - 20 * std::log10(a[order[k]])) < 1.0, "peak %d level %.2f", k, p.levelDb);
        }
        CHECK(std::fabs(m.carrierHz - f[0]) < 3.0, "carrier %.2f", m.carrierHz);
        // a band round the second tone only
        Measurement m2;
        CHECK(measure(*src, 0, N, 5000, 20000, o, m2) && !m2.peaks.empty() && std::fabs(m2.peaks[0].freqHz - 12500) < 3, "band measure");
    }
    {   // band-limited noise 20 kHz wide in white noise: widths and SNR
        const size_t N = 1 << 20;
        std::vector<cf32> x(N);
        std::mt19937 rng(5);
        dect2::normal_distribution<float> nd(0, 1);
        for (auto& v : x) v = cf32{nd(rng), nd(rng)};
        // low-pass at 10 kHz: a long moving filter via the window designer
        const std::vector<float> h = designLowpass(10000 / rate, 2000 / rate);
        std::vector<cf32> y(N);
        for (size_t i = h.size(); i < N; i++) { cf32 a(0, 0); for (size_t k = 0; k < h.size(); k++) a += x[i - k] * h[k]; y[i] = a; }
        double ps = 0;
        for (size_t i = h.size(); i < N; i++) ps += std::norm(y[i]);
        ps /= (double)(N - h.size());
        const double snrWant = 20;   // dB in the band: noise power over the whole 200 kHz is ps / 100 * 10 (the band is a tenth of it)
        const double pn = ps / std::pow(10.0, snrWant / 10) * 10;
        addNoise(y, pn, 9);
        auto src = Source::fromSamples(y, rate);
        MeasureOptions o; o.fftSize = 4096; o.ident = false; o.symbolRate = false; o.bursts = false;
        Measurement m;
        CHECK(measure(*src, 0, N, 0, 0, o, m), "measure");
        printf("  noise band: obw %.0f Hz, -3 dB %.0f Hz, -20 dB %.0f Hz (limited %d), snr %.1f dB, centre %.0f Hz, floor %.1f dB\n", m.obwHz, m.bw3Hz, m.bw20Hz, m.bw20Limited, m.snrDb, m.centreHz, m.noiseFloorDb);
        CHECK(std::fabs(m.obwHz - 20000) < 1500, "obw %.0f", m.obwHz);
        CHECK(std::fabs(m.bw3Hz - 20000) < 2500, "bw3 %.0f", m.bw3Hz);
        CHECK(std::fabs(m.snrDb - snrWant) < 2.5, "snr %.1f", m.snrDb);
        CHECK(std::fabs(m.centreHz) < 500, "centre %.0f", m.centreHz);
    }
    {   // symbol rate from the numbers of the signal alone
        struct S { const char* n; int kind; double rate, baud; bool diff; };
        const S cases[] = {{"2-FSK", KFsk2, 19200, 1200, false}, {"4-FSK", KFsk4, 38400, 2400, false}, {"GMSK", KGmsk, 38400, 2400, false}, {"OOK", KAsk, 16000, 1000, false},
                           {"BPSK", KBpsk, 38400, 2400, false}, {"QPSK", KQpsk, 38400, 2400, true}, {"8PSK", KPsk8, 38400, 2400, true}};
        for (const S& c : cases) {
            GenSpec g; g.kind = c.kind; g.rate = c.rate; g.baud = c.baud; g.nbits = 6000; g.snrDb = 25; g.differential = c.diff; g.offsetHz = 0;
            GenResult r = generate(g);
            SymEstimate e;
            const bool ok = estimateSymbolRate(r.x.data(), r.x.size(), r.rate, e);
            printf("  symbol rate %-6s: %.1f Bd (%.1f dB, %s), want %.0f\n", c.n, e.baud, e.strengthDb, e.method.c_str(), c.baud);
            CHECK(ok && std::fabs(e.baud - c.baud) < 0.02 * c.baud, "%s: %.1f vs %.0f", c.n, e.baud, c.baud);
        }
    }
    {   // bursts: a keyed signal between noise
        GenSpec g; g.kind = KFsk2; g.rate = 19200; g.baud = 1200; g.nbits = 600; g.snrDb = 25; g.leadSec = 0.2; g.tailSec = 0.3;
        GenResult r = generate(g);
        auto src = Source::fromSamples(r.x, r.rate);
        MeasureOptions o; o.ident = false; o.fftSize = 2048;
        Measurement m;
        CHECK(measure(*src, 0, r.x.size(), 0, 0, o, m), "measure");
        CHECK(m.bursts.size() == 1, "bursts %zu", m.bursts.size());
        if (m.bursts.size() == 1) {
            const double len = (double)(r.signalEnd - r.signalStart) / r.rate;
            printf("  burst: start %.4f s (want 0.2), length %.4f s (want %.4f), duty %.2f\n", m.bursts[0].startSec, m.bursts[0].lenSec, len, m.duty);
            CHECK(std::fabs(m.bursts[0].startSec - 0.2) < 0.01, "burst start %.4f", m.bursts[0].startSec);
            CHECK(std::fabs(m.bursts[0].lenSec - len) < 0.03 * len, "burst length %.4f", m.bursts[0].lenSec);
        }
        CHECK(m.sym.baud > 0 && std::fabs(m.sym.baud - 1200) < 30, "symbol rate in measure(): %.1f", m.sym.baud);
        // the same with the signal identifier on
        o.ident = true;
        Measurement m2;
        CHECK(measure(*src, 0, r.x.size(), 0, 0, o, m2), "measure with ident");
        printf("  ident: %s%s\n", m2.identValid ? m2.ident.summary.c_str() : "(not valid)", "");
    }
    {   // the CSV of the peaks
        Measurement m;
        m.peaks.push_back({1234.5, -20.25, 12.0});
        const std::string csv = peaksCsv(m, 100e6);
        CHECK(csv.find("1,1234.500,100001234.500,-20.25,12.000") != std::string::npos, "csv: %s", csv.c_str());
    }
    printf(fails ? "FAILED\n" : "wb_measure ok\n");
    return fails ? 1 : 0;
}
