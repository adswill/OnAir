// KAAL station ID in the sweep: the ID code and the four chirp variants (unit checks against the Python prototype's test vector), then the
// receivers on a transmitted signal: the in-lock voice receiver and the sweep-only scanner read the callsign, the scanner down to
// -20 dB per-sample SNR (data power over noise power per 48 kHz sample) and with a frequency offset, and neither reports an ID for noise,
// for a signal with plain up sweeps, or for the sweep of another ID.
#include "dect2/kaal/kaal_rx.h"
#include "dect2/kaal/kaal_sweepid.h"
#include "kaal_rx_gen.h"
#include <chrono>
#include <cstdio>
using namespace dect2;
using namespace dect2::kaal;
#include "../common/test_util.h"
#include "../common/test_long.h"

static double corr(int a, int b) {
    const auto& A = sweepVariantTemplate(a);
    const auto& B = sweepVariantTemplate(b);
    std::complex<double> s(0, 0);
    double ea = 0, eb = 0;
    for (int i = 0; i < kSwL; i++) {
        s += std::complex<double>(A[(size_t)i]) * std::conj(std::complex<double>(B[(size_t)i]));
        ea += std::norm(std::complex<double>(A[(size_t)i]));
        eb += std::norm(std::complex<double>(B[(size_t)i]));
    }
    return std::abs(s) / std::sqrt(ea * eb);
}

static std::vector<std::complex<float>> noiseOnly(size_t n, double power, uint64_t seed) {
    kaalrx::Gauss g;
    g.st ^= seed * 0x2545F4914F6CDD1DULL;
    std::vector<std::complex<float>> o(n);
    const double sc = std::sqrt(power / 2);
    for (auto& v : o) { double a, b; g.pair(a, b); v = std::complex<float>((float)(a * sc), (float)(b * sc)); }
    return o;
}

static bool scanFor(const std::vector<std::complex<float>>& sig, std::string& call, float& gate, uint64_t& scans) {
    SweepIdScanner sc;
    sc.setSynchronous(true);
    sc.configure(48000.0);
    SweepIdResult r;
    bool got = false;
    for (size_t i = 0; i < sig.size(); i += 4800) {
        sc.feed(&sig[i], std::min<size_t>(4800, sig.size() - i));
        if (sc.poll(r)) { got = true; call = r.callsign; }
    }
    gate = sc.lastGateDb();
    scans = sc.scans();
    return got;
}

int main() {
    {   // the code: test vector LY2ABC /P
        uint8_t bits[44];
        CHECK(packSweepId("LY2ABC", 1, bits), "pack");
        const auto syms = sweepIdSymbols("LY2ABC", 1);
        const int want[24] = {2, 3, 0, 1, 1, 0, 3, 1, 0, 2, 2, 2, 3, 2, 2, 0, 2, 1, 0, 3, 3, 2, 0, 1};
        int bad = 0;
        for (int i = 0; i < 24; i++) bad += syms[(size_t)i] != want[i];
        CHECK(bad == 0, "symbols of LY2ABC/P differ from the prototype in %d places", bad);
        std::string b;
        int sfx = -1;
        CHECK(unpackSweepId(bits, b, sfx) && b == "LY2ABC" && sfx == 1, "unpack '%s' %d", b.c_str(), sfx);
        bits[10] ^= 1;
        CHECK(!unpackSweepId(bits, b, sfx), "a flipped bit must fail the CRC");
        CHECK(!packSweepId("LY2ABCD", 0, bits) && !packSweepId("LY2a", 0, bits) && !packSweepId("AB", 16, bits), "unusable calls are refused");
        CHECK(splitCallsign("ly2abc/p", b, sfx) && b == "LY2ABC" && sfx == 1, "split ly2abc/p");
        CHECK(splitCallsign("OH2XYZ/MM", b, sfx) && b == "OH2XYZ" && sfx == 3, "split /MM");
        CHECK(splitCallsign("ONAIR", b, sfx) && b == "ONAIR" && sfx == 0, "split ONAIR");
        CHECK(!splitCallsign("TOOLONGCALL", b, sfx) && !splitCallsign("", b, sfx) && !splitCallsign("AB#C", b, sfx), "unsendable calls");
        CHECK(joinCallsign("LY2ABC", 1) == "LY2ABC/P", "join");
        double worst = 0;
        for (int a = 0; a < 4; a++) for (int c = a + 1; c < 4; c++) worst = std::max(worst, corr(a, c));
        double self = corr(0, 0);
        printf("variant cross-correlation worst %.4f (%.1f dB), self %.3f\n", worst, 20 * std::log10(worst + 1e-12), self);
        CHECK(worst < 0.02 && std::fabs(self - 1) < 1e-6, "variants not orthogonal enough: %.4f", worst);
        // samples of the variants against the Python kaal_phy_id.py templates
        static const double kPy[8][4] = {{0, 1000, 0.681619124, 0.731707161}, {0, 24000, 0.015551812, 0.999879063}, {1, 1000, 0.681619124, -0.731707161},
                                         {1, 23000, -0.931772807, -0.363041920}, {2, 5000, 1.385895844, 0.281589612}, {2, 12000, -1.007745906, -0.992193624},
                                         {3, 12120, 1.414213562, 0.0}, {3, 20000, 0.785948936, -1.175705860}};
        double dworst = 0;
        for (const auto& q : kPy) {
            const auto z = sweepSampleAt((int)q[0], (int)q[1]);
            dworst = std::max(dworst, std::abs(z - std::complex<double>(q[2], q[3])));
        }
        CHECK(dworst < 1e-6, "variant samples differ from the Python by %g", dworst);
        // equal energy
        double e0 = 0, e2 = 0;
        for (const auto& v : sweepVariantTemplate(0)) e0 += std::norm(v);
        for (const auto& v : sweepVariantTemplate(2)) e2 += std::norm(v);
        CHECK(std::fabs(e0 / e2 - 1) < 0.01, "energy up %.0f fast1 %.0f", e0, e2);
    }
    // quick run: the report-only SNRs are skipped, the scanner cases cut to the strong-with-offset, 0 dB and -20 dB ones, fewer noise and no-ID
    // signals (each must give no ID; the 9-of-10 ratio at -20 dB keeps its 10 seeds); ONAIR_TEST_LONG=1 runs everything
    const bool L = testLong();
    // a 40 s signal with the ID LY2ABC/P at 16 kbit/s
    const std::vector<int16_t> pcm = kaalrx::testPcm(16000 * 40);
    const std::vector<uint8_t> bits = kaalrx::frameBits(pcm, 16);
    double pw = 0, pwPlain = 0;
    const std::vector<kaalrx::cd> tx = kaalrx::transmit(bits, 16, "LY2ABC/P", pw);
    const std::vector<kaalrx::cd> txPlain = kaalrx::transmit(bits, 16, "", pwPlain);
    double pwNone = 0;
    const std::vector<kaalrx::cd> txNoSweep = kaalrx::transmit(bits, 16, "", pwNone, -200.0);
    printf("signal %.1f s\n", tx.size() / 48000.0);
    {   // the in-lock receiver
        for (double cfo : {0.0, 150.0, -150.0}) {
            kaal::VoiceRx vr;
            vr.configure(48000.0);
            const auto sig = kaalrx::channelSnr(tx, pw, 15.0, cfo, 11);
            for (size_t i = 0; i < sig.size(); i += 1000) vr.feed(&sig[i], std::min<size_t>(1000, sig.size() - i));
            const kaal::RxStats st = vr.stats();
            printf("in-lock, cfo %+.0f Hz: ID %s '%s' gate %.1f dB, words good %llu bad %llu\n", cfo, st.idValid ? "valid" : "none", st.idCall.c_str(), (double)st.idGateDb,
                   (unsigned long long)st.framesGood, (unsigned long long)st.framesBad);
            CHECK(st.idValid && st.idCall == "LY2ABC/P", "in-lock ID at cfo %+.0f: '%s'", cfo, st.idCall.c_str());
        }
    }
    {   // the scanner, sweep only
        struct Case { double snr, cfo; };
        const Case cases[] = {{60, 0}, {60, 150}, {0, 0}, {-10, 100}, {-18, 0}, {-18, 100}, {-20, 0}, {-20, 100}, {-20, -100}};
        for (double snr : L ? std::vector<double>{-22.0, -24.0, -26.0} : std::vector<double>{}) {   // where it gives out (reported, not checked)
            std::string call;
            float gate = 0;
            uint64_t scans = 0;
            const bool got = scanFor(kaalrx::channelSnr(tx, pw, snr, 100.0, 22), call, gate, scans);
            printf("scanner %+.0f dB, cfo +100 Hz (36 s window): %s '%s' (gate %.1f dB)\n", snr, got ? "ID" : "no ID", call.c_str(), (double)gate);
        }
        for (const Case& c : cases) {
            if (!L && !((c.snr == 60 && c.cfo != 0) || c.snr == 0 || c.snr == -20)) continue;
            const auto t0 = std::chrono::steady_clock::now();
            const auto sig = kaalrx::channelSnr(tx, pw, c.snr, c.cfo, 21);
            std::string call;
            float gate = 0;
            uint64_t scans = 0;
            const bool got = scanFor(sig, call, gate, scans);
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            printf("scanner %+.0f dB, cfo %+.0f Hz: %s '%s' (gate %.1f dB, %llu scans, %.1f s)\n", c.snr, c.cfo, got ? "ID" : "no ID", call.c_str(), (double)gate,
                   (unsigned long long)scans, sec);
            CHECK(got && call == "LY2ABC/P", "scanner at %+.0f dB, cfo %+.0f: '%s'", c.snr, c.cfo, call.c_str());
        }
    }
    {   // success rate at the edge: 10 seeds, random start inside the stream (so the cycle phase differs)
        for (double snr : L ? std::vector<double>{-20.0, -22.0} : std::vector<double>{-20.0}) {   // -22 dB is only reported
            int okN = 0;
            for (int t = 0; t < 10; t++) {
                std::string call;
                float gate = 0;
                uint64_t scans = 0;
                const auto full = kaalrx::channelSnr(tx, pw, snr, t % 2 ? 100.0 : -80.0, 500 + (uint64_t)t);
                const size_t off = (size_t)(3000 + 20011 * t);
                const std::vector<std::complex<float>> sig(full.begin() + (std::ptrdiff_t)off, full.end());
                okN += scanFor(sig, call, gate, scans) && call == "LY2ABC/P";
            }
            printf("scanner %+.0f dB, 10 seeds with random start, cfo +-100 Hz: %d of 10 read the right ID\n", snr, okN);
            if (snr > -21) CHECK(okN >= 9, "%d of 10 at %+.0f dB", okN, snr);
        }
    }
    {   // no ID where none is sent
        int falseIds = 0;
        float maxGate = -100;
        for (int t = 0; t < (L ? 12 : 4); t++) {
            std::string call;
            float gate = 0;
            uint64_t scans = 0;
            const auto n = noiseOnly(48000 * 40, pw, 100 + (uint64_t)t);
            if (scanFor(n, call, gate, scans)) { falseIds++; printf("FALSE ID from noise: %s\n", call.c_str()); }
            maxGate = std::max(maxGate, gate);
        }
        printf("noise only: %d false IDs, max gate %.1f dB\n", falseIds, (double)maxGate);
        CHECK(falseIds == 0, "%d false IDs from noise", falseIds);
        float maxGateV = -100;
        for (double snr : L ? std::vector<double>{10.0, 0.0, -10.0, -20.0} : std::vector<double>{10.0, -20.0}) {
            std::string call;
            float gate = 0;
            uint64_t scans = 0;
            const auto sig = kaalrx::channelSnr(txPlain, pwPlain, snr, 50.0, 31);   // plain up sweeps: no ID in it
            if (scanFor(sig, call, gate, scans)) { falseIds++; printf("FALSE ID from a plain sweep at %+.0f dB: %s\n", snr, call.c_str()); }
            maxGateV = std::max(maxGateV, gate);
            printf("plain sweeps at %+.0f dB: gate %.1f dB\n", snr, (double)gate);
        }
        for (double snr : L ? std::vector<double>{20.0, 5.0, 0.0} : std::vector<double>{20.0, 0.0}) {   // voice data and no sweep at all
            std::string call;
            float gate = 0;
            uint64_t scans = 0;
            const auto sig = kaalrx::channelSnr(txNoSweep, pwNone, snr, 50.0, 41);
            if (scanFor(sig, call, gate, scans)) { falseIds++; printf("FALSE ID from voice data without a sweep at %+.0f dB: %s\n", snr, call.c_str()); }
            printf("voice data, no sweep at %+.0f dB: gate %.1f dB\n", snr, (double)gate);
        }
        CHECK(falseIds == 0, "false IDs: %d", falseIds);
    }
    return testSummary("kaal_sweepid");
}
