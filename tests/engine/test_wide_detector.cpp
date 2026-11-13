// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The whole-span detector (core/include/dect2/dsp/carrierfind.h, core/include/dect2/engine/wide_bank.h) on synthetic multi-carrier spans with
// measured detection and false-alarm rates. Deterministic (a fixed LCG for the symbols and the noise) and quick (a few seconds of a 1 Msps span).
//
//   1. a busy span: weak / medium / strong 4FSK carriers, two on adjacent 12.5 kHz channels, one drifting 3 kHz/s across a cell boundary, one in
//      bursts, a 20 kHz wide carrier straddling two cells, an unmodulated spur, a weak carrier on a known channel (below the normal threshold):
//      every carrier found in >= 95 % of the updates, one id each (also the drifting one), the wide one once, the spur never, no false carrier
//   2. noise only, with a 10 dB hump in the noise floor (a front end's slope): no carrier at all (the per-cell floor)
//   3. the same weak carrier without the known channel: not found (the prior is what finds it)
//   4. a retune by 25 kHz: the carriers keep their ids (CarrierFinder::shift)
//   5. the bank: a late carrier's decoder gets the buffered start of the transmission; the mixer follows a drift; a retune keeps the decoders;
//      the CPU budget drops the weakest carrier (never the listened-to one), says "too slow" and gives the carrier back when the load falls;
//      the IQ recording of one carrier writes a WAV file
#include "dect2/dsp/carrierfind.h"
#include "dect2/engine/wide_bank.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

namespace {

struct Lcg {
    uint64_t s;
    explicit Lcg(uint64_t seed) : s(seed) {}
    uint32_t next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(s >> 33); }
    double uni() { return (next() + 0.5) / 2147483648.0; }   // (0, 1)
};

struct Sig {
    double hz = 0, snrDb = 20, driftHzPerSec = 0;
    int type = 0;                // 0: 4FSK 4800 Bd (+-600 / +-1800 Hz), 1: 2FSK 9600 Bd +-7 kHz (about 20 kHz wide), 2: unmodulated line
    double onSec = 0, offSec = 0, startSec = 0;   // onSec > 0: bursts (on, off, on ...) from startSec
    bool expect = true;          // the detector should find it
    // state
    std::complex<double> ph{1, 0}, rot{1, 0};
    double symLeft = 0;
    double amp = 0;
};

constexpr double kFs = 1e6, kStep = 12500, kNoise = 0.01;   // complex rms of the noise

// amplitude for a cell SNR (the noise in 0.8 of a raster step)
double ampFor(double snrDb, double fs) { return std::sqrt(std::pow(10.0, snrDb / 10) * kNoise * kNoise * 0.8 * kStep / fs); }

struct Span {
    double fs;
    std::vector<Sig> sig;
    Lcg rng{12345};
    double t = 0, humpGain = 0, humpHz = 0;
    std::complex<double> humpState[3] = {}, humpRot{1, 0}, humpPh{1, 0};
    explicit Span(double rate) : fs(rate) {}
    bool on(const Sig& s, double tt) const {
        if (tt < s.startSec) return false;
        if (s.onSec <= 0) return true;
        return std::fmod(tt - s.startSec, s.onSec + s.offSec) < s.onSec;
    }
    double gauss() { const double u = rng.uni(), v = rng.uni(); return std::sqrt(-2 * std::log(u)) * std::cos(2 * M_PI * v); }
    void block(std::vector<cf32>& out, size_t n) {
        out.assign(n, cf32(0, 0));
        const double sd = kNoise * std::sqrt(0.5);
        for (size_t i = 0; i < n; i++) {
            std::complex<double> w(gauss() * sd, gauss() * sd);
            if (humpGain > 0) {   // a broad bump in the floor (half power +-105 kHz): noise through three one-pole low-passes, moved to humpHz
                humpState[0] = 0.3 * humpState[0] + std::complex<double>(gauss() * sd, gauss() * sd);
                humpState[1] = 0.3 * humpState[1] + humpState[0];
                humpState[2] = 0.3 * humpState[2] + humpState[1];
                humpPh *= humpRot;
                w += humpGain * humpState[2] * humpPh;
            }
            out[i] = cf32((float)w.real(), (float)w.imag());
        }
        if (humpGain > 0) humpPh /= std::abs(humpPh);
        for (Sig& s : sig) {
            if (s.amp == 0) s.amp = ampFor(s.snrDb, fs);
            const double sps = fs / (s.type == 1 ? 9600.0 : 4800.0);
            for (size_t i = 0; i < n; i++) {
                if (s.symLeft <= 0) {   // a new symbol: its frequency (the drift moves at symbol steps)
                    double dev = 0;
                    if (s.type == 0) { static const double lv[4] = {-1800, -600, 600, 1800}; dev = lv[rng.next() & 3]; }
                    else if (s.type == 1) dev = (rng.next() & 1) ? 7000 : -7000;
                    const double f = s.hz + s.driftHzPerSec * (t + i / fs) + dev;
                    s.rot = std::polar(1.0, 2 * M_PI * f / fs);
                    s.symLeft += sps;
                    s.ph /= std::abs(s.ph);
                }
                s.symLeft -= 1;
                s.ph *= s.rot;
                if (on(s, t + i / fs)) out[i] += cf32((float)(s.amp * s.ph.real()), (float)(s.amp * s.ph.imag()));
            }
        }
        t += n / fs;
    }
    double trueHz(const Sig& s) const { return s.hz + s.driftHzPerSec * t; }
};

CarrierCfg cfgBase() {
    CarrierCfg c; c.stepHz = kStep; c.rejectTones = true; c.maxCarriers = 16; c.holdSec = 8; c.debounceSec = 0.3;
    return c;
}

struct Score {
    std::map<size_t, int> hits;             // signal index -> updates in which it was found active
    std::map<size_t, std::set<int>> ids;     // signal index -> ids it was found as
    std::set<int> falseIds;                  // ids that match no signal
    int updates = 0;
};

// one update's carriers against the truth: a carrier matches a signal within 0.6 of a step (1.2 steps for the wide one)
void score(const Span& sp, const CarrierFinder& f, Score& sc, bool count) {
    if (count) sc.updates++;
    for (const Carrier& c : f.carriers()) {
        int best = -1; double bd = 1e18;
        for (size_t k = 0; k < sp.sig.size(); k++) {
            const double tol = sp.sig[k].type == 1 ? 1.2 * kStep : 0.6 * kStep, d = std::fabs(c.hz - sp.trueHz(sp.sig[k]));
            if (d < tol && d < bd) { bd = d; best = (int)k; }
        }
        if (best < 0 || !sp.sig[(size_t)best].expect) { sc.falseIds.insert(c.id); continue; }
        sc.ids[(size_t)best].insert(c.id);
        if (count && c.active) sc.hits[(size_t)best]++;
    }
}

void busySpan() {
    Span sp(kFs);
    sp.sig = {
        {-300000 + 500, 12},                 // 0 weak
        {-200000, 20},                       // 1 medium
        {-100000 - 700, 35},                 // 2 strong
        {50000, 25}, {62500, 25},            // 3, 4 adjacent channels
        {150000 - 4000, 22, 3000},           // 5 drifting across the boundary at 156250 (at 3.4 s)
        {250000, 20, 0, 0, 0.6, 0.9, 0.3},   // 6 bursts
        {-400000 + 6250, 22, 0, 1},          // 7 20 kHz wide, between two raster points
        {350000 + 200, 30, 0, 2},            // 8 a spur: never a carrier
        {-50000, 7},                         // 9 weak, on a known channel
    };
    sp.sig[8].expect = false;
    CarrierCfg cfg = cfgBase();
    cfg.priorHz = {-50000};
    CarrierFinder f; f.configure(kFs, cfg);
    std::vector<cf32> x;
    Score sc;
    const double total = 6.0;
    bool burstSeen = false;
    while (sp.t < total - 1e-9) {
        sp.block(x, (size_t)(kFs * 0.1));
        f.feed(x.data(), x.size());
        score(sp, f, sc, sp.t > 1.0);
        if (std::getenv("WD_DEBUG")) { printf("t %.1f true %.0f:", sp.t, sp.trueHz(sp.sig[5])); for (const Carrier& c : f.carriers()) if (std::fabs(c.hz - sp.trueHz(sp.sig[5])) < 20000) printf(" [%d %.0f cell %.0f %s snr %.1f]", c.id, c.hz, c.cellHz, c.active ? "on" : "off", c.snrDb); printf("\n"); }
        if (!burstSeen && sp.t > 0.3 + 0.55 && sp.t < 0.3 + 0.65) {   // the first burst is reported within its first 0.55 s
            CHECK(!sc.ids[6].empty(), "the burst carrier was not reported within 0.55 s of its first burst");
            burstSeen = true;
        }
    }
    // detection rates (the burst one: over its bursts only it is active; it must stay one id)
    int detected = 0, expected = 0;
    for (size_t k = 0; k < sp.sig.size(); k++) {
        if (!sp.sig[k].expect || sp.sig[k].onSec > 0) continue;
        expected++;
        const double rate = sc.updates ? (double)sc.hits[k] / sc.updates : 0;
        printf("  signal %zu at %+8.0f Hz, %4.1f dB: found in %5.1f %% of %d updates, %zu id(s)\n", k, sp.sig[k].hz, sp.sig[k].snrDb, 100 * rate, sc.updates, sc.ids[k].size());
        CHECK(rate >= 0.95, "signal %zu (%.0f Hz, %.1f dB): found in only %.1f %% of the updates", k, sp.sig[k].hz, sp.sig[k].snrDb, 100 * rate);
        if (rate >= 0.95) detected++;
        CHECK(sc.ids[k].size() == 1, "signal %zu was reported under %zu ids (it must keep one: drift, merge)", k, sc.ids[k].size());
    }
    CHECK(sc.ids[6].size() == 1, "the burst carrier was reported under %zu ids (it must be held between bursts)", sc.ids[6].size());
    CHECK(sc.ids[8].empty(), "the unmodulated spur was reported as a carrier");
    printf("busy span: %d of %d steady carriers detected, %zu false carrier(s) (ids", detected, expected, sc.falseIds.size());
    for (int id : sc.falseIds) printf(" %d", id);
    printf(")\n");
    CHECK(sc.falseIds.empty(), "%zu false carriers in the busy span", sc.falseIds.size());
    // the wide one is one carrier with its width measured; the adjacent pair is two
    int wide = 0; double wideW = 0;
    for (const Carrier& c : f.carriers()) if (std::fabs(c.hz - sp.sig[7].hz) < 1.2 * kStep) { wide++; wideW = c.widthHz; }
    CHECK(wide == 1, "the 20 kHz carrier is %d carriers", wide);
    CHECK(wideW > 14000, "the 20 kHz carrier's width measured as %.0f Hz", wideW);
    CHECK(sc.ids[3].size() == 1 && sc.ids[4].size() == 1 && *sc.ids[3].begin() != *sc.ids[4].begin(), "adjacent carriers merged or lost");
    // the drifting one ended in the next cell, with its first id
    bool drift = false;
    for (const Carrier& c : f.carriers()) if (std::fabs(c.hz - sp.trueHz(sp.sig[5])) < 1500 && sc.ids[5].count(c.id)) drift = true;
    CHECK(drift, "the drifting carrier is not followed to %.0f Hz", sp.trueHz(sp.sig[5]));
    for (const Carrier& c : f.carriers()) if (std::fabs(c.hz + 50000) < 3000) CHECK(c.prior, "the carrier on the known channel is not marked");

    // 4. a retune by +25 kHz: every signal now sits 25 kHz lower in the span; the ids stay
    std::map<int, double> before;
    for (const Carrier& c : f.carriers()) before[c.id] = c.hz;
    f.shift(25000);
    for (Sig& s : sp.sig) s.hz -= 25000;
    for (const Carrier& c : f.carriers()) CHECK(before.count(c.id) && std::fabs(c.hz - (before[c.id] - 25000)) < 1, "carrier %d not moved by the retune", c.id);
    Score sc2;
    for (int i = 0; i < 10; i++) { sp.block(x, (size_t)(kFs * 0.1)); f.feed(x.data(), x.size()); score(sp, f, sc2, true); }
    for (size_t k : {0u, 1u, 2u, 3u, 4u, 7u}) {
        CHECK(sc2.ids[k].size() == 1 && sc.ids[k].count(*sc2.ids[k].begin()), "signal %zu got a new id after the retune", k);
        CHECK(sc2.hits[k] >= 9, "signal %zu found in %d of 10 updates after the retune", k, sc2.hits[k]);
    }
    CHECK(sc2.falseIds.empty(), "%zu false carriers after the retune", sc2.falseIds.size());
}

void noiseOnly() {
    Span sp(kFs);
    sp.humpGain = 1.7; sp.humpHz = 200000;   // about +14 dB over a few hundred kHz around +200 kHz
    sp.humpRot = std::polar(1.0, 2 * M_PI * sp.humpHz / kFs);
    CarrierFinder f; f.configure(kFs, cfgBase());
    std::set<int> ids; int carrierUpdates = 0, updates = 0;
    std::vector<cf32> x;
    while (sp.t < 4.0 - 1e-9) {
        sp.block(x, (size_t)(kFs * 0.1)); f.feed(x.data(), x.size());
        updates++;
        if (!f.carriers().empty()) carrierUpdates++;
        for (const Carrier& c : f.carriers()) ids.insert(c.id);
    }
    printf("noise only (14 dB hump in the floor): %zu false carrier(s), in %d of %d updates\n", ids.size(), carrierUpdates, updates);
    CHECK(ids.empty(), "%zu false carriers on noise with a sloped floor", ids.size());
    // the same with one floor for the span: the hump makes carriers (what the per-cell floor is for)
    Span sp2(kFs); sp2.humpGain = sp.humpGain; sp2.humpRot = sp.humpRot;
    CarrierCfg g = cfgBase(); g.localCells = 0;
    CarrierFinder h; h.configure(kFs, g);
    std::set<int> ids2;
    while (sp2.t < 2.0 - 1e-9) { sp2.block(x, (size_t)(kFs * 0.1)); h.feed(x.data(), x.size()); for (const Carrier& c : h.carriers()) ids2.insert(c.id); }
    printf("  (with one floor for the whole span: %zu false carriers)\n", ids2.size());
    CHECK(!ids2.empty(), "the hump is too small to test the per-cell floor (one floor for the span finds nothing either)");
}

void noPrior() {
    Span sp(kFs);
    sp.sig = {{-50000, 7}, {100000, 20}};
    CarrierFinder f; f.configure(kFs, cfgBase());
    std::vector<cf32> x;
    while (sp.t < 2.0 - 1e-9) { sp.block(x, (size_t)(kFs * 0.1)); f.feed(x.data(), x.size()); }
    bool weak = false, strong = false;
    for (const Carrier& c : f.carriers()) { if (std::fabs(c.hz + 50000) < 3000) weak = true; if (std::fabs(c.hz - 100000) < 3000) strong = true; }
    CHECK(!weak, "the 7 dB carrier was found without the known channel (the threshold is 9 dB)");
    CHECK(strong, "the 20 dB carrier was not found");
    // minimum SNR: a 12 dB carrier is not reported when 15 dB is asked for, a 20 dB one is
    Span s2(kFs); s2.sig = {{-50000, 12}, {100000, 20}};
    CarrierCfg c = cfgBase(); c.minSnrDb = 15;
    CarrierFinder g; g.configure(kFs, c);
    while (s2.t < 2.0 - 1e-9) { s2.block(x, (size_t)(kFs * 0.1)); g.feed(x.data(), x.size()); }
    weak = strong = false;
    for (const Carrier& q : g.carriers()) { if (std::fabs(q.hz + 50000) < 3000) weak = true; if (std::fabs(q.hz - 100000) < 3000) strong = true; }
    CHECK(!weak && strong, "minimum SNR 15 dB: weak %d strong %d", (int)weak, (int)strong);
}

// ---- the bank, with a fake receiver that counts what it is fed
struct FakeTel { uint64_t seq = 0; int state = 0; };
struct FakeRx {
    size_t fed = 0; uint64_t seq = 0;
    void configure(double) {}
    void feed(const cf32*, size_t n) { fed += n; seq++; }
    bool telemetry(FakeTel& t, uint64_t last) { if (seq == last) return false; t.seq = seq; t.state = 2; return true; }
};

void bank() {
    const double fs = 250000, out = 25000;
    WideOps<FakeRx, FakeTel> ops;
    ops.outRate = out; ops.occupiedHz = 10000; ops.preRollSec = 0.5;
    std::vector<size_t> firstFeed;   // samples each new receiver got right away (the pre-roll)
    WideBank<FakeRx, FakeTel> b(ops);
    CarrierCfg cfg = cfgBase(); cfg.maxCarriers = 8;
    b.configure(fs, cfg);
    b.setCenterHz(100e6);
    Span sp(fs);
    sp.sig = {{-75000, 30}, {-25000, 25}, {25000 - 2000, 20, 600}, {75000, 15, 0, 0, 0, 0, 1.0}};   // the last one starts at 1 s
    std::vector<cf32> x;
    auto run = [&](double secs) { const double end = sp.t + secs; while (sp.t < end - 1e-9) { sp.block(x, (size_t)(fs * 0.1)); b.feed(x.data(), x.size()); } };
    run(3.0);
    CHECK(b.liveCount() == 4, "bank: %zu carriers, expected 4", b.liveCount());
    // the late one (on since 1.0 s): its decoder got the signal from about 0.5 s before it was found
    FakeTel t; std::vector<WideCarrier> rows; uint64_t sq = 0; WideStatus st;
    b.snapshot(t, rows, 0, &sq, &st);
    int lateId = 0, weakId = 0, driftId = 0; double weakLvl = 1e9;
    for (const auto& r : rows) {
        if (std::fabs(r.hz - 75000) < 3000) lateId = r.id;
        if (std::fabs(r.hz - sp.trueHz(sp.sig[2])) < 3000) driftId = r.id;
        if (r.levelDb < weakLvl) { weakLvl = r.levelDb; weakId = r.id; }
        CHECK(r.ageSec > 0 && r.widthHz > 0, "row %d: age %.1f width %.0f", r.id, r.ageSec, r.widthHz);
    }
    CHECK(lateId > 0, "the late carrier has no row");
    // the drifting one: the mixer follows (within the follow step of 6 % of a raster step)
    const double mix = b.mixerHz(driftId);
    CHECK(driftId > 0 && std::fabs(mix - sp.trueHz(sp.sig[2])) < 0.1 * kStep, "the mixer of the drifting carrier is at %.0f Hz, the carrier at %.0f Hz", mix, sp.trueHz(sp.sig[2]));
    // recording one carrier
    const std::string dir = std::filesystem::temp_directory_path().string();
    const std::string path = b.startRecording(lateId, dir);
    CHECK(!path.empty(), "recording did not start");
    run(0.5);
    b.stopRecording(lateId);
    std::error_code ec;
    const auto sz = path.empty() ? 0 : std::filesystem::file_size(path, ec);
    CHECK(!ec && sz > 44 + 0.4 * 4 * kStep * 8, "recording: %llu bytes", (unsigned long long)sz);
    if (!path.empty()) std::filesystem::remove(path, ec);
    // a retune by 12.5 kHz: the same ids, the mixers moved with it
    std::vector<int> idsBefore = b.liveIds();
    std::map<int, double> mixBefore; for (int id : idsBefore) mixBefore[id] = b.mixerHz(id);
    b.setCenterHz(100e6 + 12500);
    for (Sig& s : sp.sig) s.hz -= 12500;
    run(1.0);
    std::vector<int> idsAfter = b.liveIds();
    CHECK(std::set<int>(idsBefore.begin(), idsBefore.end()) == std::set<int>(idsAfter.begin(), idsAfter.end()), "the retune changed the carriers (%zu before, %zu after)", idsBefore.size(), idsAfter.size());
    for (int id : idsAfter) if (id != driftId) CHECK(std::fabs(b.mixerHz(id) - (mixBefore[id] - 12500)) < 0.1 * kStep, "carrier %d's mixer at %.0f after the retune (was %.0f)", id, b.mixerHz(id), mixBefore[id]);
    // the CPU budget: a load of 90 % against a budget of 50 %: the weakest carrier goes (not the one listened to, which is the weakest here)
    b.setSelected(weakId);
    b.setCpuBudget(0.5);
    b.setLoadOverride(0.9);
    run(3.0);
    st = b.status();
    std::vector<int> ids = b.liveIds();
    const bool keptSel = std::find(ids.begin(), ids.end(), weakId) != ids.end();
    printf("bank: CPU budget 50 %%, load 90 %%: %zu carriers left, cap %d, too slow %d, dropped %llu\n", ids.size(), st.cap, (int)st.tooSlow, (unsigned long long)st.droppedForCpu);
    CHECK(st.tooSlow && ids.size() < 4 && ids.size() >= 2, "too slow: %d, %zu carriers", (int)st.tooSlow, ids.size());
    CHECK(keptSel, "the carrier listened to was dropped for the CPU budget");
    // the load falls: the carriers come back (one per 5 s)
    b.setLoadOverride(0.1);
    run(25.0);
    st = b.status();
    printf("bank: load 10 %%: %zu carriers, cap %d, too slow %d\n", b.liveCount(), st.cap, (int)st.tooSlow);
    CHECK(!st.tooSlow && b.liveCount() == 4, "after the load fell: too slow %d, %zu carriers", (int)st.tooSlow, b.liveCount());
}

void preRoll() {
    // a carrier that starts at 1.0 s: its receiver must have been fed from before the finder found it (the start of the transmission)
    const double fs = 250000, out = 25000;
    WideOps<FakeRx, FakeTel> ops; ops.outRate = out; ops.occupiedHz = 10000; ops.preRollSec = 0.5;
    WideBank<FakeRx, FakeTel> b(ops);
    b.configure(fs, cfgBase());
    Span sp(fs);
    sp.sig = {{50000, 25, 0, 0, 0, 0, 1.0}};
    std::vector<cf32> x;
    size_t fedAtStart = 0; bool seen = false;
    while (sp.t < 3.0 - 1e-9) {
        sp.block(x, (size_t)(fs * 0.1)); b.feed(x.data(), x.size());
        if (!seen && b.liveCount() == 1) {
            seen = true;
            size_t fed = 0; b.forEach([&](FakeRx& r) { fed = r.fed; });
            fedAtStart = fed;
            const double foundAfter = sp.t - 1.0;
            printf("pre-roll: found %.2f s after it started; its receiver got %.2f s of signal at once\n", foundAfter, fed / out);
            CHECK(fed / out >= foundAfter - 0.05 && fed / out >= 0.45, "the late carrier's receiver got %.2f s (found %.2f s after the start)", fed / out, foundAfter);
        }
    }
    CHECK(seen && fedAtStart > 0, "the late carrier was not found");
}

} // namespace

int main() {
    busySpan();
    noiseOnly();
    noPrior();
    bank();
    preRoll();
    return testSummary("wide_detector");
}
