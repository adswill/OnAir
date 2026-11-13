// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 3.9 sensitivity pass A (docs/SENSITIVITY-3.9-A.md): broadcast, TV and satellite decoders against the thresholds measured with sens_sweep.
//
// Default (short, a few seconds of signal per mode): for every mode in the table below, three seeded trials at the recorded AWGN threshold
// + 2 dB must decode (at least two of three), the same at the carrier error of a 20 ppm radio, and two noise-only trials must give nothing.
// It also prints the CPU seconds the receiver used per second of signal on the clean test signal (all threads; getrusage).
// ONAIR_TEST_LONG=1: the full sweep (sens_sweep's own search, 6 trials a point, 1 dB steps) for AWGN, 20 ppm carrier and a 50 ppm clock, and
// every threshold must be no worse than the recorded one + 1 dB. ONAIR_SENS39A_MODE=<id> limits either run to one mode;
// ONAIR_SENS39A_SNR=<dB> [ONAIR_SENS39A_SCEN=<scenario>] prints the six trials of one point instead (diagnosis).
// The trials and the impairments are those of tools/sens/sens_sweep.cpp (included here), so the figures are the ones in the doc.
#define SENS_SWEEP_NO_MAIN
#include "sens_sweep.cpp"
#include <sys/resource.h>

namespace {

struct Row {
    const char* id;
    double awgn, cfo20;   // recorded thresholds (dB, sens_sweep's SNR definition), after this pass
    bool shortRun;        // in the default run (false: wideband or long-trial modes, ONAIR_TEST_LONG only)
};

// Measured on the Mac with ONAIR_TEST_LONG=1 (6 trials a point, 1 dB steps below the last 4 dB step), see the doc; the Linux runner reads
// within about 1 dB, and the default run checks 2 dB above them.
const Row kRows[] = {
    {"am", 27, 27, true},          // the criterion is a 1 kHz tone 20 dB over the rest of the audio, not a decoder threshold
    {"am-sync", 27, 27, true},
    {"fm-rds", 16, 16, false},     // 3.6/3.7 figures (Linux)
    {"cdr", -4, -4, false},
    {"drm16", 9, 9, false},
    {"meteor", 5, 8, true},
    {"hrpt", 2, 2, true},          // 3.9: 4 -> 2 dB
    {"fy3ahrpt", 2, 6, true},
    {"goes", -1, -1, true},
    {"gk2alrit", -1, -1, true},
    {"gk2ahrit", 2, 2, true},
    {"iridium", 13, 13, false},    // Linux 13 / 13 (Mac 11 / 12); about 4 CPU seconds per signal second here: long run only
    {"hdr-fm", 16, 18, false},     // Linux quick tier (2 dB steps)
};

double cpuSeconds() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return (double)u.ru_utime.tv_sec + 1e-6 * u.ru_utime.tv_usec + (double)u.ru_stime.tv_sec + 1e-6 * u.ru_stime.tv_usec;
}

const Scenario* findScenario(const std::vector<Scenario>& v, const char* name) {
    for (const auto& s : v) if (s.name == name) return &s;
    return nullptr;
}

} // namespace

int main() {
#ifdef _WIN32
    _putenv_s("DECT2_NOPACE", "1");
#else
    setenv("DECT2_NOPACE", "1", 1);
#endif
    const bool longRun = getenv("ONAIR_TEST_LONG") && atoi(getenv("ONAIR_TEST_LONG")) > 0;
    const char* only = getenv("ONAIR_SENS39A_MODE");
    const auto modes = allModes();
    int fails = 0, ran = 0;
    Cfg cfg; cfg.trials = 6; cfg.quick = false;
    const int nthAll = (int)std::max(1u, std::min(4u, std::thread::hardware_concurrency()));
    for (const Row& row : kRows) {
        if (only && std::string(only) != row.id) continue;
        if (!longRun && !row.shortRun && !only) continue;
        const Mode* mp = nullptr;
        for (const auto& m : modes) if (m.id == row.id) mp = &m;
        if (!mp) { printf("FAIL %s: not in the sens_sweep registry\n", row.id); fails++; continue; }
        const Mode& m = *mp;
        Prepared P;
        P.trialN = (size_t)(m.secs * m.rate);
        P.maxOff = (size_t)(0.5 * m.secs * m.rate);
        P.base = m.gen(P.trialN + P.maxOff + (size_t)(m.rate * 0.01) + 64);
        scaleTo(P.base, 0.2);
        P.power = activePower(P.base);
        const int nth = std::max(1, std::min(nthAll, m.maxJobs));
        // CPU on the clean signal, one receiver alone in the process
        double cpuPerSec = 0;
        {
            Sig x(P.base.begin(), P.base.begin() + (long)P.trialN);
            const double c0 = cpuSeconds();
            auto s = m.make(m.rate);
            for (size_t o = 0; o < x.size(); o += 65536) s->feed(x.data() + o, std::min<size_t>(65536, x.size() - o));
            s->finish();
            const Metric mm = s->poll();
            cpuPerSec = (cpuSeconds() - c0) / m.secs;
            if (mm.good < 1) { printf("FAIL %s: no valid output on the clean test signal\n", row.id); fails++; continue; }
        }
        Scenario ref; ref.name = "reference";
        Cfg rc = cfg; rc.trials = 3;
        const Point r0 = measure(m, P, ref, 60, 3, 1, rc, nth);
        const double need = std::max(1.0, std::floor(0.5 * r0.goodMean));
        const auto scs = scenariosFor(m);
        ran++;
        printf("== %s: clean %.0f units/trial (needs %.0f), lock %.2f s, CPU %.3f s per signal second\n", row.id, r0.goodMean, need, r0.lockMed, cpuPerSec);
        if (const char* dbg = getenv("ONAIR_SENS39A_SNR")) {   // diagnosis: every trial of one point (awgn) with its unit counts
            const Scenario* sc = findScenario(scs, getenv("ONAIR_SENS39A_SCEN") ? getenv("ONAIR_SENS39A_SCEN") : "awgn");
            if (!sc) { printf("no such scenario\n"); return 2; }
            const double snr = atof(dbg);
            fflush(stdout);
            const uint64_t key = mix64(hashStr(m.id) ^ mix64(hashStr(sc->name)) ^ mix64((uint64_t)llround(snr * 10) + 1000) ^ cfg.seed);
            for (int i = 0; i < 6; i++) {
                const Trial t = runTrial(m, P, *sc, snr, mix64(key + (uint64_t)i), need);
                printf("   trial %d at %g dB: good %.0f bad %.0f lock %.2f s %s\n", i, snr, t.good, t.bad, t.lockS, t.ok ? "ok" : "fail");
                fflush(stdout);
            }
            continue;
        }
        if (!longRun) {
            for (const char* sn : {"awgn", "cfo20ppm"}) {
                const Scenario* sc = findScenario(scs, sn);
                if (!sc) continue;
                const double thr = std::string(sn) == "awgn" ? row.awgn : row.cfo20;
                const Point p = measure(m, P, *sc, thr + 2, 3, need, cfg, nth);
                const bool ok = p.ok >= 2;
                printf("   %-9s %+5.1f dB: %d/3 decoded, lock %.2f s %s\n", sn, thr + 2, p.ok, p.lockMed, ok ? "ok" : "FAIL");
                if (!ok) fails++;
            }
            Scenario no; no.name = "noise-only"; no.noiseOnly = true;
            int fl = 0;
            for (int i = 0; i < 2; i++) fl += runTrial(m, P, no, 8.0, mix64(hashStr(m.id) ^ 0x39a0 ^ (uint64_t)i), 1).locked ? 1 : 0;
            printf("   noise only: %d/2 false decodes %s\n", fl, fl ? "FAIL" : "ok");
            if (fl) fails++;
            continue;
        }
        for (const char* sn : {"awgn", "cfo20ppm", "sro50ppm"}) {
            const Scenario* sc = findScenario(scs, sn);
            if (!sc) continue;
            const ScenResult r = sweep(m, P, *sc, need, cfg, nth, m.snrHi, -8);
            const double lim = (std::string(sn) == "cfo20ppm" ? row.cfo20 : row.awgn) + 1;
            const bool ok = r.hasThreshold && r.threshold <= lim + 1e-9;
            printf("   %-9s threshold %s dB (limit %.0f), lock %.2f s %s\n", sn, r.hasThreshold ? jnum(r.threshold).c_str() : "-", lim, r.lockMed, ok ? "ok" : "FAIL");
            if (!ok) fails++;
            printf("     ");
            for (const auto& p : r.pts) printf(" %g:%d/%d", p.snr, p.ok, p.n);
            printf("\n");
        }
    }
    printf("%d modes, %d failures\n", ran, fails);
    return fails ? 1 : 0;
}
