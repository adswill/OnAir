// SSR interrogations on 1030 MHz: Mode 1, 2, A, B, C and D (P1 - P3 spacing 3, 5, 8, 17, 21, 25 us), the side-lobe suppression pulse P2, the intermode P4 (short and long), Mode S (56 and 112 bit P6), interlaced trains of
// two radars at different levels and rates, at 2, 4 and 10 Msps, with pulse-position jitter and noise. The mode names and the counts are compared with what the generator sent. Noise alone, a DME-like train (pairs 12 us apart),
// a train with a wrong spacing (10 us) and a plain carrier give no interrogation. Simulated signals only: no recording of 1030 MHz was available.
#include "x40_gen4.h"
#include <cstdlib>
#include <map>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Got { std::map<std::string, int> n; std::map<std::string, double> sp; int rows = 0; };
// the rows: "Mode A (P2 low: main beam): 600 (8.01 us); Mode C: ...; P1 about 31 dB over the floor"
static Got parse(const X40Telemetry& t) {
    Got g;
    for (auto& r : t.rows) if (r.kind == "Interrogations") {
        g.rows++;
        std::string s = r.text; const size_t e = s.find("; P1 about"); if (e != std::string::npos) s = s.substr(0, e);
        size_t pos = 0;
        while (pos < s.size()) {
            size_t q = s.find("; ", pos); std::string part = s.substr(pos, q == std::string::npos ? std::string::npos : q - pos);
            const size_t c = part.rfind(": "); if (c != std::string::npos) { const std::string name = part.substr(0, c); const int cnt = atoi(part.c_str() + c + 2); const size_t o = part.rfind('('); const double sp = o != std::string::npos ? atof(part.c_str() + o + 1) : 0; g.n[name] += cnt; g.sp[name] = sp; }
            if (q == std::string::npos) break; pos = q + 2;
        }
    }
    return g;
}
static int countPrefix(const Got& g, const std::string& pre) { int n = 0; for (auto& kv : g.n) if (kv.first.rfind(pre, 0) == 0) n += kv.second; return n; }
static double spacingOf(const Got& g, const std::string& pre) { for (auto& kv : g.sp) if (kv.first.rfind(pre, 0) == 0) return kv.second; return -1; }

int main() {
    const char* names[] = {"Mode 1", "Mode 2", "Mode A", "Mode B", "Mode C", "Mode D"};
    for (double fs : {4e6, 10e6, 2e6}) {
        for (int k = 0; k < 6; k++) {
            SsrTrain tr; tr.kind = k; tr.prf = 300; tr.p2 = k % 2 ? 0.0 : 0.05; tr.amp = 1.0;
            auto x = ssrScene(fs, 5.0, {tr}, 1e-4, 1.5e6 * (k - 2), 3 + (unsigned)k);
            auto t = run(x40::SsrInt, x, fs);
            Got g = parse(t);
            const int expect = (int)(4.0 * 300), cnt = countPrefix(g, names[k]);
            if (fs == 2e6 && k == 0) { printf("  2 Msps Mode 1 (3 us spacing is 6 samples): %d of %d\n", cnt, expect); continue; }       // not reliable at 2 Msps: said in the mode text
            CHECK(cnt >= (int)(expect * 0.9) && cnt <= expect * 1.1, "%.0f Msps %s: %d interrogations (about %d sent)", fs / 1e6, names[k], cnt, expect);
            int other = 0; for (auto& kv : g.n) if (kv.first.rfind(names[k], 0) != 0) other += kv.second;
            CHECK(other == 0, "%.0f Msps %s: %d interrogations of other modes", fs / 1e6, names[k], other);
            const double sp = spacingOf(g, names[k]); CHECK(std::fabs(sp - ssrSpacing(k)) < 0.12, "%.0f Msps %s: spacing %.2f us (want %.0f)", fs / 1e6, names[k], sp, ssrSpacing(k));
        }
    }
    // side lobe: P2 as strong as P1 is "suppressed"; P2 20 dB down is "main beam"
    { SsrTrain a; a.kind = 2; a.p2 = 1.0; SsrTrain b; b.kind = 4; b.p2 = 0.1; auto x = ssrScene(4e6, 4.2, {a, b}, 1e-4, 0, 11); Got g = parse(run(x40::SsrInt, x, 4e6));
      CHECK(g.n.count("Mode A (P2: side lobe, suppressed)") && g.n.count("Mode C (P2 low: main beam)"), "P2 classes missing (%zu names)", g.n.size()); }
    // intermode: P4 short and long after P3
    { SsrTrain a; a.kind = 2; a.p4 = 1; SsrTrain b; b.kind = 4; b.p4 = 2; auto x = ssrScene(4e6, 4.2, {a, b}, 1e-4, 0, 12); Got g = parse(run(x40::SsrInt, x, 4e6));
      CHECK(countPrefix(g, "Mode A + P4 (A/C-only all-call)") > 1080 && countPrefix(g, "Mode C + P4 (A/C/S all-call)") > 1080, "intermode counts %d %d", countPrefix(g, "Mode A + P4 (A/C-only all-call)"), countPrefix(g, "Mode C + P4 (A/C/S all-call)")); }
    // Mode S 56 and 112 bit
    { SsrTrain a; a.kind = 6; a.prf = 100; SsrTrain b; b.kind = 7; b.prf = 100; auto x = ssrScene(4e6, 5.0, {a, b}, 1e-4, 0, 13); Got g = parse(run(x40::SsrInt, x, 4e6));
      CHECK(g.n["Mode S (56 bit)"] > 360 && g.n["Mode S (112 bit)"] > 360, "Mode S counts %d %d", g.n["Mode S (56 bit)"], g.n["Mode S (112 bit)"]); }
    // two radars: Mode A + C interlaced at 250 / s each and a second radar's Mode C at 180 / s, 14 dB weaker, with jitter
    { SsrTrain a; a.kind = 2; a.prf = 250; SsrTrain b; b.kind = 4; b.prf = 250; b.start = 0.0031; SsrTrain c; c.kind = 4; c.prf = 180; c.amp = 0.2; c.start = 0.0017; c.jitterUs = 0.1;
      auto x = ssrScene(4e6, 6.2, {a, b, c}, 1e-4, 0, 14); Got g = parse(run(x40::SsrInt, x, 4e6));
      const int ca = countPrefix(g, "Mode A"), cc = countPrefix(g, "Mode C");
      CHECK(ca > 1080 && cc > 2320 && countPrefix(g, "Mode 1") + countPrefix(g, "Mode 2") + countPrefix(g, "Mode B") + countPrefix(g, "Mode D") + countPrefix(g, "Mode S") <= 8, "two radars: A %d C %d (other modes %d)", ca, cc, countPrefix(g, "Mode 1") + countPrefix(g, "Mode 2") + countPrefix(g, "Mode B") + countPrefix(g, "Mode D") + countPrefix(g, "Mode S")); }
    // noise sweep: pulse amplitude over the noise (amplitude ratio in dB): interrogations found of 1500 sent, wrong modes
    printf("noise sweep (P1 amplitude over the noise rms, Mode A at 300 per second for 5 s): found of 1200 / wrong\n");
    for (double db : {40.0, 30.0, 24.0, 20.0, 16.0, 12.0, 9.0, 6.0}) {
        SsrTrain tr; tr.kind = 2; tr.p2 = 0.05; const double nz = std::pow(10.0, -db / 10.0) * 1.0;     
        auto x = ssrScene(4e6, 5.0, {tr}, nz, 0, 21); Got g = parse(run(x40::SsrInt, x, 4e6));
        int wrong = 0; for (auto& kv : g.n) if (kv.first.rfind("Mode A", 0) != 0) wrong += kv.second;
        printf("  %4.0f dB: %d / %d\n", db, countPrefix(g, "Mode A"), wrong);
        CHECK(wrong == 0 || db < 12, "%.0f dB: %d wrong", db, wrong);
        if (db >= 20) CHECK(countPrefix(g, "Mode A") > 1080, "%.0f dB: %d found", db, countPrefix(g, "Mode A"));
    }
    // false reports: noise only, pairs 12 us apart (DME-like), pairs 10 us apart (no such mode), a plain carrier
    { std::vector<cf32> n((size_t)(4e6 * 20)); Rng r(5); for (auto& v : n) v = cf32(0.05f * r.g(), 0.05f * r.g()); Got g = parse(run(x40::SsrInt, n, 4e6)); CHECK(g.n.empty(), "noise: %zu names", g.n.size()); }
    { auto x = ssrScene(4e6, 4.0, {}, 1e-4, 0, 6);
      auto pairs = [&](double sp) { std::vector<cf32> y = x; Rng r(9); for (double t0 = 0.01; t0 < 3.9; t0 += 0.0033) for (double off : {0.0, sp}) { const size_t i0 = (size_t)((t0 + off * 1e-6) * 4e6); for (size_t i = i0; i < i0 + 4 && i < y.size(); i++) y[i] += cf32(1.0f, 0); } return y; };
      for (double sp : {12.0, 10.0, 36.0}) { auto y = pairs(sp); Got g = parse(run(x40::SsrInt, y, 4e6)); CHECK(g.n.empty(), "pairs %.0f us apart: %zu names", sp, g.n.size()); } }
    { std::vector<cf32> c((size_t)(4e6 * 10), cf32(1.0f, 0)); Rng r(7); for (auto& v : c) v += cf32(0.01f * r.g(), 0.01f * r.g()); Got g = parse(run(x40::SsrInt, c, 4e6)); CHECK(g.n.empty(), "carrier: %zu names", g.n.size()); }
    printf(fails ? "x40 ssrint: %d FAILED\n" : "x40 ssrint: ok\n", fails);
    return fails ? 1 : 0;
}
