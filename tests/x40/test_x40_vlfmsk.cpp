// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VLF MSK monitor: bit rate and carrier from the two lines of the squared signal, at 50, 100, 200 and 300 bit/s, carrier offset and drift, off-centre tuning, on / off / rate change logging,
// the station hint, a noise sweep and noise alone. Simulated signals only.
#include "x40_gen2.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::string factS(const X40Telemetry& t, const char* key) { for (auto& f : t.facts) if (f.first == key) return f.second; return ""; }
static double num(const std::string& s) { return atof(s.c_str()); }
static X40Telemetry go(const std::vector<cf32>& x, double fs, double offsetHz = 0, double centre = 0) { X40Receiver rx; rx.setVariant(x40::VlfMsk); rx.setCenterHz(centre); rx.configure(fs); rx.setSignalOffset(offsetHz);
    for (size_t i = 0; i < x.size(); i += 4096) rx.feed(x.data() + i, std::min<size_t>(4096, x.size() - i)); X40Telemetry t; rx.telemetry(t, 0); return t; }

int main() {
    for (double fs : {8000.0, 48000.0}) for (double rate : {50.0, 100.0, 200.0, 300.0}) for (double fc : {0.0, 140.0, -260.0}) {
        Msk m; m.rateBd = rate; m.fcHz = fc; m.driftHzPerSec = fc == 0 ? 0.5 : 0; m.snrDb = 12;
        auto t = go(mskSignal(m, fs, 40.0), fs);
        const double expectFc = fc + (fc == 0 ? 0.5 * 34.0 : 0);   // the drift: the last 4 s window of the run is centred 34 s in
        CHECK(factS(t, "state").rfind("MSK", 0) == 0, "fs %.0f rate %.0f fc %.0f: state '%s'", fs, rate, fc, factS(t, "state").c_str());
        CHECK(std::fabs(num(factS(t, "bit rate")) - rate) < 2.5, "rate read %s want %.0f", factS(t, "bit rate").c_str(), rate);
        CHECK(std::fabs(num(factS(t, "carrier")) - expectFc) < 4.0, "carrier read %s want %.1f", factS(t, "carrier").c_str(), expectFc);
    }
    // off-centre tuning that is announced to the receiver
    { Msk m; m.fcHz = 600; auto t = go(mskSignal(m, 48000, 40.0), 48000, 600); CHECK(std::fabs(num(factS(t, "carrier"))) < 3.0, "off-centre carrier %s", factS(t, "carrier").c_str()); }
    // on from 8 s to 32 s: one "on" row, one "off" row; the rate changes 50 to 100 at 20 s: one "changed" row
    { Msk m; m.snrDb = 12; m.from = 8; m.to = 32; auto t = go(mskSignal(m, 8000, 48.0), 8000); int on = 0, off = 0; for (auto& r : t.rows) { if (r.id == "on") on++; if (r.id == "off") off++; } CHECK(on == 1 && off == 1, "on %d off %d", on, off); }
    { Msk a; a.rateBd = 50; a.snrDb = 12; a.to = 24; Msk b; b.rateBd = 100; b.snrDb = 12; b.from = 0; b.seed = 2; auto xa = mskSignal(a, 8000, 24.0), xb = mskSignal(b, 8000, 24.0); xa.insert(xa.end(), xb.begin(), xb.end()); auto t = go(xa, 8000);
      int ch = 0; for (auto& r : t.rows) if (r.id == "changed") ch++; CHECK(ch == 1, "rate change rows %d", ch); }
    // the station hint: tuned 24.0 kHz, carrier measured at +30 Hz: NAA; 25.0 kHz: nothing listed
    { Msk m; m.fcHz = 30; m.snrDb = 12; auto t = go(mskSignal(m, 8000, 40.0), 8000, 0, 24000.0); CHECK(factS(t, "carrier near the listed").find("NAA") != std::string::npos, "hint '%s'", factS(t, "carrier near the listed").c_str());
      auto u = go(mskSignal(m, 8000, 40.0), 8000, 0, 25000.0); CHECK(factS(u, "carrier near the listed").empty(), "hint where none is listed"); }
    // noise sweep: signal power over the noise in 2 kHz, 200 bit/s, 40 s
    printf("noise sweep (200 bit/s MSK, 40 s, signal over the noise in 2 kHz): state, rate read\n");
    for (double snr : {12.0, 6.0, 3.0, 0.0, -3.0, -6.0}) {
        Msk m; m.snrDb = snr; auto t = go(mskSignal(m, 8000, 40.0), 8000);
        printf("  %+.0f dB: '%s', rate %s\n", snr, factS(t, "state").c_str(), factS(t, "bit rate").c_str());
        if (snr >= 3) CHECK(factS(t, "state") == "MSK 200 bit/s" && std::fabs(num(factS(t, "bit rate")) - 200) < 3, "level %+.0f dB", snr);
    }
    // noise alone: 20 minutes in 30 runs of 40 s
    { int falseOn = 0; for (int k = 0; k < 30; k++) { Msk m; m.to = 0; m.seed = 50 + k; m.snrDb = 0; auto t = go(mskSignal(m, 8000, 40.0), 8000); for (auto& r : t.rows) if (r.id == "on") falseOn++; }
      printf("noise alone, 20 minutes: %d false transmitters\n", falseOn); CHECK(falseOn == 0, "false on rows %d", falseOn); }
    printf(fails ? "x40 vlfmsk: %d FAILED\n" : "x40 vlfmsk: ok\n", fails);
    return fails ? 1 : 0;
}
