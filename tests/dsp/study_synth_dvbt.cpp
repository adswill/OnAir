// Phase study of synthetic wideband (docs/SYNTH-WIDEBAND.md section 6): a DVB-T channel received by 5 simulated narrow radios with realistic
// oscillators, stitched by SynthWideband, decoded by OnAir's own DVB-T receiver.
//
//   DVB-T generator (native rate Fa) -> per radio: LO (slice centre + ppm x 600 MHz + PLL phase noise) -> 128-tap decimation by 4 -> sample
//   clock (same crystal: same ppm, thermal drift) -> white phase floor -> receiver noise -> start delay -> SynthWideband (5 slices) -> DvbtReceiver
//
// Oscillators: Wiener phase (1/f^2, Lorentzian linewidth lw Hz) + white floor (dBc/Hz); TCXO: +-1 ppm, +-0.002 ppm/s; ordinary crystal:
// +-30 ppm, +-0.03 ppm/s (warming up). Shared clock: one reference for all (common ppm, drift and Wiener part); each PLL adds its own residual
// (Ornstein-Uhlenbeck, the VCO noise outside a 50 kHz loop) and a +-5 Hz fractional-N offset. A single wide radio with radio 0's oscillator is
// the reference. Simplifications: the LO phase is applied after the decimation filter (it moves slowly against the 14 us filter), no 8-bit
// quantisation, no adjacent channels.
//
// Default (ctest): one short case with a loose check. SYNTH_STUDY=1: the whole matrix (a few minutes on 2 threads). SYNTH_CASE=text: only the
// cases whose name contains the text.
#include "dect2/dsp/synth_wide.h"
#include "dect2/source/synth_group.h"
#include "dect2/dvbt/dvbt_gen.h"
#include "dect2/dvbt/dvbt_rx.h"
#include "dect2/t2/t2.h"
#include "../common/jobs.h"
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
using testjobs::jprintf;
static std::atomic<int> fails{0};
static const double kPi = 3.14159265358979323846;
static const double kRf = 600e6;

enum Clock { FreeTcxo, FreeXo, Shared };
static const char* clockName(Clock c) { return c == FreeTcxo ? "free TCXO" : c == FreeXo ? "free crystal" : "shared clock"; }

struct Case {
    double bw; int mod; Clock clock; bool track; double lw; bool single; double seconds; bool seg = false;   // seg: the DVB-T per-segment phase hook
    std::string name() const {
        static const char* mn[3] = {"QPSK", "16-QAM", "64-QAM"};
        char b[160];
        snprintf(b, sizeof b, "%g MHz %-6s %-12s lw %4.1f Hz %s", bw, mn[mod], clockName(clock), lw, single ? "single wide radio" : seg ? "stitched, tracking + DVB-T segments" : track ? "stitched, tracking" : "stitched, no tracking");
        return b;
    }
};

struct Result {
    bool tps = false; size_t good = 0, bad = 0, lost = 0; double mer = 0; uint64_t rsCorr = 0, rsFail = 0;
    double firstGood = -1; std::vector<synth::PairStatus> seams; int locked = 0;
};

// one simulated narrow radio
struct SimRadio {
    double fc = 0;                 // slice centre (Hz from the channel centre)
    double ppm0 = 0, drift = 0;    // its crystal: ppm at t = 0 and ppm per second
    double pllHz = 0;              // fractional-N residual
    double lw = 30, floorDbc = -100;
    bool ownWiener = true;         // false: the shared clock's common Wiener part comes from outside, plus an OU residual
    double ouA = 0, ouS = 0, ou = 0;
    double wien = 0, wienS = 0;
    double ph = 0;                 // LO phase accumulator (rad)
    std::vector<float> h; std::vector<cf32> hist; int hp = 0, dph = 0;   // hist: twice the filter length, so the window is one piece
    synth::FracResampler rs;
    std::vector<cf32> dec, out;
    long skip = 0;
    double sigma = 0, floorS = 0;
    std::mt19937 rng;
    std::normal_distribution<double> nd{0.0, 1.0};
    void init(double Fa, unsigned seed) {
        rng.seed(seed);
        // 128-tap Kaiser lowpass at fs/2 (Fa/8): flat to 0.42 fs
        const int T = 128; const double fcN = 0.125, beta = 8.0;
        auto i0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 40; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
        h.resize(T); double sum = 0;
        for (int i = 0; i < T; i++) {
            const double m = i - (T - 1) / 2.0, r = m / ((T - 1) / 2.0);
            const double sc = std::fabs(m) < 1e-12 ? 2 * fcN : std::sin(2 * kPi * fcN * m) / (kPi * m);
            h[i] = (float)(sc * i0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0(beta)); sum += h[i];
        }
        for (auto& v : h) v = (float)(v / sum);
        hist.assign(2 * T, cf32(0, 0));
        rs.configure(Fa / 4);
        wienS = std::sqrt(2 * kPi * lw / Fa);
        floorS = std::sqrt(std::pow(10.0, floorDbc / 10.0) * (Fa / 4));   // white phase, per output sample
    }
    // x: the air block at Fa; common: the shared Wiener phase per air sample (or null)
    void process(const cf32* x, size_t n, const double* common, double Fa, double t0) {
        dec.clear();
        const double ppm = ppm0 + drift * t0;
        const double f = fc + ppm * 1e-6 * kRf + pllHz;
        const double dphi = 2 * kPi * f / Fa;
        const int T = (int)h.size();
        for (size_t i = 0; i < n; i++) {
            hist[(size_t)hp] = hist[(size_t)(hp + T)] = x[i];
            if (++hp == T) hp = 0;
            ph += dphi;
            if (++dph == 4) {
                dph = 0;
                float re = 0, im = 0;
                const cf32* w = &hist[(size_t)hp];   // the T newest samples, oldest first (the filter is symmetric)
                for (int k = 0; k < T; k++) { re += w[k].real() * h[k]; im += w[k].imag() * h[k]; }
                // the LO: slice centre + error, Wiener phase (own or shared + OU residual)
                double th = ph;
                if (ownWiener) th += wien; else th += common[i] + ou;
                dec.push_back(cf32(re, im) * std::polar(1.0f, (float)-std::remainder(th, 2 * kPi)));
            }
            if (ownWiener) wien += wienS * nd(rng);
            else { ou = ou * (1 - ouA) + ouS * nd(rng); }
        }
        ph = std::remainder(ph, 2 * kPi);
        // the radio's sample clock runs fast by ppm: it reads the signal at 1/(1+ppm) per sample
        rs.set(-ppm, 0);
        out.clear();
        rs.process(dec.data(), dec.size(), out);
        for (auto& v : out) {
            const double e = floorS * nd(rng);
            v *= cf32((float)(1 - 0.5 * e * e), (float)e);
            v += cf32((float)(sigma * nd(rng)), (float)(sigma * nd(rng)));
        }
        if (skip > 0) { const long k = std::min<long>(skip, (long)out.size()); out.erase(out.begin(), out.begin() + k); skip -= k; }
    }
};

static Result runCase(const Case& c, unsigned seed) {
    Result R;
    const double Fa = nativeRateHz(c.bw), fs = Fa / 4;
    dvbt::Params p; p.mode = dvbt::k8K; p.guard = dvbt::kGi8; p.mod = c.mod; p.crHp = p.crLp = dvbt::kR23;
    uint32_t counter = 0;
    dvbt::Generator gen(p, [&](uint8_t* pkt) {
        pkt[0] = 0x47; pkt[1] = 0x01; pkt[2] = 0x00; pkt[3] = 0x10;
        memcpy(pkt + 4, &counter, 4);
        for (int i = 8; i < 188; i++) pkt[i] = (uint8_t)(counter * 31 + i * 7);
        counter++;
    }, seed);
    DvbtReceiver rx;
    rx.configure(Fa, c.bw);
    size_t fed = 0;
    const double settle = c.seconds * 0.45;
    uint32_t last = 0;
    rx.setPacketCallback([&](const uint8_t* pk, size_t n, double) {
        for (size_t i = 0; i < n; i++) {
            const uint8_t* q = pk + i * 188;
            const bool counted = (double)fed / Fa > settle;
            bool ok = !(q[1] & 0x80) && q[0] == 0x47;
            uint32_t cc = 0; memcpy(&cc, q + 4, 4);
            for (int j = 8; j < 188 && ok; j++) ok = q[j] == (uint8_t)(cc * 31 + j * 7);
            if (ok) { if (R.firstGood < 0) R.firstGood = (double)fed / Fa; if (counted && R.good && cc > last + 1) R.lost += cc - last - 1; last = cc; if (counted) R.good++; }
            else if (counted) R.bad++;
        }
    });
    std::mt19937 rng(seed * 7 + 3);
    std::uniform_real_distribution<double> U(-1, 1);
    // signal power (the generator's output level) for the noise
    std::vector<cf32> sym, air;
    gen.nextSymbol(sym);
    double pw = 0; for (auto& v : sym) pw += std::norm(v); pw /= sym.size();
    const double occ = 7.61e6 * c.bw / 8.0, snrDb = 30;
    const double n0 = pw / occ / std::pow(10.0, snrDb / 10.0);   // per Hz
    air = sym;
    // the slices
    const SlicePlan plan = planSlices(5, fs, occ + 0.2e6 * c.bw / 8.0);
    if (!plan.ok) { jprintf("plan failed: %s\n", plan.why.c_str()); fails++; return R; }
    synth::SynthCfg cfg = plan.cfg;
    cfg.trackPhase = c.track;
    cfg.trackLinewidthHz = std::max(2.0, 2 * c.lw);
    synth::SynthWideband sw;
    if (!sw.configure(cfg) || std::fabs(sw.outRate() - Fa) > 1) { jprintf("stitcher rate %.0f vs %.0f\n", sw.outRate(), Fa); fails++; return R; }
    if (c.seg) {
        std::vector<double> seams;
        for (int i = 0; i + 1 < 5; i++) seams.push_back(0.5 * (plan.sliceOffsetHz[(size_t)i] + plan.sliceOffsetHz[(size_t)i + 1]));
        rx.setPhaseSegments(seams, plan.overlapHz);
    }
    const int S = c.single ? 1 : 5;
    std::vector<SimRadio> rad((size_t)S);
    const double commonPpm = (c.clock == FreeXo ? 30.0 : 1.0) * U(rng), commonDrift = (c.clock == FreeXo ? 0.03 : 0.002) * U(rng);
    for (int i = 0; i < S; i++) {
        SimRadio& r = rad[(size_t)i];
        r.fc = c.single ? 0 : plan.sliceOffsetHz[(size_t)i];
        r.lw = c.lw; r.floorDbc = -100;
        if (c.clock == Shared) {
            r.ppm0 = commonPpm; r.drift = commonDrift; r.pllHz = 5 * U(rng); r.ownWiener = false;
            r.ouA = 2 * kPi * 50e3 / Fa; r.ouS = std::sqrt(2 * kPi * c.lw / Fa);
        } else {
            const double sp = c.clock == FreeXo ? 30.0 : 1.0, dr = c.clock == FreeXo ? 0.03 : 0.002;
            r.ppm0 = sp * U(rng); r.drift = dr * U(rng);
        }
        r.init(Fa, seed * 31 + i);
        r.sigma = std::sqrt(n0 * fs / 2);
        r.skip = c.single ? 0 : (long)(150 * (U(rng) + 1));
        if (c.single) {   // the wide radio itself: no decimation; handled below
            r.sigma = std::sqrt(n0 * Fa / 2);
        }
    }
    std::normal_distribution<double> nd(0, 1);
    std::vector<double> common;
    double cw = 0;
    const double cwS = std::sqrt(2 * kPi * c.lw / Fa);
    const size_t total = (size_t)(c.seconds * Fa), B = 65536;
    std::vector<cf32> wide;
    size_t made = 0;
    double wph = 0, wwien = 0;
    while (made < total) {
        while (air.size() < B) { gen.nextSymbol(sym); air.insert(air.end(), sym.begin(), sym.end()); }
        const double t0 = (double)made / Fa;
        if (c.single) {
            SimRadio& r = rad[0];
            const double f = (r.ppm0 + r.drift * t0) * 1e-6 * kRf + r.pllHz;
            wide.resize(B);
            for (size_t i = 0; i < B; i++) {
                wph += 2 * kPi * f / Fa;
                wwien += (c.clock == Shared ? cwS : r.wienS) * nd(rng);
                const double e = r.floorS * 2 * nd(rng);
                wide[i] = air[i] * std::polar(1.0f, (float)-std::remainder(wph + wwien + e, 2 * kPi)) +
                          cf32((float)(r.sigma * nd(rng)), (float)(r.sigma * nd(rng)));
            }
            wph = std::remainder(wph, 2 * kPi);
        } else {
            common.resize(B);
            for (size_t i = 0; i < B; i++) { common[i] = cw; cw += cwS * nd(rng); }
            for (int i = 0; i < S; i++) {
                rad[(size_t)i].process(air.data(), B, common.data(), Fa, t0);
                sw.push(i, rad[(size_t)i].out.data(), rad[(size_t)i].out.size());
            }
            wide.clear();
            sw.pull(wide);
        }
        air.erase(air.begin(), air.begin() + (long)B);
        made += B;
        fed += wide.size();
        if (!wide.empty()) rx.feed(wide.data(), wide.size());
    }
    RxTelemetry t;
    rx.telemetry(t, 0);
    R.tps = t.dvbt.tpsOk; R.mer = t.dataSnrDb; R.rsCorr = t.dvbt.rsCorrected; R.rsFail = t.dvbt.rsFailed;
    if (!c.single) for (int p2 = 0; p2 < 4; p2++) { R.seams.push_back(sw.pair(p2)); }
    if (!c.single) for (int i = 1; i < 5; i++) R.locked += sw.slice(i).locked ? 1 : 0;
    return R;
}

int main() {
    const bool full = getenv("SYNTH_STUDY") && atoi(getenv("SYNTH_STUDY")) > 0;
    const char* only = getenv("SYNTH_CASE");
    std::vector<Case> cases;
    if (!full) {
        cases.push_back({8, dvbt::kQpsk, FreeTcxo, true, 30, false, 1.6});
    } else {
        const double secs = 2.4;
        for (double bw : {8.0, 7.0, 6.0})
            for (int mod : {dvbt::kQpsk, dvbt::k16Qam, dvbt::k64Qam}) {
                for (Clock ck : {FreeTcxo, Shared})
                    for (bool tr : {false, true}) cases.push_back({bw, mod, ck, tr, 30, false, secs});
                if (bw == 8) cases.push_back({bw, mod, FreeTcxo, true, 30, true, secs});
                if (bw == 8) { cases.push_back({bw, mod, FreeTcxo, true, 30, false, secs, true}); cases.push_back({bw, mod, Shared, true, 30, false, secs, true}); }
            }
        for (double lw : {1.0, 3.0, 10.0})
            for (bool tr : {false, true}) cases.push_back({8, dvbt::k64Qam, FreeTcxo, tr, lw, false, secs});
        cases.push_back({8, dvbt::k16Qam, FreeXo, true, 30, false, secs});
        cases.push_back({8, dvbt::k64Qam, FreeXo, true, 30, false, secs});
    }
    testjobs::Jobs jobs;
    for (size_t k = 0; k < cases.size(); k++) {
        const Case c = cases[k];
        if (only && c.name().find(only) == std::string::npos) continue;
        jobs.add([c, k] {
            const Result r = runCase(c, 101 + (unsigned)k);
            const double per = r.good + r.bad ? 100.0 * (double)r.bad / (double)(r.good + r.bad) : 100.0;
            std::string seams;
            for (const auto& s : r.seams) { char b[80]; snprintf(b, sizeof b, " %.0f/%.1f", s.mismatchDb, s.trackRmsDeg); seams += b; }
            jprintf("%-62s tps %d  good %6zu bad %5zu (PER %5.1f %%) lost %4zu  MER %5.1f dB  RS corr %6llu fail %5llu  first %.2fs  locked %d  seams dB/deg%s\n",
                    c.name().c_str(), (int)r.tps, r.good, r.bad, per, r.lost, r.mer, (unsigned long long)r.rsCorr, (unsigned long long)r.rsFail, r.firstGood, r.locked, seams.c_str());
            if (!getenv("SYNTH_STUDY")) {
                if (!(r.tps && r.good > 500 && per < 5.0)) { jprintf("FAIL: the short case did not decode\n"); fails++; }
            }
        });
    }
    jobs.run(2);
    jprintf(fails ? "synth_dvbt study FAILED\n" : "synth_dvbt study done\n");
    return fails ? 1 : 0;
}
