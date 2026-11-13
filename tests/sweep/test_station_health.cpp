// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Station health report on a fake radio: broadcast-like blocks and tones in white noise, an ADC that clips, a clock that is off by a known ppm.
#include "dect2/sweep/station_health.h"
#include "dect2/common/stable_random.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
using namespace dect2::sweep;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Block { double hz, bw, power; };   // power: total, spread over tones 10 kHz apart (bw 0: one tone)
class FakeRadio : public SweepSource {
public:
    double rate = 8e6, sigma2 = 1e-4, ppm = 0;   // ppm: the radio's clock runs fast, so it tunes and reports every frequency that much too high
    std::vector<Block> blocks;
    bool retune(double hz, std::string&) override { fc_ = hz; return true; }
    size_t capture(cf32* out, size_t n) override {
        dect2::normal_distribution<float> g(0.f, (float)std::sqrt(sigma2 / 2));
        for (size_t i = 0; i < n; i++, t_++) {
            float re = g(rng_), im = g(rng_);
            for (auto& b : blocks) {
                const int k = b.bw > 0 ? (int)(b.bw / 10e3) : 1;
                for (int j = 0; j < k; j++) {
                    const double hz = (b.bw > 0 ? b.hz - b.bw / 2 + (j + 0.5) * 10e3 : b.hz) * (1 + ppm * 1e-6);
                    const double d = hz - fc_;
                    if (std::fabs(d) > 0.4 * rate) continue;
                    const double ph = 2 * M_PI * std::fmod(d * (double)t_ / rate, 1.0) + j * 2.399;
                    const double a = std::sqrt(b.power / k);
                    re += (float)(a * std::cos(ph)); im += (float)(a * std::sin(ph));
                }
            }
            out[i] = cf32(std::max(-1.f, std::min(1.f, re)), std::max(-1.f, std::min(1.f, im)));
        }
        return n;
    }
    double sampleRate() const override { return rate; }
    void discard(double s) override { cf32 d[256]; size_t k = (size_t)(s * rate); while (k) { size_t m = std::min<size_t>(k, 256); capture(d, m); k -= m; } }
private:
    double fc_ = 0;
    uint64_t t_ = 0;
    std::mt19937 rng_{4321};
};

static const BandResult* find(const std::vector<BandResult>& v, const char* id) { for (auto& b : v) if (b.band.id == id) return &b; return nullptr; }
static bool has(const HealthReport& r, FindCode c, const char* where = nullptr) {
    for (auto& f : r.findings) if (f.code == c && (!where || f.where == where)) return true;
    return false;
}

int main() {
    std::string err;
    HealthConfig hc;
    for (auto& b : healthBands()) if (b.id == "fm" || b.id == "adsb") hc.bands.push_back(b);
    CHECK(hc.bands.size() == 2, "bands %zu", hc.bands.size());
    CHECK(healthBands(0, 1000e6).size() == 5, "bands inside 1 GHz: %zu", healthBands(0, 1000e6).size());

    // 1. a healthy FM band: four stations 30 to 38 dB over the noise, quiet ADS-B band
    {
        FakeRadio r;
        r.blocks = {{91.1e6, 180e3, 0.004}, {95.5e6, 180e3, 0.016}, {99.9e6, 180e3, 0.008}, {104.3e6, 180e3, 0.002}};
        std::vector<BandResult> out;
        CHECK(runHealthCheck(r, hc, out, err), "%s", err.c_str());
        const BandResult* fm = find(out, "fm");
        CHECK(fm && fm->measured, "fm band");
        if (fm) {
            printf("fm: floor %.1f dB/bin, margin %.1f dB, %d FM found, strongest %.1f dB, clip %.4f\n", fm->floorDb, fm->adcMarginDb, fm->ref.count, fm->ref.strongestSnrDb, fm->adc.clipFraction);
            CHECK(fm->ref.level == RefLevel::Ok && fm->ref.count == 4, "FM level %d count %d", (int)fm->ref.level, fm->ref.count);
            CHECK(std::fabs(fm->ref.strongestHz - 95.5e6) < 100e3, "strongest %.3f MHz", fm->ref.strongestHz / 1e6);
            CHECK(fm->adcMarginDb > 8 && fm->adcMarginDb < 12, "margin %.1f", fm->adcMarginDb);
        }
        HealthInput in; in.bands = out; in.gainDb = 30; in.gainMaxDb = 60;
        HealthReport rep = assessHealth(in);
        CHECK(rep.worst == Severity::Info && has(rep, FindCode::AllGood), "healthy station: %zu findings, worst %d", rep.findings.size(), (int)rep.worst);
        CHECK(healthJson(rep).find("\"all_good\"") != std::string::npos, "json");
    }
    // 2. no FM at all, and a weak one
    {
        FakeRadio r;
        std::vector<BandResult> out;
        CHECK(runHealthCheck(r, hc, out, err), "%s", err.c_str());
        HealthInput in; in.bands = out; in.gainDb = 30; in.gainMaxDb = 60;
        HealthReport rep = assessHealth(in);
        CHECK(has(rep, FindCode::RefAbsent, "FM"), "absent FM not reported");
        FakeRadio w;
        w.blocks = {{95.5e6, 180e3, 1.5e-4}};   // about 14 dB over the noise
        CHECK(runHealthCheck(w, hc, out, err), "%s", err.c_str());
        in.bands = out;
        rep = assessHealth(in);
        const BandResult* fm = find(out, "fm");
        CHECK(fm && fm->ref.level == RefLevel::Weak, "weak FM level %d (snr %.1f)", fm ? (int)fm->ref.level : -1, fm ? fm->ref.strongestSnrDb : 0);
        CHECK(has(rep, FindCode::RefWeak, "FM"), "weak FM not reported");
    }
    // 3. a strong FM carrier overloads the ADC while the ADS-B band is ADC-limited: the advice is a filter, not less gain
    {
        FakeRadio r;
        r.sigma2 = 2e-5;
        r.blocks = {{95.5e6, 0, 2.0}};
        std::vector<BandResult> out;
        CHECK(runHealthCheck(r, hc, out, err), "%s", err.c_str());
        const BandResult* fm = find(out, "fm");
        const BandResult* ad = find(out, "adsb");
        CHECK(fm && fm->adc.clipFraction > 0.001, "no clipping seen: %.4f", fm ? fm->adc.clipFraction : -1);
        CHECK(ad && ad->adcMarginDb < 6, "adsb margin %.1f", ad ? ad->adcMarginDb : -99);
        HealthInput in; in.bands = out; in.gainDb = 20; in.gainMaxDb = 60;
        HealthReport rep = assessHealth(in);
        CHECK(has(rep, FindCode::StrongOutOfBand, "fm"), "filter advice missing");
        CHECK(has(rep, FindCode::AdcLimited, "adsb"), "raise-gain advice missing");
        CHECK(rep.worst == Severity::Bad, "worst %d", (int)rep.worst);
        in.gainDb = 60;
        rep = assessHealth(in);
        CHECK(has(rep, FindCode::AdcLimitedMax, "adsb"), "add-LNA advice at maximum gain missing");
    }
    // 4. overload with nothing else wrong: lower the gain
    {
        BandResult b; b.band.id = "fm"; b.measured = true; b.adcMarginDb = 20; b.adc.n = 1000; b.adc.clipFraction = 0.02; b.adc.peak = 1; b.adc.rmsDbfs = -6;
        HealthInput in; in.bands = {b};
        HealthReport rep = assessHealth(in);
        CHECK(has(rep, FindCode::Overload, "fm") && !has(rep, FindCode::StrongOutOfBand), "plain overload");
    }
    // 5. wasted gain and a raised noise floor in one band
    {
        BandResult a, b;
        a.band = {"fm", 87.5e6, 108e6, 25e3, HealthRef::Fm}; a.measured = true; a.densityDbfsHz = -120; a.adcMarginDb = 35; a.adc.n = 100; a.adc.rmsDbfs = -20;
        b.band = {"uhf", 470e6, 694e6, 100e3, HealthRef::DvbT}; b.measured = true; b.densityDbfsHz = -100; b.adcMarginDb = 15; b.adc.n = 100; b.adc.rmsDbfs = -40;
        HealthInput in; in.bands = {a, b};
        HealthReport rep = assessHealth(in);
        CHECK(has(rep, FindCode::WastedGain, "fm"), "wasted gain");
        CHECK(has(rep, FindCode::NoiseExcess, "uhf"), "noise excess in uhf");
        CHECK(!has(rep, FindCode::NoiseExcess, "fm"), "fm is the quiet one");
        // the same densities, but the low band is the higher one by the galactic slope: no complaint
        a.densityDbfsHz = -100; b.densityDbfsHz = -112;
        in.bands = {a, b};
        rep = assessHealth(in);
        CHECK(!has(rep, FindCode::NoiseExcess), "galactic slope taken as noise");
    }
    // 6. frequency error from a known carrier
    {
        FakeRadio r;
        r.ppm = 12.0;
        r.blocks = {{100.0e6, 0, 0.01}};
        SweepConfig c; c.startHz = 99.9e6; c.stopHz = 100.1e6; c.binHz = 2e3; c.dwellSec = 0.05; c.settleSec = 0.001;
        Sweep s;
        CHECK(runSweep(r, c, s, err), "%s", err.c_str());
        PpmResult p = frequencyError(s, 100.0e6, 50e3);
        CHECK(p.valid && std::fabs(p.ppm - 12.0) < 0.5, "ppm %.2f (valid %d)", p.ppm, p.valid);
        PpmResult none = frequencyError(s, 99.95e6, 5e3);
        CHECK(!none.valid, "carrier found where there is none");
        HealthInput in; in.havePpm = true; in.ppm = p;
        BandResult b; b.band.id = "fm"; b.measured = true; b.adcMarginDb = 15; in.bands = {b};
        CHECK(has(assessHealth(in), FindCode::PpmHigh), "ppm advice");
        in.ppm.ppm = 1.5;
        CHECK(has(assessHealth(in), FindCode::PpmOk), "ppm ok");
    }
    // 7. ADS-B range: a good all-round station, a short one, and one blind to the west
    {
        float sec[36];
        for (int i = 0; i < 36; i++) sec[i] = 150.f - (i % 5) * 10.f;
        RangeResult g = assessRange(sec, 36, 5000);
        CHECK(g.valid && g.blindSectors == 0 && g.sectorsWithData == 36 && std::fabs(g.maxNm - 150) < 0.1, "good range");
        for (int i = 25; i < 30; i++) sec[i] = 0;
        RangeResult w = assessRange(sec, 36, 5000);
        CHECK(w.valid && w.blindSectors == 5 && w.blindStart == 25, "blind %d from %d", w.blindSectors, w.blindStart);
        HealthInput in; in.haveRange = true; in.range = w;
        BandResult b; b.band.id = "adsb"; b.measured = true; b.adcMarginDb = 15; in.bands = {b};
        HealthReport rep = assessHealth(in);
        CHECK(has(rep, FindCode::RangeBlind), "blind advice");
        for (int i = 0; i < 36; i++) sec[i] = 20.f;
        in.range = assessRange(sec, 36, 5000);
        CHECK(has(assessHealth(in), FindCode::RangeShort), "short range advice");
        CHECK(!assessRange(sec, 36, 10).valid, "10 positions are not enough");
        in.range = assessRange(sec, 36, 10);
        CHECK(has(assessHealth(in), FindCode::RangeNone), "no-data note");
    }
    printf(fails ? "station_health: %d FAILED\n" : "station_health: ok\n", fails);
    return fails ? 1 : 0;
}
