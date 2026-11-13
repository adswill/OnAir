// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sensitivity table of the slot modes: the SNR (in 2500 Hz, AWGN) where half of the messages decode, measured through the real modulators
// (ftxAddTransmissionEx) and the real decoder (HfdigFtx), against the threshold the WSJT-X User Guide publishes for each mode (its
// "S/N threshold" column). Several stations per slot at the same SNR (spread over the passband, each with its own time and frequency offset),
// a receiver clock error and a slow drift. By default a quick run: three SNR points per mode and a regression bound; "--full" prints the
// whole curve (more points, more slots). Nothing is transmitted.
#include "../hfdig/hfdig_wsjt_testutil.h"
#include "../common/test_util.h"
#include "../common/test_long.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;
using namespace wsjttest;

struct ModeRow {
    int mode, sub;
    double period;
    const char* name;
    double ref;          // WSJT-X User Guide threshold, dB in 2500 Hz (NAN: none published)
    double bound;        // the regression bound: half the messages must decode at or below this SNR (measured on this decoder, plus 1 dB)
    int stations;        // per slot
    double lo, hi;       // the audio span the stations are spread over
    double dtMax;        // time offsets up to +- this (s)
};

static std::string msgFor(int mode, int i) {
    static const char* calls[] = {"K1ABC", "W9XYZ", "DL1ABC", "G4XYZ", "JA1ABC", "VK2XYZ", "OH2ABC", "SP5XYZ", "EA3ABC", "PY2XYZ", "ZL1ABC", "LY2XYZ"};
    static const char* grids[] = {"FN42", "EN61", "JO62", "IO91", "PM95", "QF56", "KP20", "KO02", "JN11", "GG66", "RF73", "KO24"};
    const char* c = calls[i % 12];
    const char* g = grids[i % 12];
    if (mode == 3 || mode == 9) return std::string(c) + " " + g + " 37";   // WSPR / FST4W: call, grid, power
    return std::string("CQ ") + c + " " + g;
}

// fraction of the messages decoded at snr (n slots)
static double rate(const ModeRow& r, double snr, int slots, uint64_t seed, bool impair) {
    int sent = 0, got = 0;
    for (int s = 0; s < slots; s++) {
        std::vector<St> st;
        uint64_t q = seed * 7919 + (uint64_t)s * 104729 + (uint64_t)(snr * 10 + 1000);
        auto rnd = [&]() { q = q * 6364136223846793005ull + 1442695040888963407ull; return (double)(q >> 11) / 9007199254740992.0; };
        for (int i = 0; i < r.stations; i++) {
            const double span = (r.hi - r.lo) / r.stations;
            const double hz = r.lo + span * (i + 0.25 + 0.5 * rnd());
            st.push_back({msgFor(r.mode, i + s * r.stations), hz, snr, (2 * rnd() - 1) * r.dtMax});
        }
        Imp f; f.sub = r.sub; f.period = r.period;
        if (impair) { f.ppm = 15; f.drift = r.mode == 3 || r.mode == 9 ? 0.001 : 0.005; }   // a sound card clock error and a slow drift (0.12 / 0.3 Hz per slot)
        const auto a = audio(r.mode, st, 1, f, seed + (uint64_t)s * 31 + (uint64_t)(snr * 100 + 5000));
        const auto d = decode(a, r.mode, f);
        for (const auto& x : st) { sent++; if (find(d, x.msg)) got++; }
    }
    return sent ? (double)got / sent : 0;
}

int main(int argc, char** argv) {
    const bool full = argc > 1 && !strcmp(argv[1], "--full");
    const char* only = argc > 2 ? argv[2] : nullptr;   // --full NAME: one mode
    // WSJT-X User Guide, "Modulation" / "S/N threshold" (dB in 2500 Hz): FT8 -21, FT4 -17.5, JT4A -23, JT9A -27, JT65A -25, Q65-60A -24.9,
    // FST4-60 -28.1, FST4W-120 -32.8, WSPR -31, MSK144 (one 72 ms frame, no averaging) about -8. FT2 is not a WSJT-X mode (no published value).
    const ModeRow rows[] = {
        {0, 0, 0, "FT8", -21, -18.5, 8, 300, 2700, 0.5},
        {1, 0, 0, "FT4", -17.5, -13.5, 6, 300, 2700, 0.3},
        {2, 0, 0, "FT2", NAN, -11.5, 6, 300, 2700, 0.2},
        {3, 0, 0, "WSPR", -31, -27.5, 4, 1420, 1580, 1.0},
        {4, 0, 0, "JT65A", -25, -21.5, 6, 300, 2500, 1.0},
        {5, 0, 0, "JT9A", -27, -23.5, 8, 300, 2700, 1.0},
        {10, 0, 0, "JT4A", -23, -22.0, 6, 300, 2500, 1.0},
        {6, 0, 60, "Q65-60A", -24.9, -21.5, 6, 300, 2500, 1.0},
        {7, 0, 60, "FST4-60", -28.1, -24.0, 6, 300, 2500, 1.0},
        {9, 0, 120, "FST4W-120", -32.8, -27.0, 3, 1420, 1580, 1.0},
        {8, 0, 0, "MSK144", -8, -3.5, 1, 1400, 1600, 0.0},
    };
    printf("%-10s %8s %8s %8s   decoded fraction per SNR\n", "mode", "WSJT-X", "50 % at", "bound");
    for (const ModeRow& r : rows) {
        if (only && strcmp(only, r.name)) continue;
        const double c = std::isnan(r.ref) ? r.bound - 1 : r.ref;
        std::vector<double> pts;
        if (full) for (double x = c - 4; x <= c + 6.01; x += 1) pts.push_back(x);
        // the quick run decodes at the bound only (the point 3 dB below is printed, not checked; 3 dB above is checked with the clock error and drift,
        // the harder case); ONAIR_TEST_LONG=1: all three points
        else if (testLong()) pts = {r.bound - 3, r.bound, r.bound + 3};
        else pts = {r.bound};
        const int slots = full ? 4 : 1;
        std::vector<double> fr;
        std::string line;
        for (double x : pts) {
            fr.push_back(rate(r, x, slots, 11, false));
            char b[32]; snprintf(b, sizeof b, " %+.1f:%.2f", x, fr.back()); line += b;
        }
        // the 50 % point: linear between the first point at or above one half and the one before it
        double fifty = NAN;
        for (size_t i = 0; i < fr.size(); i++)
            if (fr[i] >= 0.5) { fifty = i == 0 ? pts[0] : pts[i - 1] + (0.5 - fr[i - 1]) / std::max(1e-9, fr[i] - fr[i - 1]) * (pts[i] - pts[i - 1]); break; }
        char refs[16]; if (std::isnan(r.ref)) snprintf(refs, sizeof refs, "-"); else snprintf(refs, sizeof refs, "%.1f", r.ref);
        printf("%-10s %8s %8.1f %8.1f  %s\n", r.name, refs, fifty, r.bound, line.c_str());
        // regression bound: at the bound at least half decode, and 3 dB above it nearly all
        const size_t ib = full || !testLong() ? 0 : 1;
        if (!full) {
            CHECK(fr[ib] >= 0.5, "%s: only %.0f %% decoded at %.1f dB (the bound)", r.name, 100 * fr[ib], r.bound);
            if (testLong()) CHECK(fr[2] >= 0.8, "%s: only %.0f %% decoded at %.1f dB", r.name, 100 * fr[2], r.bound + 3);
            // the same 3 dB above the bound with a clock error and a drift
            const double imp = rate(r, r.bound + 3, 1, 23, true);
            printf("%-10s with 15 ppm and drift at %+.1f dB: %.2f\n", r.name, r.bound + 3, imp);
            CHECK(imp >= 0.8, "%s: only %.0f %% decoded at %.1f dB with clock error and drift", r.name, 100 * imp, r.bound + 3);
        } else CHECK(!std::isnan(fifty) && fifty <= r.bound, "%s: 50 %% at %.1f dB, bound %.1f", r.name, fifty, r.bound);
    }
    return testSummary("ham_snr");
}
