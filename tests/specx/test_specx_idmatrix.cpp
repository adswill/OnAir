// "What is this?" on the app's own test signals: every mode that has a generator is rendered at random frequency offsets and SNRs, the
// identifier (dect2/engine/sig_ident.h) is run on the region a click would select, and the result is scored against the mode that made the
// signal. Prints the per-family and per-mode accuracy, the confusions, the reliability of the confidence numbers and the reject rates; the
// thresholds at the end are the measured values minus a margin (docs/IDENTIFIER.md says what they mean, and what they do not).
//
//   test_specx_idmatrix            the full run: SNR 10 / 20 / 30 dB, 2 trials each, with and without the frequency known (a few minutes)
//   test_specx_idmatrix --quick    SNR 20 dB, 1 trial, no frequency: for tuning
//   test_specx_idmatrix --mode id  only the cases whose label contains id; --v prints every case
#include "dect2/bandplan/bandplan.h"
#include "../common/cpu_report.h"
#include "../common/ci_slack.h"
#include "dect2/common/stable_random.h"
#include "dect2/channels/freq_db.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/engine/sig_ident.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/source/source.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;
using namespace dect2::sigid;
#include "../common/test_util.h"

namespace {

struct Case {
    std::string label, modeId, subId;   // the truth: ModeTuning::id and the sub-mode id ("" none)
    int std = 0;                        // engine standard (makeModeSynth); legacy: 0 = DVB-T2, -1 = DVB-T, -2 = ATSC
    const SubMode* sub = nullptr;
    double rate = 0, mhz = 0;
};

std::vector<Case> allCases() {
    std::vector<Case> v;
    auto add = [&](const char* label, const char* id, const char* sub, int std, double rate, double mhz, const SubMode* s = nullptr) {
        Case c; c.label = label; c.modeId = id; c.subId = sub; c.std = std; c.rate = rate; c.mhz = mhz; c.sub = s; v.push_back(c);
    };
    add("dvbt2", "dvbt", "", 0, 10e6, 522);
    add("dvbt", "dvbt", "", -1, 10e6, 522);
    add("atsc", "atsc", "", -2, 10e6, 575);
    add("dab", "dab", "", 4, 2.048e6, 227.36);
    add("atsc3", "atsc3", "", 5, 8e6, 575);
    add("isdbt", "isdbt", "", 6, 8e6, 557);
    add("fm", "fm", "", 7, 1e6, 98.5);
    int nsub = 0;
    const SubMode* subs = subModes(nsub);
    std::set<int> hasSub;
    for (int i = 0; i < nsub; i++) hasSub.insert(subs[i].stdMode);
    for (int s = 8; s < 200; s++) {
        const ModeTuning* mt = modeTuning(s);
        if (!mt || hasSub.count(s)) continue;
        double rate = std::max(mt->sampleRate, mt->minSampleRate);
        if (rate > 12e6 && mt->minSampleRate <= 12e6) rate = std::max(mt->minSampleRate, 10e6);
        add(mt->id, mt->id, "", s, rate, mt->defMhz);
    }
    for (int i = 0; i < nsub; i++) {
        const ModeTuning* mt = modeTuning(subs[i].stdMode);
        if (!mt) continue;
        double rate = std::max(mt->sampleRate, mt->minSampleRate);
        if (rate > 12e6) rate = 10e6;
        add(subs[i].id, mt->id, subs[i].id, subs[i].stdMode, rate, subs[i].defMhz);
        v.back().sub = &subs[i];
    }
    return v;
}

// n samples of a mode's test signal at the rate (after a short settling stretch)
bool render(Case& c, size_t n, std::vector<cf32>& out, std::string& why) {
    SynthConfig sc;
    sc.snrDb = 60;   // clean: the noise is added afterwards, at the SNR the case asks for
    const size_t skip = (size_t)(c.rate * 0.1);
    std::vector<cf32> all;
    if (c.std <= 0) {   // the TV standards come through the synthetic source (paced fast)
        DeviceInfo d;
        TuneSettings t;
        t.centerHz = c.mhz * 1e6; t.sampleRate = c.rate; t.bandwidthMhz = 8;
        t.synth.snrDb = 60; t.synth.pace = 40;
        t.synth.atsc = c.std == -2; t.synth.dvbt = c.std == -1;
        if (c.std == -2) t.bandwidthMhz = 6;
        IqRing ring(1u << 24);
        auto src = makeSource(d);
        if (!src->start(t, ring, why)) return false;
        all.resize(skip + n);
        size_t got = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (got < all.size() && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 60) {
            const size_t k = ring.read(all.data() + got, all.size() - got);
            got += k;
            if (!k) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        c.rate = src->sampleRate() > 0 ? src->sampleRate() : c.rate;   // the source runs at its own rate (the native one of the standard)
        src->stop();
        if (got < all.size()) { why = "source too slow"; return false; }
    } else {
        sc.mode = c.std;
        if (c.std == 4) sc.dab = true;
        if (c.sub) applySubModeSynth(sc, *c.sub);
        auto g = makeModeSynth(c.std, sc, c.rate);
        if (!g) { why = "no generator"; return false; }
        all.resize(skip + n);
        g->generate(all.data(), all.size());
    }
    out.assign(all.begin() + (long)skip, all.end());
    return true;
}

struct Rng { std::mt19937 g; explicit Rng(uint32_t s) : g(s) {} double u(double a, double b) { return dect2::uniform_real_distribution<double>(a, b)(g); } double n() { return dect2::normal_distribution<double>(0, 1)(g); } };

// The signal the identifier is asked about is the strongest one in the clean render (a few generators put theirs off the centre, or send several
// channels). It is moved by a random frequency, noise is added so that the SNR is that signal's average power over its width, and the click
// lands near its middle.
struct Prepared { std::vector<cf32> x; double offHz = 0, bw = 0, clickOff = 0; };
Prepared prepare(const std::vector<cf32>& clean, double fs, double snrDb, Rng& r) {
    Prepared p;
    double power = 0;
    for (const cf32& v : clean) power += std::norm(v);
    power /= clean.size();
    const std::vector<Region> regs = findRegions(clean.data(), clean.size(), fs, 24, 10);
    double c0 = 0, w0 = fs * 0.1, frac = 1;
    if (!regs.empty()) {
        c0 = regs[0].centreHz; w0 = std::max(regs[0].bwHz, fs / 4096 * 4);
        double tot = 0;
        for (const Region& g : regs) tot += std::pow(10.0, g.powerDb / 10);
        frac = std::pow(10.0, regs[0].powerDb / 10) / tot;
    }
    p.bw = w0;
    const double lim = 0.45 * fs - 0.5 * w0;
    const double lo = -lim - c0, hi = lim - c0;
    p.offHz = hi > lo + 1e3 ? r.u(lo, hi) : 0;
    p.x = clean;
    double ph = 0; const double d = 2 * M_PI * p.offHz / fs;
    const double sigma = std::sqrt(power * frac / std::pow(10.0, snrDb / 10) * (fs / w0) / 2);
    for (auto& v : p.x) {
        v *= cf32((float)std::cos(ph), (float)std::sin(ph));
        ph = std::fmod(ph + d, 2 * M_PI);
        v += cf32((float)(sigma * r.n()), (float)(sigma * r.n()));
    }
    p.clickOff = c0 + p.offHz + r.u(-0.2, 0.2) * std::min(w0, 25e3);   // a click lands near, not exactly on, the middle
    return p;
}

bool isTruth(const Case& c, const Candidate& k) {
    if (k.modeId != c.modeId) return false;
    return c.subId.empty() || k.subId == c.subId;
}

// Modes whose signals this identifier cannot tell apart by their spectrum and timing: the weak-signal tone modes of the WSJT family, the
// keyboard MFSK modes, LoRa with its different networks, the 100 Bd FSK of NAVTEX and DSC, the plain carriers. Getting one of the group right
// counts as a group-level hit (reported separately from the strict, mode-level result).
int groupOf(const std::string& mode, const std::string& sub) {
    static const char* const g[][12] = {
        {"ft8", "ft4", "ft2", "wspr", "jt65", "jt9", "q65", "fst4", "js8", nullptr},
        {"olivia", "contestia", "mfsk", "thor", "dominoex", nullptr},
        {"loraraw", "meshtastic", "meshcore", "loraaprs", "lorawan", nullptr},
        {"navtex", "dsc", nullptr},
        {"vor", "ils", "wefax", "time", nullptr},
    };
    const std::string id = sub.empty() ? mode : sub;
    for (int i = 0; i < 5; i++) for (int j = 0; g[i][j]; j++) if (id == g[i][j]) return i + 1;
    return 0;
}
bool sameGroup(const Case& c, const Candidate& k) {
    const int a = groupOf(c.modeId, c.subId), b = groupOf(k.modeId, k.subId);
    return a && a == b;
}

// Test signals in which there is nothing for a spectrum to identify, left out of the figures (and said so in the output): GNSS and radio astronomy are
// below the noise floor by design, the spectrum tools (explorer, analyser) are not signals, the UAT generator is silent for its first seconds.
const char* notIdentifiable(const std::string& label) {
    if (label == "gnss") return "spread spectrum below the noise floor";
    if (label == "astro") return "radio astronomy: a signal below the noise floor";
    if (label == "specx") return "the spectrum explorer's own test signal, not a mode";
    if (label == "specan") return "the spectrum analyser's own test signal, not a mode";
    if (label == "uat") return "the generator is silent in the first seconds";
    return nullptr;
}

std::string famOf(const Case& c) {
    for (size_t i = 0; i < signatureCount(); i++) {
        const SigInfo& s = signatureInfo(i);
        if (c.modeId == s.modeId && (c.subId.empty() || c.subId == s.subId)) return s.family;
    }
    return "(no signature)";
}

struct Tally { int n = 0, top1 = 0, top3 = 0, wrongSure = 0, unknown = 0, group1 = 0; };

// --signatures FILE: the rows of core/src/engine/sig_known_gen.cpp. Every mode with a test signal and no hand-written row in sig_known.cpp is
// rendered, moved off the centre, given 30 dB of SNR and measured (blind, no verify); its row says what came out. Rerun after a new mode is
// added (docs/IDENTIFIER.md); the identifier's own rows for such modes are only as good as that one measurement.
int writeSignatures(const char* path) {
    static const char* const kModEnum[ModCount] = {"ModUnknown", "ModNoise", "ModCarrier", "ModAm", "ModFm", "ModFsk2", "ModFsk4", "ModGmsk", "ModBpsk", "ModQpsk",
                                                   "ModPi4Dqpsk", "ModPsk8", "ModQam", "ModOfdm", "ModChirp", "ModPulse"};
    auto handWritten = [](const Case& c) {
        for (size_t i = 0; i < signatureCount(); i++) {
            const SigInfo& s = signatureInfo(i);
            if (s.source == 0 && c.modeId == s.modeId && (c.subId.empty() || c.subId == s.subId)) return true;
        }
        return false;
    };
    FILE* fo = fopen(path, "w");
    if (!fo) { printf("cannot write %s\n", path); return 1; }
    fprintf(fo, "// Generated by test_specx_idmatrix --signatures (do not edit): the signatures of the modes without a hand-written row in sig_known.cpp,\n"
                "// measured by the identifier on each mode's own test signal at 30 dB SNR (docs/IDENTIFIER.md).\n#include \"sig_known.h\"\n\nnamespace dect2 {\nnamespace sigid {\n\nconst Known kKnownMeasured[] = {\n");
    int rows = 0;
    for (Case& c : allCases()) {
        if (notIdentifiable(c.label) || handWritten(c)) continue;
        std::vector<cf32> clean;
        std::string why;
        const size_t n = (size_t)std::min(c.rate * captureSeconds(c.rate), 3.0e6);
        if (!render(c, n, clean, why)) continue;
        Rng r(0x5167u + (uint32_t)rows);
        const Prepared p = prepare(clean, c.rate, 30, r);
        const Region rg = regionAt(p.x.data(), p.x.size(), c.rate, p.clickOff);
        Options o; o.loHz = rg.loHz; o.hiHz = rg.hiHz; o.usePriors = false; o.verify = false;
        const Result res = identify(p.x.data(), p.x.size(), c.rate, o);
        const Features& f = res.f;
        if (!f.valid || f.mod <= ModNoise) { printf("  %s: nothing measured (%s)\n", c.label.c_str(), f.why.c_str()); continue; }
        const ModeTuning* mt = modeTuning(c.std);
        const char* name = c.sub ? c.sub->name : mt ? mt->name : c.label.c_str();
        char rates[64] = "";
        if (f.mod == ModOfdm && f.ofdmTuSec > 0) snprintf(rates, sizeof rates, "%.5g", f.ofdmTuSec);
        else if (f.mod != ModChirp && f.symRate > 0) snprintf(rates, sizeof rates, "%.5g", f.symRate);
        const double dev = f.fskLevels ? std::max(std::fabs(f.devHz[0]), std::fabs(f.devHz[f.fskLevels - 1])) : 0;
        const int duty = f.duty < 0.9 ? 2 : f.duty > 0.97 ? 1 : 0;
        std::string note = res.summary;
        for (char& ch : note) if (ch == '"' || ch == '\\') ch = '\'';
        fprintf(fo, "{\"%s\", \"%s\", \"%s\", \"Other modes\", MB(%s), %s, {%s}, {%.4g}, %.0f, %d, %.3g, %.3g, SyncNone, {}, \"measured on its test signal: %s\"},\n", name, c.modeId.c_str(), c.subId.c_str(),
                kModEnum[f.mod], kModEnum[f.mod], rates, f.obwHz, dev, duty, f.bursts >= 3 ? f.burstMs : 0.0, f.burstPeriodMs, note.c_str());
        printf("  %-12s %s\n", c.label.c_str(), res.summary.c_str());
        rows++;
    }
    if (!rows) fprintf(fo, "{nullptr, nullptr, nullptr, nullptr, 0, 0, {}, {}, 0, 0, 0, 0, SyncNone, {}, nullptr},\n");
    fprintf(fo, "};\nconst int kKnownMeasuredCount = %d;\n\n} // namespace sigid\n} // namespace dect2\n", rows);
    fclose(fo);
    printf("%d rows written to %s\n", rows, path);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool quick = false, verbose = false;
    std::string only;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--quick")) quick = true;
        else if (!strcmp(argv[i], "--v")) verbose = true;
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "--signatures") && i + 1 < argc) { (void)builtinFrequencies(); return writeSignatures(argv[i + 1]); }
        else if (!strcmp(argv[i], "--list")) { for (const Case& c : allCases()) printf("%s|%s|%s|%d|%.0f|%.3f\n", c.label.c_str(), c.modeId.c_str(), c.subId.c_str(), c.std, c.rate, c.mhz); return 0; }
    }
    (void)builtinFrequencies(); (void)bandplan::builtinPlans();   // parsed on first use: not inside the timing of the first case
    const std::vector<double> snrs = quick ? std::vector<double>{getenv("SNR") ? atof(getenv("SNR")) : 20.0} : std::vector<double>{10, 20, 30};
    const int trials = quick ? 1 : 2;
    const std::vector<int> freqKnown = quick ? std::vector<int>{0} : std::vector<int>{0, 1, 2};   // 0 blind, 1 the true frequency, 2 a wrong one (27.5 MHz)

    std::vector<Case> cases = allCases();
    // [freqKnown][snr] -> family -> tally;  per mode -> tally (blind, all SNRs)
    std::map<std::string, Tally> famTally[3][3], modeTally[3];
    std::map<std::string, std::map<std::string, int>> confusion;   // blind, SNR 20: truth -> predicted top-1 -> count
    Tally all[3][3];
    struct Bin { int n = 0, right = 0; double conf = 0; };
    Bin rel[3][10];
    double maxMs = 0, sumMs = 0; int nMs = 0;
    std::map<std::string, double> slow;
    std::vector<std::string> skipped, excluded;

    for (Case& c : cases) {
        if (!only.empty() && c.label.find(only) == std::string::npos) continue;
        if (const char* why = notIdentifiable(c.label)) { excluded.push_back(c.label + " (" + why + ")"); continue; }
        std::vector<cf32> clean;
        std::string why;
        const size_t n = (size_t)std::min(c.rate * captureSeconds(c.rate), 3.0e6);
        if (!render(c, n, clean, why)) { skipped.push_back(c.label + " (" + why + ")"); continue; }
        if (verbose && getenv("STATS")) {
            double pw = 0, pk = 0; size_t nz = 0;
            for (const cf32& v : clean) { const double a = std::norm(v); pw += a; pk = std::max(pk, a); nz += a > 1e-8; }
            printf("   S %-10s n %zu rate %.0f rms %.4f peak %.3f nonzero %.1f %%\n", c.label.c_str(), clean.size(), c.rate, std::sqrt(pw / clean.size()), std::sqrt(pk), 100.0 * nz / clean.size());
        }
        const std::string fam = famOf(c);
        for (size_t si = 0; si < snrs.size(); si++)
            for (int tr = 0; tr < trials; tr++) {
                Rng r(0x9E3779B9u * (uint32_t)(std::hash<std::string>()(c.label) + si * 17 + tr * 101));
                const Prepared p = prepare(clean, c.rate, snrs[si], r);
                for (int fk = 0; fk < (int)freqKnown.size(); fk++) {
                    const int fmode = freqKnown[fk];
                    const bool known = fmode != 0;
                    const int slot = quick ? 0 : fmode;
                    Options o;
                    o.rfHz = fmode == 1 ? c.mhz * 1e6 - p.offHz : fmode == 2 ? 27.5e6 - p.offHz : 0;
                    o.usePriors = known;
                    const Region rg = regionAt(p.x.data(), p.x.size(), c.rate, p.clickOff);
                    o.loHz = rg.loHz; o.hiHz = rg.hiHz;
                    if (verbose && getenv("REGIONS")) for (const Region& q : findRegions(p.x.data(), p.x.size(), c.rate, 8, 6)) printf("   region [%+.0f %+.0f] snr %.1f bw %.0f click %.0f\n", q.loHz, q.hiHz, q.snrDb, q.bwHz, p.clickOff);
                    const auto t0 = std::chrono::steady_clock::now();
                    const Result res = identify(p.x.data(), p.x.size(), c.rate, o);
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    maxMs = std::max(maxMs, ms); sumMs += ms; nMs++;
                    slow[c.label] = std::max(slow[c.label], ms);
                    int rank = -1;
                    for (size_t k = 0; k < res.cands.size() && k < 3; k++) if (isTruth(c, res.cands[k])) { rank = (int)k; break; }
                    const bool called = res.verdict != VerdictUnknown && !res.cands.empty();
                    const bool grp1 = called && rank != 0 && sameGroup(c, res.cands[0]);
                    const bool top1 = rank == 0 && called;   // a reject is a miss for top-1 (it is the honest answer, but not the right one)
                    const bool top3 = rank >= 0 && called;
                    const double conf = res.cands.empty() ? 0 : res.cands[0].confidence;
                    Tally* ts[3] = {&famTally[slot][snrs.size() == 3 ? (int)si : 0][fam], &modeTally[slot][c.label], &all[slot][snrs.size() == 3 ? (int)si : 0]};
                    for (Tally* t : ts) {
                        t->n++; t->top1 += top1; t->top3 += top3; t->unknown += !called; t->group1 += top1 || grp1;
                        t->wrongSure += called && rank != 0 && !grp1 && conf >= 0.5;
                    }
                    if (called) {
                        Bin& b = rel[slot][std::min(9, (int)(conf * 10))];
                        b.n++; b.right += rank == 0; b.conf += conf;
                    }
                    if (!known && (quick || si == 1)) confusion[c.label][called && !res.cands.empty() ? res.cands[0].name : "(unknown)"]++;
                    if (const char* csv = getenv("CSV")) {   // for fitting the confidence constants offline: one line per case, every candidate with its score
                        static FILE* fcsv = fopen(csv, "w");
                        fprintf(fcsv, "%s,%s,%s,%.0f,%d,%d", c.label.c_str(), c.modeId.c_str(), c.subId.c_str(), snrs[si], fmode, rank);
                        for (const Candidate& k : res.cands) fprintf(fcsv, ",%s|%s|%.1f%s", k.modeId.c_str(), k.subId.c_str(), k.score, isTruth(c, k) ? "|T" : "");
                        fprintf(fcsv, "\n"); fflush(fcsv);
                    }
                    if (verbose && getenv("FEAT")) {
                        const Features& g = res.f;
                        printf("   F %-10s mod %-10s rate %9.1f obw %9.0f bw3 %9.0f bw20 %9.0f bw26 %9.0f flat %d duty %.2f bursts %3d len %7.2f per %7.2f kurt %.2f car %5.1f lev%d %+.0f/%+.0f/%+.0f/%+.0f tu %.1f tg %.1f snr %.1f\n", c.label.c_str(), modName(g.mod), g.symRate, g.obwHz, g.bw3Hz, g.bw20Hz, g.bw26Hz, g.flatTop,
                               g.duty, g.bursts, g.burstMs, g.burstPeriodMs, g.envKurt, g.carrierDb, g.fskLevels, g.devHz[0], g.devHz[1], g.devHz[2], g.devHz[3], g.ofdmTuSec * 1e6, g.ofdmTgSec * 1e6, g.snrDb);
                    }
                    if (verbose)
                        printf("%-14s snr %2.0f off %+8.0f bw %7.0f reg [%+.0f %+.0f] %s %-7s -> %-22s %3.0f%% %s | %s\n", c.label.c_str(), snrs[si], p.offHz, p.bw, rg.loHz, rg.hiHz, fmode == 1 ? "freq " : fmode == 2 ? "wrong" : "blind", rank == 0 ? "ok" : rank > 0 ? "top3" : "MISS",
                               res.cands.empty() ? "-" : res.cands[0].name.c_str(), conf * 100, res.verdict == VerdictUnknown ? "unknown" : res.verdict == VerdictAmbiguous ? "ambiguous" : "likely", res.summary.c_str());
                }
            }
    }
    for (const auto& s : skipped) printf("skipped (no generator): %s\n", s.c_str());
    if (!excluded.empty()) { printf("left out of the figures, nothing for a spectrum to identify:"); for (const auto& e : excluded) printf(" %s;", e.c_str()); printf("\n"); }

    auto pct = [](int a, int b) { return b ? 100.0 * a / b : 0.0; };
    for (int slot = 0; slot < (quick ? 1 : 3); slot++) {
        printf("\n==== %s ====\n", quick ? "blind (frequency not used), SNR 20 dB" : slot == 2 ? "a wrong frequency given (27.5 MHz: the prior must not make a signal that does not fit into a match)" : slot ? "frequency known (database and band plan used)" : "blind (frequency not used)");
        for (int si = 0; si < (quick ? 1 : 3); si++) {
            if (!quick) printf("-- SNR %.0f dB: top-1 %.1f %% (group-level %.1f %%), top-3 %.1f %%, rejected %.1f %%, wrong with confidence >= 50 %%: %.1f %% (%d cases)\n", snrs[si], pct(all[slot][si].top1, all[slot][si].n), pct(all[slot][si].group1, all[slot][si].n),
                               pct(all[slot][si].top3, all[slot][si].n), pct(all[slot][si].unknown, all[slot][si].n), pct(all[slot][si].wrongSure, all[slot][si].n), all[slot][si].n);
            else printf("-- top-1 %.1f %% (group-level %.1f %%), top-3 %.1f %%, rejected %.1f %%, wrong with confidence >= 50 %%: %.1f %% (%d cases)\n", pct(all[slot][0].top1, all[slot][0].n), pct(all[slot][0].group1, all[slot][0].n), pct(all[slot][0].top3, all[slot][0].n),
                        pct(all[slot][0].unknown, all[slot][0].n), pct(all[slot][0].wrongSure, all[slot][0].n), all[slot][0].n);
            std::map<std::string, Tally>& ft = famTally[slot][si];
            for (const auto& kv : ft) printf("   %-24s n %3d  top-1 %5.1f %%  top-3 %5.1f %%  group-level top-1 %5.1f %%\n", kv.first.c_str(), kv.second.n, pct(kv.second.top1, kv.second.n), pct(kv.second.top3, kv.second.n), pct(kv.second.group1, kv.second.n));
        }
        printf("-- per mode (all SNRs): ");
        int shown = 0;
        for (const auto& kv : modeTally[slot]) {
            if (kv.second.top1 * 2 >= kv.second.n) continue;   // only the ones below 50 % top-1
            printf("%s%s %d/%d", shown++ ? ", " : "", kv.first.c_str(), kv.second.top1, kv.second.n);
        }
        printf("%s\n", shown ? "" : "all modes at least 50 % top-1");
        printf("-- confidence reliability (is the number honest?): bin  n  mean-conf  right\n");
        for (int b = 0; b < 10; b++) if (rel[slot][b].n) printf("   %d0-%d0 %%  n %4d  conf %5.1f %%  right %5.1f %%\n", b, b + 1, rel[slot][b].n, 100 * rel[slot][b].conf / rel[slot][b].n, pct(rel[slot][b].right, rel[slot][b].n));
    }
    printf("\n==== confusions (blind, SNR %s dB): truth -> top-1 where it was not the truth ====\n", quick ? "20" : "20");
    for (const auto& kv : confusion) {
        std::string line;
        for (const auto& p : kv.second) {
            // the truth label is a mode label; compare by the name the candidate carries, best effort: print everything that is not the majority
            line += " " + p.first + " x" + std::to_string(p.second) + ";";
        }
        printf("  %-14s%s\n", kv.first.c_str(), line.c_str());
    }

    // ---- reject: signals that are none of the modes
    int rej[3] = {}, rejN[3] = {};
    {
        const double fs = 1e6;
        Rng r(77);
        for (int t = 0; t < 12; t++) {
            std::vector<cf32> x((size_t)(fs * 0.5));
            const int kind = t % 3;   // 0 noise only, 1 band-limited noise 12.5 kHz at +100 kHz, 2 two unrelated tones
            double lp = 0, ph1 = 0, ph2 = 0;
            for (size_t i = 0; i < x.size(); i++) {
                cf32 nz((float)(0.01 * r.n()), (float)(0.01 * r.n()));
                if (kind == 1) {   // single-pole low-pass of white noise, mixed to +100 kHz
                    lp = 0.97 * lp + 0.03 * r.n();
                    const double a = 2 * M_PI * 100e3 * i / fs;
                    nz += cf32((float)(0.25 * lp * std::cos(a)), (float)(0.25 * lp * std::sin(a)));
                } else if (kind == 2) {
                    ph1 += 2 * M_PI * (80e3 + 30 * r.n()) / fs; ph2 += 2 * M_PI * (113e3 + 30 * r.n()) / fs;
                    nz += cf32((float)(0.1 * std::cos(ph1) + 0.1 * std::cos(ph2)), (float)(0.1 * std::sin(ph1) + 0.1 * std::sin(ph2)));
                }
                x[i] = nz;
            }
            Options o; o.rfHz = 0; o.usePriors = false;
            if (kind) { const Region rg = regionAt(x.data(), x.size(), fs, kind == 1 ? 100e3 : 96e3); o.loHz = rg.loHz; o.hiHz = rg.hiHz; }
            const Result res = identify(x.data(), x.size(), fs, o);
            rejN[kind]++;
            rej[kind] += res.verdict == VerdictUnknown || !res.f.valid;
            if (verbose) printf("reject kind %d: %s | %s\n", kind, res.summary.c_str(), res.cands.empty() ? "no candidates" : res.cands[0].name.c_str());
        }
        printf("\n==== reject option: noise %d/%d, band-limited noise %d/%d, two tones %d/%d answered \"unknown\" ====\n", rej[0], rejN[0], rej[1], rejN[1], rej[2], rejN[2]);
    }
    printf("\nidentify(): mean %.0f ms, worst %.0f ms\n", nMs ? sumMs / nMs : 0, maxMs);
    {
        std::vector<std::pair<double, std::string>> sl;
        for (const auto& kv : slow) sl.push_back({kv.second, kv.first});
        std::sort(sl.rbegin(), sl.rend());
        printf("slowest:");
        for (size_t i = 0; i < sl.size() && i < 12; i++) printf(" %s %.0f ms;", sl[i].second.c_str(), sl[i].first);
        printf("\n");
    }

    if (!only.empty() || verbose) { printf("(partial run: thresholds not checked)\n"); return 0; }
    // ---- thresholds: the measured values (docs/IDENTIFIER.md) minus a margin of about five points, which the platforms' floating point can eat.
    // They hold the line against a change that makes the identifier worse; they are not claims about real signals.
    auto fam = [&](int slot, int si, const char* name) { const auto it = famTally[slot][si].find(name); return it == famTally[slot][si].end() ? Tally() : it->second; };
    if (quick) {
        CHECK(pct(all[0][0].top1, all[0][0].n) >= 42, "blind top-1 at 20 dB %.1f %% (>= 42)", pct(all[0][0].top1, all[0][0].n));
        CHECK(pct(all[0][0].top3, all[0][0].n) >= 50, "blind top-3 at 20 dB %.1f %% (>= 50)", pct(all[0][0].top3, all[0][0].n));
        CHECK(pct(all[0][0].wrongSure, all[0][0].n) <= 14, "wrong with confidence >= 50 %%: %.1f %% (<= 14)", pct(all[0][0].wrongSure, all[0][0].n));
        CHECK(pct(fam(0, 0, "Broadcast").top1, fam(0, 0, "Broadcast").n) >= 70, "broadcast top-1 at 20 dB %.1f %% (>= 70)", pct(fam(0, 0, "Broadcast").top1, fam(0, 0, "Broadcast").n));
        CHECK(pct(fam(0, 0, "Digital voice").top1, fam(0, 0, "Digital voice").n) >= 75, "digital voice top-1 at 20 dB %.1f %% (>= 75)", pct(fam(0, 0, "Digital voice").top1, fam(0, 0, "Digital voice").n));
    } else {
        static const double kBlind1[3] = {33, 42, 45}, kBlind3[3] = {39, 50, 53}, kKnown1[3] = {42, 51, 55};
        for (int si = 0; si < 3; si++) {
            CHECK(pct(all[0][si].top1, all[0][si].n) >= kBlind1[si], "blind top-1 at %.0f dB %.1f %% (>= %.0f)", snrs[si], pct(all[0][si].top1, all[0][si].n), kBlind1[si]);
            CHECK(pct(all[0][si].top3, all[0][si].n) >= kBlind3[si], "blind top-3 at %.0f dB %.1f %% (>= %.0f)", snrs[si], pct(all[0][si].top3, all[0][si].n), kBlind3[si]);
            CHECK(pct(all[1][si].top1, all[1][si].n) >= kKnown1[si], "frequency known: top-1 at %.0f dB %.1f %% (>= %.0f)", snrs[si], pct(all[1][si].top1, all[1][si].n), kKnown1[si]);
            CHECK(pct(all[2][si].top1, all[2][si].n) >= pct(all[0][si].top1, all[0][si].n) - 10, "a wrong frequency costs more than 10 points at %.0f dB: %.1f %% against %.1f %% blind", snrs[si], pct(all[2][si].top1, all[2][si].n), pct(all[0][si].top1, all[0][si].n));
            CHECK(pct(all[0][si].wrongSure, all[0][si].n) <= 14, "wrong with confidence >= 50 %% at %.0f dB: %.1f %% (<= 14)", snrs[si], pct(all[0][si].wrongSure, all[0][si].n));
        }
        CHECK(pct(fam(0, 1, "Broadcast").top1, fam(0, 1, "Broadcast").n) >= 70, "broadcast top-1 at 20 dB %.1f %% (>= 70)", pct(fam(0, 1, "Broadcast").top1, fam(0, 1, "Broadcast").n));
        CHECK(pct(fam(0, 1, "Digital voice").top1, fam(0, 1, "Digital voice").n) >= 75, "digital voice top-1 at 20 dB %.1f %% (>= 75)", pct(fam(0, 1, "Digital voice").top1, fam(0, 1, "Digital voice").n));
    }
    // the stated confidence must be honest: of the answers stated at 80 % or more, at least 85 % were right (all settings together)
    {
        int n = 0, right = 0;
        for (int slot = 0; slot < (quick ? 1 : 3); slot++) for (int b = 8; b < 10; b++) { n += rel[slot][b].n; right += rel[slot][b].right; }
        CHECK(n == 0 || pct(right, n) >= 85, "answers stated at 80 %% or more were right in %.1f %% of %d cases (>= 85)", pct(right, n), n);
    }
    CHECK(rej[0] == rejN[0], "noise only answered \"unknown\" %d of %d times", rej[0], rejN[0]);
    CHECK(rej[2] >= rejN[2] - 1, "two unrelated tones answered \"unknown\" %d of %d times", rej[2], rejN[2]);
    CHECK(rej[1] >= rejN[1] / 2, "band-limited noise answered \"unknown\" %d of %d times", rej[1], rejN[1]);
    // speed: the target is 200 ms; the test allows a slow CI machine
    cpuCal("specx_idmatrix", "identify() on average", nMs ? sumMs / nMs : 0.0, "ms", '<', 150 * ciSlack(), nMs > 0);
    CHECK(nMs && sumMs / nMs < 150 * ciSlack(), "identify() took %.0f ms on average, the target is 100 and the limit here 150", nMs ? sumMs / nMs : 0.0);
    cpuCal("specx_idmatrix", "identify() at worst", maxMs, "ms", '<', 500 * ciSlack());
    CHECK(maxMs < 500 * ciSlack(), "identify() took %.0f ms at worst, the target is 200 and the limit here 500", maxMs);
    return testSummary("specx_idmatrix");
}
