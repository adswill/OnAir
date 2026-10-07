// DVB-S/S2 receiver, hard cases: how close to the threshold it works, a gap in the samples, reset in the middle, phase noise, VCM, an adjacent carrier.
// The thresholds are compared with the Es/N0 of table 13 of EN 302 307-1 (DVB-S2, ideal demodulator) and the Eb/N0 of EN 300 421 (DVB-S).
#include "dect2/dvbs_testkit.h"
#include <cstdio>
#include <cstdlib>

using namespace dect2;
using namespace dect2::dvbs;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    // ---- DVB-S2 close to the threshold: 0.8 dB above table 13 (normal frames), 2 Msym/s. Over 5 s of signal at most 3 % of the frames may fail
    {
        struct Mc { int mod, rate; };
        const Mc mcs[] = {{kQpsk, 0}, {kQpsk, 3}, {kQpsk, 9}, {k8psk, 4}, {k8psk, 6}, {k16apsk, 5}, {k16apsk, 8}, {k32apsk, 6}, {k32apsk, 9}};
        for (const Mc& m : mcs) {
            RunConfig rc = makeRun(2, m.mod, m.rate, 2e6, 4e6, s2QefEsN0(m.mod, m.rate, false) + 0.8, m.mod == kQpsk && m.rate == 0 ? 9.0 : 5.0);      // the lowest rate needs a few seconds to find
            const RunResult r = runCase(rc);
            const double frames = rc.secs * 2e6 / s2FrameSymbols(m.mod, false, false);
            const double okFrames = (double)r.tel.blocksOk, badFrames = (double)r.tel.blocksBad;
            char w[64]; snprintf(w, sizeof w, "DVB-S2 %s %s at QEF + 0.8 dB", s2ModName(m.mod), s2RateName(m.rate));
            CHECK(okFrames > frames * (m.rate == 0 && m.mod == kQpsk ? 0.6 : 0.8), "%s: %.0f of %.0f frames decoded; %s", w, okFrames, frames, dvbsSummary(r.tel).c_str());
            CHECK(badFrames <= std::max(2.0, 0.03 * frames), "%s: %.0f bad frames of %.0f", w, badFrames, frames);
            CHECK(r.bad == 0, "%s: %llu damaged packets", w, (unsigned long long)r.bad);
            printf("%s: %.0f frames ok, %.0f bad of %.0f; first packet after %.2f s\n", w, okFrames, badFrames, frames, r.firstPacketSecs);
        }
    }
    // ---- DVB-S at the threshold of EN 300 421 (Eb/N0 for a quasi error free stream: 3.2, 4.4, 5.2, 6.2, 6.9 dB for the five rates, converted to Es/N0)
    {
        static const double ebn0[5] = {3.2, 4.4, 5.2, 6.2, 6.9};
        static const double rate[5] = {0.5, 2.0 / 3, 0.75, 5.0 / 6, 7.0 / 8};
        for (int i = 0; i < 5; i++) {
            const double es = ebn0[i] + 10 * std::log10(2 * rate[i] * 188.0 / 204.0) + 0.5;      // half a dB of margin
            RunConfig rc = makeRun(1, kQpsk, i, 2e6, 4e6, es, 5.0);
            const RunResult r = runCase(rc);
            char w[64]; snprintf(w, sizeof w, "DVB-S QPSK %d/%d at QEF + 0.5 dB (Es/N0 %.1f)", i == 0 ? 1 : i == 1 ? 2 : i == 2 ? 3 : i == 3 ? 5 : 7, i == 0 ? 2 : i == 1 ? 3 : i == 2 ? 4 : i == 3 ? 6 : 8, es);
            CHECK(r.good > dvbsNetBitrate(rc.sig.tx) / 1504.0 * 3.5, "%s: %llu good packets; %s", w, (unsigned long long)r.good, dvbsSummary(r.tel).c_str());
            CHECK(r.bad == 0 && r.gaps == 0, "%s: %llu damaged packets, %llu jumps", w, (unsigned long long)r.bad, (unsigned long long)r.gaps);
            printf("%s: %llu packets, bad %llu\n", w, (unsigned long long)r.good, (unsigned long long)r.bad);
        }
    }

    // ---- a gap of 5 ms in the samples: nothing wrong may come out, and the stream is back within a second
    for (int std = 1; std <= 2; std++) {
        RunConfig rc = makeRun(std, kQpsk, std == 1 ? 1 : 5, 2e6, 4e6, 12.0, 5.0);
        rc.dropoutAt = 2.0; rc.dropoutSecs = 0.005;
        const RunResult r = runCase(rc);
        char w[64]; snprintf(w, sizeof w, "%s with a 5 ms gap", std == 1 ? "DVB-S" : "DVB-S2");
        const double perSec = dvbsNetBitrate(rc.sig.tx) / 1504.0;
        // DVB-S passes on the packets that Reed-Solomon could not repair with the error indicator set (a few around the gap); DVB-S2 has none
        CHECK(r.errorFlag == r.bad && r.bad <= (std == 1 ? 16u : 0u), "%s: %llu damaged packets, %llu of them flagged", w, (unsigned long long)r.bad, (unsigned long long)r.errorFlag);
        CHECK(r.good > perSec * 3.0, "%s: %llu good packets (a second or more of lock was wanted after the gap)", w, (unsigned long long)r.good);
        CHECK(r.gaps >= 1 && r.gaps <= 6, "%s: %llu jumps in the counter (the gap itself is one)", w, (unsigned long long)r.gaps);
        CHECK(r.lost < perSec * 1.1, "%s: %llu packets lost (more than a second's worth)", w, (unsigned long long)r.lost);
        printf("%s: %llu packets, %llu lost in %llu jumps\n", w, (unsigned long long)r.good, (unsigned long long)r.lost, (unsigned long long)r.gaps);
    }

    // ---- reset() in the middle (the engine does it after a retune): the stream starts again within a second
    for (int std = 1; std <= 2; std++) {
        RunConfig rc = makeRun(std, kQpsk, std == 1 ? 1 : 5, 2e6, 4e6, 12.0, 4.0);
        rc.resetAt = 2.0;
        const RunResult r = runCase(rc);
        char w[64]; snprintf(w, sizeof w, "%s with reset()", std == 1 ? "DVB-S" : "DVB-S2");
        CHECK(r.bad == 0, "%s: %llu damaged packets", w, (unsigned long long)r.bad);
        CHECK(r.good > dvbsNetBitrate(rc.sig.tx) / 1504.0 * 2.2 && r.tel.tsLock, "%s: %llu good packets, %s", w, (unsigned long long)r.good, dvbsSummary(r.tel).c_str());
        printf("%s: %llu packets\n", w, (unsigned long long)r.good);
    }

    // ---- LNB phase noise: the "non DTH" mask of EN 302 307-1 annex M with 8PSK and 16APSK a few dB above their thresholds; the typical consumer mask of annex H.8
    // with QPSK and 8PSK, where the loop has to be wide
    {
        struct P { int mask, mod, rate; double margin; };
        const P cases[] = {{3, k8psk, 6, 3.0}, {3, k16apsk, 6, 3.0}, {3, k32apsk, 7, 3.0}, {1, kQpsk, 5, 6.0}, {1, k8psk, 5, 12.0}};
        for (const P& p : cases) {
            RunConfig rc = makeRun(2, p.mod, p.rate, 2e6, 4e6, s2QefEsN0(p.mod, p.rate, false) + p.margin, 4.0);
            rc.sig.phaseNoise = p.mask;
            const RunResult r = runCase(rc);
            char w[96]; snprintf(w, sizeof w, "DVB-S2 %s %s, phase noise mask %d, QEF + %.0f dB", s2ModName(p.mod), s2RateName(p.rate), p.mask, p.margin);
            const double frames = rc.secs * 2e6 / s2FrameSymbols(p.mod, false, false);
            CHECK((double)r.tel.blocksOk > frames * 0.7 && r.tel.blocksBad <= frames * 0.1 + 2, "%s: %llu frames ok, %llu bad of %.0f", w, (unsigned long long)r.tel.blocksOk, (unsigned long long)r.tel.blocksBad, frames);
            CHECK(r.bad == 0, "%s: %llu damaged packets", w, (unsigned long long)r.bad);
            printf("%s: %llu ok, %llu bad of %.0f frames\n", w, (unsigned long long)r.tel.blocksOk, (unsigned long long)r.tel.blocksBad, frames);
        }
    }

    // ---- VCM: the MODCOD changes from frame to frame through seven formats (QPSK 2/3 to 32APSK 4/5, short and normal frames, with and without pilots)
    {
        RunConfig rc = makeRun(2, kQpsk, 5, 2e6, 4e6, 22.0, 5.0);
        rc.sig.tx.vcm = true;
        const RunResult r = runCase(rc);
        CHECK(r.bad == 0 && r.gaps == 0, "VCM: %llu damaged packets, %llu jumps", (unsigned long long)r.bad, (unsigned long long)r.gaps);
        CHECK(r.good > dvbsNetBitrate(rc.sig.tx) / 1504.0 * 3.5 && r.tel.vcm, "VCM: %llu good packets; %s", (unsigned long long)r.good, dvbsSummary(r.tel).c_str());
        printf("VCM: %llu packets, no errors\n", (unsigned long long)r.good);
    }
    // ---- dummy PLFRAMEs between the data frames
    {
        RunConfig rc = makeRun(2, kQpsk, 5, 2e6, 4e6, 12.0, 4.0);
        rc.sig.tx.dummyEvery = 3;
        const RunResult r = runCase(rc);
        CHECK(r.bad == 0 && r.gaps == 0 && r.good > 4000, "dummy frames: %llu good, %llu damaged, %llu jumps", (unsigned long long)r.good, (unsigned long long)r.bad, (unsigned long long)r.gaps);
        CHECK(r.tel.framesDummy > 20, "dummy frames counted: %llu", (unsigned long long)r.tel.framesDummy);
        printf("dummy frames: %llu packets, %llu dummy frames skipped\n", (unsigned long long)r.good, (unsigned long long)r.tel.framesDummy);
    }

    // ---- an adjacent carrier of the same size on the next transponder (4.5 MHz away) at the same level: the carrier at the centre is the one decoded
    {
        DvbsSignalConfig a;
        a.tx.standard = 2; a.tx.mod = kQpsk; a.tx.rate = 5; a.tx.symbolRate = 2e6; a.sampleRate = 10e6; a.snrDb = 14;
        DvbsSignalConfig b = a;
        b.cfoHz = 4.5e6; b.tx.mod = k8psk; b.tx.rate = 4; b.seed = 9;
        uint64_t pn = 0;
        a.ts = [&pn](uint8_t* p) { testPacket(pn++, p); };
        DvbsSignal sa(a), sb(b);
        DvbsReceiver rx;
        rx.setBlocking(true);
        rx.configure(10e6);
        uint64_t good = 0, bad = 0;
        rx.setPacketCallback([&](const uint8_t* p, size_t n, double) {
            for (size_t i = 0; i < n; i++, p += 188) {
                const uint64_t idx = ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) | ((uint64_t)p[6] << 8) | p[7];
                uint8_t ex[188]; testPacket(idx, ex);
                if (memcmp(p, ex, 188) == 0) good++; else bad++;
            }
        });
        std::vector<cf32> x(65536), y(65536);
        for (int k = 0; k < 40; k++) {
            sa.generate(x.data(), x.size()); sb.generate(y.data(), y.size());
            for (size_t i = 0; i < x.size(); i++) {
                cf32 v = x[i] + y[i];
                auto q = [](float f) { return std::round(std::min(127.f, std::max(-128.f, f * 128.f))) / 128.f; };
                x[i] = cf32(q(v.real()), q(v.imag()));
            }
            rx.feed(x.data(), x.size());
        }
        rx.flush();
        DvbsTelemetry t;
        rx.telemetry(t, 0);
        CHECK(good > 3000 && bad == 0, "adjacent carrier: %llu good packets, %llu damaged; %s", (unsigned long long)good, (unsigned long long)bad, dvbsSummary(t).c_str());
        printf("adjacent carrier: %llu good packets, %llu damaged, %s\n", (unsigned long long)good, (unsigned long long)bad, dvbsSummary(t).c_str());
    }

    printf(fails ? "dvbs rx3: FAILED (%d)\n" : "dvbs rx3: ok\n", fails);
    return fails ? 1 : 0;
}
