// DTMB single carrier mode (C=1): the transmitter model (36 system information symbols, then 3744 data symbols in the time domain, PN595 header),
// the frequency domain equaliser against the transmitted symbols, the receiver on the test signal for every modulation, and the carrier mode
// decision between C=1 and C=3780 for PN595 signals. The C=1 frame layout (system information first, no scrambling beyond the data scrambler) is
// taken from the dtmb-sdr project (c1.cpp) and the standard as recalled; it is not checked against a real signal.
#include "dect2/dtmb_demod.h"
#include "dect2/dtmb_front.h"
#include "dect2/dtmb_gen.h"
#include "dect2/dtmb_map.h"
#include "dect2/dtmb_testkit.h"
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace dect2;
using namespace dect2::dtmb;
using namespace dect2::dtmb::kit;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Symbol level: header estimates, leakage removal and the single carrier equaliser on frames 3 .. 3+n-1 against the transmitted body symbols
struct Eq { double mer = 0; int siBest = -1; float siScore = 0; };
static Eq equalise(const SignalConfig& sc, int nFrames, int pre, int post) {
    Signal sig(sc, testPacketSource(1));
    FrameTx ref(sc.tx, testPacketSource(1));
    const int Lf = frameLength(sc.tx.header), Lh = headerInfo(sc.tx.header).length;
    const int total = nFrames + 6;
    std::vector<std::vector<cf32>> body((size_t)total);
    std::vector<cf32> tmp((size_t)Lf);
    for (int f = 0; f < total; f++) { ref.nextFrame(tmp.data()); body[(size_t)f] = ref.lastCarriers(); }
    const size_t nIn = (size_t)((double)total * Lf * sc.rate / kSymbolRate) + 8000;
    std::vector<cf32> in(nIn), s;
    sig.generate(in.data(), nIn);
    SrrcResampler rs;
    rs.configure(sc.rate);
    rs.setDcRemoval(false);
    rs.process(in.data(), nIn, s);
    HeaderEstimator est(sc.tx.header);
    est.setWindow(pre, post);
    SingleCarrierEqualizer eq;
    Eq out;
    double err = 0, pw = 0;
    std::vector<cf32> sym((size_t)kBody);
    std::vector<float> var((size_t)kBody);
    Taps ta, tb;
    int8_t ca[945], cb[945];
    pnHeader(sc.tx.header, 0, ca);
    pnHeader(sc.tx.header, 0, cb);
    for (int f = 3; f < 3 + nFrames; f++) {
        const long S = (long)f * Lf, S2 = (long)(f + 1) * Lf;
        est.estimate(&s[(size_t)S], 0, ta);
        est.estimate(&s[(size_t)S2], 0, tb);
        HeaderEstimator::clean(ta, ta.tapVar, 12.f);
        HeaderEstimator::clean(tb, tb.tapVar, 12.f);
        eq.run(&s[(size_t)(S + Lh)], ta, ca, tb, cb, Lh, std::sqrt(headerInfo(sc.tx.header).powerRatio / 2.0), sym.data(), var.data());
        for (int k = 0; k < kBody; k++) { err += std::norm(sym[(size_t)k] - body[(size_t)f][(size_t)k]); pw += std::norm(body[(size_t)f][(size_t)k]); }
        if (f == 3) {
            float w[36], sc22[22];
            for (int i = 0; i < 36; i++) w[i] = 1.f / std::max(var[(size_t)i], 1e-9f);
            siScores(sym.data(), w, sc22);
            int best = 0;
            for (int i = 1; i < 22; i++) if (sc22[i] > sc22[best]) best = i;
            out.siBest = best + 3; out.siScore = sc22[best];
        }
    }
    out.mer = 10 * std::log10(pw / err);
    return out;
}

static SignalConfig c1Signal(Mapping m, Rate r, bool mode2, double snr) {
    SignalConfig sc = makeSignal(Header::Pn595, m, r, mode2, 10e6, snr);
    sc.tx.carriers = 1;
    sc.tx.phaseRotate = false;
    return sc;
}

int main() {
    // ---- the transmitter model: the first 36 body symbols are the system information words, the rest is data
    {
        TxConfig tc; tc.header = Header::Pn595; tc.carriers = 1; tc.phaseRotate = false; tc.profile.map = Mapping::Qam16; tc.profile.rate = Rate::R06;
        FrameTx tx(tc, testPacketSource(1));
        std::vector<cf32> fr((size_t)frameLength(Header::Pn595));
        tx.nextFrame(fr.data());
        uint8_t chips[36];
        siChips(siIndex(tc.profile), chips);
        int bad = 0;
        for (int s = 0; s < 36; s++) {
            const cf32 v = fr[(size_t)(595 + s)];
            if (std::fabs(v.real() - v.imag()) > 1e-5f || (v.real() > 0) != (chips[s] != 0) || std::fabs(std::abs(v) - 1.f) > 1e-5f) bad++;
        }
        CHECK(bad == 0, "%d system information symbols differ from the word", bad);
        double p = 0;
        for (int k = 0; k < kBody; k++) p += std::norm(fr[(size_t)(595 + k)]);
        CHECK(std::fabs(p / kBody - 1.0) < 0.05, "body power %.3f", p / kBody);
        double ph = 0;
        for (int n = 0; n < 595; n++) ph += std::norm(fr[(size_t)n]);
        CHECK(std::fabs(ph / 595 - 1.0) < 1e-5, "PN595 header power %.3f (the header of C=1 is not boosted)", ph / 595);
    }

    // ---- the equaliser at symbol level
    struct Case { double snr; int echoDb; double echoDelay; int pre, post; double minMer; };
    const Case cases[] = {
        {200, 0, 0, 24, 100, 38},
        {30, 0, 0, 24, 100, 27},
        {30, 6, 40, 24, 100, 23},
        {30, 3, -50, 60, 90, 20},
        {25, 10, 120, 24, 150, 20},
    };
    for (const Case& c : cases) {
        SignalConfig sc = c1Signal(Mapping::Qam16, Rate::R06, false, c.snr);
        if (c.echoDb) sc.echoes.push_back({(double)c.echoDb, c.echoDelay});
        const Eq r = equalise(sc, 6, c.pre, c.post);
        printf("  C=1 snr %5.0f echo %d@%.0f: symbol MER %.1f dB, system information %d (expected %d) score %.2f\n", c.snr, c.echoDb, c.echoDelay, r.mer, r.siBest, siIndex(sc.tx.profile), r.siScore);
        CHECK(r.mer > c.minMer, "MER %.1f dB below %.1f", r.mer, c.minMer);
        CHECK(r.siBest == siIndex(sc.tx.profile), "system information %d", r.siBest);
    }

    // ---- the receiver: every modulation, bit exact packets, carrier mode reported
    struct Rx { const char* name; Mapping m; Rate r; bool mode2; double snr; double cfo; int echoDb; double echoDelay; };
    const Rx rxs[] = {
        {"4QAM 0.4 mode 1", Mapping::Qam4, Rate::R04, false, 12, 0, 0, 0},
        {"4QAM 0.8 mode 2, carrier offset 7 kHz", Mapping::Qam4, Rate::R08, true, 14, 7000, 0, 0},
        {"4QAM-NR 0.8", Mapping::Qam4Nr, Rate::R08, false, 10, 0, 0, 0},
        {"16QAM 0.6, echo 6 dB at +40 samples", Mapping::Qam16, Rate::R06, false, 28, -2500, 6, 40},
        {"32QAM 0.8 (frame pairs)", Mapping::Qam32, Rate::R08, false, 30, 0, 0, 0},
        {"64QAM 0.4", Mapping::Qam64, Rate::R04, false, 30, 0, 0, 0},
    };
    for (const Rx& x : rxs) {
        Scenario o;
        o.sc = c1Signal(x.m, x.r, x.mode2, x.snr);
        o.sc.cfoHz = x.cfo;
        if (x.echoDb) o.sc.echoes.push_back({(double)x.echoDb, x.echoDelay});
        o.seconds = x.mode2 ? 2.0 : 1.5;
        const Result r = run(o);
        const double perSec = netBitrate(Header::Pn595, o.sc.tx.profile) / kTsBits;
        printf("  %-40s first packet %.2f s, %llu good, %llu wrong, %llu missing; codewords ok %llu bad %llu; C=%d C/N %.1f MER %.1f\n", x.name, r.firstGoodSec, (unsigned long long)r.good,
               (unsigned long long)r.wrong, (unsigned long long)r.missing, (unsigned long long)r.tel.blocksOk, (unsigned long long)r.tel.blocksBad, r.tel.carriers, r.tel.snrPnDb, r.tel.merDb);
        CHECK(r.wrong == 0 && r.backwards == 0 && r.seqBack == 0, "%s: wrong or misordered packets", x.name);
        CHECK(r.tel.carriers == 1 && r.tel.siOk && r.tel.header == 1, "%s: carriers %d", x.name, r.tel.carriers);
        CHECK(r.tel.mapping == (int)x.m && r.tel.rate == (int)x.r && r.tel.interleaver == (x.mode2 ? 2 : 1), "%s: parameters from the system information", x.name);
        CHECK(r.firstGoodSec >= 0 && r.firstGoodSec < (x.mode2 ? 0.7 : 0.45), "%s: first packet at %.2f s", x.name, r.firstGoodSec);
        CHECK(r.missing <= 3 * (uint64_t)packetsPerFrame(o.sc.tx.profile) + 8, "%s: %llu packets missing", x.name, (unsigned long long)r.missing);
        CHECK((double)r.good > 0.95 * perSec * (o.seconds - 0.6), "%s: %llu packets", x.name, (unsigned long long)r.good);
        CHECK(std::fabs(r.tel.cfoHz - x.cfo) < 40, "%s: carrier offset %.1f", x.name, r.tel.cfoHz);
    }

    // ---- carrier mode decision: the same header, two bodies
    {
        Scenario o;
        o.sc = makeSignal(Header::Pn595, Mapping::Qam16, Rate::R06, false, 10e6, 30);   // multi-carrier
        o.seconds = 1.2;
        Result r = run(o);
        CHECK(r.good > 8000 && r.wrong == 0 && r.tel.carriers == 3780, "PN595 multi-carrier: %llu good, carriers %d", (unsigned long long)r.good, r.tel.carriers);
        o.sc = c1Signal(Mapping::Qam16, Rate::R06, false, 30);
        r = run(o);
        CHECK(r.good > 8000 && r.wrong == 0 && r.tel.carriers == 1, "PN595 single carrier: %llu good, carriers %d", (unsigned long long)r.good, r.tel.carriers);
        o.sc = makeSignal(Header::Pn945, Mapping::Qam16, Rate::R06, false, 10e6, 30);
        r = run(o);
        CHECK(r.good > 8000 && r.tel.carriers == 3780, "PN945: carriers %d", r.tel.carriers);
    }

    printf(failures ? "dtmb_c1: %d FAILED\n" : "dtmb_c1: all passed\n", failures);
    return failures ? 1 : 0;
}
