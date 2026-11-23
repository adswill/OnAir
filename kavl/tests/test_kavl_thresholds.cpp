// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAVL decode thresholds: for every modcod, the lowest Es/N0 (0.5 dB steps) at which at least 99% of the code words arrive intact, on a plain noise channel
// and through RTL-SDR-like receiver impairments (kavl_test_util.h rtlChannel). Slow: only with KAVL_MEASURE=1 (else it passes at once).
// KAVL_MEASURE_FRAMES=n frames per point (default 40), KAVL_MEASURE_ONLY=<modcod id> one modcod.
// Goal gate (owner, 2026-11-23): the 1440p60 rate needs 256-QAM 10/15 or 11/15 at 20 dB or less in 500 kHz; the test fails when 256-QAM 10/15 needs more
// than 20 dB on the noise channel.
#include "kavl_test_util.h"
#include <cstdlib>
#include <string>

using namespace kavl;
using namespace kavltest;

namespace {

// a rough starting point 2 dB below the expected threshold
double guess(const Modcod& m) {
    const double bits = m.bitsPerSym * m.rate15 / 15.0;
    return 10.0 * std::log10(std::pow(2.0, bits) - 1.0) - 1.0;
}

double threshold(int m, bool rtl, int frames, bool verbose) {
    const Modcod& mc = *modcod(m);
    for (double s = guess(mc); s < 45.0; s += 0.5) {
        Channel c = rtl ? rtlChannel(s) : Channel();
        c.esn0Db = s;
        if (!rtl) {
            c.cfoHz = 1234.0;
            c.delaySamples = 0.21;
        }
        const auto r = runLink(m, frames, c, 1000 + static_cast<uint64_t>(s * 10));
        const double ok = r.blocks ? static_cast<double>(r.blocksOk) / r.blocks : 0.0;
        if (verbose)
            std::printf("  %-15s %s %5.1f dB: frames %d/%d code words %d/%d (%.1f%%) est %.1f MER %.1f\n", mc.name, rtl ? "rtl " : "awgn", s, r.framesFound,
                        r.framesSent, r.blocksOk, r.blocks, 100.0 * ok, r.esn0Est, r.merDb);
        if (ok >= 0.99) return s;
    }
    return 99.0;
}

}  // namespace

int main() {
    const char* e = std::getenv("KAVL_MEASURE");
    if (!e || std::string(e) != "1") {
        std::printf("test_kavl_thresholds: skipped (set KAVL_MEASURE=1 to measure)\n");
        return 0;
    }
    int frames = 40;
    if (const char* f = std::getenv("KAVL_MEASURE_FRAMES")) frames = std::max(10, std::atoi(f));
    int only = -1;
    if (const char* o = std::getenv("KAVL_MEASURE_ONLY")) only = std::atoi(o);
    const bool verbose = std::getenv("KAVL_MEASURE_VERBOSE") && std::string(std::getenv("KAVL_MEASURE_VERBOSE")) == "1";
    std::printf("KAVL thresholds, %d frames per point, %.0f ksym/s, %.0f kHz occupied\n", frames, kDefaultSymRate / 1e3, occupiedHz(kDefaultSymRate) / 1e3);
    std::printf("| Modcod | Net rate (Mbit/s) | Noise channel (dB) | RTL-SDR channel (dB) |\n|---|---|---|---|\n");
    double goal = 99.0;
    for (int m = 0; m < kNumModcods; m++) {
        if (only >= 0 && m != only) continue;
        const Modcod& mc = *modcod(m);
        const double a = threshold(m, false, frames, verbose);
        const double r = threshold(m, true, frames, verbose);
        if (m == 3) goal = a;
        std::printf("| %s | %.3f | %.1f | %.1f |\n", mc.name, netBitRate(mc, kDefaultSymRate) / 1e6, a, r);
        std::fflush(stdout);
    }
    if (only < 0 || only == 3) {
        std::printf("goal: 256-QAM 10/15 at %.1f dB (must be <= 20 dB): %s\n", goal, goal <= 20.0 ? "PASS" : "FAIL");
        return goal <= 20.0 ? 0 : 1;
    }
    return 0;
}
