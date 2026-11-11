// CW skimmer timing benchmark: the 0.2.7 timing (cw_morse.h) against the adaptive one (cw_decoder.h) on the whole receiver (CwReceiver),
// the scenarios of docs/SENSITIVITY-3.9-E.md section 3. Not a ctest: build by hand, run
//   cw_skim_bench rx [seeds=2] [secs=80]        the receiver itself, every timing choice (legacy, adaptive, automatic)
//   cw_skim_bench cap <dir> [seeds=2] [secs=80] the receiver once per case, the detector's key saved (one byte a millisecond)
//   cw_skim_bench rep <dir> [seeds=2]           the saved keys through both timings (fast: for tuning the decoder)
//   cw_skim_bench one <dir> <case> <snr> <seed> the saved keys of one case through both, the texts printed
// CER: the edit distance of the middle copies of the text (not the first and the last) in the decoded text.
#include "fist_harness.h"
#include "dect2/cw/cw_gen.h"
#include "dect2/cw/cw_rx.h"
#include "dect2/cw/cw_tel.h"
#include "dect2/cw/cw_morse.h"
#include "dect2/cw/cw_decoder.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace dect2;

namespace {
constexpr double kRate = 250000, kOff = -20000, kSig = 500;
const char* kText = "CQ DE LY2ABC LY2ABC K";
const double kSnr[] = {0, 2, 4, 6, 10, 20};

struct Case { const char* name; double wpm, fw, jitter, ratio; };
const Case kCases[] = {
    {"machine", 22, 0, 0, 3},
    {"hand", 20, 0, 0.12, 3.4},
    {"sloppy", 20, 0, 0.22, 2.7},
    {"farnsworth", 20, 12, 0, 3},
};

CwGenSignal signalOf(const Case& c, double snr) {
    CwGenSignal s;
    s.offsetHz = kSig; s.wpm = c.wpm; s.farnsworthWpm = c.fw; s.jitter = c.jitter; s.dashRatio = c.ratio;
    s.text = kText; s.snrDb = snr; s.pauseSec = 1.0;
    return s;
}

std::string refText(const Case& c, double secs) {
    const CwGenSignal s = signalOf(c, 20);
    const double per = CwGenerator::messageSec(s) + s.pauseSec;
    const int copies = (int)((secs - s.startSec - 1.0) / per);
    std::string r;
    for (int i = 1; i + 1 < copies; i++) { if (!r.empty()) r += ' '; r += kText; }
    return r;
}

double cer(const std::string& ref, const std::string& dec) {
    return ref.empty() ? 0 : 100.0 * fh::editDistance(ref, fh::norm(dec)) / (double)ref.size();
}

struct RxOut { std::string text; std::vector<uint8_t> keys; };

RxOut runRx(const Case& c, double snr, uint32_t seed, double secs, int timing, bool tap) {
    CwGenConfig g;
    g.rate = kRate; g.dialOffsetHz = kOff; g.seed = seed; g.signals = {signalOf(c, snr)};
    CwGenerator gen(g);
    CwReceiver r;
    r.setSilent(true);
    r.configure(kRate);
    r.setSignalOffset(kOff);
    r.setTimingDecoder(timing);
    std::map<int, std::vector<uint8_t>> keys;
    std::map<int, size_t> first;
    size_t ms = 0;
    if (tap) r.setKeyTap([&](int id, double hz, bool k) {
        if (std::fabs(hz - kSig) > 40) return;
        auto& v = keys[id];
        if (v.empty()) first[id] = ms;
        v.push_back(k ? 1 : 0);
    });
    std::vector<cf32> x(25000);
    CwTelemetry t;
    uint64_t seq = 0;
    for (double tt = 0; tt < secs; tt += 0.1) {
        gen.generate(x.data(), x.size());
        r.feed(x.data(), x.size());
        ms += 100;
        CwTelemetry n;
        if (r.telemetry(n, seq)) { seq = n.seq; t = std::move(n); }
    }
    RxOut o;
    const CwSignal* best = nullptr;
    for (const auto& s : t.signals) if (std::fabs(s.offsetHz - kSig) < 40 && (!best || s.chars > best->chars)) best = &s;
    if (best) o.text = best->text;
    size_t most = 0; int id = -1;
    for (auto& [k, v] : keys) if (v.size() > most) { most = v.size(); id = k; }
    if (id >= 0) { o.keys.assign(first[id], 0); o.keys.insert(o.keys.end(), keys[id].begin(), keys[id].end()); }
    return o;
}

// the receiver's two timing paths, replayed on a saved key
std::string replayLegacy(const std::vector<uint8_t>& k) {
    cw::Timing tm; cw::Keyer ky;
    std::string s;
    for (uint8_t b : k) { ky.sample(b != 0, tm); s += tm.take(); }
    return s;
}
std::string replayFist(const std::vector<uint8_t>& k) {
    cw::FistDecoder d;
    bool key = false, started = false; int run = 0, cnt = 0;
    std::string s;
    cw::DecTok t[32];
    for (uint8_t b : k) {
        const bool raw = b != 0;
        const int g = d.locked() ? std::max(3, std::min(25, (int)std::lround(0.33 * d.dotMs()))) : 6;
        cnt = raw != key ? cnt + 1 : 0;
        run++;
        if (cnt >= g) {
            const int len = run - cnt;
            if (key) d.mark(len); else if (started) d.space(len);
            started = true; key = raw; run = cnt; cnt = 0;
        }
        if (!key && started && run % 10 == 0) d.idle(run);
        for (size_t n; (n = d.pop(t, 32)) > 0;) for (size_t i = 0; i < n; i++) s += t[i].text;
    }
    return s;
}

std::string keyPath(const char* dir, int ci, double snr, int seed) {
    char b[512]; snprintf(b, sizeof b, "%s/%s_%02d_%d.key", dir, kCases[ci].name, (int)snr, seed);
    return b;
}
std::string secsPath(const char* dir) { return std::string(dir) + "/secs.txt"; }
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: see the file header\n"); return 2; }
    const std::string cmd = argv[1];
    if (cmd == "rx") {
        const int seeds = argc > 2 ? atoi(argv[2]) : 2;
        const double secs = argc > 3 ? atof(argv[3]) : 80;
        const char* tn[] = {"auto", "legacy", "adaptive"};
        printf("| CER %% at SNR in 500 Hz |"); for (double s : kSnr) printf(" %g |", s); printf("\n");
        for (int ci = 0; ci < 4; ci++) {
            const std::string ref = refText(kCases[ci], secs);
            for (int tmg : {1, 2, 0}) {
                printf("| %s, %s |", kCases[ci].name, tn[tmg]);
                for (double snr : kSnr) {
                    double e = 0;
                    for (int sd = 1; sd <= seeds; sd++) e += cer(ref, runRx(kCases[ci], snr, (uint32_t)sd, secs, tmg, false).text);
                    printf(" %.1f |", e / seeds); fflush(stdout);
                }
                printf("\n");
            }
        }
        return 0;
    }
    if (cmd == "cap" && argc > 2) {
        const int seeds = argc > 3 ? atoi(argv[3]) : 2;
        const double secs = argc > 4 ? atof(argv[4]) : 80;
        if (FILE* f = fopen(secsPath(argv[2]).c_str(), "w")) { fprintf(f, "%g\n", secs); fclose(f); }
        for (int ci = 0; ci < 4; ci++)
            for (double snr : kSnr)
                for (int sd = 1; sd <= seeds; sd++) {
                    const RxOut o = runRx(kCases[ci], snr, (uint32_t)sd, secs, 1, true);
                    FILE* f = fopen(keyPath(argv[2], ci, snr, sd).c_str(), "wb");
                    if (!f) { perror("key file"); return 1; }
                    fwrite(o.keys.data(), 1, o.keys.size(), f); fclose(f);
                    const std::string rl = replayLegacy(o.keys);
                    printf("%s %g dB seed %d: %zu ms, receiver legacy CER %.1f, replayed %.1f%s\n", kCases[ci].name, snr, sd, o.keys.size(),
                           cer(refText(kCases[ci], secs), o.text), cer(refText(kCases[ci], secs), rl), fh::norm(rl) == fh::norm(o.text) ? "" : "  (texts differ)");
                    fflush(stdout);
                }
        return 0;
    }
    if ((cmd == "rep" || cmd == "one") && argc > 2) {
        double secs = 80;
        if (FILE* f = fopen(secsPath(argv[2]).c_str(), "r")) { if (fscanf(f, "%lf", &secs) != 1) secs = 80; fclose(f); }
        auto load = [&](int ci, double snr, int sd) {
            std::vector<uint8_t> k;
            if (FILE* f = fopen(keyPath(argv[2], ci, snr, sd).c_str(), "rb")) {
                uint8_t b[4096]; size_t n;
                while ((n = fread(b, 1, sizeof b, f)) > 0) k.insert(k.end(), b, b + n);
                fclose(f);
            }
            return k;
        };
        if (cmd == "one" && argc > 5) {
            int ci = 0; for (int i = 0; i < 4; i++) if (!strcmp(kCases[i].name, argv[3])) ci = i;
            const auto k = load(ci, atof(argv[4]), atoi(argv[5]));
            printf("REF: %s\nLEG: %s\nADA: %s\n", refText(kCases[ci], secs).c_str(), fh::norm(replayLegacy(k)).c_str(), fh::norm(replayFist(k)).c_str());
            return 0;
        }
        const int seeds = argc > 3 ? atoi(argv[3]) : 2;
        double tl = 0, ta = 0, worst = -1e9; int nw = 0;
        printf("| CER %% at SNR in 500 Hz |"); for (double s : kSnr) printf(" %g |", s); printf("\n");
        for (int ci = 0; ci < 4; ci++) {
            const std::string ref = refText(kCases[ci], secs);
            double l[6] = {}, a[6] = {};
            for (int si = 0; si < 6; si++)
                for (int sd = 1; sd <= seeds; sd++) {
                    const auto k = load(ci, kSnr[si], sd);
                    l[si] += cer(ref, replayLegacy(k)) / seeds;
                    a[si] += cer(ref, replayFist(k)) / seeds;
                }
            printf("| %s, legacy |", kCases[ci].name); for (double v : l) printf(" %.1f |", v); printf("\n");
            printf("| %s, adaptive |", kCases[ci].name); for (double v : a) printf(" %.1f |", v); printf("\n");
            for (int si = 0; si < 6; si++) { tl += l[si]; ta += a[si]; worst = std::max(worst, a[si] - l[si]); if (a[si] > l[si] + 0.05) nw++; }
        }
        printf("sum legacy %.1f adaptive %.1f; worst adaptive - legacy %+.1f; %d points worse\n", tl, ta, worst, nw);
        return 0;
    }
    fprintf(stderr, "usage: see the file header\n");
    return 2;
}
