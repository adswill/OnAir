// 3.9 sensitivity pass D (docs/SENSITIVITY-3.9-D.md): the 90 % threshold of the HF ARQ decoders (ARQ-E, ARQ-N, G-TOR) on their own
// generator's signal (24 kHz complex, hfu_fsk.h) with white noise; SNR = signal power over the noise in 3 kHz, as in test_hfutil_fsk.cpp.
// A trial passes when the whole message is read; each trial has its own noise and carrier phase and a tuning error of +-30 Hz.
// Default: three trials at the recorded threshold + 2 dB per mode must pass. ONAIR_TEST_LONG=1: the sweep (10 trials a point, 1 dB steps).
// ONAIR_HFUSENS_CASE=<name prefix> runs one case; HFU_FORCE=1 gives the decoder the baud and the shift (as under Advanced).
// Simulated signals only.
#include "dect2/hfutil/hfu_fsk.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>
#include <ctime>
using namespace dect2;
using namespace dect2::hfu;
#include "../common/test_util.h"

namespace {

double cpuSeconds() { return (double)std::clock() / CLOCKS_PER_SEC; }   // process CPU time (wall time on Windows)

struct Case { const char* name; FskGenCfg g; std::string msg; double thr; };

std::vector<Case> cases() {
    std::vector<Case> v;
    auto arq = [&](const char* n, FskKind k, double baud, double shift, int cyc, double thr) {
        Case c; c.name = n; c.g.kind = k; c.g.baud = baud; c.g.shift = shift; c.g.cycle = cyc; c.g.idleChars = 200;
        c.msg = "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 12345"; c.thr = thr; v.push_back(c);
    };
    arq("ARQ-E 96/8", FskKind::ArqE, 96, 400, 8, -3);      // 3.9: 3 -> -3 dB (baud from the eye)
    arq("ARQ-E 184.6/4", FskKind::ArqE, 184.6, 400, 4, 0);
    arq("ARQ-N 96", FskKind::ArqN, 96, 850, 0, -3);
    for (int b : {100, 300}) {
        Case c; c.name = b == 100 ? "G-TOR 100" : "G-TOR 300"; c.g.kind = FskKind::GTor; c.g.gtorBaud = b; c.g.gtorGolay = false;
        c.msg = "G-TOR test frame at the given speed, with a CRC check."; c.thr = b == 100 ? -2 : 10; v.push_back(c);   // 3.9: G-TOR 100 6 -> -2 dB
    }
    return v;
}

std::vector<cf32> noisy(std::vector<cf32> v, double cfo, double snrDb, uint64_t seed) {
    std::mt19937 g((uint32_t)(seed * 2654435761u + 7));
    std::uniform_real_distribution<double> U(0, 2 * M_PI);
    double ph = U(g), p = 0, n0 = 0;
    for (auto& s : v) if (std::norm(s) > 0) { p += std::norm(s); n0++; }
    p /= std::max(1.0, n0);
    const double st = 2 * M_PI * cfo / 24000;
    for (auto& s : v) { s *= cf32((float)std::cos(ph), (float)std::sin(ph)); ph += st; }
    const double n = p / std::pow(10.0, snrDb / 10) * 24000.0 / 3000.0;
    std::normal_distribution<float> d(0, (float)std::sqrt(n / 2));
    for (auto& s : v) s += cf32(d(g), d(g));
    return v;
}

bool trial(const Case& c, const std::vector<cf32>& clean, double snr, uint64_t seed, double* cpu = nullptr) {
    const double cfo = (seed & 1) ? 30.0 : -30.0;
    auto s = noisy(clean, cfo, snr, seed);
    FskRx rx(c.g.kind);
    if (getenv("HFU_FORCE")) { rx.setBaud(c.g.kind == FskKind::GTor ? c.g.gtorBaud : c.g.baud); rx.setShift(c.g.kind == FskKind::GTor ? 170 : c.g.shift); }
    std::string text;
    rx.setTextSink([&](const std::string& t) { text += t; });
    const double c0 = cpuSeconds();
    for (size_t i = 0; i < s.size(); i += 2048) rx.push(s.data() + i, std::min<size_t>(2048, s.size() - i));
    if (cpu) *cpu = cpuSeconds() - c0;
    return text.find(c.msg) != std::string::npos;
}

int rateAt(const Case& c, const std::vector<cf32>& clean, double snr, int n) {
    int ok = 0;
    for (int i = 0; i < n; i++) ok += trial(c, clean, snr, (uint64_t)(snr * 10 + 1000) * 131 + (uint64_t)i);
    return ok;
}

} // namespace

int main() {
    const bool longRun = getenv("ONAIR_TEST_LONG") && atoi(getenv("ONAIR_TEST_LONG")) > 0;
    const char* only = getenv("ONAIR_HFUSENS_CASE");   // a name prefix
    for (const Case& c : cases()) {
        if (only && std::string(c.name).rfind(only, 0) != 0) continue;
        const auto clean = genFsk(c.g, c.msg);
        double cpu = 0;
        CHECK(trial(c, clean, 30, 1, &cpu), "%s: no copy at 30 dB", c.name);
        printf("%s: CPU %.4f s per signal second\n", c.name, cpu / (clean.size() / 24000.0));
        if (!longRun) { CHECK(rateAt(c, clean, c.thr + 2, 3) == 3, "%s at %.0f dB", c.name, c.thr + 2); continue; }
        double thr = NAN;
        std::string pts;
        for (double snr = c.thr + 5; snr >= c.thr - 6; snr -= 1) {
            const int ok = rateAt(c, clean, snr, 10);
            char b[32]; snprintf(b, sizeof b, " %g:%d/10", snr, ok); pts += b;
            if (ok >= 9) thr = snr; else if (!std::isnan(thr) || snr < c.thr - 2) break;
        }
        printf("  %-14s threshold %s dB (3 kHz) |%s\n", c.name, std::isnan(thr) ? "-" : std::to_string((int)thr).c_str(), pts.c_str());
        fflush(stdout);
        CHECK(!std::isnan(thr) && thr <= c.thr + 1, "%s threshold", c.name);
    }
    return testSummary("hfutil_sens39");
}
