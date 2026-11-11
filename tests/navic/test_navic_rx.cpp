// NavIC: the code table against the ICD's own check column, subframe FEC round trip with noise, and the receiver against its generator. Simulated only:
// no real recording of a NavIC signal went through this test.
#include "dect2/navic/navic.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <random>
#include <vector>
#include "../common/test_long.h"
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static unsigned oct(const char* s) { return (unsigned)strtol(s, nullptr, 8); }
int main() {
    // Table 7, the "First 10 chips in octal" columns of the ICD
    static const char* const l5[14] = {"0130", "1731", "0713", "1215", "0117", "1624", "1753", "1317", "1547", "0233", "1663", "0203", "0455", "1025"};
    static const char* const s[14] = {"1420", "1202", "0716", "1524", "0556", "1323", "1561", "1331", "0361", "0501", "0156", "0226", "1272", "1362"};
    for (int p = 1; p <= 14; p++) {
        CHECK(navicFirst10Octal(p) == oct(l5[p - 1]), "L5 PRN %d: %o", p, navicFirst10Octal(p));
        CHECK(navicFirst10Octal(p, true) == oct(s[p - 1]), "S PRN %d: %o", p, navicFirst10Octal(p, true));
    }
    uint8_t c[1023]; CHECK(!navicChips(0, c) && !navicChips(15, c), "PRN range");

    // FEC: encode, noise, decode
    std::mt19937 g(5); std::normal_distribution<float> n(0.f, 1.f);
    for (double sigma : {0.3, 0.6, 0.8, 1.2}) {
        int ok = 0, total = 30;
        for (int t = 0; t < total; t++) {
            uint8_t b[kNavicBits] = {};
            for (int i = 8; i < 262; i++) b[i] = (uint8_t)(g() & 1);
            navicSetCrcTail(b);
            uint8_t sym[kNavicSymbolsPerSubframe]; navicEncodeSubframe(b, sym);
            float soft[kNavicCoded];
            for (int i = 0; i < kNavicCoded; i++) soft[i] = (sym[16 + i] ? -1.f : 1.f) + n(g) * (float)sigma;
            uint8_t out[kNavicBits];
            if (navicDecodeSymbols(soft, out) && std::equal(b, b + kNavicBits, out)) ok++;
        }
        printf("noise sigma %.1f: %d of %d subframes\n", sigma, ok, total);
        if (sigma <= 0.6) CHECK(ok == total, "%d of %d", ok, total);
    }
    { uint8_t b[kNavicBits] = {}; navicSetCrcTail(b); b[100] ^= 1; CHECK(!navicCrcOk(b), "CRC misses an error"); }

    // Subframe fields: bit numbers of the ICD (TOWC 9-25, alert 26, autonav 27, ID 28-29, WN 31-40 in subframe 1, message ID 31-36 and PRN 257-262 in 3 and 4)
    {
        uint8_t b[kNavicBits] = {};
        const auto put = [&](int first, int len, unsigned v) { for (int i = 0; i < len; i++) b[first - 1 + i] = (uint8_t)((v >> (len - 1 - i)) & 1); };
        put(9, 17, 70000); put(26, 1, 1); put(28, 2, 0); put(31, 10, 987);
        NavicFields f; navicParse(b, f);
        CHECK(f.towc == 70000 && f.alert && !f.autonav && f.subframeId == 1 && f.wn == 987, "subframe 1: %u %d %d", f.towc, f.subframeId, f.wn);
        put(28, 2, 3); put(31, 6, 26); put(257, 6, 9);
        NavicFields g; navicParse(b, g);
        CHECK(g.subframeId == 4 && g.msgId == 26 && g.prnId == 9 && g.wn == -1, "subframe 4: %d %d %d", g.subframeId, g.msgId, g.prnId);
    }

    // Receiver
    // the default run: the weaker signal only (the subframes need the whole 34 s); ONAIR_TEST_LONG=1 both
    for (double cn0 : {46.0, 42.0}) {
        if (!testLong() && cn0 > 44) continue;
        NavicGenConfig gc; gc.cn0Db = cn0; gc.nSats = 2; gc.dopplerHz = 2800;
        NavicGenerator gen(gc);
        NavicReceiver rx; rx.configure(kNavicFs);
        std::vector<cf32> x(16384);
        const double dur = cn0 > 44 ? 34 : 34;
        for (double t = 0; t < dur; t += 16384 / kNavicFs) { gen.generate(x.data(), x.size()); rx.feed(x.data(), x.size()); }
        NavicTelemetry t; rx.telemetry(t, 0);
        printf("C/N0 %.0f: %s\n", cn0, navicSummary(t).c_str());
        for (const auto& v : t.sats) printf("  PRN %d C/N0 %.1f Doppler %.0f frames %u bad %u SF %d TOWC %u WN %d state %d\n", v.prn, v.cn0, v.dopplerHz, v.frames, v.bad, v.lastSubframe, v.towc, v.wn, v.state);
        int sats = 0, withFrames = 0;
        for (const auto& v : t.sats) { sats++; if (v.frames > 0) withFrames++; CHECK(v.prn == 2 || v.prn == 5, "phantom PRN %d", v.prn); if (v.frames) CHECK(v.towc >= 5000, "TOWC %u", v.towc); }
        CHECK(sats == 2, "tracked %d", sats);
        CHECK(withFrames == 2, "%d satellites decoded at %.0f dB-Hz", withFrames, cn0);
    }
    printf(fails ? "FAILED %d\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
