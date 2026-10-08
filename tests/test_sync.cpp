// Sync test: generator -> impairments -> T2Receiver. Verifies P1/S1/S2, GI, CFO and frame length detection.
#include "dect2/t2gen.h"
#include "dect2/t2rx.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <algorithm>
#include <cstdint>
#include <vector>

using namespace dect2;

struct Case { int s2; int gi; bool ext; double snr, cfo, echoDb; int echoDelay; double sroPpm; int frames; };

static bool run(const Case& c, bool verbose) {
    TxParams tp; tp.s2field1 = c.s2; tp.giIdx = c.gi; tp.ext = c.ext; tp.s1 = 0;
    T2Generator gen(tp);
    const double fn = nativeRateHz(8);
    T2Receiver rx;
    rx.configure(fn, 8);
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.f, 1.f);
    double nsig = std::pow(10.0, -c.snr / 20.0) / std::sqrt(2.0);
    double ph = 0, dph = 2 * M_PI * c.cfo / fn;
    std::vector<cf32> frame, stream;
    for (int f = 0; f < c.frames; f++) {
        gen.nextFrame(frame);
        stream.insert(stream.end(), frame.begin(), frame.end());
    }
    // echo
    std::vector<cf32> y(stream.size());
    float ea = (float)std::pow(10.0, -c.echoDb / 20.0);
    for (size_t i = 0; i < stream.size(); i++) {
        cf32 v = stream[i];
        if (c.echoDelay > 0 && i >= (size_t)c.echoDelay && c.echoDb > 0) v += ea * stream[i - c.echoDelay];
        y[i] = v;
    }
    // sample-rate offset via linear interpolation (adequate for a sync test)
    std::vector<cf32> z;
    {
        double step = 1.0 + c.sroPpm * 1e-6, pos = 0;
        while (pos + 1 < (double)y.size()) {
            size_t i0 = (size_t)pos; double fr = pos - i0;
            z.push_back(y[i0] * (float)(1 - fr) + y[i0 + 1] * (float)fr);
            pos += step;
        }
    }
    for (auto& v : z) {
        v *= cf32((float)std::cos(ph), (float)std::sin(ph));
        ph += dph;
        v += cf32(nd(rng), nd(rng)) * (float)nsig;
    }
    RxTelemetry t;
    uint64_t seq = 0;
    for (size_t i = 0; i < z.size(); i += 65536) {
        rx.feed(z.data() + i, std::min<size_t>(65536, z.size() - i));
        if (rx.telemetry(t, seq)) seq = t.seq;
    }
    rx.telemetry(t, 0);
    const FftMode* fm = fftModeFromS2(c.s2);
    // phase 2: channel estimate from the P2 pilots
    bool chOk = t.chValid && t.extCarriers == c.ext;
    if (chOk && fm->nP2 >= 2 && c.snr < 100 && c.sroPpm == 0) chOk = std::fabs(t.p2SnrDb - c.snr) < 3.5 || c.echoDb > 0;
    if (chOk && c.echoDb > 0 && c.echoDelay > 0) {
        int main = 0;
        for (size_t i = 0; i < t.irDb.size(); i++) if (t.irDb[i] > t.irDb[main]) main = (int)i;
        int best = -1;
        for (size_t i = 0; i < t.irDb.size(); i++) {
            if (std::abs((int)i - main) < 4) continue;
            if (best < 0 || t.irDb[i] > t.irDb[best]) best = (int)i;
        }
        chOk = best >= 0 && std::abs((best - main) - c.echoDelay) <= 2 && std::fabs(t.irDb[best] + c.echoDb) < 3.0;
        if (!chOk && verbose) printf("  echo check: main %d best %d (delta %d, want %d) level %.1f want %.1f\n", main, best, best - main, c.echoDelay, best >= 0 ? t.irDb[best] : 0.f, -c.echoDb);
    }
    // phase 2: L1 signalling
    const L1Pre& gp = gen.l1pre();
    bool l1Ok = t.l1preOk && t.l1pre.numDataSyms == gp.numDataSyms && t.l1pre.guardInterval == c.gi && t.l1pre.pilotPattern == gen.params().pp &&
                t.l1pre.bwtExt == (c.ext ? 1 : 0) && t.l1pre.cellId == gp.cellId && t.l1pre.s2 == gp.s2 && t.l1pre.postScrambled == gp.postScrambled;
    bool postOk = t.l1postOk && t.l1post.numPlp == 1 && t.l1post.plps.size() == 1 && t.l1post.plps[0].mod == 2 && t.l1post.rf[0].freq == 522000000u;
    if (!l1Ok || !postOk) { if (verbose || true) printf("  L1 check: pre=%d post=%d (pre fail %llu, post fail %llu) numData %d/%d\n", t.l1preOk, t.l1postOk, (unsigned long long)t.l1preBad, (unsigned long long)t.l1postBad, t.l1pre.numDataSyms, gp.numDataSyms); }
    bool dataOk = t.dataValid && t.dataPp == gen.params().pp;
    if (dataOk && c.sroPpm == 0 && c.snr < 100) dataOk = std::fabs(t.dataSnrDb - c.snr) < (c.s2 == 3 ? 6.0 : 3.5);
    if (!dataOk && verbose) printf("  data stage: valid=%d snr %.1f (want %.1f) pp %d\n", t.dataValid, t.dataSnrDb, c.snr, t.dataPp);
    chOk = chOk && l1Ok && postOk && dataOk;
    bool ok = chOk && t.p1.valid && t.p1.s1 == 0 && t.p1.s2field1 == c.s2 && t.state == 2 && t.giIdx == c.gi &&
              std::fabs(t.cfoHz - c.cfo) < 0.05 * fn / fm->n && t.fftN == fm->n;
    if (verbose || !ok)
        printf("%s %-3s gi %-6s snr %4.1f cfo %+8.1f echo %.0fdB/%d sro %+.0f | p1=%d s1=%d s2=%d conf %.2f  state %d gi=%s margin %.2f  cfo %+8.1f  cp %.2f(%.1f dB) frame %.1f ms syms %d sro %.0f  nsym %llu | ch=%d ext=%d p2snr %.1f data %.1f\n",
               ok ? "PASS" : "FAIL", fm->name, guardName(c.gi), c.snr, c.cfo, c.echoDb, c.echoDelay, c.sroPpm, t.p1.valid, t.p1.s1,
               t.p1.s2field1, t.p1.conf, t.state, t.giIdx >= 0 ? guardName(t.giIdx) : "-", t.giMargin, t.cfoHz, t.cpCorr, t.cpSnrDb,
               t.frameMs, t.symbolsPerFrame, t.sroPpm, (unsigned long long)t.symbols, t.chValid, t.extCarriers, t.p2SnrDb, t.dataSnrDb);
    return ok;
}

// No P1 present: pure noise, and an OFDM signal with the P1 blanked out. Must not claim a lock.
static bool runNegative(bool blankP1, bool verbose) {
    TxParams tp; tp.s2field1 = 1; tp.giIdx = 2;
    T2Generator gen(tp);
    T2Receiver rx;
    const double fn = nativeRateHz(8);
    rx.configure(fn, 8);
    std::mt19937 rng(11);
    std::normal_distribution<float> nd(0.f, 0.05f);
    std::vector<cf32> frame, z;
    for (int f = 0; f < 6; f++) {
        gen.nextFrame(frame);
        if (blankP1) { for (int i = 0; i < kP1Len; i++) frame[i] = 0; }
        else for (auto& v : frame) v = 0;
        z.insert(z.end(), frame.begin(), frame.end());
    }
    for (auto& v : z) v += cf32(nd(rng), nd(rng));
    for (size_t i = 0; i < z.size(); i += 65536) rx.feed(z.data() + i, std::min<size_t>(65536, z.size() - i));
    RxTelemetry t;
    rx.telemetry(t, 0);
    bool ok = t.p1Count == 0 && t.state == 0;
    if (verbose || !ok) printf("%s negative(%s): p1Count=%llu state=%d\n", ok ? "PASS" : "FAIL", blankP1 ? "OFDM, no P1" : "noise", (unsigned long long)t.p1Count, t.state);
    return ok;
}

// The P1 search on its own: a signal with short frames (many P1s), a clock error, a frequency error and noise, fed in 64K chunks; every
// P1 the receiver accepts is recorded (count and position).
struct P1Run {
    std::vector<std::pair<uint64_t, uint64_t>> seen;
    uint64_t evaluated = 0, rescans = 0, samples = 0;
    uint64_t evaluatedLocked = 0, samplesLocked = 0;   // from the third P1 on (the frame length is known by then)
    int state = 0;
    double frameMs = 0;
    float minMetric = 99;
};

// DECT2_NOP1GATE: the receiver searches everything with the exact P1 metric
static void setGateOff(bool on) {
#ifdef _WIN32
    _putenv_s("DECT2_NOP1GATE", on ? "1" : "");   // an empty value removes it
#else
    if (on) setenv("DECT2_NOP1GATE", "1", 1); else unsetenv("DECT2_NOP1GATE");
#endif
}

static P1Run runP1(int s2, int dataSyms, double snr, double cfo, double sroPpm, int frames, bool gateOff) {
    TxParams tp; tp.s2field1 = s2; tp.giIdx = 2; tp.dataSymbols = dataSyms;
    T2Generator gen(tp);
    const double fn = nativeRateHz(8);
    std::vector<cf32> frame, stream;
    for (int f = 0; f < frames; f++) { gen.nextFrame(frame); stream.insert(stream.end(), frame.begin(), frame.end()); }
    std::vector<cf32> z;
    for (double step = 1.0 + sroPpm * 1e-6, pos = 0; pos + 1 < (double)stream.size(); pos += step) {
        size_t i0 = (size_t)pos; double fr = pos - i0;
        z.push_back(stream[i0] * (float)(1 - fr) + stream[i0 + 1] * (float)fr);
    }
    std::mt19937 rng(23);
    std::normal_distribution<float> nd(0.f, 1.f);
    const double nsig = std::pow(10.0, -snr / 20.0) / std::sqrt(2.0), dph = 2 * M_PI * cfo / fn;
    double ph = 0;
    for (auto& v : z) { v = v * cf32((float)std::cos(ph), (float)std::sin(ph)) + cf32(nd(rng), nd(rng)) * (float)nsig; ph += dph; }
    setGateOff(gateOff);   // the receiver reads it when it is configured
    T2Receiver rx;
    rx.configure(fn, 8);
    setGateOff(false);
    P1Run r;
    RxTelemetry t;
    uint64_t last = 0;
    for (size_t i = 0; i < z.size(); i += 65536) {
        rx.feed(z.data() + i, std::min<size_t>(65536, z.size() - i));
        rx.telemetry(t, 0);
        if (t.p1Count != last) {
            last = t.p1Count; r.seen.emplace_back(t.p1Count, t.p1.pos); r.minMetric = std::min(r.minMetric, t.p1.metric);
            if (t.p1Count == 3) { r.evaluatedLocked = t.p1Evaluated; r.samplesLocked = i; }
        }
    }
    r.evaluated = t.p1Evaluated; r.rescans = t.p1Rescans; r.samples = z.size(); r.state = t.state; r.frameMs = t.frameMs;
    r.evaluatedLocked = r.evaluated - r.evaluatedLocked; r.samplesLocked = r.samples - r.samplesLocked;
    return r;
}

// The cheap first stage of the search must not lose a P1 the exact metric finds: the same P1s at the same positions with and without it,
// from a clean signal down to where P1s get too weak to pass the metric threshold.
static bool runP1Gate(int s2, int dataSyms, double snr, double cfo, double sroPpm, bool verbose) {
    P1Run a = runP1(s2, dataSyms, snr, cfo, sroPpm, 30, false), b = runP1(s2, dataSyms, snr, cfo, sroPpm, 30, true);
    bool ok = a.seen == b.seen;
    if (verbose || !ok)
        printf("%s P1 gate %s snr %4.1f cfo %+6.0f sro %+4.0f: P1s %zu/%zu (exact search %zu), weakest metric %.2f, exact metric at %.1f%%/%.1f%% of positions\n",
               ok ? "PASS" : "FAIL", fftModeFromS2(s2)->name, snr, cfo, sroPpm, a.seen.size(), (size_t)30, b.seen.size(), b.minMetric,
               100.0 * a.evaluated / a.samples, 100.0 * b.evaluated / b.samples);
    return ok;
}

// Once locked, P1 is searched for only around where the frame cadence puts it: no expected P1 may be missed (each miss would mean a search of
// everything since the last P1). With the cheap first stage the exact metric runs over a small part of the samples from the start; without
// it, once the frame length is known (the clock error is within the 80 ppm the frame length check allows).
static bool runP1Track(double sroPpm, bool gateOff, bool verbose) {
    const int frames = 24;
    P1Run r = runP1(1, 0, 20, -5000, sroPpm, frames, gateOff);
    const double frac = (double)r.evaluated / r.samples, fracLocked = r.samplesLocked ? (double)r.evaluatedLocked / r.samplesLocked : 1;
    bool ok = r.state == 2 && r.frameMs > 0 && r.seen.size() + 1 >= (size_t)frames && r.rescans == 0 && (gateOff ? fracLocked < 0.10 : frac < 0.02);
    if (verbose || !ok)
        printf("%s P1 tracking sro %+4.0f%s: state %d, frame %.2f ms, P1s %zu/%d, searched again %llu times, exact metric at %.2f%% of positions (%.2f%% once locked)\n",
               ok ? "PASS" : "FAIL", sroPpm, gateOff ? " (exact search only)" : "", r.state, r.frameMs, r.seen.size(), frames, (unsigned long long)r.rescans, 100 * frac, 100 * fracLocked);
    return ok;
}

int main(int argc, char** argv) {
    bool verbose = argc > 1;
    int fail = 0, total = 0;
    auto go = [&](Case c) { total++; if (!run(c, verbose)) fail++; };
    // every FFT size at a typical setting, clean
    for (int s2 : {0, 1, 2, 3, 4, 5}) go({s2, 2, false, 30, 0, 0, 0, 0, 4});
    // every guard interval at 8K
    for (int gi = 0; gi < 7; gi++) go({1, gi, false, 25, 2300, 0, 0, 0, 4});
    // CFO sweep (HackRF can be tens of kHz off)
    for (double cfo : {-14000., -9000., -4000., 6000., 12000., 20000.}) go({1, 2, false, 25, cfo, 0, 0, 0, 4});
    // noise and echo, clock error
    go({1, 2, false, 12, 3000, 0, 0, 0, 4});
    go({1, 2, false, 8, 3000, 0, 0, 0, 5});
    go({1, 2, false, 25, 3000, 8, 300, 0, 4});
    go({1, 2, false, 25, 3000, 0, 0, 20, 5});
    go({1, 2, true, 25, 3000, 0, 0, 0, 4});
    total += 2; fail += !runNegative(false, verbose); fail += !runNegative(true, verbose);
    // the P1 search: the cheap first stage against the exact metric everywhere, and the windowed search once locked
    for (double snr : {10., 0., -2., -3., -4.}) { total++; fail += !runP1Gate(1, 6, snr, 7000, 60, verbose); }
    total++; fail += !runP1Gate(3, 20, -3, -12000, -80, verbose);
    total++; fail += !runP1Gate(5, 3, -2, 2500, 30, verbose);
    for (double sro : {75., -75.}) for (bool gateOff : {false, true}) { total++; fail += !runP1Track(sro, gateOff, verbose); }
    printf("%d/%d passed\n", total - fail, total);
    return fail ? 1 : 0;
}
