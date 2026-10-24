// The spectrum looks' pure maths (app/gfx/spec_look.h): the 1-2-5 frequency ticks and their labels, the peak-keeping pixel trace, the
// smoothing, the auto range, the waterfall view's level interpolation and low-pass (app/gfx/wf_view.h), the new maps and the presets.
#include "../../app/gfx/spec_look.h"
#include "../../app/gfx/colormap.h"
#include "../../app/gfx/wf_view.h"
#include "../../app/gfx/spec_pipe.h"
#include <cstdio>
#include <random>
#include <string>
#include <vector>
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }
static bool near(double a, double b, double e = 1e-9) { return std::fabs(a - b) <= e; }

int main() {
    using namespace slook;
    // 1-2-5 steps
    check(near(step125(0.7), 1) && near(step125(1), 1) && near(step125(1.3), 2) && near(step125(3), 5) && near(step125(6), 10) && near(step125(0.013), 0.02), "step125");
    check(near(nextStep125(1), 2) && near(nextStep125(2), 5) && near(nextStep125(5), 10), "next step");
    // labels: units and decimals
    check(tickLabel(100.5, 0.5, LabelsAbsolute, 0, 1) == "100.5", "MHz label");
    check(tickLabel(17.74, 0.01, LabelsAbsolute, 0, 1) == "17.74", "MHz 2 decimals");
    check(tickLabel(0.198, 0.001, LabelsAbsolute, 0, 0) == "198", "kHz label below 1 MHz");
    check(tickLabel(10500, 100, LabelsAbsolute, 0, 2) == "10.5", "GHz label");
    check(tickLabel(99.98, 0.005, LabelsOffset, 100.0, 1) == "-20K" && tickLabel(100.0, 0.005, LabelsOffset, 100.0, 1) == "0" && tickLabel(100.005, 0.005, LabelsOffset, 100.0, 1) == "5K", "offset K labels");
    check(tickLabel(101.5, 0.5, LabelsOffset, 100.0, 1) == "1.5K" || tickLabel(101.5, 0.5, LabelsOffset, 100.0, 1) == "1500K", "offset under 1 MHz step in K");
    check(tickLabel(102, 1, LabelsOffset, 100.0, 1) == "2M", "offset M labels");
    check(tickLabel(100.0125, 0.0025, LabelsOffset, 100.0, 1) == "12.5K", "offset decimals");
    // the shipped look shows absolute frequencies (3.0.0 showed offsets "0, 1, 2, -1, -2 M")
    check(lookSet(0).labelMode == LabelsAbsolute, "default look labels are frequencies");
    { const auto t = freqTicks(97, 99, 1000, 8, 28, lookSet(0).labelMode, 98.0);
      bool has98 = false, off = false;
      for (const auto& k : t) if (k.major && !k.label.empty()) { if (k.label == "98" || k.label == "98.0") has98 = true; if (k.label == "0" || k.label.back() == 'M' || k.label.back() == 'K') off = true; }
      check(has98 && !off, "FM span ticks are absolute MHz"); }
    check(freqUnit(0.1, 0.5) == 0 && freqUnit(87, 108) == 1 && freqUnit(10400, 10500) == 2, "units");
    // ticks over many spans and widths: majors on the step, labels never touch, never leave the axis, a minor between majors
    const double charPx = 8, gap = 28;
    for (double span : {0.001, 0.0123, 0.2, 2.4, 10.0, 20.0, 61.44}) {
        for (double w : {180.0, 640.0, 1000.0, 2560.0}) {
            for (int mode : {LabelsAbsolute, LabelsOffset}) {
                const double lo = 433.92 - span * 0.37, hi = lo + span;
                const auto t = freqTicks(lo, hi, w, charPx, gap, mode, 433.92);
                double lastEnd = -1e9; int majors = 0; bool overlap = false, outside = false, inRange = true;
                for (const auto& k : t) {
                    inRange = inRange && k.mhz >= lo - 1e-9 && k.mhz <= hi + 1e-9;
                    if (!k.major || k.label.empty()) continue;
                    majors++;
                    const double x = (k.mhz - lo) / span * w, half = k.label.size() * charPx / 2;
                    if (x - half < lastEnd + gap * 0.99) overlap = true;
                    if (x - half < 0 || x + half > w) outside = true;
                    lastEnd = x + half;
                }
                check(!overlap, "labels never touch");
                check(!outside, "labels stay on the axis");
                check(inRange, "ticks inside the range");
                if (w >= 640) check(majors >= 2, "at least two labels on a normal width");
            }
        }
    }
    {   // a concrete axis: 17.73 .. 17.805 MHz over 1100 px with 8 px digits: 0.01 MHz steps, minors at 2 kHz
        const auto t = freqTicks(17.73, 17.805, 1100, 8, 28, LabelsAbsolute, 0);
        int maj = 0, mn = 0;
        for (const auto& k : t) (k.major ? maj : mn)++;
        check(maj == 8 && mn > maj, "SDRoxide-like axis: 8 majors with minors");
        bool lab = false; for (const auto& k : t) if (k.label == "17.77") lab = true;
        check(lab, "label 17.77 present");
    }
    check(freqTicks(1, 1, 100, 8, 20, 0, 0).empty() && freqTicks(0, 1, 0, 8, 20, 0, 0).empty(), "degenerate axes");

    // the pixel trace: a one-bin peak among 65536 bins on 1000 pixels survives (max per column), the min keeps a one-bin dip
    {
        std::vector<float> y(65536, -100.f);
        y[31234] = -20.f; y[40000] = -140.f;
        std::vector<float> o;
        check(decimate(y.data(), y.size(), 0.0, 1.0, 0.0, 65536.0, 1000, InterpLinear, false, o) && o.size() == 1000, "decimate size");
        float mx = -1e9f; for (float v : o) mx = std::max(mx, v);
        check(mx == -20.f, "one-bin peak kept");
        check(o[(size_t)(31234 * 1000 / 65536)] == -20.f, "peak in its own column");
        decimate(y.data(), y.size(), 0.0, 1.0, 0.0, 65536.0, 1000, InterpLinear, true, o);
        float mn = 1e9f; for (float v : o) mn = std::min(mn, v);
        check(mn == -140.f, "one-bin dip kept by the min");
        // zoomed in: 10 bins over 1000 pixels, linear passes through the bin values at the bin centres, cubic stays near them
        std::vector<float> z = {-100, -90, -60, -30, -60, -90, -100, -95, -100, -100};
        decimate(z.data(), z.size(), 0.0, 1.0, -0.5, 9.5, 1000, InterpLinear, false, o);
        check(near(o[350], -30.0, 1.0), "linear: the peak bin's value at its centre");
        bool mono = true; for (int i = 151; i < 345; i++) mono = mono && o[i] >= o[i - 1] - 1e-4f;
        check(mono, "linear rises between rising bins");
        decimate(z.data(), z.size(), 0.0, 1.0, -0.5, 9.5, 1000, InterpCubic, false, o);
        float cm = -1e9f; for (float v : o) cm = std::max(cm, v);
        check(cm >= -30.5f && cm <= -30.f + 30.f * 0.25f + 0.01f, "cubic: the peak near the bin, a little overshoot at most");
        // the view outside the bins, and nonsense
        check(!decimate(z.data(), 1, 0, 1, 0, 1, 10, 0, false, o) && !decimate(z.data(), z.size(), 0, 1, 1, 1, 10, 0, false, o), "decimate refuses nonsense");
    }
    {   // smoothing: a [1 2 1] pass keeps a flat line, halves a one-pixel spike, keeps the ends
        std::vector<float> v(9, -100.f); v[4] = -20.f;
        smoothPasses(v, 1);
        check(near(v[4], -60.0, 1e-4) && near(v[3], -80.0, 1e-4) && v[0] == -100.f, "one pass");
        std::vector<float> f(20, -77.f); smoothPasses(f, 3);
        bool flat = true; for (float x : f) flat = flat && near(x, -77.0, 1e-4);
        check(flat, "flat stays flat");
    }
    {   // percentile and auto range
        std::vector<float> v; for (int i = 0; i < 100; i++) v.push_back((float)i);
        check(near(percentile(v, 0.2), 20, 1) && near(percentile(v, 0), 0) && near(percentile(v, 1), 99), "percentile");
        check(std::isnan(percentile({}, 0.5)), "percentile of nothing");
        float lo, hi;
        autoRange(-100.f, -40.f, 6.f, 2.f, 45.f, 12.f, lo, hi);
        check(lo == -106.f && hi == -55.f, "auto range: floor - 6, at most floor + 45");
        autoRange(-100.f, -80.f, 6.f, 2.f, 45.f, 12.f, lo, hi);
        check(lo == -106.f && hi == -78.f, "auto range: top + headroom");
        autoRange(-100.f, -99.f, 6.f, 2.f, 45.f, 12.f, lo, hi);
        check(hi - lo >= 12.f, "auto range: minimum span");
    }
    {   // levels and the low-pass
        check(lerpLevel(0, 200, 0.5f) == 100 && lerpLevel(10, 20, -1) == 10 && lerpLevel(10, 20, 2) == 20, "level interpolation");
        uint8_t a[5] = {10, 10, 10, 10, 10}, b[5] = {10, 10, 210, 10, 10}, c[5] = {10, 10, 10, 10, 10}, o[5];
        slook::lowpassRow(a, b, c, 5, o);
        check(o[2] == 60 && o[0] == 10 && o[1] > 10 && o[1] < o[2], "low-pass spreads a spike down and across");
        uint8_t f[4] = {77, 77, 77, 77}, g[4];
        slook::lowpassRow(nullptr, f, nullptr, 4, g);
        check(g[0] == 77 && g[3] == 77, "low-pass keeps a flat row");
    }
    {   // the waterfall view in Smooth mode: zoomed in it fills every pixel column, with levels in between the source columns
        const int W = 8192, H = 64;
        std::vector<uint8_t> lv((size_t)W * H, 0);
        for (int r = 0; r < H; r++) { lv[(size_t)r * W + 1000] = 0; lv[(size_t)r * W + 1001] = 200; }
        uint32_t lut[256]; for (int i = 0; i < 256; i++) lut[i] = 0xFF000000u | (uint32_t)i;
        plt::WfView v; v.mode = plt::WfView::Smooth;
        bool full = false; std::vector<int> dirty;
        check(v.update(lv.data(), W, H, 0, H - 1, H, lut, 1, 1000, 1002, 800, 1, full, dirty) && full && v.cols == 800, "smooth view: a pixel a column when zoomed in");
        const uint32_t* row = v.px.data() + (size_t)v.rowOf(H - 1) * v.stride;
        bool rising = true; int distinct = 0; uint32_t prev = 0xFFFFFFFF;
        for (int x = 200; x < 600; x++) { rising = rising && (row[x] & 255) >= (row[x - 1] & 255); if (row[x] != prev) distinct++; prev = row[x]; }
        check(rising && distinct > 100, "smooth view: a ramp, not two steps");
        plt::WfView n;   // the usual mode: no more columns than the source
        n.update(lv.data(), W, H, 0, H - 1, H, lut, 1, 1000, 1002, 800, 1, full, dirty);
        check(n.cols == 2, "usual view: two columns");
        v.mode = plt::WfView::Smooth | plt::WfView::LowPass;   // a mode change rebuilds
        check(v.update(lv.data(), W, H, 0, H - 1, H, lut, 1, 1000, 1002, 800, 1, full, dirty) && full, "mode change rebuilds");
    }
    {   // the looks and their maps
        check(NumLooks == 1 && !lookSet(OnAir).classic && std::string(lookSet(OnAir).name) == "OnAir" && &lookSet(1) == &lookSet(0) && &lookSet(2) == &lookSet(0), "one look; old saved numbers read as it");
        check(lookSet(0).fill && lookSet(0).strip && lookSet(0).interp == InterpCubic, "look traits");
        check(lookSet(0).wfMap == cmap::NavyHeat && kMapSdrppSmooth == cmap::SdrppSmooth && kMapOxide == cmap::OxideBlue
              && kMapNavyHeat == cmap::NavyHeat && kMapBlueGreen == cmap::BlueGreen, "look map (measured on the reference)");
        for (int m = 0; m < cmap::NumMaps; m++) {   // no competitor names in what the user sees
            const std::string nm = cmap::name(m);
            check(nm.find("SDR") == std::string::npos && nm.find("Oxide") == std::string::npos, "neutral palette names");
        }
        check(std::string(cmap::kTracePresets[kTracePaleCyan].name) == "Pale cyan" && std::string(cmap::kTracePresets[kTracePaleSteel].name) == "Pale steel", "measured trace colours");
        // Navy heat is SDR++'s Classic: navy floor (0,0,32), white at 5/14, yellow at 6/14, dark red at the top
        check(cmap::color(cmap::NavyHeat, 0.f) == 0xFF200000u && cmap::color(cmap::NavyHeat, 5.f / 14) == 0xFFFFFFFFu && cmap::color(cmap::NavyHeat, 6.f / 14) == 0xFF00FFFFu, "navy heat stops");
        const uint32_t bg = cmap::color(cmap::BlueGreen, 0.2f);
        check(((bg >> 16) & 255) > 100 && ((bg >> 8) & 255) < 20 && (bg & 255) < 5, "blue green: a mid blue floor");
        float b1, s1_, b2, s2_; floorPlace(cmap::NavyHeat, b1, s1_); floorPlace(cmap::BlueGreen, b2, s2_);
        check(b1 < b2 && s1_ > s2_ && s2_ > 30, "floor places: Navy heat's floor at the dark end, Blue green's in its mid blue");
        check(kTraceLightBlue < cmap::kNumTracePresets && std::string(cmap::kTracePresets[kTraceLightBlue].name) == "Light blue" && std::string(cmap::kTracePresets[kTraceCyan].name) == "Cyan", "look trace presets");
        check(std::string(cmap::name(cmap::SdrppSmooth)) == "Smooth heat" && std::string(cmap::name(cmap::OxideBlue)) == "Deep blue" && std::string(cmap::name(cmap::SdrClassic)) == "Deep heat", "map names");
        const uint32_t s0 = cmap::color(cmap::SdrppSmooth, 0.f), s1 = cmap::color(cmap::SdrppSmooth, 1.f), o0 = cmap::color(cmap::OxideBlue, 0.02f);
        check(s0 == 0xFF000000u && s1 == 0xFFFFFFFFu, "SDR++ smooth: black to white");
        check(((o0 >> 16) & 255) > (o0 & 255) && ((o0 >> 16) & 255) > ((o0 >> 8) & 255), "Oxide: deep blue at the floor");
        // no hard steps: neighbouring levels never jump by more than a little in any channel
        for (int m : {(int)cmap::SdrppSmooth, (int)cmap::OxideBlue}) {
            int worst = 0;
            for (int i = 1; i < 256; i++) {
                const uint32_t p = cmap::color(m, (i - 1) / 255.f), q = cmap::color(m, i / 255.f);
                for (int sh : {0, 8, 16}) worst = std::max(worst, std::abs((int)((p >> sh) & 255) - (int)((q >> sh) & 255)));
            }
            check(worst <= 12, "look maps are smooth");
        }
    }
    {   // the test spectrum: carriers well over the floor, the OFDM block flat, deterministic
        std::vector<float> d, e;
        demoSpectrum(d, 8192, 100.0, 10.0, 1.5, 7);
        demoSpectrum(e, 8192, 100.0, 10.0, 1.5, 7);
        check(d == e && d.size() == 8192, "demo deterministic");
        std::vector<float> c(d); const float fl = percentile(c, 0.2);
        const float fm = *std::max_element(d.begin() + 8192 * 13 / 100, d.begin() + 8192 * 15 / 100);
        check(fl > -110 && fl < -95 && fm > fl + 40, "demo: floor and an FM carrier");
        float ofdm = 0; for (int i = 8192 * 40 / 100; i < 8192 * 48 / 100; i++) ofdm += d[(size_t)i];
        ofdm /= 8192 * 8 / 100.f;
        check(ofdm > fl + 15 && ofdm < fl + 40, "demo: the OFDM block");
    }
    {   // the pipeline steps (spec_pipe.h): the floor, its tracker, the floor-anchored range, the levels
        std::vector<float> s(4096, -100.f), scr;
        for (size_t i = 0; i < s.size(); i++) s[i] = -100.f + (float)(i % 7) * 0.5f;   // noise between -100 and -97
        for (size_t i = 1000; i < 3000; i++) s[i] = -60.f;                           // a block over half the band
        const float fl = spipe::noiseFloor(s.data(), s.size(), scr);
        check(fl >= -100.f && fl <= -99.f, "floor: the low part of the noise, not the block");
        spipe::FloorTracker t;
        check(t.update(-90.f, 0.25f) == -90.f, "tracker: the first value at once");
        const float a1 = t.update(-80.f, 0.25f);
        check(a1 > -90.f && a1 < -88.f, "tracker: a step is followed slowly (2 s)");
        check(t.update(-40.f, 0.25f) == -40.f, "tracker: a jump of more than 15 dB at once");
        float lo, hi;
        spipe::floorRange(-100.f, 3.f, 80.f, lo, hi);
        check(lo == -103.f && hi == -23.f, "floor range");
        std::vector<uint8_t> l(4);
        const float row[8] = {-103, -103, -63, -103, -23, -20, -200, -103};
        spipe::levelsRow(row, 8, 4, -103.f, -23.f, l.data());
        check(l[0] == 0 && l[1] == 128 && l[2] == 255 && l[3] == 0, "levels: the highest bin of each column, clamped");
        std::vector<float> sm(3, 0.f); const float in[3] = {10, 10, 10};
        spipe::emaStep(sm.data(), in, 3, 1.f / 30, 0.f);
        check(sm[0] == 10.f, "ema: tau 0 is the raw frame");
    }
    {   // the owner's comb (a big FFT on a 1600 px plot drew a wall of spikes): ADS-B-like noise at 1024..131072 bins over 4 MHz, two FFT
        // blocks a frame (chi-square noise per bin), 3 s of frames through the power average whose time constant follows the bins per
        // pixel, then one value per pixel column (the highest bin): the noise floor's roughness stays under 1.5 dB and, per Hz, its level
        // moves less than 2.5 dB from the smallest to the largest FFT
        std::mt19937 rng(7);
        std::exponential_distribution<float> ex(1.f);
        const double fs = 4e6; const int px = 1600;
        float lo = 1e9f, hi = -1e9f;
        for (size_t n = 1024; n <= 131072; n *= 2) {
            const float off = spipe::perHzOffset(fs, n);
            std::vector<float> fr(n), sm(n), pw, col;
            const double bpp = (double)n / px;
            const float alpha = spipe::alphaFor(1.f / 30, spipe::traceTau(0.08f, bpp));
            for (int f = 0; f < 90; f++) {
                for (size_t i = 0; i < n; i++) fr[i] = 10.f * std::log10((ex(rng) + ex(rng)) * 0.5f * (float)(fs / n) * 1e-12f) - off;   // -120 dBFS/Hz noise
                spipe::powerEma(pw, fr.data(), sm.data(), n, alpha);
            }
            slook::decimate(sm.data(), n, -fs / 2e6, fs / 1e6 / n, -fs / 2e6, fs / 2e6, px, InterpCubic, false, col);
            const double r = spipe::roughness(col.data(), col.size());
            std::vector<float> c(col); std::nth_element(c.begin(), c.begin() + (long)c.size() / 2, c.end()); const float fl = c[c.size() / 2];   // the median column
            lo = std::min(lo, fl); hi = std::max(hi, fl);
            char w[96]; snprintf(w, sizeof w, "smooth trace at %zu bins (roughness %.2f dB)", n, r);
            check(r < 1.5, w);
        }
        char w[96]; snprintf(w, sizeof w, "per Hz the floor stays (%.1f .. %.1f dB)", lo, hi);
        check(hi - lo < 2.5f && lo > -125 && hi < -115, w);
        check(std::fabs(spipe::fastDb10(spipe::fastPow10db(-73.3f)) + 73.3f) < 0.01f && std::fabs(spipe::fastPow10db(10.f) - 10.f) < 0.001f, "fast dB maths");
    }
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("spec_look: all passed\n");
    return 0;
}
