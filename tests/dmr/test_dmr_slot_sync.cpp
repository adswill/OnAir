// DMR subscriber TX slot timing (dmr_slot_sync.h, TS 102 361-1 4.2 / 4.5 / 5.1.1): listen to a simulated repeater output, fit its TDMA frame from
// the CACH reports of the receiver, key up with the lead the fit gives, and check where OnAir's own inbound bursts land: each burst centre must sit
// on the centre of the outbound burst of the same inbound slot number plus 30 ms (offset channel timing), or of the other number (aligned), inside
// the inbound guard time. Impairments: carrier offset, noise, clock error, a random start phase, every slot / timing combination, a late key-up,
// wrongly decoded TC bits. Simulated signals only (no repeater recording with a known timing).
#include "dect2/dmr/dmr_slot_sync.h"
#include "dect2/dmr/dmr_tx.h"
#include "dect2/dmr/dmr_gen.h"
#include "dect2/dmr/dmr_rx.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static double wrapTo(double x, double p) { x = std::fmod(x, p); if (x >= p / 2) x -= p; if (x < -p / 2) x += p; return x; }

struct Ev { double t; int family, slot, at; };

// run a signal through the receiver; events on the input clock
static std::vector<Ev> receive(const std::vector<cf32>& x, double rate) {
    DmrReceiver rx;
    rx.setSilent(true);
    rx.configure(rate);
    std::vector<Ev> ev;
    rx.setBurstTimingCallback([&](double t, int f, int s, int a) { ev.push_back({t, f, s, a}); });
    const size_t blk = 16384;
    for (size_t i = 0; i < x.size(); i += blk) rx.feed(x.data() + i, std::min(blk, x.size() - i));
    return ev;
}

static std::vector<cf32> gen(ModeSynth& s, double secs) {
    std::vector<cf32> x((size_t)(secs * s.sampleRate()));
    s.generate(x.data(), x.size());
    return x;
}

// the centre of the first carrier-on segment of a transmission (half-amplitude crossings of a smoothed envelope), seconds
static double firstBurstCentre(const std::vector<cf32>& x, double rate) {
    const size_t w = (size_t)(rate * 0.0002);
    std::vector<double> e(x.size());
    double acc = 0, peak = 0;
    for (size_t i = 0; i < x.size(); i++) {
        acc += std::abs(x[i]);
        if (i >= w) acc -= std::abs(x[i - w]);
        e[i] = acc / (double)w;
        peak = std::max(peak, e[i]);
    }
    size_t a = 0;
    while (a < e.size() && e[a] < peak / 2) a++;
    size_t b = a;
    while (b < e.size() && e[b] >= peak / 2) b++;
    return ((double)a + (double)b) / 2.0 / rate - (double)w / 2.0 / rate;
}

int main() {
    const double sym = 1.0 / 4800.0;
    const bool longRun = std::getenv("ONAIR_TEST_LONG") != nullptr;   // the high sample rates and more combinations

    // ---- 1. the generator's delay to symbol 0 of its first burst, and that the lead moves the burst by exactly that much
    double genDelay = 0;
    {
        DmrTxConfig t; t.rate = 2.4e6; t.myId = 2550123; t.dest = 91; t.call = DmrTxCall::Kerchunk; t.slot = 1;
        auto s = makeDmrTxSynth(t);
        DmrGenConfig gc = dmrTxGenConfig(t); gc.snrDb = 80;
        auto s0 = makeDmrSynthFromConfig(gc);
        const std::vector<cf32> x = gen(*s0, 0.2);
        const double c = firstBurstCentre(x, t.rate);
        genDelay = c - kDmrBurstCentreSym * sym;
        printf("generator: first burst centre %.4f ms, delay to symbol 0 %.4f ms (library constant %.4f ms)\n", c * 1e3, genDelay * 1e3, dmrTxGenDelaySec() * 1e3);
        CHECK(std::fabs(genDelay - dmrTxGenDelaySec()) < 30e-6, "generator delay %.4f ms differs from dmrTxGenDelaySec %.4f ms", genDelay * 1e3, dmrTxGenDelaySec() * 1e3);
        for (double lead : {0.0123, 0.0471, 0.1}) {
            gc.txLeadSec = lead;
            auto s1 = makeDmrSynthFromConfig(gc);
            const double c1 = firstBurstCentre(gen(*s1, 0.3), t.rate);
            CHECK(std::fabs(c1 - c - lead) < 30e-6, "lead %.4f s moved the burst by %.4f ms", lead, (c1 - c) * 1e3);
        }
        // slot 2: the generator's first burst is 30 ms later
        gc.txLeadSec = 0; gc.txSlot = 2;
        auto s2 = makeDmrSynthFromConfig(gc);
        const double c2 = firstBurstCentre(gen(*s2, 0.3), t.rate);
        CHECK(std::fabs(c2 - c - kDmrSlotSec) < 30e-6, "slot 2 burst %.4f ms after slot 1 (wanted 30)", (c2 - c) * 1e3);
    }

    // ---- 2. the receiver's burst times against the generator's outbound truth at several sample rates; TC gives the right slot
    for (double rate : {1.2e6, 2.0e6, 2.4e6, 8e6, 10e6}) {
        if (rate > 3e6 && !longRun) continue;
        DmrGenConfig gc; gc.rate = rate; gc.snrDb = 30; gc.seed = 3;
        auto s = makeDmrSynthFromConfig(gc);
        const std::vector<cf32> x = gen(*s, 2.0);
        const std::vector<Ev> ev = receive(x, rate);
        double sum = 0, sum2 = 0; int n = 0, wrongSlot = 0;
        for (const Ev& e : ev) {
            if (e.family != 0) continue;
            // outbound slot k: 12 CACH symbols, then the burst; slot k is TS1 for even k
            const double k = std::round((e.t - genDelay - (12 + kDmrBurstCentreSym) * sym) / kDmrSlotSec);
            const double truth = k * kDmrSlotSec + (12 + kDmrBurstCentreSym) * sym + genDelay;
            const double r = e.t - truth;
            sum += r; sum2 += r * r; n++;
            if (e.slot != (((long)k & 1) ? 2 : 1)) wrongSlot++;
        }
        const double mean = n ? sum / n : 0, sd = n ? std::sqrt(std::max(0.0, sum2 / n - mean * mean)) : 0;
        printf("receiver %.1f Msps: %d outbound bursts, time error %+.4f ms (sd %.4f ms), wrong slot %d\n", rate / 1e6, n, mean * 1e3, sd * 1e3, wrongSlot);
        CHECK(n > 40, "%.1f Msps: only %d outbound bursts timed", rate / 1e6, n);
        CHECK(std::fabs(mean) < 0.05e-3, "%.1f Msps: receiver burst time off by %.4f ms", rate / 1e6, mean * 1e3);
        CHECK(sd < 0.05e-3, "%.1f Msps: receiver burst time spread %.4f ms", rate / 1e6, sd * 1e3);
        CHECK(wrongSlot == 0, "%.1f Msps: %d bursts with the wrong slot", rate / 1e6, wrongSlot);
    }

    // ---- 3. listen, fit, key up: every logical channel x channel timing, with impairments and a random start phase
    struct Case { double rate, cfo, snr, ppm, drop, wait; };
    const Case cases[] = {{2.4e6, 0, 35, 0, 0.0071, 0.5}, {2.4e6, 1500, 14, 0, 0.0213, 1.3}, {2.0e6, -900, 18, 5, 0.0442, 0.2}, {10e6, 2500, 20, 0, 0.0167, 3.0}};
    int ci = 0;
    for (const Case& cs : cases) for (int logical = 1; logical <= 2; logical++) for (int aligned = 0; aligned <= 1; aligned++) {
        // short run: every channel x timing combination once, each under a different impairment case; long run: all of them
        const int idx = ci++;
        if (!longRun && (idx % 4) != (int)(&cs - cases)) continue;
        DmrGenConfig gc; gc.rate = cs.rate; gc.snrDb = cs.snr; gc.cfoHz = cs.cfo; gc.sroPpm = cs.ppm; gc.seed = 11 + logical;
        auto s = makeDmrSynthFromConfig(gc);
        std::vector<cf32> x = gen(*s, 2.5 + cs.drop);
        // the receiver starts listening at a random point of the repeater's frame
        const size_t drop = (size_t)(cs.drop * cs.rate);
        x.erase(x.begin(), x.begin() + (long)drop);
        const double t0 = (double)drop / cs.rate;   // input clock 0 = repeater time t0
        DmrSlotClock clk;
        for (const Ev& e : receive(x, cs.rate)) if (e.family == 0) clk.observe(e.t, e.slot, e.at);
        const DmrSlotClock::Fit f = clk.fit();
        CHECK(f.ok, "case %.1f Msps cfo %.0f: no fit (%d of %d used, rms %.3f ms)", cs.rate / 1e6, cs.cfo, f.used, f.seen, f.rmsMs);
        if (!f.ok) continue;
        // the transmission's first sample goes on the air `wait` s after listening stopped
        const double txStart = (double)x.size() / cs.rate + cs.wait;
        DmrTxConfig t; t.rate = cs.rate; t.myId = 2550123; t.dest = 91; t.colourCode = 1; t.slot = logical; t.call = DmrTxCall::GroupVoice; t.voiceSeconds = 0.72;
        const DmrTxLead l = dmrTxApplySlotSync(t, logical, aligned != 0, txStart, f);
        CHECK(l.ok, "no lead");
        DmrGenConfig tc = dmrTxGenConfig(t); tc.snrDb = 40;
        auto txs = makeDmrSynthFromConfig(tc);
        const std::vector<cf32> y = gen(*txs, 1.4);
        const std::vector<Ev> in = receive(y, cs.rate);
        int n = 0, bad = 0; double worst = 0;
        const int inSlot = DmrSlotClock::inboundSlot(logical, aligned != 0);
        for (const Ev& e : in) {
            if (e.family != 1) continue;
            const double tRep = txStart + e.t + t0;   // on the repeater's clock
            // the repeater's outbound slot inSlot centres, then the inbound one 30 ms later
            const double outC = (inSlot == 2 ? kDmrSlotSec : 0) + (12 + kDmrBurstCentreSym) * sym + genDelay;
            const double frame = kDmrFrameSec * (1 + cs.ppm * 1e-6);
            const double r = wrapTo(tRep - (outC + kDmrSlotSec), frame);
            // the guard time is 1.25 ms each side; keep well inside it
            worst = std::max(worst, std::fabs(r));
            if (std::fabs(r) > 0.25e-3) bad++;
            n++;
        }
        printf("%.1f Msps cfo %+5.0f snr %2.0f ppm %.0f: ch %d %s -> inbound slot %d, lead %.4f ms, %d bursts, worst %.4f ms (fit rms %.4f ms, %d obs)\n",
               cs.rate / 1e6, cs.cfo, cs.snr, cs.ppm, logical, aligned ? "aligned" : "offset", inSlot, l.leadSec * 1e3, n, worst * 1e3, f.rmsMs, f.used);
        CHECK(n >= 4, "only %d inbound bursts found", n);   // the voice header, burst A of each superframe and the terminator carry a sync
        CHECK(bad == 0, "%d inbound bursts outside 0.25 ms of their slot (worst %.4f ms)", bad, worst * 1e3);
    }

    // ---- 4. robustness of the fit: wrong TC bits, a stale fit, too few observations, a clock that went back
    {
        DmrSlotClock clk;
        const double a = 100.0123;
        int k = 0;
        for (int i = 0; i < 120; i++, k++) {
            int slot = (k & 1) ? 2 : 1;
            if (i % 9 == 4) slot = 3 - slot;   // about 11 % wrongly decoded TC
            clk.observe(a + k * kDmrSlotSec + ((i * 37) % 11 - 5) * 2e-6, slot, i % 2);
        }
        const DmrSlotClock::Fit f = clk.fit();
        CHECK(f.ok, "fit with wrong TC bits failed");
        CHECK(std::fabs(wrapTo(f.ts1CentreSec - a, kDmrFrameSec)) < 20e-6, "fit phase off by %.4f ms", wrapTo(f.ts1CentreSec - a, kDmrFrameSec) * 1e3);
        CHECK(f.used >= 100, "only %d observations used", f.used);
        // offset timing, channel 1: inbound slot 1 centre = outbound slot 1 centre + 30 ms
        const double st = DmrSlotClock::inboundBurstStart(f, 1, false, 200.0);
        CHECK(std::fabs(wrapTo(st + kDmrBurstCentreSym / 4800.0 - (a + kDmrSlotSec), kDmrFrameSec)) < 20e-6, "inbound slot 1 start wrong");
        CHECK(st >= 200.0 && st < 200.0 + kDmrFrameSec, "start %.6f not the first after 200 s", st);
        // aligned channel 1 = inbound slot 2 = outbound slot 1 centre + 60 ms (same centre as outbound slot 1)
        const double sa = DmrSlotClock::inboundBurstStart(f, 1, true, 200.0);
        CHECK(std::fabs(wrapTo(sa + kDmrBurstCentreSym / 4800.0 - a, kDmrFrameSec)) < 20e-6, "aligned inbound start wrong");
        const DmrTxLead stale = dmrTxLeadFor(f, 1, false, f.lastSec + 30, 0);
        CHECK(!stale.ok, "a fit 30 s old was used");
        const DmrTxLead fresh = dmrTxLeadFor(f, 2, false, f.lastSec + 2, 0);
        CHECK(fresh.ok && fresh.leadSec >= 0 && fresh.leadSec < kDmrFrameSec, "fresh lead %.4f", fresh.leadSec);
        CHECK(fresh.busy == 1 || fresh.busy == 0, "busy state not reported");
        clk.observe(5.0, 1, 0);   // the clock went back: start again
        CHECK(!clk.fit().ok, "fit kept after the clock went back");
        DmrSlotClock none;
        CHECK(!none.fit().ok && !dmrTxLeadFor(none.fit(), 1, false, 1.0, 0).ok, "fit without observations");
    }

    printf(fails ? "dmr_slot_sync: %d FAILED\n" : "dmr_slot_sync: all passed\n", fails);
    return fails ? 1 : 0;
}
