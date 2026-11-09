// BTSC: compander round trip across levels and frequencies, and the whole chain from the FM sound carrier (simulated signals only): separation, pilot, SAP, offsets, noise.
#include "dect2/atv/atv_btsc.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static double toneAmp(const std::vector<float>& v, size_t from, double f, double rate) {
    std::complex<double> a = 0; size_t n = 0;
    for (size_t i = from; i < v.size(); i++, n++) a += (double)v[i] * std::polar(1.0, -2 * M_PI * f * (double)i / rate);
    return n ? 2 * std::abs(a) / (double)n : 0;
}
int main() {
    // compander alone
    for (double f : {100.0, 400.0, 1000.0, 5000.0, 12000.0}) for (double lv : {0.003, 0.03, 0.2, 0.5}) {
        if (f >= 5000 && lv > 0.05) continue;   // the transmitter's limiter territory
        BtscCompander c, e;
        std::vector<float> o; o.reserve(192000);
        for (int i = 0; i < 192000; i++) o.push_back(e.expand(c.compress((float)(lv * std::sin(2 * M_PI * f * i / 192000)))));
        double a = toneAmp(o, 96000, f, 192000);
        CHECK(std::fabs(20 * std::log10(a / lv)) < 0.2, "compander %.0f Hz level %.3f: out %.4f", f, lv, a);
    }
    {   // compression law: the wideband part halves the level change in dB at 300 Hz (0.1 of input swing against 0.01)
        auto rms = [](double lv) { BtscCompander c; double p = 0; for (int i = 0; i < 192000; i++) { float y = c.compress((float)(lv * std::sin(2 * M_PI * 300.0 * i / 192000))); if (i > 144000) p += y * y; } return std::sqrt(p / 48000); };
        double d = 20 * std::log10(rms(0.2) / rms(0.02));
        printf("20 dB in gives %.1f dB out\n", d);
        CHECK(d > 7 && d < 13, "compression law: 20 dB in, %.1f out", d);
    }
    struct Case { double cfo, cnr; bool st, sap; double minSep; };
    for (const Case c : {Case{0, 200, true, true, 25}, Case{0, 50, true, true, 20}, Case{1800, 40, true, true, 18}, Case{-2500, 40, true, false, 18}, Case{0, 40, false, false, 0}, Case{0, 35, true, true, 12}}) {
        BtscTxConfig tc; tc.rate = 2.0e6; tc.carrierHz = c.cfo + 310e3; tc.cnrDb = c.cnr; tc.stereo = c.st; tc.sap = c.sap;
        BtscTx tx(tc); BtscRx rx; rx.configure(tc.rate);
        std::vector<cf32> s; tx.generate((size_t)(3 * tc.rate), s);
        std::vector<float> l, r;
        for (size_t o = 0; o < s.size(); o += 65536) rx.process(s.data() + o, std::min<size_t>(65536, s.size() - o), 310e3, false, l, r);
        const BtscStatus st = rx.status();
        size_t from = l.size() / 2;
        double la = toneAmp(l, from, tc.toneLHz, 48000), rla = toneAmp(r, from, tc.toneLHz, 48000), ra = toneAmp(r, from, tc.toneRHz, 48000), lra = toneAmp(l, from, tc.toneRHz, 48000);
        double sepL = 20 * std::log10(la / std::max(1e-9, rla)), sepR = 20 * std::log10(ra / std::max(1e-9, lra));
        printf("cfo %+6.0f cnr %3.0f stereo %d sap %d: pilot %d (%.2f kHz, %+.1f Hz) sap %d L tone %.3f, sep L %.1f R %.1f dB\n", c.cfo, c.cnr, c.st, c.sap, st.pilot, st.pilotKhz, st.pilotErrHz, st.sap, la, sepL, sepR);
        CHECK(st.pilot == c.st, "pilot found %d, sent %d", st.pilot, c.st);
        if (c.st) { CHECK(sepL > c.minSep && sepR > c.minSep, "separation %.1f / %.1f dB", sepL, sepR); CHECK(std::fabs(la / tc.level - 1) < 0.25, "L level %.3f", la); }
        else CHECK(std::fabs(la / tc.level / 0.5 - 1) < 0.25 || la > 0, "mono level %.3f", la);
        if (c.sap) {
            CHECK(st.sap, "SAP not found");
            BtscTx tx2(tc); BtscRx rx2; rx2.configure(tc.rate); std::vector<cf32> s2; tx2.generate((size_t)(3 * tc.rate), s2);
            std::vector<float> a, b;
            for (size_t o = 0; o < s2.size(); o += 65536) rx2.process(s2.data() + o, std::min<size_t>(65536, s2.size() - o), 310e3, true, a, b);
            double sa = toneAmp(a, a.size() / 2, tc.sapHz, 48000);
            printf("   SAP tone %.3f (sent %.3f)\n", sa, tc.level);
            CHECK(std::fabs(sa / tc.level - 1) < 0.3, "SAP level %.3f", sa);
        } else CHECK(!st.sap, "SAP false detection");
    }
    printf(fails ? "btsc FAILED\n" : "btsc passed\n");
    return fails ? 1 : 0;
}
