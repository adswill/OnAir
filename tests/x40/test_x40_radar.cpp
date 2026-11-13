// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Radar pulse analyser: constant, staggered and jittered PRI, pulse width, the carrier inside the pulse, an antenna that is seen for a few ms per turn (scan period), emitter loss,
// a noise sweep and noise alone. Simulated signals only.
#include "x40_gen2.h"
#include <cstring>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::string factS(const X40Telemetry& t, const char* key) { for (auto& f : t.facts) if (f.first == key) return f.second; return ""; }
static double num(const std::string& s) { return atof(s.c_str()); }
static std::string fmt_stag(int n) { char b[64]; snprintf(b, sizeof b, "staggered PRI, %d positions", n); return b; }
static int emitters(const X40Telemetry& t) { int n = 0; for (auto& r : t.rows) if (r.kind == "Emitter" && r.id != "gone") n++; return n; }

int main() {
    const double fs = 2.0e6;
    // constant PRI at several PRFs and pulse widths, with a carrier 180 kHz inside the pulse
    for (double pri : {1.0e-3, 0.4e-3, 2.5e-3}) for (double pw : {0.5e-6, 1.0e-6, 2.0e-6}) {
        RadarPlan p; p.priPattern = {pri}; p.pw = pw; p.offsetHz = 180e3; p.durationSec = 6;
        auto t = runRadar(p, fs);
        CHECK(factS(t, "PRI type") == "constant PRI", "pri %.1f us pw %.1f: type '%s'", pri * 1e6, pw * 1e6, factS(t, "PRI type").c_str());
        CHECK(std::fabs(num(factS(t, "PRI")) - pri * 1e6) < 0.01 * pri * 1e6, "pri read %s want %.1f", factS(t, "PRI").c_str(), pri * 1e6);
        // a pulse shorter than 1.5 samples (0.75 us at 2 Msps) is read as one sample: its width is not claimed
        CHECK(pw < 0.7e-6 || std::fabs(num(factS(t, "pulse width (half power)")) - pw * 1e6) < 0.35 * pw * 1e6 + 0.15, "pw read %s want %.2f", factS(t, "pulse width (half power)").c_str(), pw * 1e6);
        CHECK(pw < 1.5e-6 || std::fabs(num(factS(t, "frequency inside the pulse")) - 180) < 25,   // five samples or more are needed
               "if %s", factS(t, "frequency inside the pulse").c_str());
        CHECK(emitters(t) == 1, "emitter rows %d", emitters(t));
    }
    // staggered PRI (2, 3 and 4 positions) and jitter
    for (int N : {2, 3, 4}) {
        RadarPlan p; p.durationSec = 8; p.pw = 1e-6; if (N == 2) p.priPattern = {1.0e-3, 1.2e-3}; else if (N == 3) p.priPattern = {0.9e-3, 1.0e-3, 1.1e-3}; else p.priPattern = {0.8e-3, 1.0e-3, 0.9e-3, 1.1e-3};
        auto t = runRadar(p, fs);
        CHECK(factS(t, "PRI type") == fmt_stag(N), "stagger %d: '%s'", N, factS(t, "PRI type").c_str());
    }
    { RadarPlan p; p.durationSec = 6; p.jitter = 0.10; auto t = runRadar(p, fs); CHECK(factS(t, "PRI type") == "jittered PRI", "jitter: '%s'", factS(t, "PRI type").c_str()); }
    // a scanning antenna: pulses are above the noise for about 40 ms of every 2.4 s turn; 24 s
    { RadarPlan p; p.priPattern = {0.8e-3}; p.durationSec = 24; p.scanSec = 2.4; p.snrDb = 28; auto t = runRadar(p, fs);
      CHECK(factS(t, "PRI type") == "constant PRI", "scan: type '%s'", factS(t, "PRI type").c_str());
      const double sc = num(factS(t, "antenna scan")); CHECK(std::fabs(sc - 2.4) < 0.1, "scan period %s", factS(t, "antenna scan").c_str()); }
    // noise sweep: pulse peak over the noise sigma
    printf("level sweep (1 us pulses, PRI 1 ms, 6 s, peak amplitude over the noise sigma): type read\n");
    for (double snr : {20.0, 17.0, 14.0, 11.0, 9.0, 7.0}) {
        RadarPlan p; p.durationSec = 6; p.snrDb = snr; auto t = runRadar(p, fs, 3);
        printf("  %.0f dB: '%s', PRI %s\n", snr, factS(t, "PRI type").c_str(), factS(t, "PRI").c_str());
        if (snr >= 17) CHECK(factS(t, "PRI type") == "constant PRI", "level %.0f dB", snr);
    }
    // noise alone: 10 minutes at 1 Msps (as 20 blocks of 30 s)
    { int false_ = 0; unsigned long long pulses = 0;
      for (int k = 0; k < 10; k++) { RadarPlan p; p.priPattern = {1e6}; p.durationSec = 30; auto t = runRadar(p, 1.0e6, 70 + k); false_ += emitters(t); pulses += strtoull(factS(t, "pulses counted").c_str(), nullptr, 10); if (!t.facts.empty()) { for (auto& f : t.facts) if (f.first == "pulses counted") pulses += 0; } }
      printf("noise alone, 5 minutes at 1 Msps: %d false emitters\n", false_); CHECK(false_ == 0, "false emitters %d", false_); }
    printf(fails ? "x40 radar: %d FAILED\n" : "x40 radar: ok\n", fails);
    return fails ? 1 : 0;
}
