// sens_sweep: the shared sensitivity / lock-time measurement for every mode that has a built-in test signal. docs/SENSITIVITY.md.
//
//   sens_sweep [--list] [--mode REGEX] [--scenario REGEX] [--quick | --full] [--trials N] [--threads N] [--out DIR] [--seed S]
//              [--snr-hi dB] [--snr-lo dB] [--heavy]
//
// For each mode and each scenario (a channel impairment, or none) it plays the mode's clean test signal through the impairment chain at a series
// of SNRs into a fresh receiver, many seeded trials per point, and reports: success rate (the receiver counted at least half as many valid units as
// on a clean run; valid = its own CRC / Reed-Solomon / LDPC check passed), time to lock (seconds of signal until the first valid unit; median and
// 90th percentile over the successful trials), the threshold SNR (lowest SNR with >= 90 % success, scanning down from the top until the first
// point below 90 %) and the packet / block error ratio. Trials are deterministic: seed = hash(mode, scenario, snr, trial), the result of a trial never
// depends on which thread ran it.
#include "sens_modes.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <regex>
#include <thread>

using namespace sens;

namespace {

struct Scenario {
    std::string name, desc;
    double cfoHz = 0, sroPpm = 0, pnHz = 0;
    bool noiseOnly = false;   // false lock test: the signal is replaced by zeros before the noise goes on
    std::string path;   // "", echo, tu6, rician
};

std::vector<Scenario> scenariosFor(const Mode& m) {
    const double lim = 0.45 * m.rate - 0.5 * m.bwHz;   // keep the channel inside the sampled band
    const double ppm1 = std::min(2e-6 * m.mhz * 1e6, lim), ppm2 = std::min(20e-6 * m.mhz * 1e6, std::min(0.25 * m.bwHz, lim));
    const double offc = std::min(0.2 * m.bwHz + 0.05 * m.rate, lim);
    char b[160];
    std::vector<Scenario> s;
    auto add = [&](const char* n, std::string d, double cfo, double sro, double pn, const char* path) { Scenario x; x.name = n; x.desc = d; x.cfoHz = cfo; x.sroPpm = sro; x.pnHz = pn; x.path = path; s.push_back(x); };
    add("awgn", "white noise only", 0, 0, 0, "");
    snprintf(b, sizeof b, "carrier error of a 2 ppm radio (%.0f Hz)", ppm1); add("cfo2ppm", b, ppm1, 0, 0, "");
    snprintf(b, sizeof b, "carrier error of a 20 ppm radio (%.0f Hz, capped at a quarter of the channel)", ppm2); add("cfo20ppm", b, ppm2, 0, 0, "");
    snprintf(b, sizeof b, "channel off the centre by %.0f Hz", offc); add("offcentre", b, offc, 0, 0, "");
    add("sro50ppm", "sample clock 50 ppm fast", 0, 50, 0, "");
    snprintf(b, sizeof b, "static echo -6 dB at %.1f us", m.echoUs); add("echo", b, 0, 0, 0, "echo");
    snprintf(b, sizeof b, "Rayleigh fading, 6 tap TU6, Doppler %.0f Hz", m.fdHz); add("tu6", b, 0, 0, 0, "tu6");
    snprintf(b, sizeof b, "Rician (K = 10 dB) over TU6, Doppler %.0f Hz", m.fdHz); add("rician", b, 0, 0, 0, "rician");
    snprintf(b, sizeof b, "oscillator phase noise, %.3g Hz linewidth (Wiener)", m.pnHz); add("phasenoise", b, 0, 0, m.pnHz, "");
    snprintf(b, sizeof b, "2 ppm carrier error + 20 ppm clock + echo + %.3g Hz phase noise", m.pnHz); add("combined", b, ppm1, 20, m.pnHz, "echo");
    return s;
}

struct Prepared {
    Sig base;           // clean, noise free: secs * 1.5 long
    double power = 0;   // active power of base
    size_t trialN = 0;
    size_t maxOff = 0;
};

struct Trial { bool ok = false, locked = false; double lockS = 0, good = 0, bad = 0; };

Trial runTrial(const Mode& m, const Prepared& P, const Scenario& sc, double snrDb, uint64_t seed, double need) {
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
    const double sigma2 = P.power * (m.rate / m.bwHz) / std::pow(10.0, snrDb / 10);
    addNoise(x, sigma2, rng);

    Trial t;
    auto s = m.make(m.rate);
    const size_t chunk = 65536;
    const size_t pollEvery = std::max<size_t>(1, (size_t)std::ceil(0.05 * m.rate / (double)chunk));
    size_t k = 0;
    for (size_t o = 0; o < x.size(); o += chunk, k++) {
        const size_t n = std::min(chunk, x.size() - o);
        s->feed(x.data() + o, n);
        if (!t.locked && (k % pollEvery == pollEvery - 1)) {
            s->settle();
            const Metric mm = s->poll();
            if (mm.good >= 1) { t.locked = true; t.lockS = (double)(o + n) / m.rate; }
        }
    }
    s->finish();
    const Metric mm = s->poll();
    if (!t.locked && mm.good >= 1) { t.locked = true; t.lockS = (double)x.size() / m.rate; }
    t.good = mm.good; t.bad = mm.bad;
    t.ok = t.locked && mm.good >= need;
    return t;
}

struct Point {
    double snr = 0;
    int n = 0, ok = 0;
    double lockMed = -1, lockP90 = -1, goodMean = 0, per = -1;
    double rate() const { return n ? (double)ok / n : 0; }
};

double pct(std::vector<double> v, double p) {
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, (size_t)(p * (double)(v.size() - 1) + 0.5))];
}

struct Cfg {
    bool quick = false, heavy = false;
    int trials = 12, threads = 0;
    uint64_t seed = 1;
    double snrHi = NAN, snrLo = -8;
    std::string out = ".";
};

Point measure(const Mode& m, const Prepared& P, const Scenario& sc, double snr, int n, double need, const Cfg& cfg, int nthreads) {
    std::vector<Trial> res((size_t)n);
    std::atomic<int> next{0};
    const uint64_t key = mix64(hashStr(m.id) ^ mix64(hashStr(sc.name)) ^ mix64((uint64_t)llround(snr * 10) + 1000) ^ cfg.seed);
    auto work = [&] {
        for (;;) {
            const int i = next++;
            if (i >= n) return;
            res[(size_t)i] = runTrial(m, P, sc, snr, mix64(key + (uint64_t)i), need);
        }
    };
    std::vector<std::thread> th;
    for (int t = 0; t < std::min(nthreads, n); t++) th.emplace_back(work);
    for (auto& t : th) t.join();
    Point p; p.snr = snr; p.n = n;
    std::vector<double> lock;
    double pe = 0; int pn = 0;
    for (const auto& t : res) {
        p.goodMean += t.good;
        if (t.ok) { p.ok++; lock.push_back(t.lockS); }
        if (t.good + t.bad > 0) { pe += t.bad / (t.good + t.bad); pn++; }
    }
    p.goodMean /= n;
    p.lockMed = pct(lock, 0.5); p.lockP90 = pct(lock, 0.9);
    p.per = pn ? pe / pn : -1;
    return p;
}

// Share of noise-only trials (the noise level of a signal at 8 dB) in which the receiver reports any valid unit.
double falseLockRate(const Mode& m, const Prepared& P, const Cfg& cfg, int nthreads, int n) {
    Scenario sc; sc.name = "noise-only"; sc.noiseOnly = true;
    const uint64_t key = mix64(hashStr(m.id) ^ 0xfa15e10c ^ cfg.seed);
    std::vector<Trial> res((size_t)n);
    std::atomic<int> next{0};
    auto work = [&] { for (;;) { const int i = next++; if (i >= n) return; res[(size_t)i] = runTrial(m, P, sc, 8.0, mix64(key + (uint64_t)i), 1); } };
    std::vector<std::thread> th;
    for (int t = 0; t < std::min(nthreads, n); t++) th.emplace_back(work);
    for (auto& t : th) t.join();
    int fl = 0;
    for (const auto& t : res) fl += t.locked ? 1 : 0;
    return (double)fl / n;
}

struct ScenResult {
    Scenario sc;
    std::vector<Point> pts;       // measured, high SNR first
    bool hasThreshold = false;
    double threshold = NAN, lockMed = -1, lockP90 = -1;   // lock times at the point 3 dB above the threshold (or the nearest measured above it)
    double perAtThr = -1;
    std::string note;
};

std::string jnum(double v) { if (!std::isfinite(v)) return "null"; char b[40]; snprintf(b, sizeof b, "%.4g", v); return b; }
std::string jstr(const std::string& s) { std::string o = "\""; for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; } return o + "\""; }

ScenResult sweep(const Mode& m, const Prepared& P, const Scenario& sc, double need, const Cfg& cfg, int nthreads, double hi, double lo) {
    ScenResult r; r.sc = sc;
    const int nFine = cfg.trials, nCoarse = std::max(2, cfg.trials / 3);
    const double coarse = 4.0, fine = cfg.quick ? 2.0 : 1.0;
    std::map<int, Point> done;   // key: snr * 10
    auto at = [&](double snr, int n) -> Point& {
        const int k = (int)llround(snr * 10);
        auto it = done.find(k);
        if (it != done.end() && it->second.n >= n) return it->second;
        return done[k] = measure(m, P, sc, snr, n, need, cfg, nthreads);
    };
    // coarse scan from the top until the success rate first falls under 90 %
    double passSnr = NAN, failSnr = NAN;
    for (double s = hi; s >= lo - 1e-9; s -= coarse) {
        const Point& p = at(s, nCoarse);
        if (p.rate() >= 0.9) passSnr = s;
        else { failSnr = s; break; }
    }
    if (std::isnan(passSnr)) {
        r.note = "no 90 % success even at the top SNR";
    } else {
        // refine below the last passing coarse point, with the full trial count, until the first point under 90 %
        for (double s = passSnr; s >= lo - 1e-9; s -= fine) {
            if (!std::isnan(failSnr) && s <= failSnr + 1e-9) break;
            if (at(s, nFine).rate() < 0.9) break;
        }
        if (std::isnan(failSnr)) r.note = "still decoding at the lowest SNR tried";
    }
    for (auto it = done.rbegin(); it != done.rend(); ++it) r.pts.push_back(it->second);
    // threshold: scanning down, the last point still >= 90 % before the first one below it
    double thr = NAN;
    for (const auto& p : r.pts) {
        if (p.rate() >= 0.9) thr = p.snr;
        else if (!std::isnan(thr)) break;
    }
    if (!std::isnan(thr)) {
        r.hasThreshold = true; r.threshold = thr;
        const Point* ref = nullptr;
        for (const auto& p : r.pts) if (p.snr >= thr + 3 - 1e-9 && p.rate() >= 0.9) ref = &p;   // the lowest measured point 3 dB or more above
        if (!ref) for (const auto& p : r.pts) if (std::fabs(p.snr - thr) < 1e-6) ref = &p;
        if (ref) { r.lockMed = ref->lockMed; r.lockP90 = ref->lockP90; }
        for (const auto& p : r.pts) if (std::fabs(p.snr - thr) < 1e-6) r.perAtThr = p.per;
    }
    return r;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _putenv_s("DECT2_NOPACE", "1");
#else
    setenv("DECT2_NOPACE", "1", 1);
#endif   // decoder threads wait instead of dropping a frame: deterministic
    Cfg cfg;
    std::string modeRe = ".*", scenRe = ".*";
    bool list = false, gaps = false, trialsSet = false;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--list") list = true;
        else if (a == "--gaps") gaps = true;
        else if (a == "--mode") modeRe = val();
        else if (a == "--scenario") scenRe = val();
        else if (a == "--quick") cfg.quick = true;
        else if (a == "--full") cfg.quick = false;
        else if (a == "--heavy") cfg.heavy = true;
        else if (a == "--trials") { cfg.trials = atoi(val()); trialsSet = true; }
        else if (a == "--threads") cfg.threads = atoi(val());
        else if (a == "--out") cfg.out = val();
        else if (a == "--seed") cfg.seed = strtoull(val(), nullptr, 10);
        else if (a == "--snr-hi") cfg.snrHi = atof(val());
        else if (a == "--snr-lo") cfg.snrLo = atof(val());
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (cfg.quick && !trialsSet) cfg.trials = 4;
    if (cfg.threads <= 0) cfg.threads = (int)std::max(1u, std::thread::hardware_concurrency());
    std::regex mre(modeRe, std::regex::icase), sre(scenRe, std::regex::icase);
    const auto modes = allModes();
    if (list) {
        for (const auto& m : modes) {
            printf("%-10s %-30s std %-3d %s%s\n", m.id.c_str(), m.name.c_str(), m.stdMode, m.family.c_str(), m.heavy ? "  (heavy)" : "");
            for (const auto& s : scenariosFor(m)) printf("      %-11s %s\n", s.name.c_str(), s.desc.c_str());
        }
        return 0;
    }
    if (gaps) {   // every mode of the tuning table: has a test signal / is in the harness
        std::vector<int> seen;
        printf("%-14s %-34s %-4s %-12s %s\n", "id", "name", "std", "test signal", "in sens_sweep");
        for (int s = 8; s < 400; s++) {
            const ModeTuning* t = dect2::modeTuning(s);
            if (!t || std::find(seen.begin(), seen.end(), s) != seen.end()) continue;
            seen.push_back(s);
            SynthConfig c;
            const bool syn = (bool)dect2::makeModeSynth(s, c, t->sampleRate > 0 ? t->sampleRate : 2e6);
            bool in = false;
            for (const auto& m : modes) in |= m.stdMode == s;
            printf("%-14s %-34s %-4d %-12s %s\n", t->id, t->name, s, syn ? "yes" : "no", in ? "yes" : "no");
        }
        return 0;
    }
    std::filesystem::create_directories(cfg.out);
    std::vector<std::string> modeJson;
    for (const auto& m : modes) {
        if (!std::regex_search(m.id, mre) && !std::regex_search(m.name, mre)) continue;
        if (m.heavy && !cfg.heavy) { printf("%s: skipped (heavy: needs --heavy)\n", m.id.c_str()); continue; }
        const auto t0 = std::chrono::steady_clock::now();
        Prepared P;
        P.trialN = (size_t)(m.secs * m.rate);
        P.maxOff = (size_t)(0.5 * m.secs * m.rate);
        P.base = m.gen(P.trialN + P.maxOff + (size_t)(m.rate * 0.01) + 64);
        scaleTo(P.base, 0.2);
        P.power = activePower(P.base);
        const int nth = std::max(1, std::min(cfg.threads, m.maxJobs));
        // reference run on the clean signal (60 dB): the units a trial must reach to count, and the best case lock time
        Scenario clean0; clean0.name = "reference";
        std::vector<double> goods;
        Cfg rc = cfg; rc.trials = 3;
        const Point ref = measure(m, P, clean0, 60, 3, 1, rc, nth);
        const double refGood = ref.goodMean;
        if (refGood < 1) {
            printf("%-8s no valid output on the clean test signal (reference run): skipped. Receiver wiring needs a look.\n", m.id.c_str());
            modeJson.push_back("{\"mode\":" + jstr(m.id) + ",\"name\":" + jstr(m.name) + ",\"error\":\"no valid output on the clean test signal\"}");
            continue;
        }
        const double need = std::max(1.0, std::floor(0.5 * refGood));
        const double hi = std::isnan(cfg.snrHi) ? m.snrHi : cfg.snrHi;
        printf("\n== %s (%s), std %d, rate %.3f Msps, %.1f s per trial, noise bandwidth %.4g kHz, %d trials/pt%s, %d threads\n", m.name.c_str(), m.id.c_str(), m.stdMode,
               m.rate / 1e6, m.secs, m.bwHz / 1e3, cfg.trials, cfg.quick ? " (quick)" : "", nth);
        printf("   clean reference: %.0f valid units/trial (a trial needs %.0f), lock %.2f s\n", refGood, need, ref.lockMed);
        printf("   %-11s %9s %10s %10s %9s  %s\n", "scenario", "thr (dB)", "lock med s", "lock p90 s", "PER@thr", "note");
        std::vector<std::string> sj;
        for (const auto& sc : scenariosFor(m)) {
            if (!std::regex_search(sc.name, sre)) continue;
            if (cfg.quick && sc.name != "awgn" && sc.name != "combined" && sc.name != "tu6" && sc.name != "cfo20ppm") continue;
            ScenResult r = sweep(m, P, sc, need, cfg, nth, hi, cfg.snrLo);
            printf("   %-11s %9s %10s %10s %9s  %s\n", sc.name.c_str(), r.hasThreshold ? jnum(r.threshold).c_str() : "-", r.hasThreshold ? jnum(r.lockMed).c_str() : "-",
                   r.hasThreshold ? jnum(r.lockP90).c_str() : "-", r.perAtThr >= 0 ? jnum(r.perAtThr).c_str() : "-", r.note.c_str());
            fflush(stdout);
            std::string j = "{\"name\":" + jstr(sc.name) + ",\"desc\":" + jstr(sc.desc) + ",\"threshold_db\":" + (r.hasThreshold ? jnum(r.threshold) : "null") +
                            ",\"lock_med_s\":" + jnum(r.lockMed) + ",\"lock_p90_s\":" + jnum(r.lockP90) + ",\"per_at_threshold\":" + jnum(r.perAtThr) +
                            ",\"note\":" + jstr(r.note) + ",\"points\":[";
            for (size_t i = 0; i < r.pts.size(); i++) {
                const Point& p = r.pts[i];
                j += (i ? "," : "") + std::string("{\"snr_db\":") + jnum(p.snr) + ",\"n\":" + std::to_string(p.n) + ",\"success\":" + jnum(p.rate()) + ",\"lock_med_s\":" + jnum(p.lockMed) +
                     ",\"lock_p90_s\":" + jnum(p.lockP90) + ",\"valid_units_mean\":" + jnum(p.goodMean) + ",\"per\":" + jnum(p.per) + "}";
            }
            sj.push_back(j + "]}");
        }
        const double fl = falseLockRate(m, P, cfg, nth, cfg.quick ? 12 : 24);
        printf("   false-lock rate on noise only: %.3f\n", fl);
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::string mj = "{\"mode\":" + jstr(m.id) + ",\"name\":" + jstr(m.name) + ",\"family\":" + jstr(m.family) + ",\"std\":" + std::to_string(m.stdMode) +
                         ",\"rate_sps\":" + jnum(m.rate) + ",\"trial_secs\":" + jnum(m.secs) + ",\"noise_bw_hz\":" + jnum(m.bwHz) + ",\"trials\":" + std::to_string(cfg.trials) +
                         ",\"quick\":" + (cfg.quick ? "true" : "false") + ",\"seed\":" + std::to_string(cfg.seed) + ",\"ref_valid_units\":" + jnum(refGood) +
                         ",\"ref_lock_s\":" + jnum(ref.lockMed) + ",\"false_lock_rate\":" + jnum(fl) + ",\"wall_s\":" + jnum(el) + ",\"scenarios\":[";
        for (size_t i = 0; i < sj.size(); i++) mj += (i ? "," : "") + sj[i];
        mj += "]}";
        modeJson.push_back(mj);
        if (FILE* f = fopen((cfg.out + "/sens-" + m.id + ".json").c_str(), "w")) { fputs((mj + "\n").c_str(), f); fclose(f); }
        printf("   (%.0f s)\n", el);
    }
    if (FILE* f = fopen((cfg.out + "/sens-baseline.json").c_str(), "w")) {
        fputs("{\"tool\":\"sens_sweep\",\"version\":1,\"modes\":[", f);
        for (size_t i = 0; i < modeJson.size(); i++) { fputs(i ? ",\n" : "\n", f); fputs(modeJson[i].c_str(), f); }
        fputs("\n]}\n", f);
        fclose(f);
    }
    return 0;
}
