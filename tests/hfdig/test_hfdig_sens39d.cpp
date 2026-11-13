// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 3.9 sensitivity pass D (docs/SENSITIVITY-3.9-D.md): the 90 % threshold of the SSTV and HamDRM decoders on their own test audio
// (hfdig_gen.h) with white noise; SNR in 3 kHz as in test_hfdig_sstv.cpp and test_hfdig_hamdrm.cpp. SSTV: a trial passes when a complete
// picture comes out with a mean error of at most 20 levels against the test picture (+-50 Hz tuning error, 50 ppm clock error, a new noise
// each trial); HamDRM mode B: the JPEG file complete and byte exact. Default: three trials at the recorded threshold + 2 dB. ONAIR_TEST_LONG=1:
// the sweep (8 trials a point, 1 dB steps) and the decoder's CPU. Simulated signals only.
#include "dect2/engine/mode_synth.h"
#include "dect2/hfdig/hfdig_gen.h"
#include "dect2/hfdig/hfdig_hamdrm.h"
#include "dect2/hfdig/hfdig_sstv.h"
#include "dect2/hfdig/hfdig_tel.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <ctime>
using namespace dect2;
#include "../common/test_util.h"

namespace {

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 3) {}
    double u() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return ((s >> 11) + 0.5) / 9007199254740992.0; }
    double g() { return std::sqrt(-2 * std::log(u())) * std::cos(6.283185307179586 * u()); }
};

double cpuSeconds() { return (double)std::clock() / CLOCKS_PER_SEC; }   // process CPU time (wall time on Windows)

double mae(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 255;
    double s = 0;
    for (size_t i = 0; i < a.size(); i++) s += std::abs((int)a[i] - (int)b[i]);
    return s / (double)a.size();
}

int modeIndex(const char* name) {
    for (int m = 0; m < sstvModeCount(); m++) if (std::string(sstvModeInfo(m).name) == name) return m;
    return -1;
}

bool sstvTrial(int m, double snr, uint64_t seed, double* cpu = nullptr) {
    const SstvModeInfo& mi = sstvModeInfo(m);
    SynthConfig cfg;
    cfg.modeOpt[1] = m;
    cfg.modeVal[0] = (seed & 1) ? -50 : 50;
    cfg.sroPpm = (seed & 2) ? -50 : 50;
    auto gen = makeSstvTestAudio(cfg);
    auto dec = makeSstvDecoder();
    Rng nz(seed);
    const double sigma = std::sqrt(0.125 / std::pow(10.0, snr / 10.0) * 4000.0 / 3000.0);
    const double sec = 0.4 + 0.91 + mi.lineMs * (mi.width > 320 && mi.height > 300 ? mi.height / 2 : mi.height) / 1000.0 + 5;
    std::vector<float> buf(800);
    HfdigSstvTelemetry t;
    double used = 0;
    for (int blk = 0; blk < (int)(sec * 10); blk++) {
        gen->generate(buf.data(), buf.size());
        for (float& v : buf) v += (float)(sigma * nz.g());
        const double c0 = cpuSeconds();
        dec->feedAudio(buf.data(), buf.size());
        used += cpuSeconds() - c0;
        if (blk % 10 == 0) { dec->telemetry(t); if (t.picturesDone >= 1) break; }
    }
    dec->telemetry(t);
    if (cpu) *cpu = used / sec;
    std::vector<uint8_t> ref;
    sstvTestPicture(mi.width, mi.height, ref);
    return t.picturesDone >= 1 && t.image && t.image->complete && mae(t.image->rgb, ref) <= 20;
}

bool drmTrial(double snr, uint64_t seed, double* cpu = nullptr) {
    const std::vector<uint8_t>& pic = hamdrmTestPicture();
    HamdrmTxConfig c;
    c.offsetHz = (seed & 1) ? 20 : -20;
    auto tx = makeHamdrmTx(c, pic);
    auto dec = makeHamdrmDecoder();
    Rng nz(seed);
    const double sigma = std::sqrt(0.125 / std::pow(10.0, snr / 10.0) * 4000.0 / 3000.0);
    std::vector<float> buf(800);
    HfdigHamdrmTelemetry t;
    double used = 0, sec = 0;
    for (int blk = 0; blk < 600; blk++) {
        tx->generate(buf.data(), buf.size());
        for (float& v : buf) v += (float)(sigma * nz.g());
        const double c0 = cpuSeconds();
        dec->feedAudio(buf.data(), buf.size());
        used += cpuSeconds() - c0;
        sec = (blk + 1) * 0.1;
        if (blk % 5 == 4) { dec->telemetry(t); if (t.filesDone >= 1) break; }
    }
    dec->telemetry(t);
    if (cpu) *cpu = used / sec;
    return !t.history.empty() && t.history[0]->data == pic;
}

struct Case { const char* name; int sstv; double thr; };   // sstv: mode index, -1 HamDRM mode B

} // namespace

int main() {
    const bool longRun = getenv("ONAIR_TEST_LONG") && atoi(getenv("ONAIR_TEST_LONG")) > 0;
    std::vector<Case> cs = {{"SSTV Robot 36", modeIndex("Robot 36"), 14}, {"SSTV Martin M1", modeIndex("Martin M1"), 7}, {"HamDRM mode B", -1, 11}};
    for (const Case& c : cs) {
        if (c.sstv < -1 || (c.sstv < 0 && std::string(c.name).find("SSTV") != std::string::npos)) { printf("FAIL %s: mode not found\n", c.name); fails++; continue; }
        auto tr = [&](double snr, uint64_t seed, double* cpu = nullptr) { return c.sstv >= 0 ? sstvTrial(c.sstv, snr, seed, cpu) : drmTrial(snr, seed, cpu); };
        double cpu = 0;
        CHECK(tr(30, 1, &cpu), "%s: no copy at 30 dB", c.name);
        printf("%s: CPU %.4f s per audio second\n", c.name, cpu);
        if (!longRun) {
            int ok = 0;
            for (int i = 0; i < 3; i++) ok += tr(c.thr + 2, 100 + (uint64_t)i);
            CHECK(ok == 3, "%s at %.0f dB: %d of 3", c.name, c.thr + 2, ok);
            continue;
        }
        double thr = NAN;
        std::string pts;
        for (double snr = c.thr + 6; snr >= c.thr - 8; snr -= 1) {
            int ok = 0;
            for (int i = 0; i < 8; i++) ok += tr(snr, (uint64_t)(snr * 10 + 1000) * 7 + (uint64_t)i);
            char b[32]; snprintf(b, sizeof b, " %g:%d/8", snr, ok); pts += b;
            if (ok >= 8 * 9 / 10 + 1) thr = snr; else if (!std::isnan(thr) || snr < c.thr - 3) break;
        }
        printf("  %-14s threshold %s dB (3 kHz) |%s\n", c.name, std::isnan(thr) ? "-" : std::to_string((int)thr).c_str(), pts.c_str());
        fflush(stdout);
    }
    return testSummary("hfdig_sens39d");
}
