// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Spectrum occupancy monitor: duty cycle and transmissions of channels against what the generator keyed, with a radio offset, a drifting radio, three input rates,
// the noise sweep and the false reports on noise alone. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Busy { double mhz, duty; int events; };
// "busiest k" facts: "145.1234 MHz: busy 49.7 % of the time, 10 transmissions, longest 1.0 s"
static std::vector<Busy> busiest(const X40Telemetry& t) {
    std::vector<Busy> v;
    for (auto& f : t.facts) if (f.first.rfind("busiest", 0) == 0) { Busy b{}; if (sscanf(f.second.c_str(), "%lf MHz: busy %lf %% of the time, %d", &b.mhz, &b.duty, &b.events) == 3) v.push_back(b); }
    return v;
}

// the centre of the channel that is d channels from the middle, as the monitor lays its grid out
static double chanCentre(double fs, int d) {
    const int chBins = std::max(2, (int)std::lround(25000 / (fs / 2048))), nCh = (int)std::floor(2 * 0.45 * 2048 / chBins);
    return (nCh / 2 + d - nCh / 2.0 + 0.5) * chBins * (fs / 2048);
}

int main() {
    // tones at known offsets from the tuned frequency (145 MHz); duty 0.5, 0.2, 0.9, 0.35
    struct Case { double fs, lo, drift; };
    for (Case c : {Case{2000000, 0, 0}, Case{2000000, 3000, 0}, Case{2000000, -2000, 40}, Case{1000000, 0, 0}, Case{4000000, 1500, 0}}) {
        const double cnr = 25;      // dB in 25 kHz
        ToneScene sc(c.fs, noiseFor(cnr, 25000, c.fs), 5);
        const int dch[4] = {-12, -3, 6, 15}; double hz[4]; for (int k = 0; k < 4; k++) hz[k] = chanCentre(c.fs, dch[k]) + 0.1 * 25000;
        const double duty[4] = {0.5, 0.2, 0.9, 0.35}, per[4] = {2.0, 1.0, 5.0, 1.6};
        for (int k = 0; k < 4; k++) { SchedTone s; s.hz = hz[k] + c.lo; s.driftHzPerSec = c.drift; s.duty = duty[k]; s.period = per[k]; s.phase0 = 0.3 * k; sc.tones.push_back(s); }
        auto t = runChunks(x40::Occupancy, c.fs, 40.0, std::ref(sc), 0, 145e6);   // the radio's error moves every tone by lo (and drifts it)
        auto b = busiest(t);
        CHECK(b.size() == 4, "fs %.0f lo %.0f: %zu busy channels listed, want 4 (%s)", c.fs, c.lo, b.size(), t.headline.c_str());
        for (int k = 0; k < 4; k++) {
            // the tone ends at hz + lo(mixed by the receiver by -lo): find the listed channel within 14 kHz
            bool found = false;
            for (auto& r : b) if (std::fabs(r.mhz * 1e6 - (145e6 + hz[k])) < 16000) {
                found = true;
                CHECK(std::fabs(r.duty - 100 * duty[k]) < 3.5, "fs %.0f: %.0f kHz duty %.1f want %.0f", c.fs, hz[k] / 1000, r.duty, 100 * duty[k]);
                const int want = (int)(40.0 / per[k]); CHECK(std::abs(r.events - want) <= 1, "fs %.0f: %.0f kHz events %d want %d", c.fs, hz[k] / 1000, r.events, want);
            }
            CHECK(found, "fs %.0f lo %.0f: tone %.0f kHz not found", c.fs, c.lo, hz[k] / 1000);
        }
        CHECK(t.rows.size() > 20, "rows %zu", t.rows.size());
    }
    // weak tones: the lowest carrier to noise (25 kHz) at which a 50 % channel is still found, and the false channels
    printf("noise sweep (tone to noise in 25 kHz): channels found of 2, duty read of 50 %%, false channels\n");
    for (double cnr : {20.0, 12.0, 9.0, 7.0, 5.0}) {
        ToneScene sc(2e6, noiseFor(cnr, 25000, 2e6), 9);
        for (int k = 0; k < 2; k++) { SchedTone s; s.hz = k ? chanCentre(2e6, 8) : chanCentre(2e6, -5); s.period = 2.0; s.duty = 0.5; sc.tones.push_back(s); }
        auto t = runChunks(x40::Occupancy, 2e6, 30.0, std::ref(sc), 0, 145e6);
        auto b = busiest(t); int good = 0, falseCh = 0;
        for (auto& r : b) { const bool real = std::fabs(r.mhz * 1e6 - 145e6 - chanCentre(2e6, 8)) < 14000 || std::fabs(r.mhz * 1e6 - 145e6 - chanCentre(2e6, -5)) < 14000; if (real) good++; else falseCh++; }
        printf("  %.0f dB: found %d, duty %.1f, false %d\n", cnr, good, b.empty() ? 0.0 : b[0].duty, falseCh);
        if (cnr >= 9) { CHECK(good == 2, "%.0f dB: found %d of 2", cnr, good); CHECK(falseCh == 0, "%.0f dB false channels %d", cnr, falseCh); }
    }
    // noise alone, two input rates, 5 minutes in all: no transmission logged
    {
        int falseRows = 0;
        for (double fs : {2e6, 1e6}) { ToneScene sc(fs, 1.0, 21); auto t = runChunks(x40::Occupancy, fs, 150.0, std::ref(sc), 0, 100e6, 1.0); falseRows += (int)t.rows.size(); for (auto& f : t.facts) if (f.first == "busy now" && atoi(f.second.c_str()) != 0) falseRows++; }
        printf("noise only, 2 x 150 s: %d false reports\n", falseRows);
        CHECK(falseRows == 0, "false reports %d", falseRows);
    }
    // a steady carrier all the time: duty 100 %, one long transmission never ended: listed with no event yet
    { ToneScene sc(2e6, noiseFor(25, 25000, 2e6), 3); SchedTone s; s.hz = 55000; sc.tones.push_back(s);
      auto t = runChunks(x40::Occupancy, 2e6, 10.0, std::ref(sc), 0, 145e6); auto b = busiest(t); CHECK(b.size() == 1 && b[0].duty > 96, "steady carrier duty"); }
    printf(fails ? "x40 occupancy: %d FAILED\n" : "x40 occupancy: ok\n", fails);
    return fails ? 1 : 0;
}
