// Fast scanner: dynamic floor, hop calibration, candidates and hit tracker on synthetic spectra (a tilted floor with drifting carriers and
// bursts), the hop planner and radio ceilings, the filterbank and the channel recorders (one and 100 channels), the scan loop with a simulated
// radio, and the measured DSP speed per radio profile.
#include "dect2/fastscan/fwsweep.h"
#include "dect2/fastscan/profile.h"
#include "dect2/fastscan/scanner.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>
#include <thread>

using namespace dect2;
using namespace dect2::fastscan;

static int gFail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); gFail++; } } while (0)

static std::mt19937 rng(12345);
static double expo() { std::exponential_distribution<double> e(1.0); return e(rng); }
// dB of the average of K exponential powers around a mean level (what an averaged FFT bin of noise reads)
static float noiseDb(float meanDb, int K = 16) { double s = 0; for (int i = 0; i < K; i++) s += expo(); return meanDb + 10.f * (float)std::log10(s / K); }
static float tiltDb(size_t i, size_t n) { return -95.f + 12.f * (float)i / (float)n; }   // a floor that rises 12 dB over the band

static bool nearTo(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// what the scanner does per hop: chunks of 500 bins, each seeded by its own median
static void updateHops(DynamicFloor& f, const std::vector<float>& x, double t) {
    for (size_t a = 0; a < x.size(); a += 500) {
        const size_t m = std::min<size_t>(500, x.size() - a);
        std::vector<float> c(x.begin() + (long)a, x.begin() + (long)(a + m));
        std::nth_element(c.begin(), c.begin() + (long)(m / 2), c.end());
        f.update(a, &x[a], m, t, c[m / 2] - 1.0f);
    }
}

// ---------------------------------------------------------------- dynamic floor
static void testFloor() {
    const size_t n = 4000;
    DynamicFloor f;
    f.configure(n, FloorParams());
    std::vector<float> x(n);
    double t = 0;
    for (int pass = 0; pass < 300; pass++) {
        t += 1.0;
        for (size_t i = 0; i < n; i++) {
            x[i] = noiseDb(tiltDb(i, n));
            if (i >= 1000 && i < 1004) x[i] = tiltDb(i, n) + 25.f;                         // a steady carrier (4 bins)
            if (i >= 2000 && i < 2003 && pass % 7 == 0) x[i] = tiltDb(i, n) + 30.f;        // bursts
            if (i >= 3000 && i < 3002) x[i] = tiltDb(i, n) + 14.f + 3.f * std::sin(pass * 0.1f);   // a drifting level
        }
        updateHops(f, x, t);
    }
    // the floor sits a little under the mean noise level and follows the tilt; signals did not lift it
    double lo = 1e9, hi = -1e9;
    for (size_t i = 0; i < n; i++) {
        const double d = f.floorDb(i) - tiltDb(i, n);
        lo = std::min(lo, d); hi = std::max(hi, d);
    }
    CHECK(lo > -5.0 && hi < 1.0);
    CHECK(hi - lo < 3.5);
    for (size_t i : {1001u, 2001u, 3001u}) CHECK(nearTo(f.floorDb(i), tiltDb(i, n) - 1.5, 3.0));
    // a step of the whole band (gain change): the floor follows within a handful of looks
    for (int pass = 0; pass < 6; pass++) {
        t += 1.0;
        for (size_t i = 0; i < n; i++) x[i] = noiseDb(tiltDb(i, n) + 10.f);
        updateHops(f, x, t);
    }
    double mean = 0;
    for (size_t i = 0; i < n; i++) mean += f.floorDb(i) - (tiltDb(i, n) + 10.f);
    CHECK(mean / n > -5.0);
    // and a fall follows fast
    for (int pass = 0; pass < 4; pass++) {
        t += 1.0;
        for (size_t i = 0; i < n; i++) x[i] = noiseDb(tiltDb(i, n));
        updateHops(f, x, t);
    }
    mean = 0;
    for (size_t i = 0; i < n; i++) mean += f.floorDb(i) - tiltDb(i, n);
    CHECK(mean / n < 0.5);
    // a signal present from the first look is not its own floor when the hop median seeds it
    DynamicFloor g;
    g.configure(100, FloorParams());
    std::vector<float> y(100, -90.f);
    for (int i = 40; i < 44; i++) y[i] = -60.f;
    g.update(0, y.data(), 100, 1.0, -91.f);
    CHECK(g.floorDb(41) < -90.f);
    // persistence
    const std::string path = (std::filesystem::temp_directory_path() / "fastscan_floor_test.bin").string();
    CHECK(f.save(path, 88e6, 12500));
    DynamicFloor h;
    h.configure(n, FloorParams());
    CHECK(h.load(path, 88e6, 12500));
    CHECK(h.floorDb(123) == f.floorDb(123) || nearTo(h.floorDb(123), f.floorDb(123), 0.006));
    DynamicFloor other;
    other.configure(n, FloorParams());
    CHECK(!other.load(path, 90e6, 12500));
    std::filesystem::remove(path);
}

static void testCalibration() {
    const size_t U = 800;
    HopCalibration cal;
    cal.configure(U);
    std::vector<float> in(U), out(U);
    auto shape = [&](size_t j) {
        float s = 0;
        if (j < 60) s -= 0.2f * (60 - (float)j);             // roll-off at the lower edge, up to 12 dB
        if (j > U - 60) s -= 0.2f * ((float)j - (U - 60));   // and the upper
        if (j >= U / 2 - 2 && j <= U / 2 + 2) s += 15.f;     // DC spike
        return s;
    };
    for (int hop = 0; hop < 80; hop++) {
        const float level = -90.f + (hop % 5) * 1.5f;        // hops differ in level
        for (size_t j = 0; j < U; j++) in[j] = noiseDb(level + shape(j));
        cal.apply(in.data(), out.data());
    }
    // a new hop comes out flat, the edges and the spike gone
    for (size_t j = 0; j < U; j++) in[j] = noiseDb(-90.f + shape(j));
    cal.apply(in.data(), out.data());
    double dev = 0;
    int cnt = 0;
    for (size_t j = 0; j < U; j++) {
        if (j == 0) continue;
        dev += std::fabs(out[j] - (-90.f)); cnt++;
    }
    CHECK(dev / cnt < 2.0);
    CHECK(nearTo(out[U / 2], -90.f, 5.0));
    CHECK(nearTo(out[5], -90.f, 5.0));
}

// ---------------------------------------------------------------- candidates and tracker
static void testCandidates() {
    const size_t n = 500;
    std::vector<float> db(n, -90.f), fl(n, -90.f);
    for (int i = 100; i < 105; i++) db[i] = -70.f;       // 5 bins wide
    db[102] = -60.f;
    db[200] = -65.f;                                     // one bin
    db[300] = -70.f; db[302] = -70.f;                    // a gap of one bin: bridged
    db[400] = -70.f;                                     // masked
    std::vector<uint8_t> mask(n, 0);
    mask[400] = 1;
    const auto c = findCandidates(db.data(), fl.data(), n, 100e6, 10e3, 8, 6, 1, mask.data());
    CHECK(c.size() == 3);
    if (c.size() == 3) {
        CHECK(nearTo(c[0].freqHz, 100e6 + 102 * 10e3, 5e3) && nearTo(c[0].bwHz, 5 * 10e3, 1));
        CHECK(nearTo(c[0].snrDb, 30.0, 0.01));
        CHECK(nearTo(c[1].freqHz, 100e6 + 200 * 10e3, 1) && nearTo(c[1].bwHz, 10e3, 1));
        CHECK(nearTo(c[2].bwHz, 3 * 10e3, 1));
    }
    // a signal that never reaches the on level is not a candidate even if it passes the edge level
    std::fill(db.begin(), db.end(), -90.f);
    for (int i = 50; i < 60; i++) db[i] = -83.f;
    CHECK(findCandidates(db.data(), fl.data(), n, 0, 1, 8, 6).empty());
}

static Candidate cand(double hz, float snr, double bw = 12500) { Candidate c; c.freqHz = hz; c.snrDb = snr; c.peakDb = -90 + snr; c.bwHz = bw; return c; }

static void testTracker() {
    HitParams p;
    p.onDb = 10; p.offDb = 6; p.hangSec = 2; p.minDurSec = 0;
    HitTracker t;
    t.configure(p);
    std::vector<HitEvent> ev;
    const double lo = 100e6, hi = 101e6, f = 100.5e6;
    t.observe(0, lo, hi, {cand(f, 20)}, ev);
    CHECK(ev.size() == 1 && ev[0].kind == HitEvent::Start && t.activeCount() == 1);
    // hysteresis: between off and on it stays; below off it hangs, then ends after the hang time
    ev.clear();
    t.observe(1, lo, hi, {cand(f, 8)}, ev);
    CHECK(ev.empty() && t.activeCount() == 1);
    t.observe(2, lo, hi, {}, ev);
    CHECK(ev.empty());                 // 1 s of silence: inside the hang
    t.observe(3.2, lo, hi, {}, ev);   // last above the off level at t = 1: 2.2 s of silence is past the 2 s hang
    CHECK(ev.size() == 1 && ev[0].kind == HitEvent::End && t.activeCount() == 0);
    CHECK(t.hits().size() == 1 && t.hits()[0].count == 1 && !t.hits()[0].active);
    // a signal between off and on does not start a hit
    ev.clear();
    HitTracker u; u.configure(p);
    u.observe(0, lo, hi, {cand(f, 8)}, ev);
    CHECK(ev.empty() && u.hits().empty());
    // returns on the same frequency: the same row, count 2
    ev.clear();
    t.observe(10, lo, hi, {cand(f + 1500, 22)}, ev);
    CHECK(ev.size() == 1 && t.hits().size() == 1 && t.hits()[0].count == 2);
    // not covered by the span looked at: stays active until it is stale
    ev.clear();
    t.observe(11, 200e6, 201e6, {}, ev);
    CHECK(ev.empty() && t.activeCount() == 1);
    t.observe(60, 200e6, 201e6, {}, ev);
    CHECK(ev.size() == 1 && t.activeCount() == 0);
}

static void testTrackerMore() {
    HitParams p;
    p.onDb = 10; p.offDb = 6; p.hangSec = 1; p.minDurSec = 0.5;
    HitTracker t;
    t.configure(p);
    std::vector<HitEvent> ev;
    // a burst shorter than the minimum duration is never announced and leaves no row
    t.observe(0, 100e6, 101e6, {cand(100.2e6, 20)}, ev);
    t.observe(0.2, 100e6, 101e6, {}, ev);
    CHECK(ev.empty() && t.hits().empty());
    // long enough: announced at the look that reaches the minimum
    t.observe(1.0, 100e6, 101e6, {cand(100.2e6, 20)}, ev);
    CHECK(ev.empty());
    t.observe(1.6, 100e6, 101e6, {cand(100.2e6, 20)}, ev);
    CHECK(ev.size() == 1);
    // excluded range, locked frequency, priority flag
    HitTracker u;
    u.configure(p);
    u.setExcluded({{99e6, 100e6}});
    u.setLocked({100.5e6});
    u.setPriority({100.8e6});
    ev.clear();
    HitParams q = p; q.minDurSec = 0; u.configure(q);
    u.observe(0, 99e6, 101e6, {cand(99.5e6, 30), cand(100.5e6, 30), cand(100.8e6, 30), cand(100.65e6, 30)}, ev);
    CHECK(ev.size() == 2);
    int pr = 0;
    for (const Hit& h : u.hits()) pr += h.priority;
    CHECK(pr == 1);
    // grid snap
    HitTracker g;
    HitParams gp = q; gp.gridStepHz = 12500; gp.gridOriginHz = 100e6;
    g.configure(gp);
    ev.clear();
    g.observe(0, 100e6, 101e6, {cand(100.0126e6, 20)}, ev);
    g.observe(1, 100e6, 101e6, {cand(100.0124e6, 20)}, ev);
    CHECK(g.hits().size() == 1 && nearTo(g.hits()[0].freqHz, 100.0125e6, 1));
    // remove / the list is capped, the oldest inactive goes first
    HitParams cp = q; cp.maxHits = 5; cp.hangSec = 0;
    HitTracker c;
    c.configure(cp);
    for (int i = 0; i < 12; i++) {
        c.observe(i * 10.0, 100e6, 110e6, {cand(100e6 + i * 100e3, 20)}, ev);
        c.observe(i * 10.0 + 1, 100e6, 110e6, {}, ev);
    }
    CHECK(c.hits().size() <= 5);
    CHECK(c.hits().back().freqHz > 100e6 + 10 * 100e3);
    const uint64_t id = c.hits().back().id;
    c.remove(id);
    CHECK(c.hits().size() <= 4);
}

// ---------------------------------------------------------------- the speed of a pass over a synthetic scene, tracker over several sweeps
static void testScene() {
    // 400 bins, a floor with tilt, one carrier that drifts one bin every 20 passes and a burst on passes 30..39
    const size_t n = 400;
    DynamicFloor fl;
    fl.configure(n, FloorParams());
    HitParams p; p.onDb = 10; p.offDb = 6; p.hangSec = 3; p.mergeHz = 25e3;
    HitTracker tr;
    tr.configure(p);
    std::vector<float> x(n);
    int starts = 0, ends = 0;
    double carrierMin = 1e30, carrierMax = -1e30;
    for (int pass = 0; pass < 120; pass++) {
        const double t = pass * 1.0;
        for (size_t i = 0; i < n; i++) x[i] = noiseDb(tiltDb(i, n), 16);
        const size_t car = 120 + pass / 20;
        for (size_t i = car; i < car + 3; i++) x[i] = tiltDb(i, n) + 22.f;
        if (pass >= 30 && pass < 40) for (size_t i = 300; i < 302; i++) x[i] = tiltDb(i, n) + 18.f;
        updateHops(fl, x, t);
        std::vector<HitEvent> ev;
        auto c = findCandidates(x.data(), fl.floors().data(), n, 100e6, 12500, tr.detectDb(), 4.f);
        tr.observe(t, 100e6, 100e6 + n * 12500.0, c, ev);
        for (auto& e : ev) { if (e.kind == HitEvent::Start) starts++; else ends++; }
    }
    // the carrier is one hit (drifting inside the merge distance), the burst a second
    CHECK(tr.hits().size() == 2);
    CHECK(starts == 2 && ends == 1);
    for (const Hit& h : tr.hits()) { carrierMin = std::min(carrierMin, h.freqHz); carrierMax = std::max(carrierMax, h.freqHz); }
    CHECK(carrierMin > 100e6 + 100 * 12500.0);
    int act = tr.activeCount();
    CHECK(act == 1);
    for (const Hit& h : tr.hits()) if (nearTo(h.freqHz, 100e6 + 301 * 12500.0, 20e3)) CHECK(h.count == 1 && !h.active && h.maxSnrDb > 12.f);
}

// ---------------------------------------------------------------- planner
static void testPlan() {
    const RadioProfile& rtl = radioProfile("rtlsdr");
    FastPlanConfig c;
    c.bands = {{88e6, 108e6}};
    c.binHz = 12500;
    FastPlan p = planFast(c, rtl);
    CHECK(!p.hops.empty() && p.totalBins > 1000);
    CHECK(p.hops.front().firstBin == 0);
    // hops are contiguous, no gaps
    for (size_t i = 1; i < p.hops.size(); i++) CHECK(p.hops[i].firstBin == p.hops[i - 1].firstBin + p.binsPerHop);
    CHECK(nearTo(p.coveredHz, 20e6, 2 * p.binHz * p.binsPerHop));
    const size_t base = p.hops.size();
    // exclusion of a whole hop (a few MHz of FM broadcast), the bins masked
    c.exclusions = {{90e6, 95e6}};
    FastPlan q = planFast(c, rtl);
    CHECK(q.hops.size() < base);
    CHECK(q.mask[(size_t)q.binOf(92e6)] == 1 && q.mask[(size_t)q.binOf(97e6)] == 0);
    CHECK(q.coveredHz < p.coveredHz - 1e6);
    // priority hops come again
    c.exclusions.clear();
    c.priority = {{107e6, 108e6}};
    c.priorityEvery = 2;
    FastPlan r = planFast(c, rtl);
    int hits107 = 0;
    for (int h : r.order) hits107 += r.hops[h].priority;
    CHECK(r.order.size() > r.hops.size() && hits107 >= 3);
    // two bands on one grid
    c.priority.clear();
    c.bands = {{118e6, 137e6}, {156e6, 163e6}};
    FastPlan s = planFast(c, rtl);
    CHECK(s.hops.size() >= 10 && s.hops.back().hiHz > 160e6);
    // the speed model: a faster radio is faster, a native sweep faster still
    const double cr = ceilingMhzPerSec(radioProfile("rtlsdr"), 0.004), ca = ceilingMhzPerSec(radioProfile("airspy"), 0.004);
    const double ch = ceilingMhzPerSec(radioProfile("hackrf"), 0.004);
    RadioProfile hn = radioProfile("hackrf"); hn.nativeSweep = false;
    CHECK(cr < ca && ca < ch);
    CHECK(ceilingMhzPerSec(hn, 0.004) < ch);
    printf("  retune-limited ceiling, MHz/s at 4 ms dwell: RTL-SDR %.0f  Airspy %.0f  HackRF (libhackrf retune) %.0f  HackRF (firmware sweep) %.0f\n", cr, ca, ceilingMhzPerSec(hn, 0.004), ch);
    CHECK(profileForBoard("HackRF One", 0).id == "hackrf" && profileForBoard("rtlsdr", 0).id == "rtlsdr" && profileForBoard("Airspy R2", 10e6).id == "airspy");
    CHECK(profileForBoard("Airspy R2", 6e6).rateHz == 6e6);
    // channelisation
    std::vector<float> db(1000, -90.f), fl(1000, -92.f);
    db[1] = -50.f;
    auto ch2 = channelise(db.data(), fl.data(), 1000, 100e6, 5e3, 100e6, 25e3, 100e6, 105e6);
    CHECK(ch2.size() > 150);
    CHECK(nearTo(ch2[0].centerHz, 100e6, 1) && ch2[0].snrDb > 40 && ch2[0].bins == 3);   // bins 0..2 round to channel 0 (12.5 kHz either side)
    CHECK(ch2[0].meanDb > ch2[1].meanDb + 15);
    CHECK(guessMode(121.5e6, 25e3) == "am" && guessMode(145.5e6, 12.5e3) == "fm");
}

// ---------------------------------------------------------------- filterbank and recorders
static std::vector<cf32> fmChannels(double rate, size_t n, const std::vector<double>& offs, double toneHz, double dev, std::vector<double>* ph = nullptr) {
    std::vector<cf32> x(n, cf32(0, 0));
    std::vector<double> carrier(offs.size(), 0.0), mod(offs.size(), 0.0);
    for (size_t k = 0; k < offs.size(); k++) {
        double c = 0, m = 0;
        const double mf = toneHz * (1.0 + 0.013 * (double)k);   // every channel its own tone
        for (size_t i = 0; i < n; i++) {
            m += 2 * M_PI * mf / rate;
            c += 2 * M_PI * (offs[k] + dev * std::sin(m)) / rate;
            x[i] += cf32((float)std::cos(c), (float)std::sin(c)) * (0.5f / std::sqrt((float)offs.size()));
        }
    }
    (void)ph;
    return x;
}

static double goertzel(const std::vector<int16_t>& a, size_t from, double fs, double f) {
    double re = 0, im = 0;
    for (size_t i = from; i < a.size(); i++) { const double w = 2 * M_PI * f * (double)i / fs; re += a[i] * std::cos(w); im += a[i] * std::sin(w); }
    return std::sqrt(re * re + im * im) / std::max<size_t>(1, a.size() - from);
}

static bool readWav(const std::string& path, std::vector<int16_t>& s, uint32_t& rate) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char h[44];
    if (fread(h, 1, 44, f) != 44) { fclose(f); return false; }
    rate = h[24] | (h[25] << 8) | (h[26] << 16) | ((uint32_t)h[27] << 24);
    const uint32_t bytes = h[40] | (h[41] << 8) | (h[42] << 16) | ((uint32_t)h[43] << 24);
    s.resize(bytes / 2);
    const size_t got = fread(s.data(), 2, s.size(), f);
    fclose(f);
    return got == s.size();
}

static void testWola() {
    const double rate = 2.4e6;
    WolaBank b;
    const int M = wolaChannelsFor(rate, 12500);
    CHECK(M == 64 && b.configure(rate, M));
    const size_t n = 200000;
    // a tone 10 kHz above the centre of channel 3: channel 3 sees it at +10 kHz, channel 5 does not
    const double f0 = 3 * b.binHz() + 10e3;
    std::vector<cf32> x(n);
    for (size_t i = 0; i < n; i++) x[i] = cf32((float)std::cos(2 * M_PI * f0 * i / rate), (float)std::sin(2 * M_PI * f0 * i / rate));
    std::vector<cf32> y3, y5, y4;
    b.process(x.data(), n, [&](uint64_t, const float* re, const float* im) { y3.push_back(cf32(re[3], im[3])); y5.push_back(cf32(re[5], im[5])); y4.push_back(cf32(re[4], im[4])); });
    CHECK(y3.size() > 1000);
    double p3 = 0, p5 = 0, p4 = 0;
    for (size_t i = 100; i < y3.size(); i++) { p3 += std::norm(y3[i]); p5 += std::norm(y5[i]); p4 += std::norm(y4[i]); }
    const size_t cnt = y3.size() - 100;
    CHECK(nearTo(p3 / cnt, 1.0, 0.1));
    CHECK(10 * std::log10(p5 / cnt + 1e-20) < -50);                         // two channels away: down
    CHECK(p4 / cnt < 1.2 * p3 / cnt);   // the next channel: the tone is 27.5 kHz from its centre, inside the overlapping passband edge (by design)
    // frequency of the tone in channel 3: +10 kHz at the 4 x bin output rate
    double acc = 0;
    for (size_t i = 101; i < y3.size(); i++) acc += std::arg(y3[i] * std::conj(y3[i - 1]));
    const double fest = acc / (y3.size() - 101) * b.outRate() / (2 * M_PI);
    CHECK(nearTo(fest, 10e3, 100));
}

static void testRecordOne() {
    const double rate = 2.4e6;
    const std::string dir = (std::filesystem::temp_directory_path() / "fastscan_rec_test").string();
    std::filesystem::remove_all(dir);
    RecSettings s;
    s.dir = dir; s.chanBwHz = 12500; s.maxLoad = 100;
    RecorderBank rb;
    CHECK(rb.configure(rate, 145e6, s));
    // two FM channels (tones 1 kHz and 1.013 kHz), 5 kHz deviation, one at +123.4 kHz (off the filterbank raster), one at -400 kHz
    const std::vector<double> offs = {123400.0, -400000.0};
    CHECK(rb.start(1, 145e6 + offs[0], "fm", 1700000000));
    CHECK(rb.start(2, 145e6 + offs[1], "fm", 1700000000));
    CHECK(!rb.start(3, 145e6 + 1.2e6, "fm", 1700000000));   // outside the capture
    CHECK(rb.recording(1) && rb.stats().active == 2);
    const size_t n = (size_t)(rate * 1.0);
    std::vector<cf32> x = fmChannels(rate, n, offs, 1000.0, 3000.0);
    for (size_t at = 0; at < n; at += 65536) rb.feed(x.data() + at, std::min<size_t>(65536, n - at));
    rb.closeAll();
    int found = 0;
    for (auto& e : std::filesystem::directory_iterator(dir)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("scan_", 0) != 0 || e.path().extension() != ".wav") continue;
        std::vector<int16_t> a; uint32_t wr = 0;
        CHECK(readWav(e.path().string(), a, wr));
        CHECK(wr >= 15000 && wr <= 40000);
        CHECK(a.size() > wr * 0.8);   // about a second of audio
        const bool first = name.find("145.123") != std::string::npos;
        const bool second = name.find("144.600") != std::string::npos;
        CHECK(first || second);
        const double tone = first ? 1000.0 : 1013.0;
        const double other = first ? 1013.0 : 1000.0;
        const double want = goertzel(a, a.size() / 4, wr, tone), off = goertzel(a, a.size() / 4, wr, other + 700);
        CHECK(want > 10 * off && want > 300);
        found++;
    }
    CHECK(found == 2);
    std::filesystem::remove_all(dir);
}

static void testRecord100() {
    const double rate = 2.4e6;
    const std::string dir = (std::filesystem::temp_directory_path() / "fastscan_rec100_test").string();
    std::filesystem::remove_all(dir);
    RecSettings s;
    s.dir = dir; s.chanBwHz = 12500; s.maxLoad = 100; s.iqSlice = false;
    RecorderBank rb;
    CHECK(rb.configure(rate, 100e6, s));
    std::vector<double> offs;
    for (int k = 0; k < 100; k++) offs.push_back(-1.0e6 + 20000.0 * k + 3000.0 * (k % 3));
    int ok = 0;
    for (int k = 0; k < 100; k++) ok += rb.start(100 + k, 100e6 + offs[k], "fm", 1700000000 + k);
    CHECK(ok == 100);
    CHECK(!rb.start(999, 100e6 + 50e3, "fm", 1700000999));   // the 101st is refused
    const size_t n = (size_t)(rate * 0.5);
    std::vector<cf32> x = fmChannels(rate, n, offs, 700.0, 3000.0);
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t at = 0; at < n; at += 65536) rb.feed(x.data() + at, std::min<size_t>(65536, n - at));
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const RecStats st = rb.stats();
    CHECK(st.active == 100);
    rb.closeAll();
    int files = 0, good = 0;
    for (auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() != ".wav") continue;
        files++;
        std::vector<int16_t> a; uint32_t wr = 0;
        if (!readWav(e.path().string(), a, wr) || a.size() < wr * 0.3) continue;
        // the tone of some channel (700 Hz x 1.013^k) is in there: its strongest component is in 700..2600 Hz
        double best = 0;
        for (double f = 700; f <= 2600; f += 20) best = std::max(best, goertzel(a, a.size() / 4, wr, f));
        if (best > 200) good++;
    }
    CHECK(files == 100);
    CHECK(good >= 95);
    printf("  100 channels recorded from 2.4 Msps: %.2f s of CPU for 0.5 s of signal (%.2fx real time on the calling thread + pool)\n", sec, 0.5 / sec);
    std::filesystem::remove_all(dir);
}

static void testGuardAndFiles() {
    CpuGuard g;
    g.configure(0.7, 100);
    for (int i = 0; i < 20; i++) g.update(1.2, 1.0, 60);   // 120 % busy
    CHECK(g.cap() < 30 && g.throttled());
    const int low = g.cap();
    for (int i = 0; i < 400; i++) g.update(0.1, 0.5, low);
    CHECK(g.cap() > low);
    CHECK(g.cap() <= 100);
    const std::string nm = scanRecName(145.5e6, 1700000000, "wav");
    CHECK(nm.rfind("scan_145.500MHz_", 0) == 0 && nm.size() > 4 && nm.substr(nm.size() - 4) == ".wav");
    std::vector<RecFileInfo> f = {{"a", 400, 100}, {"b", 400, 300}, {"c", 400, 200}, {"d", 400, 400}};
    auto d = pickDeletions(f, 900, 1);
    CHECK(d.size() == 2 && d[0] == 0 && d[1] == 2);   // oldest first: a (100), c (200)
    CHECK(pickDeletions(f, 5000, 1).empty());
    CHECK(pickDeletions(f, 0, 1).size() == 3);        // the newest is never picked
    CHECK(demodFor("am") == Demod::AM && demodFor("fm") == Demod::FM && demodFor("dmr") == Demod::FM);
}

// ---------------------------------------------------------------- the scan loop with a simulated radio
struct SimSource : sweep::SweepSource {
    double rate;
    double center = 0;
    std::atomic<long> caps{0};
    long retunes = 0;
    std::mt19937 g{7};
    std::normal_distribution<float> nd{0.f, 1.f};
    double noiseAmp = 0.001;
    double phase = 0;
    SimSource(double r) : rate(r) {}
    bool realtime = false;   // capture and discard take the time the samples would take on a real radio
    bool retune(double c, std::string&) override { center = c; retunes++; return true; }
    void discard(double sec) override { if (realtime) std::this_thread::sleep_for(std::chrono::duration<double>(sec)); }
    double sampleRate() const override { return rate; }
    size_t capture(cf32* out, size_t n) override {
        const long k = caps++;
        if (realtime) std::this_thread::sleep_for(std::chrono::duration<double>((double)n / rate));
        for (size_t i = 0; i < n; i++) out[i] = cf32(nd(g), nd(g)) * (float)noiseAmp + cf32(0.003f, 0);   // noise and a DC offset
        auto tone = [&](double f, double amp, double dev, double modHz) {
            const double off = f - center;
            if (std::fabs(off) > rate * 0.4) return;
            double ph = phase;
            for (size_t i = 0; i < n; i++) {
                const double t = (double)(i + k * n) / rate;
                const double a = 2 * M_PI * (off * t + dev / (2 * M_PI * modHz) * (-std::cos(2 * M_PI * modHz * t)));
                out[i] += cf32((float)std::cos(a), (float)std::sin(a)) * (float)amp;
            }
            (void)ph;
        };
        tone(103.3e6, 0.05, 0, 1);          // a steady carrier
        if (k >= 25 && k < 60) tone(104.1e6, 0.05, 3000, 1000);   // a FM burst
        return n;
    }
};

// A new hit is named from a moment's capture on its hop: an unmodulated-looking FSK burst here; the point is that the scan parks, labels and moves on
static void testIdentifyNew() {
    const RadioProfile& rtl = radioProfile("rtlsdr");
    FastScanConfig c;
    c.plan.bands = {{102e6, 106e6}};
    c.identifyNew = true; c.threads = 1;
    FastScanner fs; std::string err;
    if (!fs.configure(c, rtl, err)) { CHECK(false); return; }
    SimSource src(fs.plan().rateHz);
    RunHooks hk; hk.maxPasses = 40;
    std::atomic<bool> cancel{false};
    CHECK(fs.run(src, &cancel, hk, err));
    const auto l = fs.labels();
    printf("labels: %zu\n", l.size());
    for (const auto& q : l) printf("  %.3f MHz: %s (%s)\n", q.freqHz / 1e6, q.name.c_str(), q.headline.c_str());
    CHECK(!l.empty());
    bool carrier = false; for (const auto& q : l) if (std::fabs(q.freqHz - 103.3e6) < 10e3) carrier = true;
    CHECK(carrier);
    std::vector<double> f; for (const auto& q : l) f.push_back(q.freqHz);
    std::sort(f.begin(), f.end());
    int dup = 0; for (size_t i = 1; i < f.size(); i++) if (f[i] - f[i - 1] < 1000) dup++;
    CHECK(dup == 0);
}

static void testScanLoop() {
    const RadioProfile& rtl = radioProfile("rtlsdr");
    FastScanConfig c;
    c.plan.bands = {{102e6, 106e6}};
    c.plan.binHz = 5000;
    c.plan.dwellSec = 0.004;
    c.hits.onDb = 12; c.hits.offDb = 8; c.hits.hangSec = 0.05; c.hits.mergeHz = 10e3;
    FastScanner sc;
    std::string err;
    CHECK(sc.configure(c, rtl, err));
    const FastPlan& pl = sc.plan();
    CHECK(pl.hops.size() >= 2);
    SimSource src(pl.rateHz);
    RunHooks hk;
    int starts = 0, ends = 0, passes = 0;
    hk.maxPasses = 12;
    hk.clock = [&] { return src.caps.load() * 0.05; };
    hk.onEvents = [&](const std::vector<HitEvent>& e) { for (auto& x : e) (x.kind == HitEvent::Start ? starts : ends)++; };
    hk.onPass = [&](const ScanStats&) { passes++; };
    CHECK(sc.run(src, nullptr, hk, err));
    CHECK(passes == 12);
    CHECK(starts >= 2 && ends >= 1);
    bool carrier = false, burst = false;
    for (const Hit& h : sc.tracker().hits()) {
        if (nearTo(h.freqHz, 103.3e6, 8e3)) { carrier = true; CHECK(h.maxSnrDb > 25.f); }
        if (nearTo(h.freqHz, 104.1e6, 15e3)) { burst = true; CHECK(h.count >= 1); }
    }
    CHECK(carrier && burst);
    CHECK(sc.tracker().hits().size() == 2);
    // the pseudo-spectrum: max-hold shows the burst and the carrier, the floor is under both and the DC spike of the source is gone
    SpectrumView v = sc.snapshot();
    CHECK(v.bins() == pl.totalBins);
    const long bc = pl.binOf(103.3e6), bb = pl.binOf(104.1e6);
    CHECK(v.maxhold[bc] > v.floor[bc] + 25.f && v.maxhold[bb] > v.floor[bb] + 12.f);
    double spread = 0;
    for (size_t i = 100; i < v.bins() - 100; i++) spread = std::max(spread, (double)std::fabs(v.floor[i] - v.floor[v.bins() / 2]));
    CHECK(spread < 6.0);
    const ScanStats st = sc.stats();
    CHECK(st.passes == 12 && st.mhzPerSec > 0);
    printf("  simulated radio (no retune delay): %.0f MHz/s over %zu hops, %.2f ms DSP per hop\n", st.mhzPerSec, pl.hops.size(), st.dspSecPerHop * 1e3);
}

static void testScanRecord() {
    const RadioProfile& rtl = radioProfile("rtlsdr");
    FastScanConfig c;
    c.plan.bands = {{102e6, 106e6}};
    c.plan.binHz = 5000;
    c.hits.onDb = 12; c.hits.offDb = 8; c.hits.hangSec = 0.2; c.hits.mergeHz = 10e3;
    FastScanner sc;
    std::string err;
    CHECK(sc.configure(c, rtl, err));
    const std::string dir = (std::filesystem::temp_directory_path() / "fastscan_scanrec_test").string();
    std::filesystem::remove_all(dir);
    RecorderBank rb;
    RecSettings s; s.dir = dir; s.maxLoad = 100;
    CHECK(rb.configure(sc.plan().rateHz, sc.plan().hops[0].centerHz, s));
    SimSource src(sc.plan().rateHz);
    RunHooks hk;
    hk.rec = &rb;
    hk.maxPasses = 40;
    hk.clock = [&] { return src.caps.load() * 0.05; };
    CHECK(sc.run(src, nullptr, hk, err));
    int wavs = 0;
    uint64_t bytes = 0;
    for (auto& e : std::filesystem::directory_iterator(dir)) if (e.path().extension() == ".wav") { wavs++; bytes += (uint64_t)e.file_size(); }
    // the burst is recorded as long as it lasts; the steady carrier too (an unmodulated carrier: silence, but a file)
    CHECK(wavs >= 1);
    CHECK(bytes > 44 * 2);
    CHECK(rb.stats().active == 0);
    std::filesystem::remove_all(dir);
}

// the real loop with the time a radio would take: scan speed against the model's ceiling
static void testScanSpeed() {
    struct Case { const char* id; double spanMhz; };
    for (const Case& k : {Case{"rtlsdr", 6}, Case{"airspy", 24}, Case{"hackrf", 45}}) {
        const RadioProfile& r = radioProfile(k.id);
        FastScanConfig c;
        c.plan.bands = {{100e6, 100e6 + k.spanMhz * 1e6}};
        c.plan.binHz = 12500;
        c.plan.dwellSec = 0.004;
        FastScanner sc;
        std::string err;
        CHECK(sc.configure(c, r, err));
        SimSource src(sc.plan().rateHz);
        src.realtime = true;
        RunHooks hk;
        hk.maxPasses = 4;
        hk.clock = [&] { return src.caps.load() * 0.05; };
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(sc.run(src, nullptr, hk, err));
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const ScanStats st = sc.stats();
        const double model = sc.plan().mhzPerSec;
        printf("  %-7s %2.0f MHz in %zu hops: measured %.0f MHz/s (planned %.0f, model ceiling %.0f); %.2f s for 4 passes\n", k.id, k.spanMhz, sc.plan().hops.size(), st.mhzPerSec,
               model, ceilingMhzPerSec(r, c.plan.dwellSec), sec);
        // timing sanity only: sleeps overshoot a lot on loaded CI runners (83 % of the plan when idle, well under 15 % on a busy Windows/macOS box);
        // it must never beat the plan and must still make progress
        CHECK(st.mhzPerSec > 0.02 * model && st.mhzPerSec < 1.4 * model);
    }
}

// ---------------------------------------------------------------- the DSP speed per radio
static void benchProfiles() {
    for (const char* id : {"rtlsdr", "airspy", "hackrf"}) {
        const RadioProfile& r = radioProfile(id);
        FastScanConfig c;
        c.plan.bands = {{100e6, 100e6 + 100 * r.rateHz * r.usableFrac}};
        c.plan.binHz = 12500;
        FastScanner sc;
        std::string err;
        if (!sc.configure(c, r, err)) { CHECK(false); continue; }
        const FastPlan& p = sc.plan();
        std::vector<cf32> buf(p.samplesPerHop);
        std::normal_distribution<float> nd(0.f, 0.001f);
        for (auto& s : buf) s = cf32(nd(rng), nd(rng));
        std::vector<HitEvent> ev;
        const int hopsN = (int)std::min<size_t>(60, p.hops.size());
        const auto t0 = std::chrono::steady_clock::now();
        for (int h = 0; h < hopsN; h++) sc.processHop(h, buf.data(), buf.size(), h * 0.01, ev);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double usable = p.binsPerHop * p.binHz / 1e6;
        const double dspMhz = usable * hopsN / sec;
        printf("  %-8s rate %5.2f Msps, %zu bins/hop, %d averages: DSP %.0f MHz/s (%.2f ms per hop); ceiling with the radio's retune %.0f MHz/s -> scan speed %.0f MHz/s\n",
               id, r.rateHz / 1e6, p.binsPerHop, p.averages, dspMhz, sec / hopsN * 1e3, ceilingMhzPerSec(r, c.plan.dwellSec),
               std::min(dspMhz, ceilingMhzPerSec(r, c.plan.dwellSec)));
        CHECK(dspMhz > 10);
    }
}


// ---------------------------------------------------------------- v2: confirmation, spurs, images, classes, whole span
struct Sig { double hz, amp, dev, modHz; long on0, onPeriod, onLen; bool mirror; };   // on: (k % onPeriod) in [on0, on0+onLen); onPeriod 0: always
struct MultiSim : sweep::SweepSource {
    double rate, center = 0;
    std::atomic<long> caps{0};
    std::mt19937 g{11};
    std::normal_distribution<float> nd{0.f, 1.f};
    std::vector<Sig> sigs;
    MultiSim(double r) : rate(r) {}
    bool retune(double c, std::string&) override { center = c; return true; }
    void discard(double) override {}
    double sampleRate() const override { return rate; }
    size_t capture(cf32* out, size_t n) override {
        const long k = caps++;
        for (size_t i = 0; i < n; i++) out[i] = cf32(nd(g), nd(g)) * 0.001f + cf32(0.003f, 0);
        for (const Sig& s : sigs) {
            if (s.onPeriod > 0 && (k % s.onPeriod < s.on0 || k % s.onPeriod >= s.on0 + s.onLen)) continue;
            auto add = [&](double f, double amp) {
                const double off = f - center;
                if (std::fabs(off) > rate * 0.4) return;
                for (size_t i = 0; i < n; i++) {
                    const double t = (double)(i + k * n) / rate;
                    const double a = 2 * M_PI * (off * t) + (s.modHz > 0 ? s.dev / s.modHz * (-std::cos(2 * M_PI * s.modHz * t)) : 0.0);
                    out[i] += cf32((float)std::cos(a), (float)std::sin(a)) * (float)amp;
                }
            };
            add(s.hz, s.amp);
            if (s.mirror) add(2 * center - s.hz, s.amp * 0.05);   // the radio's image: 26 dB down, mirrored about the tuned centre
        }
        return n;
    }
};

static void testQualifyPure() {
    CHECK(isImageOf(101.2e6, 10.f, 100.8e6, 40.f, 101.0e6, 20e3, 18.f));          // mirrored, 30 dB weaker
    CHECK(!isImageOf(101.2e6, 30.f, 100.8e6, 40.f, 101.0e6, 20e3, 18.f));         // only 10 dB weaker: a real signal
    CHECK(!isImageOf(101.3e6, 10.f, 100.8e6, 40.f, 101.0e6, 20e3, 18.f));         // not at the mirror position
    CHECK(!isImageOf(101.0e6, 10.f, 101.0e6, 40.f, 101.0e6, 20e3, 18.f));         // on the centre: DC, not an image
    CHECK(classify(145.5e6, 1000, 1) == SigClass::Carrier);
    CHECK(classify(145.5e6, 12000, 1) == SigClass::Voice);
    CHECK(classify(145.5e6, 25000, 0.3) == SigClass::Voice);
    CHECK(classify(145.5e6, 25000, 1) == SigClass::Narrowband);
    CHECK(classify(145.5e6, 80000, 1) == SigClass::Digital);
    CHECK(classify(98e6, 180000, 1) == SigClass::Broadcast);
    CHECK(classify(450e6, 180000, 1) == SigClass::Wideband);
    CHECK(classify(600e6, 6e6, 1) == SigClass::Digital);
    CHECK(classHint(125e6, 8000, 1) == "AM" && classHint(145e6, 8000, 1) == "NFM");
    for (const char* id : {"rtlsdr", "airspy", "hackrf", "sdrplay", "wide", "pluto", "generic"}) {
        const RadioProfile& r = radioProfile(id);
        const FreqRange w = wholeSpan(r);
        CHECK(w.hiHz > w.loHz);
        const ScanProfile p = wholeSpanProfile(r);
        FastScanConfig c = toConfig(p);
        FastPlan pl = planFast(c.plan, r);
        CHECK(!pl.hops.empty() && nearTo(pl.coveredHz, w.hiHz - w.loHz, 2 * pl.binHz * (double)pl.hops.size()));
        printf("  whole span %-8s %6.0f-%-6.0f MHz: %5zu hops, bin %.1f kHz, dwell %.1f ms -> model pass %.1f s (%.0f MHz/s), retune-limited ceiling %.0f MHz/s\n", id, w.loHz / 1e6,
               w.hiHz / 1e6, pl.hops.size(), pl.binHz / 1e3, pl.dwellSec * 1e3, pl.passSec, pl.mhzPerSec, ceilingMhzPerSec(r, pl.dwellSec));
    }
    CHECK(autoScan(radioProfile("rtlsdr"), 50e6).stepKhz == 12.5 && autoScan(radioProfile("hackrf"), 6e9).stepKhz == 50);
}

static void testQualifyScan() {
    const RadioProfile& rtl = radioProfile("rtlsdr");
    FastScanConfig c;
    c.plan.bands = {{100e6, 108e6}};
    c.plan.binHz = 10000;
    c.plan.dwellSec = 0.004;
    c.hits.onDb = 12; c.hits.offDb = 8; c.hits.hangSec = 0.05; c.hits.mergeHz = 25e3;
    FastScanner sc;
    std::string err;
    CHECK(sc.configure(c, rtl, err));
    const FastPlan& pl = sc.plan();
    const long H = (long)pl.hops.size();
    CHECK(H >= 4);
    auto at = [&](int hop, double off) { return pl.hops[hop].centerHz + off; };
    MultiSim src(pl.rateHz);
    // keyed voice: on for 6 sweeps, off for 6; the image of it is 26 dB down. A steady carrier (spur). A wide steady FM. A one-look flash.
    src.sigs.push_back({at(1, 300e3), 0.2, 3000, 1000, 0, H * 12, H * 6, true});
    src.sigs.push_back({at(2, -250e3), 0.05, 0, 0, 0, 0, 0, false});
    src.sigs.push_back({at(3, 200e3), 0.05, 40000, 7300, 0, 0, 0, false});
    src.sigs.push_back({at(0, 150e3), 0.1, 3000, 1000, H * 20 + 0, H * 100, 1, false});
    RunHooks hk;
    hk.maxPasses = 36;
    hk.clock = [&] { return src.caps.load() * 0.05; };
    CHECK(sc.run(src, nullptr, hk, err));
    const auto all = sc.channels(false), good = sc.channels(true);
    const double voice = at(1, 300e3), carrier = at(2, -250e3), fm = at(3, 200e3), flash = at(0, 150e3);
    auto find = [&](const std::vector<ChannelInfo>& v, double hz, double tol = 15e3) -> const ChannelInfo* {
        for (const ChannelInfo& x : v) if (nearTo(x.hit.freqHz, hz, tol)) return &x;
        return nullptr;
    };
    const ChannelInfo* v = find(good, voice);
    CHECK(v && v->confirmed && v->sweeps >= 3 && v->presence > 0.2f && v->presence < 0.9f);
    const ChannelInfo* w = find(good, fm, 40e3);
    CHECK(w && w->confirmed);
    const ChannelInfo* sp = find(all, carrier);
    CHECK(sp && sp->spur && !find(good, carrier));                             // steady narrow signal: flagged and hidden
    const ChannelInfo* im = find(all, 2 * pl.hops[1].centerHz - voice, 15e3);
    CHECK(im && im->image && !find(good, 2 * pl.hops[1].centerHz - voice, 15e3));   // the mirror of the voice signal
    const ChannelInfo* fl = find(all, flash);
    CHECK(!fl || (!fl->confirmed && !find(good, flash)));                      // seen in one sweep only: not reliable
    printf("  qualify: %zu raw hits -> %zu reliable (voice %s, spur %s, image %s, flash %s)\n", all.size(), good.size(), v ? "yes" : "no", sp && sp->spur ? "flagged" : "NO",
           im && im->image ? "flagged" : "NO", !fl ? "not seen" : (fl->confirmed ? "CONFIRMED" : "unconfirmed"));
}


// ---------------------------------------------------------------- HackRF firmware sweep: packets, plan, block-fed scan loop
struct SimBlocks : BlockSource {
    double rate;
    const FastPlan* plan = nullptr;
    long k = 0;
    std::mt19937 g{5};
    std::normal_distribution<float> nd{0.f, 1.f};
    double toneHz = 0;
    SimBlocks(double r) : rate(r) {}
    double sampleRate() const override { return rate; }
    bool next(double& c, std::vector<cf32>& out, const std::atomic<bool>*, std::string&) override {
        const int hop = (int)(k % (long)plan->hops.size());
        c = plan->hops[hop].centerHz + 1000.0;      // the firmware's centre is never exactly the plan's
        out.resize(8176);
        for (auto& s : out) s = cf32(nd(g), nd(g)) * 0.001f + cf32(0.003f, 0);
        const double off = toneHz - c;
        if (std::fabs(off) < rate * 0.4)
            for (size_t i = 0; i < out.size(); i++) { const double a = 2 * M_PI * off * (double)(i + k * out.size()) / rate; out[i] += cf32((float)std::cos(a), (float)std::sin(a)) * 0.05f; }
        k++;
        return true;
    }
};

static void testFirmwareSweep() {
    // packets: two valid blocks (one with the marker missing in between)
    std::vector<uint8_t> buf(3 * kSweepBlockBytes, 0);
    const uint64_t f0 = 2400000000ull;
    for (int blk : {0, 2}) {
        uint8_t* b = &buf[(size_t)blk * kSweepBlockBytes];
        b[0] = 0x7F; b[1] = 0x7F;
        const uint64_t f = f0 + (uint64_t)blk * 15000000ull;
        for (int i = 0; i < 8; i++) b[2 + i] = (uint8_t)(f >> (8 * i));
        reinterpret_cast<int8_t*>(b + 16)[0] = 64; reinterpret_cast<int8_t*>(b + 16)[1] = -64;
    }
    std::vector<double> centres; float first = 0;
    const size_t n = parseSweepBuffer(buf.data(), buf.size(), 7.5e6, [&](double c, const cf32* s, size_t m) { centres.push_back(c); if (centres.size() == 1) first = s[0].real(); CHECK(m == (kSweepBlockBytes - 16) / 2); });
    CHECK(n == 2 && centres.size() == 2 && nearTo(centres[0], 2407.5e6, 1) && nearTo(centres[1], 2437.5e6, 1) && nearTo(first, 0.5, 1e-6));
    CHECK(parseSweepBuffer(buf.data(), kSweepBlockBytes - 1, 0, [&](double, const cf32*, size_t) {}) == 0);   // a short buffer holds no block
    // the plan: whole MHz start, 15 MHz hops, one range
    const RadioProfile& hr = radioProfile("hackrf");
    FastPlanConfig w; w.bands = {{1e6, 6000e6}};
    const FwSweepPlan fp = planFirmwareSweep(w, hr);
    CHECK(fp.loMhz == 1 && fp.hiMhz == 6001 && nearTo(fp.stepHz, 15e6, 1) && nearTo(fp.offsetHz, 7.5e6, 1));
    FastPlanConfig w2; w2.bands = {{88.3e6, 107.9e6}};
    const FwSweepPlan f2 = planFirmwareSweep(w2, hr);
    CHECK(f2.loMhz == 88 && f2.hiMhz == 118);
    // the block loop: a tone is found in the right hop and passes are counted from the wraps
    FastScanConfig c;
    c.plan = f2.plan; c.plan.binHz = 40000; c.plan.dwellSec = 0.0004;
    c.hits.onDb = 12; c.hits.offDb = 8; c.hits.hangSec = 0.05; c.hits.mergeHz = 100e3;
    FastScanner sc;
    std::string err;
    RadioProfile r20 = hr; r20.rateHz = 20e6;
    CHECK(sc.configure(c, r20, err));
    SimBlocks src(20e6);
    src.plan = &sc.plan();
    src.toneHz = 95.2e6;
    CHECK(sc.plan().hops.size() == 2 && nearTo(sc.plan().hops[0].centerHz, 95.5e6, 2e5));
    RunHooks hk; hk.maxPasses = 6;
    hk.clock = [&] { return (double)src.k * 0.0005; };
    CHECK(sc.runBlocks(src, nullptr, hk, err));
    CHECK(sc.stats().passes == 6);
    bool found = false;
    for (const ChannelInfo& ch : sc.channels(true)) if (nearTo(ch.hit.freqHz, 95.2e6, 60e3)) found = true;
    CHECK(found);
    printf("  firmware sweep: packets parsed, plan 1-6000 MHz = sweep %u-%u MHz in %zu 15 MHz hops, block loop found the tone\n", fp.loMhz, fp.hiMhz, (size_t)((fp.hiMhz - fp.loMhz) / 15));
}

static void testProfile() {
    auto r = parseRanges("88-108, 121.5 ,, abc, 5-3, 462.5625-467.7125");
    CHECK(r.size() == 3);
    if (r.size() == 3) {
        CHECK(nearTo(r[0].loHz, 88e6, 1) && nearTo(r[0].hiHz, 108e6, 1));
        CHECK(nearTo(r[1].loHz, 121.49375e6, 1) && nearTo(r[1].hiHz, 121.50625e6, 1));
        CHECK(nearTo(r[2].hiHz, 467.7125e6, 1));
    }
    CHECK(parseRanges("").empty() && parseRanges(",;").empty());
    CHECK(formatRanges(parseRanges("88-108")) == "88-108");
    ScanProfile p;
    p.name = "My; band"; p.bands = "118-137"; p.exclusions = "121-122"; p.thresholdDb = 12; p.record = true; p.maxRecordings = 40; p.channelKhz = 25;
    const std::string s = serializeProfile(p);
    ScanProfile q;
    CHECK(parseProfile(s, q));
    CHECK(q.name == "My, band" && q.bands == "118-137" && q.exclusions == "121-122" && q.thresholdDb == 12 && q.record && q.maxRecordings == 40 && q.channelKhz == 25);
    ScanProfile bad;
    bad.name = "keep";
    CHECK(!parseProfile("nonsense", bad) && bad.name == "keep");
    CHECK(parseProfile("thr=999;maxrec=0;step=0", q) && q.thresholdDb == 40 && q.maxRecordings == 1 && q.stepKhz == 0.5);
    for (const ScanProfile& b : builtinProfiles()) {
        FastScanConfig c = toConfig(b);
        FastPlan pl = planFast(c.plan, radioProfile("rtlsdr"));
        CHECK(!pl.hops.empty());
        CHECK(c.hits.offDb < c.hits.onDb);
    }
    FastScanConfig c = toConfig(p);
    CHECK(c.hits.gridStepHz == 25000 && c.plan.exclusions.size() == 1 && c.hits.onDb == 12.f);
    Hit h; h.freqHz = 121.5e6;
    HitMemory m = hitToMemory(h, "am");
    CHECK(nearTo(m.mhz, 121.5, 1e-9) && m.mode == "am" && m.group == "Fast scan" && m.name.find("121.5000") != std::string::npos);
}

int main() {
    testProfile();
    testQualifyPure();
    testQualifyScan();
    testFirmwareSweep();
    testFloor();
    testCalibration();
    testCandidates();
    testTracker();
    testTrackerMore();
    testScene();
    testPlan();
    testWola();
    testRecordOne();
    testRecord100();
    testGuardAndFiles();
    testIdentifyNew();
    testScanLoop();
    testScanRecord();
    testScanSpeed();
    benchProfiles();
    printf(gFail ? "FAILED: %d\n" : "OK\n", gFail);
    return gFail ? 1 : 0;
}
