// Dropouts and a reset. (1) 3.3 ms of samples vanish from the stream after the first fix (a radio that drops a USB transfer): the code phase and carrier
// phase of every channel jump, the channels are lost and found again, and the fix comes back. (2) Later 5 ms of zeros (a glitch that keeps the timeline):
// the loops ride through. (3) reset() in the middle of a run: everything is forgotten, the report numbers keep growing, the fix is made again.
#include "dect2/gnss_testkit.h"
#include "dect2/test_parallel.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
using namespace dect2::gnsstest;
using dect2::testpar::CaseOut;
// every case runs on its own thread and prints through `out`, so that the log keeps the order of the cases
#define CHECK(c, ...) do { if (!(c)) out.fail(__LINE__, __VA_ARGS__); } while (0)

// (1) the two dropouts
static void dropouts(CaseOut& out) {
    {
        GnssSimConfig cfg;
        GnssSim sim(cfg, 4e6);
        int nTx = 0;
        for (auto& s : sim.sats()) nTx += s.transmitted;
        Options o;
        o.rate = 4e6; o.secs = 52; o.chunk = 16384;
        const size_t gapAt = (size_t)(30.0 * 4e6), gapLen = 13200;       // 3.3 ms
        const size_t zeroAt = (size_t)(46.0 * 4e6), zeroLen = 20000;     // 5 ms
        o.hook = [&](std::vector<cf32>& b, size_t off) -> size_t {
            if (gapAt >= off && gapAt < off + b.size()) {
                const size_t at = gapAt - off;
                b.erase(b.begin() + (long)at, b.begin() + (long)std::min(b.size(), at + gapLen));
            }
            if (zeroAt >= off && zeroAt < off + b.size())
                for (size_t i = zeroAt - off; i < std::min(b.size(), zeroAt - off + zeroLen); i++) b[i] = cf32(0.f, 0.f);
            return b.size();
        };
        Run r = run(sim, o);
        const GnssTelemetry& t = r.tel;
        int lostAtGap = 0;
        double fixBefore = -1, fixBack = -1;
        for (auto& rep : r.reports) if (rep.signalSecs > 28 && rep.signalSecs < 30 && rep.fix.valid) fixBefore = rep.signalSecs;
        // the number of channels lost in the log after the gap, and when the first satellite was locked again
        for (auto& l : r.log) if (l.find(" lost") != std::string::npos || l.find("not confirmed") != std::string::npos) lostAtGap++;
        // the time when 5 satellites had frame sync again after the gap: from the reports
        for (auto& rep : r.reports) {
            if (rep.signalSecs < 31) continue;
            int framed = 0;
            for (auto& c : rep.channels) if (c.state >= GnssChFrameSync) framed++;
            if (framed >= 5 && fixBack < 0) fixBack = rep.signalSecs;
        }
        double hz = 0, vt = 0;
        if (t.fix.valid) fixError(sim, t.fix, &hz, &vt);
        out.print("gap of 3.3 ms at 30 s: %d satellite events in the log; frame sync on 5 satellites again at %.0f s; at 52 s: %s, error %.2f / %.2f m, %u fixes, %d satellites tracked of %d\n", lostAtGap, fixBack,
               gnssSummary(t).c_str(), hz, vt, t.fix.fixCount, t.nTracked, nTx);
        CHECK(fixBefore > 0, "no fix before the gap");
        CHECK(t.fix.valid && t.fix.nSats >= nTx - 2, "no fix at the end (%d satellites)", t.fix.nSats);
        CHECK(hz < 15.0 && std::fabs(vt) < 25.0, "position error %.1f / %.1f m after the dropouts", hz, vt);
        CHECK(lostAtGap >= nTx / 2, "the gap should have cost most channels (%d events)", lostAtGap);
        CHECK(fixBack > 0 && fixBack < 50.0, "the satellites were not framed again in time (%.0f s): the receiver needs about 16 s after a gap (acquisition, bit sync, the next whole subframe)", fixBack);
        CHECK(t.nTracked >= nTx - 1, "%d of %d tracked at the end", t.nTracked, nTx);
    }
}

// (3) reset in the middle
static void resetMiddle(CaseOut& out) {
    {
        // reset in the middle
        GnssSimConfig cfg;
        GnssSim sim(cfg, 4e6);
        int nTx = 0;
        for (auto& s : sim.sats()) nTx += s.transmitted;
        Options o;
        o.rate = 4e6; o.secs = 31; o.chunk = 65536;
        uint64_t seqBefore = 0, seqAfter = 0;
        bool done = false;
        o.after = [&](GnssReceiver& rx, size_t n) {
            if (!done && n >= (size_t)(3.0 * 4e6)) {
                GnssTelemetry t;
                rx.telemetry(t, 0);
                seqBefore = t.seq;
                CHECK(t.nTracked >= 1, "nothing tracked before the reset");
                rx.reset();
                done = true;
            }
        };
        Run r = run(sim, o);
        for (auto& rep : r.reports) if (rep.signalSecs < 4.5) seqAfter = rep.seq;
        const GnssTelemetry& t = r.tel;
        double hz = 0, vt = 0;
        if (t.fix.valid) fixError(sim, t.fix, &hz, &vt);
        out.print("reset at 3 s: report number %llu before, %llu at 4 s, %llu at the end; %s, error %.2f / %.2f m\n", (unsigned long long)seqBefore, (unsigned long long)seqAfter, (unsigned long long)t.seq, gnssSummary(t).c_str(), hz, vt);
        CHECK(seqAfter > seqBefore, "the report numbers must keep growing through a reset (%llu then %llu)", (unsigned long long)seqBefore, (unsigned long long)seqAfter);
        CHECK(t.fix.valid && t.fix.nSats >= nTx - 2, "no fix after the reset (%d satellites)", t.fix.nSats);
        CHECK(hz < 15.0 && std::fabs(vt) < 25.0, "position error %.1f / %.1f m after the reset", hz, vt);
        // the clock of the receiver started again: the first fix comes about 26 s after the reset's signal start... the stream time restarts at the reset
        CHECK(t.fix.firstFixSecs > 20.0 && t.fix.firstFixSecs < 30.0, "first fix %.1f s after the reset", t.fix.firstFixSecs);
    }
}

int main() {
    // the two runs share nothing: side by side, the log keeps the order
    const int fails = dect2::testpar::runCases(2, [](size_t i, CaseOut& out) { if (i == 0) dropouts(out); else resetMiddle(out); });
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
