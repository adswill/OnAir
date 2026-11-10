// 3.9 sensitivity pass D (docs/SENSITIVITY-3.9-D.md): satellites, trunking, pagers and voice receivers of the sens_sweep registry, plus
// the modes added for this pass in tools/sens/sens_modes_d.h.
//
// Default (short): for every mode in the table, three seeded trials at the recorded AWGN threshold + 2 dB must decode (at least two of
// three), the same with a 20 ppm carrier error, and two noise-only trials must give nothing. It prints the CPU seconds the receiver used
// per second of the clean signal (all threads; getrusage).
// ONAIR_TEST_LONG=1: the full sweep (sens_sweep's search, 6 trials a point, 1 dB steps) for AWGN, 20 ppm carrier and a 50 ppm clock; every
// threshold must be no worse than the recorded one + 1 dB. ONAIR_SENS39D_MODE=<id> limits either run to one mode;
// ONAIR_SENS39D_SNR=<dB> [ONAIR_SENS39D_SCEN=<scenario>] prints the six trials of one point instead (diagnosis).
// Same trials, impairments and SNR definitions as tools/sens/sens_sweep.cpp (included here) and tests/sens/test_sens39a.cpp (pass A).
#define SENS_SWEEP_NO_MAIN
#include "sens_sweep.cpp"
#include "sens_modes_d.h"
#include <sys/resource.h>

namespace {

struct Row {
    const char* id;
    double awgn, cfo20;   // recorded thresholds (dB, sens_sweep's SNR definition), after this pass
    bool shortRun;        // in the default run
};

const Row kRows[] = {
    {"meteor", 2, 3, true},      // 3.9: 5 / 8 -> 2 / 3 dB (line acquisition, narrow loops, best-scoring sync search)
    {"tetra", 99, 99, false},
    {"tetrapol", 99, 99, false},
    {"fusion39", 99, 99, false},
    {"orbcomm", 99, 99, false},
    {"inmc", 99, 99, false},
    {"pocsag", 99, 99, false},
    {"flex", 99, 99, false},
    {"cw", 99, 99, false},
    {"dvbs1", 99, 99, false},
    {"ale", 99, 99, false},
    {"apt", 99, 99, false},
    {"edacs", 99, 99, false},
    {"smartnet", 99, 99, false},
    {"mpt1327", 99, 99, false},
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
    const char* only = getenv("ONAIR_SENS39D_MODE");
    auto modes = allModes();
    for (auto& m : sens::extraModesD()) modes.push_back(std::move(m));
    int fails = 0, ran = 0;
    Cfg cfg; cfg.trials = 6; cfg.quick = false;
    if (getenv("ONAIR_SENS39D_TRIALS")) cfg.trials = atoi(getenv("ONAIR_SENS39D_TRIALS"));
    const int nthAll = getenv("ONAIR_SENS39D_THREADS") ? atoi(getenv("ONAIR_SENS39D_THREADS")) : (int)std::max(1u, std::min(3u, std::thread::hardware_concurrency()));
    std::vector<std::string> ids;
    for (const Row& r : kRows) ids.push_back(r.id);
    if (only && std::find(ids.begin(), ids.end(), std::string(only)) == ids.end()) ids.push_back(only);   // any registry mode, for measuring
    for (const std::string& id : ids) {
        Row row{id.c_str(), 99, 99, false};
        for (const Row& r : kRows) if (id == r.id) row = r;
        if (only && id != only) continue;
        if (!longRun && !row.shortRun && !only) continue;
        const Mode* mp = nullptr;
        for (const auto& m : modes) if (m.id == id) mp = &m;
        if (!mp) { printf("FAIL %s: not in the sens_sweep registry\n", id.c_str()); fails++; continue; }
        const Mode& m = *mp;
        Prepared P;
        P.trialN = (size_t)(m.secs * m.rate);
        P.maxOff = (size_t)(0.5 * m.secs * m.rate);
        P.base = m.gen(P.trialN + P.maxOff + (size_t)(m.rate * 0.01) + 64);
        scaleTo(P.base, 0.2);
        P.power = activePower(P.base);
        const int nth = std::max(1, std::min(nthAll, m.maxJobs));
        double cpuPerSec = 0;
        {
            Sig x(P.base.begin(), P.base.begin() + (long)P.trialN);
            const double c0 = cpuSeconds();
            auto s = m.make(m.rate);
            for (size_t o = 0; o < x.size(); o += 65536) s->feed(x.data() + o, std::min<size_t>(65536, x.size() - o));
            s->finish();
            const Metric mm = s->poll();
            cpuPerSec = (cpuSeconds() - c0) / m.secs;
            if (mm.good < 1) { printf("FAIL %s: no valid output on the clean test signal\n", id.c_str()); fails++; continue; }
        }
        Scenario ref; ref.name = "reference";
        Cfg rc = cfg; rc.trials = 3;
        const Point r0 = measure(m, P, ref, 60, 3, 1, rc, nth);
        const double need = std::max(1.0, std::floor(0.5 * r0.goodMean));
        const auto scs = scenariosFor(m);
        ran++;
        printf("== %s: clean %.0f units/trial (needs %.0f), lock %.2f s, CPU %.4f s per signal second\n", id.c_str(), r0.goodMean, need, r0.lockMed, cpuPerSec);
        fflush(stdout);
        if (const char* dbg = getenv("ONAIR_SENS39D_SNR")) {
            const Scenario* sc = findScenario(scs, getenv("ONAIR_SENS39D_SCEN") ? getenv("ONAIR_SENS39D_SCEN") : "awgn");
            if (!sc) { printf("no such scenario\n"); return 2; }
            const double snr = atof(dbg);
            const uint64_t key = mix64(hashStr(m.id) ^ mix64(hashStr(sc->name)) ^ mix64((uint64_t)llround(snr * 10) + 1000) ^ cfg.seed);
            int ok = 0;
            for (int i = 0; i < cfg.trials; i++) {
                const Trial t = runTrial(m, P, *sc, snr, mix64(key + (uint64_t)i), need);
                ok += t.ok;
                printf("   trial %d at %g dB: good %.0f bad %.0f lock %.2f s %s\n", i, snr, t.good, t.bad, t.lockS, t.ok ? "ok" : "fail");
                fflush(stdout);
            }
            printf("   %d/%d\n", ok, cfg.trials);
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
            for (int i = 0; i < 2; i++) fl += runTrial(m, P, no, 8.0, mix64(hashStr(m.id) ^ 0x39d0 ^ (uint64_t)i), 1).locked ? 1 : 0;
            printf("   noise only: %d/2 false decodes %s\n", fl, fl ? "FAIL" : "ok");
            if (fl) fails++;
            continue;
        }
        const char* scenEnv = getenv("ONAIR_SENS39D_SCENS");
        std::vector<std::string> scl = {"awgn", "cfo20ppm", "sro50ppm"};
        if (scenEnv) { scl.clear(); std::string s = scenEnv, w; for (char c : s + " ") { if (c == ' ' || c == ',') { if (!w.empty()) scl.push_back(w); w.clear(); } else w += c; } }
        for (const std::string& sn : scl) {
            const Scenario* sc = findScenario(scs, sn.c_str());
            if (!sc) continue;
            const ScenResult r = sweep(m, P, *sc, need, cfg, nth, m.snrHi, -8);
            const double lim = (sn == "cfo20ppm" ? row.cfo20 : row.awgn) + 1;
            const bool ok = r.hasThreshold && r.threshold <= lim + 1e-9;
            printf("   %-9s threshold %s dB (limit %.0f), lock %.2f s %s\n", sn.c_str(), r.hasThreshold ? jnum(r.threshold).c_str() : "-", lim, r.lockMed, ok ? "ok" : "FAIL");
            if (!ok) fails++;
            printf("     ");
            for (const auto& p : r.pts) printf(" %g:%d/%d", p.snr, p.ok, p.n);
            printf("\n");
            fflush(stdout);
        }
    }
    printf("%d modes, %d failures\n", ran, fails);
    return fails ? 1 : 0;
}
