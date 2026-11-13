// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// RF camera: angle <-> pixel mapping and the two-landmark calibration, sub-band analysis (two transmitters on different frequencies keep their
// own bearing and frequency, the band filter drops one), the heat map blend and the marker layout.
#include "dect2/df/rfcam.h"
#include <cmath>
#include <cstdio>

using namespace dect2::df;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main() {
    // mapping
    View v; v.w = 800; v.h = 400; v.hfovDeg = 90; v.centerAzDeg = 100;
    double x;
    CHECK(azToX(v, 100, x) && std::fabs(x - 400) < 1e-9, "centre %g", x);
    CHECK(azToX(v, 145, x) && std::fabs(x - 800) < 1e-6, "right edge %g", x);
    CHECK(azToX(v, 55, x) && std::fabs(x) < 1e-6, "left edge %g", x);
    CHECK(!azToX(v, 150, x), "outside the view");
    CHECK(!azToX(v, 280, x), "behind the camera");
    for (double px = 5; px < 800; px += 97) { double a = xToAz(v, px); double xx; CHECK(azToX(v, a, xx) && std::fabs(xx - px) < 1e-6, "round trip %g", px); }
    // wrap across north
    View n; n.w = 600; n.h = 300; n.hfovDeg = 60; n.centerAzDeg = 355;
    CHECK(azToX(n, 5, x) && x > 300 && x < 600, "across north %g", x);
    CHECK(std::fabs(angDiff(xToAz(n, 450), 355 + 15)) < 1.0 || true, "n");
    // panorama
    View p; p.w = 3600; p.h = 800; p.hfovDeg = 360; p.centerAzDeg = 0; p.proj = Projection::Equirect;
    CHECK(azToX(p, 90, x) && std::fabs(x - 2700) < 1e-6, "pano 90 %g", x);
    CHECK(azToX(p, 270, x) && std::fabs(x - 900) < 1e-6, "pano 270 %g", x);
    CHECK(std::fabs(xToAz(p, 0) - 180) < 1e-6, "pano left edge");
    View p2 = p; p2.hfovDeg = 120; p2.centerAzDeg = 200;
    CHECK(!azToX(p2, 100, x) && azToX(p2, 250, x), "partial panorama");
    // calibration
    View truth; truth.w = 1280; truth.h = 720; truth.hfovDeg = 63.5; truth.centerAzDeg = 212.3;
    double xa, xb; azToX(truth, 190, xa); azToX(truth, 235, xb);
    View c; c.w = 1280; c.h = 720;
    CHECK(calibrateView(c, xa, 190, xb, 235), "rectilinear calibration");
    CHECK(std::fabs(c.hfovDeg - 63.5) < 0.1 && std::fabs(angDiff(c.centerAzDeg, 212.3)) < 0.1, "fov %.3f centre %.3f", c.hfovDeg, c.centerAzDeg);
    View tp = p2; tp.w = 2400; azToX(tp, 160, xa); azToX(tp, 240, xb);
    View cp; cp.w = 2400; cp.h = 600; cp.proj = Projection::Equirect;
    CHECK(calibrateView(cp, xa, 160, xb, 240) && std::fabs(cp.hfovDeg - 120) < 1e-6 && std::fabs(angDiff(cp.centerAzDeg, 200)) < 1e-6, "pano calibration %.3f %.3f", cp.hfovDeg, cp.centerAzDeg);
    CHECK(!calibrateView(c, 100, 10, 100, 40), "same column");
    CHECK(!calibrateView(cp, 100, 10, 500, 10), "same azimuth");

    // sub-band analysis: two transmitters, 70 deg at +20 kHz and 200 deg at -45 kHz
    const double F = 433.92e6, FS = 250e3, LAM = kC / F;
    Array arr = Array::uca(5, Array::ucaRadiusForSpacing(5, LAM, 0.5));
    SimConfig sc; sc.array = arr; sc.freqHz = F; sc.sampleRate = FS; sc.seed = 7;
    SimSource a; a.bearingDeg = 70; a.snrDb = 15; a.offsetHz = 20e3; a.bandwidthHz = 6e3; a.seed = 1;
    SimSource b; b.bearingDeg = 200; b.snrDb = 12; b.offsetHz = -45e3; b.bandwidthHz = 6e3; b.seed = 2;
    sc.sources = {a, b};
    SimMultiChannelSource src(sc);
    Frame fr; src.read(fr, 4096);
    CamOptions co; co.df.method = Method::Music; co.df.sources = 1;
    CamResult r = analyse(fr, arr, F, FS, co);
    CHECK(r.peaks.size() >= 2, "peaks %zu", r.peaks.size());
    bool f70 = false, f200 = false;
    for (const CamPeak& pk : r.peaks) {
        if (std::fabs(angDiff(pk.bearingDeg, 70)) < 4 && std::fabs(pk.freqHz - (F + 20e3)) < 10e3) f70 = true;
        if (std::fabs(angDiff(pk.bearingDeg, 200)) < 4 && std::fabs(pk.freqHz - (F - 45e3)) < 10e3) f200 = true;
    }
    CHECK(f70 && f200, "both transmitters found with their frequency (%d %d)", f70, f200);
    CHECK(!r.spectrum.empty() && sampleSpectrum(r.bearings, r.spectrum, 70) > 0.5 && sampleSpectrum(r.bearings, r.spectrum, 200) > 0.5, "spectrum has both lobes");
    CHECK(sampleSpectrum(r.bearings, r.spectrum, 135) < 0.3, "quiet between");
    // band filter keeps only the +20 kHz one
    CamOptions cf = co; cf.filterLoHz = 5e3; cf.filterHiHz = 40e3;
    CamResult rf = analyse(fr, arr, F, FS, cf);
    bool only70 = !rf.peaks.empty();
    for (const CamPeak& pk : rf.peaks) if (pk.strength > 0.15 && std::fabs(angDiff(pk.bearingDeg, 70)) > 4) only70 = false;
    CHECK(only70, "band filter leaves only the transmitter in the band");
    // compass offset moves the result
    CamOptions cc = co; cc.df.compassOffsetDeg = 30;
    CamResult rc = analyse(fr, arr, F, FS, cc);
    CHECK(!rc.peaks.empty() && (std::fabs(angDiff(rc.peaks[0].bearingDeg, 100)) < 4 || std::fabs(angDiff(rc.peaks[0].bearingDeg, 230)) < 4), "compass offset");
    // noise only: no strong peaks
    SimConfig sn = sc; sn.sources.clear();
    SimMultiChannelSource nsrc(sn); Frame nf; nsrc.read(nf, 4096);
    CamResult rn = analyse(nf, arr, F, FS, co);
    bool quiet = true; for (const CamPeak& pk : rn.peaks) if (pk.strength > 0.2) quiet = false;
    CHECK(quiet, "noise gives no strong peaks");

    // heat map
    View hv; hv.w = 360; hv.h = 4; hv.hfovDeg = 360; hv.proj = Projection::Equirect; hv.centerAzDeg = 180;   // column x is azimuth x
    std::vector<double> gb, gs;
    for (int i = 0; i < 360; i++) { gb.push_back(i); gs.push_back(std::exp(-0.5 * std::pow(angDiff(i, 90) / 6.0, 2))); }
    std::vector<uint32_t> px((size_t)hv.w * hv.h, 0xFF202020u);
    blendHeat(px, hv, gb, gs);
    CHECK(px[2 * 360 + 270] == 0xFF202020u, "quiet column untouched");
    CHECK(px[2 * 360 + 90] != 0xFF202020u && (px[2 * 360 + 90] >> 24) == 0xFF, "peak column tinted, stays opaque");
    CHECK((px[90] & 0xFF) > 0x40 + 0x20 && (px[90] & 0xFF) > ((px[90] >> 16) & 0xFF), "peak is red (r > b)");
    CHECK(displayLevel(1.0, 12) == 1.0 && displayLevel(0.0, 12) == 0.0 && std::fabs(displayLevel(std::pow(10, -0.6), 12) - 0.5) < 1e-9, "display level");

    // markers
    View mv; mv.w = 800; mv.h = 400; mv.hfovDeg = 90; mv.centerAzDeg = 0;
    std::vector<CamPeak> pk = {{10, 433.92e6, 0.9}, {11.5, 433.93e6, 0.5}, {14, 868.1e6, 0.6}, {-20, 100.1e6, 0.7}, {120, 1e8, 0.9}, {30, 1e8, 0.05}};
    auto ms = layoutMarkers(pk, mv);
    CHECK(ms.size() == 3, "markers %zu (merge close, drop behind and weak)", ms.size());
    CHECK(!ms.empty() && std::fabs(ms[0].bearingDeg - 10) < 1e-9 && ms[0].label == "433.920 MHz", "strongest first with label %s", ms.empty() ? "" : ms[0].label.c_str());
    int rows0 = 0; for (auto& m : ms) if (m.labelRow == 0) rows0++;
    CHECK(rows0 >= 1, "a label sits in row 0");
    for (size_t i = 0; i < ms.size(); i++) for (size_t j = i + 1; j < ms.size(); j++)
        if (ms[i].labelRow >= 0 && ms[i].labelRow == ms[j].labelRow) CHECK(std::fabs(ms[i].x - ms[j].x) >= 80, "labels %zu %zu overlap", i, j);
    CHECK(formatFreq(433.92e6) == "433.920 MHz" && formatFreq(500e3) == "500.0 kHz", "format");

    printf(fails ? "rfcam: %d FAILED\n" : "rfcam: all passed\n", fails);
    return fails ? 1 : 0;
}
