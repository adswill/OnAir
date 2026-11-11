// The CW skimmer's timing decoder: the adaptive one (cw::FistDecoder) is the default and must read at least as well as the 0.2.7 timing
// on every case of docs/SENSITIVITY-3.9-E.md section 3, and better on hand keying. Through the whole receiver (CwReceiver) on generated
// signals (simulated only): steady keying near the decoding limit, hand keying, sloppy keying, Farnsworth spacing, the first character
// after a pause, noise alone. Plus the decoder alone on keying durations: Farnsworth gaps from the first transmission, noise blips in a
// pause, a lost lock that comes back at the same speed.
// Long sweep (every SNR of the doc, 4 seeds): ONAIR_TEST_LONG=1.
#include "dect2/cw/cw_gen.h"
#include "dect2/cw/cw_rx.h"
#include "dect2/cw/cw_tel.h"
#include "dect2/cw/cw_decoder.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

namespace {
constexpr double kRate = 250000, kOff = -20000;
const char* kText = "CQ DE LY2ABC LY2ABC K";

struct Case { const char* name; double wpm, fw, jitter, ratio; };
const Case kMachine{"machine", 22, 0, 0, 3}, kHand{"hand", 20, 0, 0.12, 3.4}, kSloppy{"sloppy", 20, 0, 0.22, 2.7},
           kFarns{"farnsworth", 20, 12, 0, 3};

CwGenSignal sigOf(const Case& c, double snr, double hz) {
    CwGenSignal s;
    s.offsetHz = hz; s.wpm = c.wpm; s.farnsworthWpm = c.fw; s.jitter = c.jitter; s.dashRatio = c.ratio;
    s.text = kText; s.snrDb = snr; s.pauseSec = 1.0;
    return s;
}

int editDistance(const std::string& ref, const std::string& dec) {   // the whole reference, anywhere in the decoded text
    const size_t m = ref.size(), n = dec.size();
    std::vector<int> prev(n + 1, 0), cur(n + 1, 0);
    for (size_t i = 1; i <= m; i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= n; j++) cur[j] = std::min({prev[j - 1] + (ref[i - 1] == dec[j - 1] ? 0 : 1), prev[j] + 1, cur[j - 1] + 1});
        std::swap(prev, cur);
    }
    return *std::min_element(prev.begin(), prev.end());
}
std::string norm(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == ' ' && (o.empty() || o.back() == ' ')) continue; o += c; }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

struct Out { std::string text; CwSignal sig; double cer = 100; };

// the signal at hz through the receiver; CER (%) of the copies after the first
Out run(const Case& c, double snr, uint32_t seed, double secs, int timing, double hz = 500, double cfo = 0) {
    CwGenConfig g;
    g.rate = kRate; g.dialOffsetHz = kOff; g.seed = seed; g.cfoHz = cfo; g.signals = {sigOf(c, snr, hz)};
    CwGenerator gen(g);
    CwReceiver r;
    r.setSilent(true);
    r.configure(kRate);
    r.setSignalOffset(kOff);
    if (timing >= 0) r.setTimingDecoder(timing);
    std::vector<cf32> x(25000);
    CwTelemetry t;
    uint64_t seq = 0;
    for (double tt = 0; tt < secs; tt += 0.1) {
        gen.generate(x.data(), x.size());
        r.feed(x.data(), x.size());
        CwTelemetry n;
        if (r.telemetry(n, seq)) { seq = n.seq; t = std::move(n); }
    }
    Out o;
    const CwSignal* best = nullptr;
    for (const auto& s : t.signals) if (std::fabs(s.offsetHz - hz - cfo) < 40 && (!best || s.chars > best->chars)) best = &s;
    if (best) { o.text = best->text; o.sig = *best; }
    const CwGenSignal s = sigOf(c, 20, hz);
    const double per = CwGenerator::messageSec(s) + s.pauseSec;
    const int copies = (int)((secs - s.startSec - 1.0) / per);
    std::string ref;
    for (int i = 1; i < copies; i++) { if (!ref.empty()) ref += ' '; ref += kText; }
    o.cer = ref.empty() ? 0 : 100.0 * editDistance(ref, norm(o.text)) / (double)ref.size();
    return o;
}

// adaptive (the default) against the 0.2.7 timing, mean CER over the seeds
void compare(const Case& c, double snr, int seeds, double secs, double margin, double maxAdaptive, double* outA = nullptr, double* outL = nullptr) {
    double a = 0, l = 0;
    for (int s = 1; s <= seeds; s++) {
        a += run(c, snr, (uint32_t)s, secs, -1).cer / seeds;
        l += run(c, snr, (uint32_t)s, secs, 1).cer / seeds;
    }
    printf("  %-10s %4.0f dB: adaptive %5.1f %%  0.2.7 timing %5.1f %%\n", c.name, snr, a, l);
    CHECK(a <= l + margin, "%s %.0f dB: adaptive CER %.1f %% over the 0.2.7 timing's %.1f %%", c.name, snr, a, l);
    CHECK(a <= maxAdaptive, "%s %.0f dB: adaptive CER %.1f %% (at most %.1f)", c.name, snr, a, maxAdaptive);
    if (outA) *outA = a;
    if (outL) *outL = l;
}

// the decoder alone: durations in, text out
std::string decode(cw::FistDecoder& d, const std::vector<double>& durSec) {
    std::string s;
    cw::DecTok t[64];
    for (size_t i = 0; i < durSec.size(); i++) {
        const double ms = durSec[i] * 1000;
        if (i % 2 == 0) d.mark(ms);
        else {
            for (double k = 10; k < ms; k += 10) d.idle(k);   // a receiver calls idle() while the key is up
            d.space(ms);
        }
        for (size_t n; (n = d.pop(t, 64)) > 0;) for (size_t j = 0; j < n; j++) s += t[j].text;
    }
    for (double k = 10; k < 3000; k += 10) d.idle(k);
    for (size_t n; (n = d.pop(t, 64)) > 0;) for (size_t j = 0; j < n; j++) s += t[j].text;
    return s;
}
} // namespace

int main() {
    const bool longRun = std::getenv("ONAIR_TEST_LONG") != nullptr;

    printf("decoder alone\n");
    {   // Farnsworth 20 / 10 WPM (character gaps ~12 dots, word gaps ~28): read from the first transmission, letters together
        std::vector<double> k = cwKeying("CQ CQ DE LY2ABC LY2ABC TEST K", 20, 10);
        cw::FistDecoder d;
        const std::string s = norm(decode(d, k));
        printf("  farnsworth 20/10: \"%s\"\n", s.c_str());
        CHECK(s.find("DE LY2ABC LY2ABC TEST K") != std::string::npos, "Farnsworth 20/10: \"%s\"", s.c_str());
        cw::FistStats st; d.stats(st);
        CHECK(st.charGapDots > 6 && st.wordGapDots > st.charGapDots * 1.6f, "Farnsworth gaps: char %.1f word %.1f dots", st.charGapDots, st.wordGapDots);
    }
    {   // noise blips in a pause (the detector opening on noise) must not become a letter in front of the next word
        std::vector<double> a = cwKeying("CQ CQ DE LY2ABC LY2ABC K", 22);
        std::vector<double> b = cwKeying("CQ DE LY2ABC K", 22);
        std::vector<double> k = a;
        k.back() = 0.45;                 // the pause: blips of 12 - 22 ms with gaps between them
        for (double blip : {0.016, 0.022, 0.012, 0.020}) { k.push_back(blip); k.push_back(0.20); }
        k.push_back(0.014); k.push_back(0.065);   // the last one just before the first dash of "CQ"
        k.insert(k.end(), b.begin(), b.end());
        cw::FistDecoder d;
        const std::string s = norm(decode(d, k));
        printf("  blips in the pause: \"%s\"\n", s.c_str());
        CHECK(s.find("K CQ DE LY2ABC K") != std::string::npos, "blips in the pause: \"%s\"", s.c_str());
    }
    const double speeds[3][2] = {{25, 15}, {15, 30}, {20, 12}};
    for (const auto& w : speeds) {
        // another speed after a pause: the first word after it is read at the new speed
        std::vector<double> k = cwKeying("CQ CQ DE LY2ABC LY2ABC K", w[0]);
        k.back() = 3.0;
        const std::vector<double> b = cwKeying("TEST DL1XYZ DL1XYZ PSE K", w[1]);
        k.insert(k.end(), b.begin(), b.end());
        cw::FistDecoder d;
        const std::string s = norm(decode(d, k));
        printf("  %.0f -> %.0f WPM after a pause: \"%s\"\n", w[0], w[1], s.c_str());
        CHECK(s.find("K TEST DL1XYZ DL1XYZ PSE K") != std::string::npos, "%.0f -> %.0f WPM after a pause: \"%s\"", w[0], w[1], s.c_str());
        CHECK(std::fabs(d.wpm() - w[1]) < 0.1 * w[1], "%.0f -> %.0f WPM: speed %.1f", w[0], w[1], d.wpm());
    }
    {   // the same sender after a long noisy stretch (the lock is lost and found again): the Farnsworth gaps are kept
        std::vector<double> k = cwKeying("CQ CQ DE LY2ABC LY2ABC K", 18, 10);
        std::mt19937 g(5);
        std::uniform_real_distribution<double> um(0.004, 0.035), ug(0.02, 0.4);   // the detector opening on noise
        k.back() = 0.5;
        for (int i = 0; i < 120; i++) { k.push_back(um(g)); k.push_back(ug(g)); }
        k.back() = 2.0;
        const std::vector<double> b = cwKeying("TEST DL1XYZ DL1XYZ K", 18, 10);
        k.insert(k.end(), b.begin(), b.end());
        cw::FistDecoder d;
        const std::string s = norm(decode(d, k));
        printf("  after noise: \"%s\"\n", s.size() > 90 ? s.substr(s.size() - 90).c_str() : s.c_str());
        CHECK(s.find("DL1XYZ DL1XYZ K") != std::string::npos, "after noise: \"%s\"", s.c_str());
    }

    printf("receiver, adaptive (the default) vs the 0.2.7 timing\n");
    {
        CwReceiver r;
        CHECK(r.timingDecoder() == 0, "default timing %d", r.timingDecoder());
    }
    if (!longRun) {
        // 80 s, seeds 1 and 2: the 0.2.7 timing's CER on the same signals (cw_skim_bench rep, docs/SENSITIVITY-3.9-E.md) is the bound
        struct Pt { const Case* c; double snr, legacy, maxA; };
        const Pt pts[] = {{&kMachine, 2, 15.5, 9}, {&kHand, 2, 14.6, 9}, {&kFarns, 4, 22.1, 15}};
        for (const Pt& p : pts) {
            double a = 0;
            for (int sd = 1; sd <= 2; sd++) a += run(*p.c, p.snr, (uint32_t)sd, 80, -1).cer / 2;
            printf("  %-10s %4.0f dB: adaptive %5.1f %%  (0.2.7 timing %.1f %%)\n", p.c->name, p.snr, a, p.legacy);
            CHECK(a <= p.legacy && a <= p.maxA, "%s %.0f dB: adaptive CER %.1f %% (0.2.7 timing %.1f, bound %.1f)", p.c->name, p.snr, a, p.legacy, p.maxA);
        }
        double a = 0, l = 0;
        compare(kSloppy, 10, 2, 80, 0.0, 8, &a, &l);   // the 0.2.7 timing through setTimingDecoder(1), live
        CHECK(a < l - 8, "sloppy 10 dB: adaptive %.1f %% must beat the 0.2.7 timing's %.1f %% clearly", a, l);
    } else {
        for (const Case* c : {&kMachine, &kHand, &kSloppy, &kFarns})
            for (double snr : {0.0, 2.0, 4.0, 6.0, 10.0, 20.0}) compare(*c, snr, 4, 80, 0.0, 100);
    }
    {   // the first character after each pause: every middle copy starts "CQ DE" (6 dB, machine)
        const Out o = run(kMachine, 6, 3, 60, -1);
        const int copies = (int)(60 / (CwGenerator::messageSec(sigOf(kMachine, 6, 500)) + 1.0));
        int cq = 0;
        for (size_t p = 0; (p = o.text.find("K CQ DE", p)) != std::string::npos; p++) cq++;
        printf("  first character after the pause: %d of %d copies \"K CQ DE\" in \"%s\"\n", cq, copies, norm(o.text).substr(0, 120).c_str());
        CHECK(cq >= copies - 2, "\"K CQ DE\" %d times of %d copies", cq, copies);
        CHECK(o.sig.locked && std::fabs(o.sig.wpm - 22) < 2.5, "speed %.1f WPM, locked %d", o.sig.wpm, (int)o.sig.locked);
    }
    {   // Farnsworth through the receiver: flagged, letters together, a tuning error of 90 Hz
        const Out o = run(kFarns, 15, 2, 50, -1, -1300, 90);
        printf("  farnsworth 15 dB, 90 Hz off: \"%s\"\n", norm(o.text).substr(0, 100).c_str());
        CHECK(o.cer < 2, "Farnsworth 15 dB CER %.1f %%", o.cer);
        CHECK(o.sig.farnsworth, "Farnsworth flag");
    }
    {   // noise alone: no text
        CwGenConfig g;
        g.rate = kRate; g.dialOffsetHz = kOff; g.seed = 9;
        CwGenerator gen(g);
        CwReceiver r;
        r.setSilent(true); r.configure(kRate); r.setSignalOffset(kOff);
        std::vector<cf32> x(25000);
        CwTelemetry t; uint64_t seq = 0;
        for (double tt = 0; tt < 30; tt += 0.1) {
            gen.generate(x.data(), x.size()); r.feed(x.data(), x.size());
            CwTelemetry n; if (r.telemetry(n, seq)) { seq = n.seq; t = std::move(n); }
        }
        uint64_t chars = 0;
        for (const auto& s : t.signals) chars += s.chars;
        CHECK(chars <= 2, "noise only: %llu characters", (unsigned long long)chars);
    }
    printf(fails ? "cw fistrx: %d FAILED\n" : "cw fistrx: all passed\n", fails);
    return fails ? 1 : 0;
}
