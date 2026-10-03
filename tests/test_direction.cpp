// Direction finder: drive the wizard with simulated antennas and check where it ends up.
#include "dect2/direction.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <algorithm>
#include <string>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::mt19937 rng(21);

struct Sim {
    AntennaKind kind = AntennaKind::Directional;
    double heading0 = 0, beamwidth = 60, frontBack = 18, snr0 = 24, jitter = 1.0;
    double flat = false;
    double gainDb(double h) const {
        if (flat) return 0;
        const double dev = angDiff(h, heading0);
        if (kind == AntennaKind::Dipole) { const double c = std::fabs(std::cos(dev * M_PI / 180)); return std::max(-25.0, 20 * std::log10(std::max(1e-3, c))); }
        return std::max(-frontBack, -12.0 * (dev / (beamwidth / 2)) * (dev / (beamwidth / 2)));
    }
    DirSample sample(double h) const {
        std::normal_distribution<double> nd(0, jitter);
        const double snr = snr0 + gainDb(h) + nd(rng);
        DirSample s;
        const double req = 16.5;
        s.snrDb = snr;
        s.locked = snr > req - 3;
        s.qualityPct = s.locked ? std::min(100.0, std::max(0.0, 25 + 12.5 * (snr - req))) : 0;
        s.occupancyDb = std::max(0.0, 4 + (snr - 3) * 0.6);
        s.lossPct = s.locked ? std::max(0.0, (req - snr) * 3) : 100;
        s.multipath = MultipathLevel::None;
        return s;
    }
};

// run a complete session; spotSnr is used for omni antennas (one entry per spot)
static const DirRecommendation& run(DirectionFinder& f, const Sim& sim, const std::vector<double>& spotSnr = {}, int* count = nullptr) {
    f.start(sim.kind);
    double now = 0;
    for (int guard = 0; guard < 40 && f.state() != DirectionFinder::State::Done; guard++) {
        if (f.state() == DirectionFinder::State::WaitConfirm) {
            if (sim.kind == AntennaKind::Omni && (int)f.results().size() >= (int)spotSnr.size()) { f.finishNow(); break; }
            f.confirm(now);
        }
        while (f.state() == DirectionFinder::State::Measuring) {
            now += 0.25;
            Sim s = sim;
            if (sim.kind == AntennaKind::Omni) { s.flat = true; s.snr0 = spotSnr[std::min<size_t>(spotSnr.size() - 1, (size_t)f.targetHeading() - 1)]; }
            f.addSample(now, s.sample(f.targetHeading()));
        }
    }
    if (count) *count = f.measurementCount();
    return f.recommendation();
}

int main() {
    CHECK(std::string(compassName(0)) == "N" && std::string(compassName(47)) == "NE" && std::string(compassName(359)) == "N" && std::string(compassName(200)) == "SSW", "compass names");
    CHECK(std::fabs(angDiff(350, 10) + 20) < 1e-9 && std::fabs(angDiff(10, 350) - 20) < 1e-9, "angular difference");

    // directional antennas pointing in many directions, strong and weak signal
    double worst = 0; int maxCount = 0;
    for (double snr0 : {26.0, 20.0, 15.0})
        for (double h0 = 0; h0 < 360; h0 += 17) {
            Sim s; s.heading0 = h0; s.snr0 = snr0;
            DirectionFinder f;
            int n = 0;
            const DirRecommendation& r = run(f, s, {}, &n);
            const double err = std::fabs(angDiff(r.heading, h0));
            worst = std::max(worst, err);
            maxCount = std::max(maxCount, n);
            if (err > 16) printf("   snr0 %.0f heading %.0f -> %.1f (error %.1f, %d measurements)\n", snr0, h0, r.heading, err, n);
            CHECK(r.valid && !r.flat, "directional snr0 %.0f heading %.0f: valid %d flat %d", snr0, h0, r.valid, r.flat);
            CHECK(err <= 16, "directional snr0 %.0f heading %.0f found %.1f (error %.1f)", snr0, h0, r.heading, err);
        }
    printf("directional sweep: worst error %.1f degrees, at most %d measurements\n", worst, maxCount);
    CHECK(maxCount <= 11, "too many measurements (%d)", maxCount);

    // a narrow beam (dish-like) and a wide one
    for (double bw : {45.0, 110.0}) {
        Sim s; s.heading0 = 135; s.beamwidth = bw; s.snr0 = 24;
        DirectionFinder f; int n = 0;
        const DirRecommendation& r = run(f, s, {}, &n);
        printf("beamwidth %.0f: heading %.1f after %d measurements\n", bw, r.heading, n);
        CHECK(std::fabs(angDiff(r.heading, 135)) <= (bw < 50 ? 20 : 16), "beamwidth %.0f off target (%.1f)", bw, r.heading);
    }

    // no directivity: the finder must say so after the first four measurements
    {
        Sim s; s.flat = true; s.snr0 = 22;
        DirectionFinder f; int n = 0;
        const DirRecommendation& r = run(f, s, {}, &n);
        printf("no directivity: flat %d after %d measurements: %s\n", r.flat, n, r.text.c_str());
        CHECK(r.flat && n == 4, "flat signal not recognised (flat %d, %d measurements)", r.flat, n);
    }
    // weak and flat: the diagonals are tried before the finder gives up
    {
        Sim s; s.flat = true; s.snr0 = 6;
        DirectionFinder f; int n = 0;
        const DirRecommendation& r = run(f, s, {}, &n);
        printf("weak and flat: flat %d after %d measurements\n", r.flat, n);
        CHECK(r.flat && n == 8, "weak flat signal: flat %d, %d measurements (expected 8)", r.flat, n);
    }

    // dipole / figure-8: either end is fine
    {
        Sim s; s.kind = AntennaKind::Dipole; s.heading0 = 60; s.snr0 = 24;
        DirectionFinder f; int n = 0;
        const DirRecommendation& r = run(f, s, {}, &n);
        const double err = std::min(std::fabs(angDiff(r.heading, 60)), std::fabs(angDiff(r.heading, 240)));
        printf("dipole: heading %.1f symmetric %d after %d measurements (error %.1f)\n", r.heading, r.symmetric, n, err);
        CHECK(err <= 16 && r.symmetric, "dipole result wrong (heading %.1f, symmetric %d)", r.heading, r.symmetric);
    }

    // omnidirectional: three places, the middle one is the best
    {
        Sim s; s.kind = AntennaKind::Omni;
        DirectionFinder f; int n = 0;
        const DirRecommendation& r = run(f, s, {17, 25, 11}, &n);
        printf("omni: %s after %d spots: %s\n", r.label.c_str(), n, r.text.c_str());
        CHECK(r.valid && r.label == "Spot 2" && n == 3, "omni spots: best %s after %d", r.label.c_str(), n);
    }

    // a direction can be skipped; the session still finishes
    {
        Sim s; s.heading0 = 200; s.snr0 = 24;
        DirectionFinder f;
        f.start(AntennaKind::Directional);
        double now = 0;
        f.skip(); // skip north
        CHECK(f.targetHeading() == 90, "after skipping north the next direction is east, got %.0f", f.targetHeading());
        for (int guard = 0; guard < 40 && f.state() != DirectionFinder::State::Done; guard++) {
            if (f.state() == DirectionFinder::State::WaitConfirm) f.confirm(now);
            while (f.state() == DirectionFinder::State::Measuring) { now += 0.25; f.addSample(now, s.sample(f.targetHeading())); }
        }
        CHECK(f.state() == DirectionFinder::State::Done && std::fabs(angDiff(f.recommendation().heading, 200)) <= 20, "skipped session ended at %.1f", f.recommendation().heading);
    }
    printf(fails ? "direction finder tests FAILED\n" : "direction finder tests passed\n");
    return fails ? 1 : 0;
}
