// DAB TII (EN 300 401 clause 14.8, transmission mode I): the pattern table and the carriers against the standard, then a network of
// transmitters from the test transmitter through noise, a carrier offset and the real DabReceiver, which has to name them all.
#include "dect2/dab.h"
#include "dect2/dab_gen.h"
#include "dect2/dab_tii.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// runs `seconds` of the signal through the receiver and returns what it reports
static DabTelemetry run(const dabgen::TxConfig& tc, double snrDb, double cfoHz, double rate, double seconds) {
    SynthConfig sc;
    sc.snrDb = snrDb; sc.cfoHz = cfoHz;
    auto syn = makeDabSynth(tc, sc, rate);
    DabTelemetry t;
    if (!syn) { CHECK(false, "no generator at %.3f Msps", rate / 1e6); return t; }
    DabReceiver rx;
    rx.configure(rate);
    rx.audio().setSilent(true);
    rx.select(-1);
    const size_t total = (size_t)(seconds * rate), chunk = 65536;
    std::vector<cf32> x(chunk);
    for (size_t done = 0; done < total;) {
        const size_t n = std::min(chunk, total - done);
        syn->generate(x.data(), n);
        for (size_t i = 0; i < n; i++) x[i] = cf32(std::round(x[i].real() * 127.f) / 127.f, std::round(x[i].imag() * 127.f) / 127.f);   // 8 bit
        rx.feed(x.data(), n);
        done += n;
    }
    rx.telemetry(t, 0);
    return t;
}

static void print(const char* name, const DabTelemetry& t) {
    printf("  %-40s state %d, %d null symbols, %zu transmitter(s):", name, t.state, t.tiiFrames, t.tii.size());
    for (const auto& f : t.tii) printf(" %d/%d %.1f dB (margin %.1f)", f.mainId, f.subId, f.levelDb, f.marginDb);
    printf("\n");
}

int main() {
    // ---- table 26: the 70 patterns are the bytes with four bits set, in ascending order (rows checked against the standard)
    {
        std::set<int> seen;
        for (int p = 0; p < dabtii::kMainIds; p++) {
            const uint8_t a = dabtii::pattern(p);
            CHECK(__builtin_popcount(a) == 4, "pattern %d has %d bits", p, __builtin_popcount(a));
            CHECK(dabtii::mainIdOfPattern(a) == p, "pattern %d does not map back", p);
            seen.insert(a);
        }
        CHECK(seen.size() == 70, "%zu different patterns", seen.size());
        // table 26: p 0 = 0000 1111, 11 = 0011 0110, 23 = 0101 1010, 24 = 0101 1100, 47 = 1010 0110, 69 = 1111 0000
        const struct { int p; uint8_t a; } rows[] = {{0, 0x0F}, {11, 0x36}, {23, 0x5A}, {24, 0x5C}, {47, 0xA6}, {69, 0xF0}};
        for (auto& r : rows) CHECK(dabtii::pattern(r.p) == r.a, "pattern %d is 0x%02X, table 26 says 0x%02X", r.p, dabtii::pattern(r.p), r.a);
        CHECK(dabtii::mainIdOfPattern(0x07) == -1 && dabtii::mainIdOfPattern(0x1F) == -1, "bytes with 3 or 5 bits are no pattern");
    }
    // ---- figure 64: pattern 11, comb 1 switches on these pairs (k, k + 1)
    {
        std::vector<int> k;
        dabtii::pairCarriers(11, 1, k);
        const std::vector<int> want = {-670, -622, -526, -478, -286, -238, -142, -94, 99, 147, 243, 291, 483, 531, 627, 675};
        CHECK(k == want, "the carriers of pattern 11, comb 1 differ from figure 64 (%zu carriers)", k.size());
        // every pair of every comb lies inside -768 .. 768 and never touches carrier 0; all 24 combs together use every carrier once
        std::set<int> all;
        for (int c = 0; c < dabtii::kSubIds; c++)
            for (int b = 0; b < 8; b++) {
                const int q[4] = {-768, -384, 1, 385};
                for (int i = 0; i < 4; i++) { const int k0 = q[i] + 2 * c + 48 * b; all.insert(k0); all.insert(k0 + 1); }
            }
        CHECK(all.size() == 1536 && *all.begin() == -768 && *all.rbegin() == 768 && !all.count(0), "the combs cover %zu carriers", all.size());
    }
    // ---- a network: three transmitters as one receiver hears them (the main transmitter, a nearer and a farther filler of the same region)
    {
        dabgen::TxConfig tc;
        tc.tii = {{11, 1, 0.0}, {11, 5, -6.0}, {40, 17, -12.0}};
        const DabTelemetry t = run(tc, 20, 1700, 2.048e6, 6.0);
        print("3 transmitters, 20 dB, +1.7 kHz", t);
        CHECK(t.state == 2, "no lock");
        CHECK(t.tii.size() == 3, "%zu transmitters found, 3 sent", t.tii.size());
        if (t.tii.size() == 3) {
            CHECK(t.tii[0].mainId == 11 && t.tii[0].subId == 1, "strongest: %d/%d, want 11/1", t.tii[0].mainId, t.tii[0].subId);
            CHECK(t.tii[1].mainId == 11 && t.tii[1].subId == 5, "second: %d/%d, want 11/5", t.tii[1].mainId, t.tii[1].subId);
            CHECK(t.tii[2].mainId == 40 && t.tii[2].subId == 17, "third: %d/%d, want 40/17", t.tii[2].mainId, t.tii[2].subId);
            const float d1 = t.tii[0].levelDb - t.tii[1].levelDb, d2 = t.tii[0].levelDb - t.tii[2].levelDb;
            CHECK(std::fabs(d1 - 6.f) < 1.5f && std::fabs(d2 - 12.f) < 2.f, "levels %.1f / %.1f dB below the strongest, 6 / 12 sent", d1, d2);
        }
    }
    // ---- a weak signal: the TII is 16 dB below the main signal's power, still found at 10 dB SNR
    {
        dabgen::TxConfig tc;
        tc.tii = {{23, 9, 0.0}};
        const DabTelemetry t = run(tc, 10, -3200, 2.048e6, 6.0);
        print("1 transmitter, 10 dB, -3.2 kHz", t);
        CHECK(t.tii.size() == 1 && t.tii[0].mainId == 23 && t.tii[0].subId == 9, "the transmitter 23/9 at 10 dB: %zu found", t.tii.size());
    }
    // ---- another sample rate (resampled inside the receiver)
    {
        dabgen::TxConfig tc;
        tc.tii = {{69, 23, 0.0}, {0, 0, -3.0}};
        const DabTelemetry t = run(tc, 25, 0, 2.4e6, 6.0);
        print("2 transmitters at 2.4 Msps", t);
        CHECK(t.tii.size() == 2 && t.tii[0].mainId == 69 && t.tii[0].subId == 23 && t.tii[1].mainId == 0 && t.tii[1].subId == 0, "2.4 Msps: %zu found", t.tii.size());
    }
    // ---- no TII: nothing found (no false transmitters from the noise of the null symbols)
    {
        dabgen::TxConfig tc;
        const DabTelemetry t = run(tc, 15, 500, 2.048e6, 6.0);
        print("no TII, 15 dB", t);
        CHECK(t.state == 2 && t.tii.empty(), "%zu transmitters found in a signal without TII", t.tii.size());
    }
    printf(fails ? "dab tii: FAILED\n" : "dab tii: ok\n");
    return fails ? 1 : 0;
}
