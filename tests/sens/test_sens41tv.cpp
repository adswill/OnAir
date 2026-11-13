// Sensitivity pass 4.1 TV (docs/SENSITIVITY-4.1-TV.md): the gaps 4.0 left open in DVB-T, DVB-T2, DTMB, ATSC 1.0 / 3.0, DRM and ISDB-T.
// The mode's own test signal through the sens_sweep channel chain (tools/sens/sens_impair.h: echo, TU6 / Rician fading with Jakes Doppler,
// Wiener phase noise, carrier and clock error) and white noise into a fresh receiver, seeded trials. A trial passes when the receiver counted
// at least half the valid units of a clean run (as sens_sweep). Each trial also records its lock time (seconds of signal to the first valid unit,
// polled every 50 ms), so a clock-error loss can be split into lock time and steady-state loss. Every point is run twice with the same seeds:
// the 4.1 receiver path and the 4.0 path of the same receiver (A/B switches set through the environment, offSwitches()).
//   default: a short guard of the 4.1 gains; noise alone must give nothing
//   ONAIR_SENS_CALIBRATE=1: the table. S41_MODES (regex), S41_SCEN (regex), S41_TRIALS (8), S41_SNR "hi:lo:step", S41_SNR_<id>, S41_SECS,
//   S41_FD (Doppler, Hz), S41_PN (phase-noise linewidth, Hz), S41_NOAB=1 (only the new path)
// Simulated signals only: no recording is involved.
#include "sens_modes_d.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <thread>

using namespace sens;

namespace {

struct Scen { std::string name; double cfoHz = 0, sroPpm = 0, pnHz = 0; std::string path; bool noiseOnly = false; };

std::vector<Scen> scenarios(const Mode& m) {
    const double lim = 0.45 * m.rate - 0.5 * m.bwHz;
    const double ppm1 = std::min(2e-6 * m.mhz * 1e6, lim);
    std::vector<Scen> s;
    s.push_back({"awgn", 0, 0, 0, ""});
    s.push_back({"sro50", 0, 50, 0, ""});
    s.push_back({"sro-50", 0, -50, 0, ""});
    s.push_back({"echo", 0, 0, 0, "echo"});
    s.push_back({"tu6", 0, 0, 0, "tu6"});
    s.push_back({"rician", 0, 0, 0, "rician"});
    s.push_back({"phasenoise", 0, 0, m.pnHz, ""});
    s.push_back({"combined", ppm1, 20, m.pnHz, "echo"});
    s.push_back({"tu6sro", ppm1, 50, m.pnHz, "tu6"});
    return s;
}

struct Prep { Sig base; double power = 0; size_t trialN = 0, maxOff = 0; double need = 1; };

Sig impaired(const Mode& m, const Prep& P, const Scen& sc, double snrDb, uint64_t seed) {
    std::mt19937 rng((uint32_t)(seed ^ (seed >> 32)));
    const size_t off = P.maxOff ? (size_t)(mix64(seed ^ 0x51ed) % P.maxOff) : 0;
    const size_t pad = (size_t)(m.rate * 0.01) + 64;
    Sig x(P.base.begin() + (long)off, P.base.begin() + (long)std::min(P.base.size(), off + P.trialN + pad));
    if (sc.sroPpm != 0) x = clockError(x, sc.sroPpm);
    if (!sc.path.empty()) x = multipath(x, profileTaps(sc.path, m.echoUs), m.rate, m.fdHz, rng);
    if (x.size() > P.trialN) x.resize(P.trialN);
    std::uniform_real_distribution<double> U(0, 2 * M_PI);
    if (sc.noiseOnly) std::fill(x.begin(), x.end(), cf32(0, 0));
    shift(x, sc.cfoHz, m.rate, U(rng));
    phaseNoise(x, sc.pnHz, m.rate, rng);
    addNoise(x, P.power * (m.rate / m.bwHz) / std::pow(10.0, snrDb / 10), rng);
    return x;
}

struct Tr { double good = 0, lockS = -1; };

Tr trial(const Mode& m, const Prep& P, const Scen& sc, double snrDb, uint64_t seed, bool timeLock) {
    const Sig x = impaired(m, P, sc, snrDb, seed);
    auto s = m.make(m.rate);
    const size_t chunk = 65536;
    const size_t pollEvery = std::max<size_t>(1, (size_t)std::ceil(0.05 * m.rate / (double)chunk));
    Tr t;
    size_t k = 0;
    for (size_t o = 0; o < x.size(); o += chunk, k++) {
        const size_t n = std::min(chunk, x.size() - o);
        s->feed(x.data() + o, n);
        if (timeLock && t.lockS < 0 && (k % pollEvery == pollEvery - 1)) {
            s->settle();
            if (s->poll().good >= 1) t.lockS = (double)(o + n) / m.rate;
        }
    }
    s->finish();
    t.good = s->poll().good;
    if (timeLock && t.lockS < 0 && t.good >= 1) t.lockS = (double)x.size() / m.rate;
    return t;
}

struct Pt { double pass = 0, frac = 0, lockMed = -1; };

Pt run(const Mode& m, const Prep& P, const Scen& sc, double snr, int n, int threads, bool timeLock) {
    std::vector<Tr> g((size_t)n);
    std::atomic<int> next{0};
    auto work = [&] { for (;;) { const int i = next++; if (i >= n) return; g[(size_t)i] = trial(m, P, sc, snr, mix64(hashStr(m.id) ^ hashStr(sc.name) * 31 ^ (uint64_t)(snr * 16 + 4096) * 131 ^ (uint64_t)i), timeLock); } };
    std::vector<std::thread> th;
    for (int k = 0; k < std::max(1, std::min(threads, n)); k++) th.emplace_back(work);
    for (auto& t : th) t.join();
    Pt r;
    std::vector<double> lk;
    for (const auto& v : g) {
        if (v.good >= P.need) r.pass += 1;
        r.frac += std::min(1.0, v.good / std::max(1.0, 2 * P.need));
        if (v.lockS >= 0) lk.push_back(v.lockS);
    }
    r.pass /= n; r.frac /= n;
    if (!lk.empty()) { std::sort(lk.begin(), lk.end()); r.lockMed = lk[lk.size() / 2]; }
    return r;
}

// the A/B switches of each receiver: set, they give its 4.0 path
std::vector<const char*> offSwitches(const std::string& id) {
    if (id == "atsc") return {"DECT2_ATSC_NO41"};
    if (id.rfind("atsc3", 0) == 0) return {"DECT2_ATSC3_NO41"};
    if (id.rfind("drm", 0) == 0) return {"DECT2_DRM_NOCPE"};
    if (id == "dvbt") return {"DECT2_DVBT_NO41"};
    if (id == "dvbt2") return {"DECT2_T2_NO41"};
    if (id == "dtmb") return {"DECT2_DTMB_NO41"};
    return {};
}
void setOff(const std::vector<const char*>& sw, bool off) {
    for (const char* s : sw) {
#ifndef _WIN32
        if (off) setenv(s, "1", 1); else unsetenv(s);
#else
        _putenv_s(s, off ? "1" : "");
#endif
    }
}

Prep prepare(const Mode& m, double secs) {
    Prep P;
    P.trialN = (size_t)(secs * m.rate);
    P.maxOff = (size_t)(0.5 * secs * m.rate);
    P.base = m.gen(P.trialN + P.maxOff + (size_t)(m.rate * 0.01) + 64);
    scaleTo(P.base, 0.2);
    P.power = activePower(P.base);
    Scen clean{"awgn"};
    double sum = 0;
    for (int i = 0; i < 3; i++) sum += trial(m, P, clean, 60, 77 + (uint64_t)i, false).good;
    P.need = std::max(1.0, 0.5 * sum / 3);
    return P;
}

const Mode* findMode(const std::vector<Mode>& all, const std::string& id) {
    for (const auto& m : all) if (m.id == id) return &m;
    return nullptr;
}

std::vector<Mode> modes41() {
    auto all = allModes();
    for (auto& m : extraModesD()) all.push_back(std::move(m));
    return all;
}

void sweep(const Mode& m0, const std::regex& scenRe, int n, int threads) {
    Mode m = m0;
    if (getenv("S41_SECS")) m.secs = atof(getenv("S41_SECS"));
    if (getenv("S41_FD")) m.fdHz = atof(getenv("S41_FD"));
    if (getenv(("S41_FD_" + m.id).c_str())) m.fdHz = atof(getenv(("S41_FD_" + m.id).c_str()));   // per mode: S41_FD_isdbt=70
    if (getenv("S41_PN")) m.pnHz = atof(getenv("S41_PN"));
    const auto sw = offSwitches(m.id);
    const bool ab = !sw.empty() && !(getenv("S41_NOAB") && *getenv("S41_NOAB"));   // a configuration may give S41_NOAB= to keep the A/B pair
    double hi = m.snrHi, lo = m.snrHi - 20, st = 2;
    if (getenv("S41_SNR")) sscanf(getenv("S41_SNR"), "%lf:%lf:%lf", &hi, &lo, &st);
    const std::string own = "S41_SNR_" + m.id;
    if (getenv(own.c_str())) sscanf(getenv(own.c_str()), "%lf:%lf:%lf", &hi, &lo, &st);
    setOff(sw, false);
    const int thr = std::max(1, std::min(threads, m.maxJobs));
    const Prep P = prepare(m, m.secs);
    printf("== %s (%s): %.1f s trials, need %.1f units, %d trials a point, Doppler %.0f Hz, linewidth %.2f Hz, switch %s\n", m.id.c_str(), m.name.c_str(),
           m.secs, P.need, n, m.fdHz, m.pnHz, sw.empty() ? "-" : sw[0]);
    if (getenv("S41_ONE")) {   // diagnosis: one trial per scenario at this SNR (with the receiver's own debug output, e.g. ATSC_DBG=1)
        const double snr = atof(getenv("S41_ONE"));
        for (const auto& sc : scenarios(m)) {
            if (!std::regex_match(sc.name, scenRe)) continue;
            const Tr t = trial(m, P, sc, snr, 12345, true);
            printf("-- one trial %s %.1f dB: %.0f units (need %.0f), lock %.2f s\n", sc.name.c_str(), snr, t.good, P.need, t.lockS);
            fflush(stdout);
        }
        return;
    }
    for (const auto& sc : scenarios(m)) {
        if (!std::regex_match(sc.name, scenRe)) continue;
        printf("-- %s\n   SNR    new pass/frac/lock        old pass/frac/lock\n", sc.name.c_str());
        for (double s = hi; s >= lo - 1e-9; s -= st) {
            setOff(sw, false);
            const Pt a = run(m, P, sc, s, n, thr, true);
            Pt b;
            if (ab) { setOff(sw, true); b = run(m, P, sc, s, n, thr, true); setOff(sw, false); }
            printf("  %5.1f   %4.0f%% %4.0f%% %5.2fs      %4.0f%% %4.0f%% %5.2fs\n", s, 100 * a.pass, 100 * a.frac, a.lockMed, 100 * b.pass, 100 * b.frac, b.lockMed);
            fflush(stdout);
            if (a.pass == 0 && b.pass == 0 && a.frac == 0 && b.frac == 0) break;
        }
    }
    Scen no{"noise-only"}; no.noiseOnly = true;
    Prep Pn = P; Pn.need = 0.5;
    setOff(sw, false);
    const Pt fa = run(m, Pn, no, 10, 2 * n, thr, false);
    Pt fb;
    if (ab) { setOff(sw, true); fb = run(m, Pn, no, 10, 2 * n, thr, false); setOff(sw, false); }
    printf("-- noise only, %d runs: new %.0f%% of runs with a unit, old %.0f%%\n", 2 * n, 100 * fa.pass, 100 * fb.pass);
    fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    (void)argc;
#ifndef _WIN32
    setenv("DECT2_NOPACE", "1", 1);
    // S41_ALTS="A=1,B=2/C=3": the same calibration once more in a child process per configuration (receivers read some switches once per
    // process), each with those variables set and only the new path
    if (getenv("ONAIR_SENS_CALIBRATE") && getenv("S41_ALTS") && !getenv("S41_CHILD")) {
        std::string all = getenv("S41_ALTS");
        size_t a = 0;
        for (;;) {
            const size_t b = all.find('/', a);
            std::string cfg = all.substr(a, b == std::string::npos ? std::string::npos : b - a);
            std::string q;   // each NAME=value quoted for the shell (a regex holds '|')
            for (size_t i = 0, j; i <= cfg.size(); i = j + 1) { j = cfg.find(',', i); if (j == std::string::npos) j = cfg.size(); q += " '" + cfg.substr(i, j - i) + "'"; }
            cfg = q;
            printf("######## configuration: %s\n", cfg.c_str());
            fflush(stdout);
            const std::string cmd = "env S41_NOAB=1 " + cfg + " S41_CHILD=1 '" + std::string(argv[0]) + "'";
            if (std::system(cmd.c_str()) != 0) printf("(child failed)\n");
            if (b == std::string::npos) break;
            a = b + 1;
        }
        return 0;
    }
#endif
    const auto all = modes41();
    const int threads = std::max(1, (int)std::thread::hardware_concurrency());
    if (getenv("ONAIR_SENS_CALIBRATE")) {
        const std::regex modeRe(getenv("S41_MODES") ? getenv("S41_MODES") : "atsc|dvbt|dvbt2|dtmb");
        const std::regex scenRe(getenv("S41_SCEN") ? getenv("S41_SCEN") : "awgn|sro50");
        const int n = getenv("S41_TRIALS") ? atoi(getenv("S41_TRIALS")) : 8;
        for (const auto& m : all)
            if (std::regex_match(m.id, modeRe)) sweep(m, scenRe, n, threads);
        return 0;
    }
    // guard: noise alone gives no unit
    int fails = 0;
    for (const char* id : {"atsc", "drm", "isdbt"}) {   // (the receivers this pass looked at)
        const Mode* m = findMode(all, id);
        if (!m) { printf("no %s mode\n", id); fails++; continue; }
        const int thr = std::max(1, std::min(threads, m->maxJobs));
        const Prep P = prepare(*m, m->secs);
        Scen no{"noise-only"}; no.noiseOnly = true;
        Prep Pn = P; Pn.need = 0.5;
        const Pt f = run(*m, Pn, no, 10, 4, thr, false);
        printf("%s: noise only %.0f%% of runs with a unit\n", id, 100 * f.pass);
        if (f.pass > 0) { printf("FAIL: %s decoded noise\n", id); fails++; }
    }
    // DRM 64-QAM through 1 Hz oscillator phase noise at 26 dB: the 4.0 receiver passed no trial (1 % of the units), the common-phase
    // correction passes them (docs/SENSITIVITY-4.1-TV.md)
    if (const Mode* m = findMode(all, "drm")) {
        const Prep P = prepare(*m, m->secs);
        const Scen pn{"phasenoise", 0, 0, m->pnHz, ""};
        const Pt r = run(*m, P, pn, 26, 4, std::max(1, std::min(threads, m->maxJobs)), false);
        printf("drm: phase noise %.2f Hz at 26 dB: pass %.0f%% (units %.0f%%)\n", m->pnHz, 100 * r.pass, 100 * r.frac);
        if (r.pass < 0.74) { printf("FAIL: drm phase noise 26 dB\n"); fails++; }
    }
    printf(fails ? "FAILED\n" : "PASSED\n");
    return fails ? 1 : 0;
}
