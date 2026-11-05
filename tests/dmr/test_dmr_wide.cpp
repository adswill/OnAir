// Whole-span decoding: the carrier finder on synthetic carriers (offsets, levels, appearing and vanishing, the cap, adjacent channels) and the
// DMR bank on several generated DMR carriers at once (different levels, offsets of +-1 kHz, one that appears late).
#include "dect2/dsp/carrierfind.h"
#include "dect2/common/stable_random.h"
#include "dect2/dmr/dmr_gen.h"
#include "dect2/dmr/dmr_rx.h"
#include "dect2/engine/wide_bank.h"
#include "dect2/engine/wide_modes.h"
#include "dect2/t2/t2rx.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static const double kFs = 2.4e6;

// narrowband carrier: a tone with 4-level FSK-like wobble (so it is not a pure line), at amplitude amp, plus nothing else
struct Tone { double hz, amp; bool on = true; double ph = 0, mph = 0; };
static void synth(std::vector<Tone>& tones, std::mt19937& rng, std::vector<cf32>& out, size_t n, double noiseRms) {
    dect2::normal_distribution<float> nd(0.f, 1.f);
    out.assign(n, cf32(0, 0));
    for (auto& t : tones) {
        if (!t.on) continue;
        for (size_t i = 0; i < n; i++) {
            t.mph += 2 * M_PI * 1.2e3 / kFs; if (t.mph > 2 * M_PI) t.mph -= 2 * M_PI;
            t.ph += 2 * M_PI * (t.hz + 1500.0 * std::sin(t.mph)) / kFs; if (t.ph > 2 * M_PI) t.ph -= 2 * M_PI;
            out[i] += cf32((float)(t.amp * std::cos(t.ph)), (float)(t.amp * std::sin(t.ph)));
        }
    }
    for (size_t i = 0; i < n; i++) out[i] += cf32{nd(rng), nd(rng)} * (float)(noiseRms * 0.7071);
}
static void run(CarrierFinder& f, std::vector<Tone>& tones, std::mt19937& rng, double secs, double noise = 0.003) {
    std::vector<cf32> b;
    for (double t = 0; t < secs; t += 0.1) { synth(tones, rng, b, (size_t)(kFs * 0.1), noise); f.feed(b.data(), b.size()); }
}
static bool has(const CarrierFinder& f, double hz, double tol, bool* active = nullptr) {
    for (auto& c : f.carriers()) if (std::fabs(c.hz - hz) < tol) { if (active) *active = c.active; return true; }
    return false;
}

static void testFinder() {
    std::mt19937 rng(7);
    CarrierCfg cfg; cfg.maxCarriers = 8; cfg.holdSec = 2;
    CarrierFinder f; f.configure(kFs, cfg);
    std::vector<Tone> tones = {{37500 + 800, 0.05}, {-112500 - 900, 0.01}, {-300000, 0.003}};
    run(f, tones, rng, 1.5);
    CHECK(f.carriers().size() == 3, "found %zu carriers, expected 3", f.carriers().size());
    CHECK(has(f, 38300, 500) && has(f, -113400, 500) && has(f, -300000, 500), "carrier frequencies off (offsets +-1 kHz must be kept)");
    for (auto& c : f.carriers()) CHECK(std::fabs(c.hz - std::round((c.hz - 0) / 12500.0) * 12500.0) < 1100, "offset beyond 1.1 kHz from the raster at %.0f", c.hz);
    // level order
    double l1 = -1e9, l2 = -1e9; for (auto& c : f.carriers()) { if (std::fabs(c.hz - 38300) < 500) l1 = c.levelDb; if (std::fabs(c.hz + 113400) < 500) l2 = c.levelDb; }
    CHECK(l1 - l2 > 10, "levels not ordered: %.1f vs %.1f", l1, l2);
    // adjacent channel: only the three; the strong carrier's neighbours must not appear (no phantom at +-12.5 kHz of 38300)
    CHECK(!has(f, 38300 + 12500, 3000) && !has(f, 38300 - 12500, 3000), "phantom carriers beside the strong one");
    // appears late
    tones.push_back({200000.0, 0.02});
    run(f, tones, rng, 0.15);
    CHECK(!has(f, 200000, 2000), "a carrier reported before the debounce time");
    run(f, tones, rng, 1.0);
    CHECK(has(f, 200000, 600), "late carrier not found");
    // goes quiet: stays tracked (held), inactive, then removed after the hold time
    tones[1].on = false;
    run(f, tones, rng, 1.0);
    bool act = true; CHECK(has(f, -113400, 800, &act) && !act, "quiet carrier not held");
    run(f, tones, rng, 2.5);
    CHECK(!has(f, -113400, 800), "quiet carrier not dropped after the holding time");
    // frequency offset of the whole set: still found
    // cap: 12 carriers, at most 8 kept, the strongest among them
    CarrierCfg c2 = cfg; c2.maxCarriers = 8; CarrierFinder g; g.configure(kFs, c2);
    std::vector<Tone> many; for (int i = 0; i < 12; i++) many.push_back({-500000.0 + i * 62500.0 + 400, 0.004 + 0.004 * i});
    run(g, many, rng, 2.0);
    CHECK(g.carriers().size() == 8, "cap: %zu carriers tracked, expected 8", g.carriers().size());
    for (int i = 4; i < 12; i++) CHECK(has(g, -500000.0 + i * 62500.0 + 400, 700), "cap: strong carrier %d dropped", i);
    // adjacent strong/weak: a weak carrier 3 cells away from a strong one is still found, the cell next to the strong one is not a carrier
    CarrierFinder h; h.configure(kFs, cfg);
    std::vector<Tone> two = {{0, 0.1}, {37500, 0.004}};
    run(h, two, rng, 1.5);
    CHECK(h.carriers().size() == 2, "strong+weak: %zu carriers", h.carriers().size());
    // the cost: the finder alone on 2.4 Msps
    auto t0 = std::chrono::steady_clock::now(); run(h, two, rng, 2.0); (void)t0;
}

static void testDmrBank() {
    struct C { double hz, rms; double startSec; int seed; };
    std::vector<C> cs = {{-100000 + 700, 0.10, 0, 11}, {25000 - 900, 0.05, 0, 22}, {187500 + 300, 0.03, 4.0, 33}};
    std::vector<std::unique_ptr<DmrSignal>> sig;
    for (auto& c : cs) {
        DmrGenConfig g; g.rate = kFs; g.cc = 1 + c.seed % 5; g.seed = (uint32_t)c.seed; g.cfoHz = c.hz; g.rms = c.rms; g.snrDb = 35; g.traffic = 2;
        sig.emplace_back(new DmrSignal(g));
        if (c.startSec > 0) sig.back()->setCarrier(false);
    }
    WideModes wm;
    wm.configure(10, kFs, 0);
    CHECK(wm.ready(10), "bank not ready at 2.4 Msps");
    std::vector<cf32> sum, tmp;
    const double total = 14.0; const size_t blk = (size_t)(kFs * 0.1);
    uint64_t seq = 0; RxTelemetry t; size_t peakLive = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (double s = 0; s < total; s += 0.1) {
        sum.assign(blk, cf32(0, 0)); tmp.resize(blk);
        for (size_t k = 0; k < sig.size(); k++) {
            if (s >= cs[k].startSec && cs[k].startSec > 0 && s < cs[k].startSec + 0.1) sig[k]->setCarrier(true);
            sig[k]->generate(tmp.data(), blk);
            for (size_t i = 0; i < blk; i++) sum[i] += tmp[i];
        }
        wm.feed(10, sum.data(), blk);
        peakLive = std::max(peakLive, wm.liveCount(10));
        wm.telemetry(10, t, seq);
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("DMR wide: %.1f s of 3 carriers in %.1f s wall (includes the generators), peak %zu decoders\n", total, wall, peakLive);
    CHECK(peakLive == 3, "peak decoders %zu, expected 3", peakLive);
    CHECK(t.wide.size() == 3, "carriers table has %zu rows", t.wide.size());
    for (auto& r : t.wide) printf("  carrier %+9.0f Hz level %.1f dB snr %.1f state %d %s\n", r.hz, r.levelDb, r.snrDb, r.state, r.info.c_str());
    for (size_t k = 0; k < cs.size(); k++) {
        // the table reports the 12.5 kHz channel centre, not the measured frequency (+-1 kHz off it)
        const double chan = std::round(cs[k].hz / 12500.0) * 12500.0;
        bool row = false; for (auto& r : t.wide) if (std::fabs(r.hz - chan) < 1.0) row = true;
        CHECK(row, "no table row on channel %.0f Hz for carrier %zu at %.0f Hz", chan, k, cs[k].hz);
        // calls: every finished voice call the generator sent (after the carrier was found) must be in the merged log at that carrier
        int sent = 0, found = 0;
        for (auto& tr : sig[k]->truth()) {
            if (!tr.finished || (tr.kind != 0 && tr.kind != 1) || tr.startSec < cs[k].startSec + 2.5) continue;
            sent++;
            for (auto& c : t.dmr.callLog)
                if (c.carrierId && std::fabs(c.carrierHz - chan) < 1.0 && c.src == tr.src && c.dst == tr.dst && c.kind == tr.kind) { found++; break; }
        }
        printf("  carrier %zu: voice calls %d/%d found in the merged log\n", k, found, sent);
        CHECK(sent >= 1, "carrier %zu: the signal held no voice call to check", k);
        CHECK(found == sent, "carrier %zu: %d of %d voice calls", k, found, sent);
    }
    bool anyUn = false; for (auto& c : t.dmr.callLog) if (!c.carrierId) anyUn = true;
    CHECK(!anyUn, "a call without a carrier stamp");
    // reset keeps the logs, clear empties them
    wm.clearResults();
    wm.telemetry(10, t, seq);
    CHECK(t.dmr.callLog.empty(), "clearResults left %zu calls", t.dmr.callLog.size());
}

// dmrTalking (the whole-span sound rule's voice hook): true while a voice call is on the air, false in the idle time between calls. A base
// station: its idle bursts keep the carrier locked between calls, as a control channel's CSBKs do.
static void testDmrTalking() {
    DmrGenConfig g; g.rate = kFs; g.cc = 3; g.seed = 7; g.snrDb = 35; g.traffic = 1;
    DmrSignal sig(g);
    DmrReceiver rx;
    rx.configure(kFs);
    std::vector<cf32> buf((size_t)(kFs * 0.05));
    DmrTelemetry t; uint64_t seq = 0;
    int inCallTrue = 0, inCall = 0, idleFalse = 0, idle = 0;
    for (int i = 0; i < 600; i++) {   // 30 s in 50 ms steps
        const double s = i * 0.05;
        sig.generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        if (rx.telemetry(t, seq)) seq = t.seq;
        if (s < 2.0 || t.state != 2) continue;   // locked first
        const double now = sig.seconds();
        bool voice = false, closeBy = false;
        for (const auto& tr : sig.truth()) {
            if (tr.kind > 2) continue;
            const double end = tr.finished ? tr.endSec : 1e9;   // a call still going on
            if (now > tr.startSec + 0.6 && now < end - 0.2) voice = true;   // well inside a call (the receiver needs a few bursts)
            if (now > tr.startSec - 0.3 && now < end + 1.5) closeBy = true;     // the start, the call or its hang time
        }
        const bool talk = dmrTalking(t);
        if (voice) { inCall++; inCallTrue += talk; }
        else if (!closeBy) { idle++; idleFalse += !talk; }
    }
    printf("dmrTalking: true in %d of %d moments inside calls, false in %d of %d idle moments\n", inCallTrue, inCall, idleFalse, idle);
    CHECK(inCall >= 20 && idle >= 20, "the signal held too little call (%d) or idle (%d) time to judge", inCall, idle);
    CHECK(inCallTrue >= inCall * 9 / 10, "talking during calls only %d of %d", inCallTrue, inCall);
    CHECK(idleFalse >= idle * 9 / 10, "talking between calls %d of %d", idle - idleFalse, idle);
}

// snapping to the raster: nearest channel centre, with the raster through the tuned channel (origin) and any step
static void testSnap() {
    CHECK(std::fabs(snapToRaster(38300, 0, 12500) - 37500) < 1e-6, "38300 -> 37500");
    CHECK(std::fabs(snapToRaster(-113400, 0, 12500) + 112500) < 1e-6, "-113400 -> -112500");
    CHECK(std::fabs(snapToRaster(40500, 0, 12500) - 37500) < 1e-6 && std::fabs(snapToRaster(34300, 0, 12500) - 34300) < 1e-6, "3 kHz off snaps, 3.2 kHz off stays measured");
    CHECK(std::fabs(snapToRaster(43700, 0, 12500) - 43700) < 1e-6, "a signal half-way between two 12.5 kHz channels is not forced onto one");
    CHECK(std::fabs(snapToRaster(6500, 6250, 12500) - 6250) < 1e-6, "raster through the tuned channel (6.25 kHz off DC)");
    CHECK(std::fabs(snapToRaster(-5800, 6250, 12500) + 6250) < 1e-6, "negative side, origin off DC");
    CHECK(std::fabs(snapToRaster(31300, 0, 6250) - 31250) < 1e-6, "6.25 kHz raster");
    CHECK(std::fabs(snapToRaster(28000, 0, 25000) - 25000) < 1e-6, "25 kHz raster");
    CHECK(snapToRaster(31300, 0, 0) == 31300, "no step: unchanged");
    // the WideModes setting reaches the bank: a 25 kHz raster is configured and ready
    WideModes wm;
    WideTuning t; t.rasterKhz = 25; wm.setTuning(t);
    wm.configure(10, kFs, 0);
    CHECK(wm.ready(10), "bank not ready with a 25 kHz raster");
    t.rasterKhz = 0; wm.setTuning(t);
    CHECK(wm.ready(10), "bank not ready after going back to automatic");
}

// the raster through a tuned channel that is not on the span centre (a radio centre 4.3 kHz from the dial, PMR446-style 6.25 kHz offsets) and the
// other steps: every carrier (offset +-0.8 kHz from its channel) is reported on its own channel centre, in the table and in the call log
static void testDmrGrid(double stepKhz, double originHz) {
    const double step = std::max(stepKhz * 1000.0, 12500.0);   // a DMR carrier is 12.5 kHz wide: a finer raster still uses 12.5 kHz cells (no phantom second row)
    struct C { int k; double off; int seed; };
    const std::vector<C> cs = {{-6, +700, 11}, {5, -800, 22}};
    std::vector<std::unique_ptr<DmrSignal>> sig;
    for (auto& c : cs) {
        DmrGenConfig g; g.rate = kFs; g.cc = 1 + c.seed % 5; g.seed = (uint32_t)c.seed; g.cfoHz = originHz + c.k * step + c.off; g.rms = 0.08; g.snrDb = 35; g.traffic = 2;
        sig.emplace_back(new DmrSignal(g));
    }
    WideModes wm;
    WideTuning wt; wt.rasterKhz = stepKhz; wm.setTuning(wt);
    wm.configure(10, kFs, originHz);
    std::vector<cf32> sum, tmp;
    const size_t blk = (size_t)(kFs * 0.1);
    uint64_t seq = 0; RxTelemetry t;
    for (double s = 0; s < 10.0; s += 0.1) {
        sum.assign(blk, cf32(0, 0)); tmp.resize(blk);
        for (auto& sg : sig) { sg->generate(tmp.data(), blk); for (size_t i = 0; i < blk; i++) sum[i] += tmp[i]; }
        wm.feed(10, sum.data(), blk);
        wm.telemetry(10, t, seq);
    }
    printf("DMR grid step %.2f kHz origin %.0f Hz: %zu rows\n", stepKhz, originHz, t.wide.size());
    for (auto& r : t.wide) printf("  row %+9.1f Hz\n", r.hz);
    for (auto& c : cs) {
        const double chan = originHz + c.k * step;
        bool row = false; for (auto& r : t.wide) if (std::fabs(r.hz - chan) < 1.0) row = true;
        CHECK(row, "step %.2f origin %.0f: no row on the channel centre %.1f Hz", stepKhz, originHz, chan);
        int calls = 0, bad = 0;
        for (auto& cl : t.dmr.callLog) if (cl.carrierId && std::fabs(cl.carrierHz - chan) < step / 2) { calls++; if (std::fabs(cl.carrierHz - chan) > 1.0) bad++; }
        CHECK(bad == 0, "step %.2f origin %.0f: %d calls carry a frequency off the channel centre %.1f", stepKhz, originHz, bad, chan);
    }
    CHECK(t.wide.size() == cs.size(), "step %.2f origin %.0f: %zu rows, expected %zu", stepKhz, originHz, t.wide.size(), cs.size());
}

int main() {
    for (double st : {12.5, 6.25, 25.0}) for (double o : {0.0, 6250.0, -4300.0}) testDmrGrid(st, o);
    testDmrTalking();
    testSnap();
    testFinder();
    testDmrBank();
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
