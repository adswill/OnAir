// A simulated radio for the automatic-gain tests (tests/source/test_autogain.cpp): the profile's gain stages with their step tables, a noise
// figure that follows the stage split (Friis, a turned-down LNA lets the later stages' noise in), a front end with a finite IIP3 (third-order
// products of strong neighbours, compression near P1dB), an ADC that clips and quantises, gain changes that reach the samples only after the
// radio's settling time, and a decoder that locks above an SNR and reports SNR / MER. Levels: dBm at the antenna; the ADC's full scale is
// 0 dBm at the ADC input (so dBFS = dBm + gain - compression). Not a physical model of one radio: it has the properties the controller relies on.
#pragma once
#include "dect2/source/autogain.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace agsim {

using namespace dect2::ag;

struct Tone { double offHz; double dbm; };

struct SimRadio {
    RadioProfile prof;
    StagePlan live, pending;
    double pendingAt = -1;
    double delaySec = 0.2;           // a gain change reaches the samples after this
    // the scene
    double rateHz = 2.4e6, chanBw = 12.5e3, chanOff = 0;
    double sigDbm = -100;            // the wanted signal's power in its channel
    double sigCrestDb = 3;           // its peak over its mean (pulsed: ADS-B about 15 dB)
    double antNoiseDb = 0;           // the antenna's noise above kT (city noise on HF / VHF)
    std::vector<Tone> blockers;      // strong signals elsewhere in the span (and their third-order products)
    int bins = 512;
    double lockAt = 8;               // the decoder locks above this SNR
    bool decoder = true;             // the mode reports SNR / MER
    double jitterDb = 0.25;
    uint32_t seed = 4242;
    // what the test reads
    double lastClip = 0;
    // the gain the profile's numbers do not show: an RTL-SDR's fixed IF gain, the Airspy's table steps (about 2 dB each)
    double fixedDb = 0, unitDb = 1;

    explicit SimRadio(const RadioProfile& p, double total = -1, bool amp = false) : prof(p) {
        live = planStages(prof, total < 0 ? (p.minTotal(amp) + p.maxTotal(amp)) / 2 : total, Policy::Balanced, 0xffffffffu, {}, amp);
        delaySec = std::max(0.05, p.settleSec * 0.8);
        if (p.id.rfind("rtl", 0) == 0) fixedDb = 20;
        if (p.id == "airspy") { unitDb = 2; fixedDb = 8; }
        if (p.id == "spyserver") unitDb = 3;
    }
    void apply(const StagePlan& p, double now) { pending = p; pendingAt = now + delaySec; }
    void advance(double now) { if (pendingAt >= 0 && now >= pendingAt) { live = pending; pendingAt = -1; } }

    static double lin(double db) { return std::pow(10.0, db / 10.0); }
    static double dbv(double x) { return 10.0 * std::log10(std::max(x, 1e-30)); }
    double r01() { seed = seed * 1664525u + 1013904223u; return (double)(seed >> 8) / 16777216.0; }
    double jit() { return (r01() - 0.5) * 2.0 * jitterDb; }

    double gain() const { return live.totalDb; }
    // the sim's own Friis (no ADC term: the ADC's noise is added at the ADC), from the profile's stage figures
    double nfDb() const {
        double f = 1, g = 1;
        for (size_t i = 0; i < prof.stages.size() && i < live.db.size(); i++) {
            const GainStage& s = prof.stages[i];
            if (!s.settable && prof.driverSplits) continue;
            if (s.kind == StageKind::Amp && live.db[i] <= 0) continue;
            const double span = s.maxDb() - s.minDb();
            const double frac = span > 0 ? (s.maxDb() - live.db[i]) / span : 0;
            f += (lin(s.nfDb + s.nfLowGainDb * std::clamp(frac, 0.0, 1.0)) - 1) / g;
            g *= lin(live.db[i]);
        }
        return dbv(f);
    }
    double frontGain() const {
        double g = 0;
        for (size_t i = 0; i < prof.stages.size() && i < live.db.size(); i++) if (prof.stages[i].front && (prof.stages[i].settable || !prof.driverSplits)) g += live.db[i];
        return g;
    }
    double iip3() const { StagePlan p = live; cascade(prof, p); return p.iip3Dbm; }
    double totalInDbm() const {
        double p = lin(sigDbm);
        for (const auto& b : blockers) p += lin(b.dbm);
        return dbv(p);
    }
    // gain compression of the front end (dB), from the total input power against P1dB ~ IIP3 - 10
    double compression() const { return std::max(0.0, (totalInDbm() - (iip3() - 10)) * 0.6); }
    struct Im3 { double offHz, dbm; };
    std::vector<Im3> im3() const {   // input-referred third-order products of every pair of strong signals
        std::vector<Im3> v;
        const double ip = iip3();
        for (size_t a = 0; a < blockers.size(); a++)
            for (size_t b = 0; b < blockers.size(); b++) {
                if (a == b) continue;
                const double f = 2 * blockers[a].offHz - blockers[b].offHz;
                const double p = 2 * blockers[a].dbm + blockers[b].dbm - 2 * ip;
                v.push_back({f, p});
            }
        return v;
    }
    double noiseDbmHz() const { return -174 + nfDb() + antNoiseDb; }
    double quantPerBinDbfs() const { return -(6.02 * prof.adcBits + 1.76) - dbv((double)bins) + 1.76; }
    double outGain() const { return gain() * unitDb + fixedDb - compression(); }

    // in-channel SNR as the decoder sees it
    double snrDb() const {
        const double g = outGain();
        const double sig = lin(sigDbm + g);
        double n = lin(noiseDbmHz() + dbv(chanBw) + g);
        n += lin(-(6.02 * prof.adcBits + 1.76)) * chanBw / rateHz;   // quantisation noise in the channel
        for (const auto& p : im3()) if (std::fabs(p.offHz - chanOff) < chanBw) n += lin(p.dbm + g);
        n += lin(totalOutDbfs()) * lastClip * 30 * std::min(1.0, chanBw / rateHz);   // clipping spreads distortion over the whole span: this channel's share (3.5: was the whole of it)
        return dbv(sig / n);
    }
    double totalOutDbfs() const {
        const double g = outGain();
        double p = lin(sigDbm + g) + lin(noiseDbmHz() + dbv(rateHz) + g) + lin(-(6.02 * prof.adcBits + 1.76));
        for (const auto& b : blockers) p += lin(b.dbm + g);
        for (const auto& x : im3()) p += lin(x.dbm + g);
        return dbv(p);
    }
    BlockStats adc() {
        const double g = outGain();
        const double noiseP = lin(noiseDbmHz() + dbv(rateHz) + g) + lin(-(6.02 * prof.adcBits + 1.76));
        const double sigma = std::sqrt(noiseP / 2);
        double toneA = std::sqrt(lin(sigDbm + g + sigCrestDb));
        for (const auto& b : blockers) toneA += std::sqrt(lin(b.dbm + g)) * 0.8;
        BlockStats s;
        const double p999 = 3.3 * sigma + toneA;
        const double sigmaTot = std::sqrt(lin(totalOutDbfs()) / 2);
        double clip = std::erfc(1.0 / (sigmaTot * std::sqrt(2.0)));
        if (toneA > 1) clip = std::max(clip, std::min(0.5, (toneA - 1) / toneA * 0.4));
        s.clipFraction = std::min(1.0, clip);
        s.p999 = std::min(1.0, p999 * (1 + jit() * 0.02));
        s.peak = std::min(1.0, p999 * 1.12);
        if (s.clipFraction > 1e-3) s.peak = 1.0;
        s.rmsDbfs = std::min(0.0, totalOutDbfs() + jit());
        s.crestDb = 20 * std::log10(std::max(s.p999, 1e-9)) - s.rmsDbfs;
        s.headroomDb = -20 * std::log10(std::max(s.p999, 1e-9));
        lastClip = s.clipFraction;
        return s;
    }
    std::vector<float> spectrum() {
        std::vector<float> v((size_t)bins);
        const double g = outGain();
        const double binHz = rateHz / bins;
        const double floorBin = dbv(lin(noiseDbmHz() + dbv(binHz) + g) + lin(quantPerBinDbfs()));
        const int chB = std::max(1, (int)std::lround(chanBw / binHz));
        const int c0 = bins / 2 + (int)std::lround(chanOff / binHz);
        auto put = [&](double off, double dbfsTotal, int width) {
            const int c = bins / 2 + (int)std::lround(off / binHz);
            const double per = dbfsTotal - dbv(width) + 1.76;   // a Hann bin holds 1.5 times the power
            for (int k = c - width / 2; k <= c + (width - 1) / 2; k++) if (k >= 0 && k < bins) v[(size_t)k] = (float)dbv(lin(v[(size_t)k]) + lin(per));
        };
        for (int i = 0; i < bins; i++) v[(size_t)i] = (float)(floorBin + jit() * 2);
        (void)c0;
        put(chanOff, sigDbm + g, chB);
        for (const auto& b : blockers) put(b.offHz, b.dbm + g, 1);
        for (const auto& x : im3()) if (std::fabs(x.offHz) < rateHz / 2) put(x.offHz, x.dbm + g, 1);
        if (lastClip > 1e-4) for (auto& x : v) x = (float)dbv(lin(x) + lin(totalOutDbfs() - dbv(bins)) * lastClip * 30);
        return v;
    }
    Quality quality() {
        Quality q;
        const double s = snrDb() + jit();
        if (!decoder || s < lockAt) return q;
        q.locked = true;
        q.snrValid = true; q.snrDb = std::min(s, 38.0);
        q.merValid = true; q.merDb = std::min(s, 35.0);
        return q;
    }
};

// Runs a controller against the radio for `sec` seconds of simulated time at `dt` per tick; calls `each(now)` after every tick
template <class F>
void run(AutoGainController& c, SimRadio& r, double& now, double sec, F each, double dt = 0.05) {
    std::vector<float> spec;
    const double end = now + sec;
    for (; now < end; now += dt) {
        r.advance(now);
        AgInput in;
        in.now = now;
        in.adc = r.adc();
        spec = r.spectrum();
        in.spectrum = &spec;
        in.rateHz = r.rateHz; in.chanBwHz = r.chanBw; in.offsetHz = r.chanOff;
        in.q = r.quality();
        AgCommand out;
        if (c.tick(in, out)) r.apply(out.plan, now);
        each(now);
    }
}
inline void run(AutoGainController& c, SimRadio& r, double& now, double sec) { run(c, r, now, sec, [](double) {}); }

} // namespace agsim
