// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ionospheric sounder processing on the test signal: two sounders (100 kHz/s every 30 s at +2 s, 500 kHz/s every 20 s at +7 s), each on
// two delayed paths, seen by a 1 Msps radio at 0.5 MHz (a narrow radio: 0.05 .. 0.95 MHz of the band). The rates, periods and start
// offsets are found, the echo delays land in the right group-range bin of each ionogram, and noise alone gives no sounder.
#include "dect2/iono/iono_dsp.h"
#include "dect2/iono/iono_gen.h"
#include "dect2/iono/iono_rx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static void run(iono::Processor& p, ModeSynth& s, double sec, double fs) {
    std::vector<cf32> b(65536);
    for (double t = 0; t < sec; t += b.size() / fs) { s.generate(b.data(), b.size()); p.feed(b.data(), b.size()); }
}

// the delay profile of the selected ionogram (linear power summed over the swept columns) and its two strongest separated peaks
static void peaks(const IonoTelemetry& t, int& p1, int& p2) {
    std::vector<double> prof(iono::kRows, 0.0);
    for (int c = 0; c < iono::kCols; c++) {
        if (t.swept.empty() || !t.swept[(size_t)c]) continue;
        for (int j = 0; j < iono::kRows; j++) prof[(size_t)j] += std::pow(10.0, t.ionoDb[(size_t)j * iono::kCols + (size_t)c] / 10);
    }
    p1 = (int)(std::max_element(prof.begin(), prof.end()) - prof.begin());
    p2 = -1;
    for (int j = 0; j < iono::kRows; j++) if (std::abs(j - p1) > 3 && (p2 < 0 || prof[(size_t)j] > prof[(size_t)p2])) p2 = j;
}

int main() {
    const double fs = 1e6, fc = 0.5e6;
    SynthConfig sc;
    sc.modeVal[0] = fc / 1e6;
    {
        auto syn = makeIonoSynth(sc, fs);
        iono::Processor p;
        p.configure(fs, fc, 0);
        run(p, *syn, 50, fs);
        IonoTelemetry t;
        p.fill(t);
        printf("  detector FFT %d, %zu sounders, %zu sweeps, %s\n", p.detectorFft(), t.sounders.size(), t.timeline.size(), ionoSummary(t).c_str());
        for (const auto& s : t.sounders)
            printf("  sounder %d: %.0f kHz/s, period %.1f s, offset %.3f s, %d sweeps, %d missed, %.1f dB, %.2f - %.2f MHz\n", s.id, s.rateHz / 1e3,
                   s.periodSec, s.offsetSec, s.sweeps, s.missed, s.snrDb, s.fLoHz / 1e6, s.fHiHz / 1e6);
        for (const auto& e : t.timeline) printf("    sweep at %.3f s, sounder %d, %.2f - %.2f MHz%s\n", e.t0, e.sounder, e.fLoHz / 1e6, e.fHiHz / 1e6, e.predicted ? " (expected)" : "");
        CHECK(t.sounders.size() == 2, "%zu sounders", t.sounders.size());
        for (int k = 0; k < 2; k++) {
            const IonoSynthSounder& want = kIonoSynth[k];
            int idx = -1;
            for (size_t i = 0; i < t.sounders.size(); i++) if (t.sounders[i].rateHz == want.rateHz) idx = (int)i;
            CHECK(idx >= 0, "no sounder of %.0f kHz/s", want.rateHz / 1e3);
            if (idx < 0) continue;
            const IonoSounder& s = t.sounders[(size_t)idx];
            CHECK(std::fabs(s.periodSec - want.periodSec) < 1e-6, "%.0f kHz/s: period %.3f", want.rateHz / 1e3, s.periodSec);
            CHECK(std::fabs(s.offsetSec - want.offsetSec) < 1e-6, "%.0f kHz/s: offset %.3f", want.rateHz / 1e3, s.offsetSec);
            const int sweepsWant = k == 0 ? 2 : 3;
            CHECK(s.sweeps == sweepsWant && s.missed == 0, "%.0f kHz/s: %d sweeps, %d missed", want.rateHz / 1e3, s.sweeps, s.missed);
            p.setSelected(idx);
            IonoTelemetry u;
            p.fill(u);
            int p1, p2;
            peaks(u, p1, p2);
            const int w1 = (int)std::lround(want.delaySec[0] / iono::kDelayBin), w2 = (int)std::lround(want.delaySec[1] / iono::kDelayBin);
            printf("  %.0f kHz/s ionogram: echoes at %.0f and %.0f km (want %.0f and %.0f)\n", want.rateHz / 1e3, iono::groupRangeKm(iono::rowDelay(p1)),
                   iono::groupRangeKm(iono::rowDelay(p2)), iono::groupRangeKm(want.delaySec[0]), iono::groupRangeKm(want.delaySec[1]));
            CHECK(std::abs(p1 - w1) <= 1 && std::abs(p2 - w2) <= 1, "%.0f kHz/s: rows %d, %d want %d, %d", want.rateHz / 1e3, p1, p2, w1, w2);
            int swept = 0;
            for (uint8_t v : u.swept) swept += v;
            CHECK(swept >= 15 && swept <= 19, "%.0f kHz/s: %d columns swept", want.rateHz / 1e3, swept);
            const std::string csv = iono::ionogramCsv(u);
            CHECK(std::count(csv.begin(), csv.end(), '\n') == 1 + swept * iono::kRows, "csv lines");
        }
        // coverage of the narrow radio: 0.05 .. 0.95 MHz
        int cov = 0;
        for (uint8_t v : t.covered) cov += v;
        CHECK(t.covered.size() == (size_t)iono::kCols && t.covered[1] && t.covered[18] && !t.covered[19] && cov == 18, "coverage %d columns", cov);
    }
    {   // 2.4 Msps at 1.2 MHz with the timing not locked: the delays are relative, the spacing of the echoes stays
        SynthConfig c2;
        c2.modeVal[0] = 1.2;
        auto syn = makeIonoSynth(c2, 2.4e6);
        iono::Processor p;
        p.configure(2.4e6, 1.2e6, 0);
        p.setLocked(false);
        run(p, *syn, 13, 2.4e6);
        IonoTelemetry t;
        p.fill(t);
        CHECK(t.sounders.size() == 2, "unlocked: %zu sounders", t.sounders.size());
        for (size_t i = 0; i < t.sounders.size(); i++) {
            const int k = t.sounders[i].rateHz == kIonoSynth[0].rateHz ? 0 : 1;
            p.setSelected((int)i);
            IonoTelemetry u;
            p.fill(u);
            int p1, p2;
            peaks(u, p1, p2);
            const int want = (int)std::lround((kIonoSynth[k].delaySec[1] - kIonoSynth[k].delaySec[0]) / iono::kDelayBin);
            printf("  unlocked %.0f kHz/s: echoes %d rows apart (want %d)\n", t.sounders[i].rateHz / 1e3, p2 - p1, want);
            CHECK(std::abs(p2 - p1 - want) <= 1, "unlocked %.0f kHz/s: rows %d, %d", t.sounders[i].rateHz / 1e3, p1, p2);
        }
    }
    {   // noise only
        sc.modeOpt[0] = 1;
        auto syn = makeIonoSynth(sc, fs);
        iono::Processor p;
        p.configure(fs, fc, 0);
        run(p, *syn, 30, fs);
        IonoTelemetry t;
        p.fill(t);
        printf("  noise: %zu sounders, %llu hits\n", t.sounders.size(), (unsigned long long)t.hits);
        CHECK(t.sounders.empty() && t.timeline.empty(), "noise gave %zu sounders", t.sounders.size());
    }
    printf(fails ? "iono dsp: %d FAILED\n" : "iono dsp: all passed\n", fails);
    return fails ? 1 : 0;
}
