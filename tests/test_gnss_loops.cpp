// The loops on their own: (A) acquisition of a single satellite of the simulated sky at several C/N0 values, many noise seeds each: how often it is found,
// how well Doppler and code phase come out, and the false alarm count on satellites that are not there; (B) a lone C/A signal made in this file (Doppler, a
// Doppler rate, random data bits, noise) through one tracking channel: lock time, Doppler and code phase accuracy, C/N0 estimate; (C) noise only, 40 s
// through the whole receiver: no satellite may be locked.
#include "dect2/gnss_acq.h"
#include "dect2/gnss_codes.h"
#include "dect2/gnss_front.h"
#include "dect2/gnss_sim.h"
#include "dect2/gnss_track.h"
#include "dect2/gnss_rx.h"
#include "dect2/gen_util.h"
#include "dect2/test_parallel.h"
#include <cmath>
#include <cstdio>
#include <iterator>
#include <random>
#include <vector>
using namespace dect2;
using dect2::testpar::CaseOut;
// every case runs on its own thread and prints through `out`, so that the log keeps the order of the cases
#define CHECK(c, ...) do { if (!(c)) out.fail(__LINE__, __VA_ARGS__); } while (0)

static GnssAcqConfig acqConfig(const std::vector<int>& prns) {
    GnssAcqConfig ac;
    ac.fftLog2 = 12; ac.blocks = 16; ac.qMin = -8; ac.qMax = 7; ac.prns = prns;
    ac.replica = [](int prn, cf32* out, int n) {
        uint8_t c[kGpsCaLen];
        if (!gpsCaChips(prn, c)) return false;
        for (int i = 0; i < n; i++) out[i] = cf32(c[(int)(((int64_t)i * 2 + 1) * kGpsCaLen / (2 * (int64_t)n))] ? -1.f : 1.f, 0.f);
        return true;
    };
    return ac;
}

static const double levelsA[] = {40, 36, 34, 32, 30};
static const double levelsB[] = {44, 38, 32};

static void partA(double lvl, bool first, CaseOut& out) {
    const int trials = 20;
    if (first) out.print("A. acquisition of one satellite, %d noise seeds per level, 16 ms of signal\n", trials);
    {
        int found = 0, dopOk = 0, phaseOk = 0, falseHits = 0, falseSearches = 0;
        double dopSum2 = 0, phSum2 = 0;
        double cnTrue = 0;
        for (int k = 0; k < trials; k++) {
            GnssSimConfig cfg;
            cfg.cn0Top = lvl + 0.3; cfg.maxSats = 1; cfg.noiseSeed = (unsigned)(100 + k); cfg.warmStart = true;
            GnssSim sim(cfg, 4e6);
            size_t si = 0;
            for (size_t i = 0; i < sim.sats().size(); i++) if (sim.sats()[i].transmitted) si = i;
            const GnssSimSat& sat = sim.sats()[si];
            cnTrue = sat.cn0;
            GnssBand band;
            band.init(4e6, 0, 4.096e6, 1 << 18);
            std::vector<cf32> x(65536);
            sim.generate(x.data(), x.size());
            for (auto& v : x) v = cf32(std::round(v.real() * 127.f) / 127.f, std::round(v.imag() * 127.f) / 127.f);
            band.process(x.data(), x.size());
            // the satellite and four that are not in the signal
            std::vector<int> prns = {sat.prn};
            for (int p = 1; p <= 32 && prns.size() < 5; p++) if (!sim.sats()[(size_t)p - 1].transmitted && p != sat.prn) prns.push_back(p);
            GnssAcq acq;
            acq.init(acqConfig(prns));
            std::vector<GnssAcqHit> hits;
            acq.work(band, 100000, [](int) { return false; }, hits);
            falseSearches += 4;
            for (auto& h : hits) {
                if (h.prn != sat.prn) { falseHits++; if (lvl == 40) out.print("      false hit: PRN %d (signal PRN %d) seed %d: Doppler %.0f, ratio %.1f (threshold %.2f)\n", h.prn, sat.prn, k, h.dopplerHz, h.ratio, acq.threshold()); continue; }
                found++;
                // truth: the Doppler from the delay's rate, and the code epoch position inside the segment
                const double t = sim.startTow() + 0.0 + (double)h.segStart / 4.096e6;
                const double d0 = sim.geometricDelay(si, t), d1 = sim.geometricDelay(si, t + 0.5);
                const double trueDop = -(d1 - d0) / 0.5 * 1575.42e6;
                const double sv = sim.svTransmitTime(si, t);
                const double u = std::fmod(sv, 1e-3);
                double tau = (1e-3 - u) * 4.096e6;
                double dph = std::fmod(h.codePhase - tau + 2048.0 + 4096.0 * 10, 4096.0) - 2048.0;
                const double dd = h.dopplerHz - trueDop;
                if (std::fabs(dd) < 150) dopOk++;
                if (std::fabs(dph) < 1.5) phaseOk++;
                dopSum2 += dd * dd; phSum2 += dph * dph;
            }
        }
        out.print("   C/N0 %.1f dB-Hz: found %2d of %d (%3.0f%%), Doppler within 150 Hz %d, code phase within 1.5 samples %d, rms errors %.0f Hz and %.2f samples; %d false hits in %d searches of absent satellites\n", cnTrue, found, trials,
               100.0 * found / trials, dopOk, phaseOk, found ? std::sqrt(dopSum2 / found) : 0.0, found ? std::sqrt(phSum2 / found) : 0.0, falseHits, falseSearches);
        if (lvl >= 40) CHECK(found >= trials * 0.9, "only %d of %d found at %.1f dB-Hz", found, trials, cnTrue);
        if (lvl >= 36) CHECK(found >= trials * 0.6, "only %d of %d found at %.1f dB-Hz", found, trials, cnTrue);
        CHECK(dopOk == found && phaseOk == found, "wrong Doppler or code phase in %d hits", found - std::min(dopOk, phaseOk));
        CHECK(falseHits <= 1, "%d false hits in %d searches", falseHits, falseSearches);
    }
}

// a lone C/A signal made here (not by the simulator): Doppler with a rate, random data bits at 50 bit/s, white noise
struct Lone {
    int prn; double cn0, dop, rate; double fs = 4.096e6; uint8_t chips[1023]; std::mt19937 rng; std::vector<int> bits;
    Lone(int p, double c, double d, double r, unsigned seed) : prn(p), cn0(c), dop(d), rate(r), rng(seed) { gpsCaChips(p, chips); }
    // the code phase in chips at time t, and the carrier phase in cycles
    double codePhase(double t) const { return 1.023e6 * t + (dop * t + 0.5 * rate * t * t) / 1540.0; }
    double carrier(double t) const { return dop * t + 0.5 * rate * t * t; }
    void make(std::vector<cf32>& x, size_t n) {
        genutil::PortableNormal nd((float)std::sqrt(0.5));
        const double A = std::sqrt(std::pow(10.0, cn0 / 10.0) / fs);
        x.resize(n);
        for (size_t i = 0; i < n; i++) {
            const double t = (double)i / fs;
            const double cp = codePhase(t);
            const int64_t ci = (int64_t)std::floor(cp);
            const size_t bi = (size_t)(ci / 20460);
            while (bits.size() <= bi) bits.push_back((int)(rng() & 1));
            const double s = (chips[ci % 1023] ? -1.0 : 1.0) * (bits[bi] ? -1.0 : 1.0);
            const double ph = 2 * M_PI * carrier(t);
            x[i] = cf32((float)(A * s * std::cos(ph)) + nd(rng), (float)(A * s * std::sin(ph)) + nd(rng));
        }
    }
};

static void partB(double cn0, bool first, CaseOut& out) {
    if (first) out.print("B. one tracking channel on a lone signal (Doppler +2500 Hz, rate +0.8 Hz/s, started 90 Hz and 0.2 chip off)\n");
    {
        Lone sig(7, cn0, 2500.0, 0.8, 5);
        const double secs = 6.0;
        std::vector<cf32> x;
        sig.make(x, (size_t)(secs * 4.096e6));
        GnssBand band;
        band.init(4.096e6, 0, 4.096e6, 1 << 20);
        GnssTracker tr;
        GnssSignalSpec sp;
        tr.start(sp, sig.chips, 7, 2500.0 + 90.0, 0, std::fmod(sig.codePhase(0.0) + 0.2, 1023.0));
        double lockAt = -1, dSum2 = 0, pSum2 = 0, cnSum = 0; int n = 0, nc = 0;
        for (size_t off = 0; off < x.size(); off += 16384) {
            const size_t m = std::min<size_t>(16384, x.size() - off);
            band.process(x.data() + off, m);
            while (tr.step(band)) {
                const double t = (double)tr.position() / 4.096e6;
                if (lockAt < 0 && tr.carrierLocked()) lockAt = t;
                if (t > 4.0 && tr.carrierLocked()) {
                    const double e = tr.dopplerHz() - (sig.dop + sig.rate * t);
                    double ph = std::fmod(tr.codePhase() - std::fmod(sig.codePhase(t), 1023.0) + 1023.0 * 4 + 511.5, 1023.0) - 511.5;
                    dSum2 += e * e; pSum2 += ph * ph; n++;
                    cnSum += tr.cn0(); nc++;
                }
            }
            band.trimTo(tr.position() - 8192);
            if (tr.lost()) break;
        }
        const double dRms = n ? std::sqrt(dSum2 / n) : 99, pRms = n ? std::sqrt(pSum2 / n) : 99, cnMean = nc ? cnSum / nc : 0;
        out.print("   C/N0 %.0f: locked after %.2f s, Doppler error rms %.2f Hz, code phase error rms %.4f chip (%.1f m), C/N0 estimate %.1f (%+.1f dB)\n", cn0, lockAt, dRms, pRms, pRms * 293.05, cnMean, cnMean - cn0);
        CHECK(lockAt > 0 && lockAt < 2.0, "C/N0 %.0f: lock after %.2f s", cn0, lockAt);
        CHECK(!tr.lost() && n > 1000, "C/N0 %.0f: channel lost or no samples (%d)", cn0, n);
        CHECK(dRms < (cn0 >= 40 ? 2.5 : 6.0), "C/N0 %.0f: Doppler error rms %.2f Hz", cn0, dRms);
        CHECK(pRms < (cn0 >= 40 ? 0.02 : 0.08), "C/N0 %.0f: code phase error rms %.4f chip", cn0, pRms);
        CHECK(std::fabs(cnMean - cn0) < 1.5, "C/N0 estimate %.1f for %.0f", cnMean, cn0);
        CHECK(pRms > 0.0005, "code phase error implausibly small");
    }
}

static void partC(CaseOut& out) {
    out.print("C. noise only (no satellites), 40 s through the receiver\n");
    GnssReceiver rx;
    rx.configure(4e6);
    int found = 0, locked = 0, notConfirmed = 0;
    rx.setLogCallback([&](const std::string& s) { if (s.find("found") != std::string::npos) found++; if (s.find("locked") != std::string::npos) locked++; if (s.find("not confirmed") != std::string::npos) notConfirmed++; });
    std::mt19937 rng(77);
    genutil::PortableNormal nd(0.14f);
    std::vector<cf32> x(65536);
    const size_t blocks = (size_t)(40.0 * 4e6 / 65536);
    for (size_t b = 0; b < blocks; b++) {
        for (auto& v : x) v = cf32(std::round(nd(rng) * 127.f) / 127.f, std::round(nd(rng) * 127.f) / 127.f);
        rx.feed(x.data(), x.size());
    }
    GnssTelemetry t;
    rx.telemetry(t, 0);
    out.print("   %d detections, %d confirmed on a second look, %d locked, %u search rounds; %s\n", found, found - notConfirmed, locked, t.searchRounds, gnssSummary(t).c_str());
    CHECK(locked == 0 && t.nTracked == 0 && t.channels.empty(), "locked %d on noise", locked);
    CHECK(found <= 3, "%d false detections in 40 s", found);
    // the search keeps going and widens round by round (+-10, +-45, +-170 kHz): the +-170 kHz round alone takes about 35 s of signal
    CHECK(t.searchRounds >= 2 && t.searchStage >= 2, "only %u search rounds, stage %d", t.searchRounds, t.searchStage);
}

int main() {
    // A (five levels), B (three levels) and C share nothing: nine jobs on four threads, the 40 s noise run first; the log keeps the order A, B, C
    const size_t nA = std::size(levelsA), nB = std::size(levelsB);
    std::vector<size_t> order = {nA + nB};
    for (size_t i = nA; i < nA + nB; i++) order.push_back(i);
    for (size_t i = 0; i < nA; i++) order.push_back(i);
    const int fails = dect2::testpar::runCases(nA + nB + 1, [&](size_t i, CaseOut& out) {
        if (i < nA) partA(levelsA[i], i == 0, out);
        else if (i < nA + nB) partB(levelsB[i - nA], i == nA, out);
        else partC(out);
    }, order);
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
