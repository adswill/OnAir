// Pure plot helpers: trace decimation keeps narrow peaks, frequency ticks get the decimals their step needs, units adapt.
#include "../../app/gfx/plot_util.h"
#include "../../app/gfx/colormap.h"
#include "../../app/gfx/wf_view.h"
#include <cstdio>
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }
int main() {
    std::vector<float> y(100000, -90.f);
    y[54321] = -20.f; y[7000] = -140.f;   // a one-bin spike and a one-bin notch
    std::vector<double> ox, oy;
    plt::envelope(y, 100.0, 1e-4, 100.0, 110.0, 800, ox, oy);
    check(ox.size() == oy.size() && ox.size() <= 1600, "envelope size bounded by 2 per column");
    float mx = -1e9f, mn = 1e9f; for (double v : oy) { mx = std::max(mx, (float)v); mn = std::min(mn, (float)v); }
    check(mx == -20.f && mn == -140.f, "envelope keeps the narrow peak and notch");
    for (size_t i = 1; i < ox.size(); i++) check(ox[i] >= ox[i - 1], "envelope x ascending");
    plt::envelope(y, 100.0, 1e-4, 100.0, 100.01, 800, ox, oy);   // zoomed in: every bin
    check(ox.size() >= 100 && ox.size() < 110, "envelope zoomed in keeps bins");
    plt::envelope(std::vector<float>{1.f}, 0, 1, 0, 1, 10, ox, oy);
    check(ox.empty(), "envelope of one bin is empty");
    check(plt::formatFreqTick(100.5, 0.5) == "100.5", "tick 0.5 MHz step");
    check(plt::formatFreqTick(100.005, 0.005) == "100.005", "tick 5 kHz step");
    check(plt::formatFreqTick(102, 2) == "102", "tick 2 MHz step");
    check(plt::formatFreqTick(0, 0.1) == "0.0", "tick zero");
    check(plt::formatFreqUnit(1200) == "1.2000 GHz", "unit GHz");
    check(plt::formatFreqUnit(100.5) == "100.5000 MHz", "unit MHz");
    check(plt::formatFreqUnit(0.0125) == "12.50 kHz", "unit kHz");
    check(plt::formatFreqUnit(0.0008) == "800 Hz", "unit Hz");
    check(plt::fillBottomAlpha(0.85f) > 0.149f && plt::fillBottomAlpha(0.85f) < 0.151f && plt::fillBottomAlpha(2) == 0 && plt::fillBottomAlpha(-1) == 1, "fill alpha clamp");
    // colour maps: opaque, the sequential maps get brighter from 0 to 1 (luminance never drops by more than a hair), endpoints as designed
    auto lum = [](uint32_t c) { return 0.2126f * (c & 255) + 0.7152f * ((c >> 8) & 255) + 0.0722f * ((c >> 16) & 255); };
    for (int m = 0; m < cmap::NumMaps; m++) {
        bool opaque = true; float prev = -1, worst = 0;
        for (int i = 0; i <= 255; i++) {
            const uint32_t c = cmap::color(m, i / 255.f);
            opaque = opaque && (c >> 24) == 0xFF;
            const bool inv = m == cmap::Paper;   // Paper is the Daylight look: light floor, dark peaks, so its luminance falls by design
            if (!cmap::hueRamp(m) && prev >= 0) worst = std::max(worst, inv ? lum(c) - prev : prev - lum(c));   // Turbo, SDR++ classic, high contrast: rainbows, not monotone by design
            prev = lum(c);
        }
        check(opaque, "colour map opaque");
        check(worst < 1.5f, "colour map luminance rises");
        check(m == cmap::Paper ? lum(cmap::color(m, 1.f)) < lum(cmap::color(m, 0.f)) : lum(cmap::color(m, 1.f)) > lum(cmap::color(m, 0.f)), "colour map end brighter than start (darker for Paper)");
        check(cmap::color(m, -3.f) == cmap::color(m, 0.f) && cmap::color(m, 9.f) == cmap::color(m, 1.f), "colour map clamps");
    }
    check(cmap::color(cmap::Viridis, 0.f) == (0xFF000000u | (84u << 16) | (1u << 8) | 68u), "viridis start colour");
    check(cmap::applyGamma(0.5f, 1.f) == 0.5f && cmap::applyGamma(0.5f, 2.f) == 0.25f && cmap::applyGamma(2.f, 2.f) == 1.f, "gamma");
    check(plt::scatterAlphaFactor(100) == 1.f && plt::scatterAlphaFactor(2400) > 0.49f && plt::scatterAlphaFactor(2400) < 0.51f && plt::scatterAlphaFactor(1000000) == 0.22f, "scatter alpha factor");
    // the waterfall view: a one-column, one-row spike survives both reductions; rows added one by one give the same picture as a rebuild
    {
        const int W = 4096, H = 512;
        std::vector<uint8_t> lv((size_t)W * H, 10);
        uint32_t lut[256]; for (int i = 0; i < 256; i++) lut[i] = 0xFF000000u | (uint32_t)i;
        long long R = -1; int wr = 0, have = 0;
        auto push = [&](int spikeCol) {
            wr = (wr + H - 1) % H;
            std::fill(lv.begin() + (size_t)wr * W, lv.begin() + (size_t)(wr + 1) * W, (uint8_t)10);
            if (spikeCol >= 0) lv[(size_t)wr * W + spikeCol] = 200;
            R++; have = std::min(H, have + 1);
        };
        for (int i = 0; i < 700; i++) push(i == 650 ? 3001 : -1);   // the spike 49 rows ago
        check(plt::WfView::groupRows(512, 400, H) == 2 && plt::WfView::groupRows(512, 600, H) == 1 && plt::WfView::groupRows(512, 100, H) == 8, "group rows");
        plt::WfView a, b; bool full = false; std::vector<int> dirty;
        check(a.update(lv.data(), W, H, wr, R, have, lut, 1, 0, W, 1000, 2, full, dirty) && full && a.cols == 1000 && a.ring == 257, "view built");
        int hits = 0; for (uint32_t c : a.px) if ((c & 255) == 200) hits++;
        check(hits == 1, "view keeps a one-column one-row spike");
        check(!a.update(lv.data(), W, H, wr, R, have, lut, 1, 0, W, 1000, 2, full, dirty), "view: nothing new, nothing done");
        for (int i = 0; i < 5; i++) push(i == 2 ? 17 : -1);
        check(a.update(lv.data(), W, H, wr, R, have, lut, 1, 0, W, 1000, 2, full, dirty) && !full && !dirty.empty(), "view: new rows, some rows");
        b.update(lv.data(), W, H, wr, R, have, lut, 1, 0, W, 1000, 2, full, dirty);
        check(a.px == b.px, "view: rows one by one == a rebuild");
        check(a.stride == 1001 && a.rowsAlloc == 258, "view: one column and one row of padding");
        bool padOk = std::equal(a.px.begin(), a.px.begin() + a.stride, a.px.begin() + (size_t)a.ring * a.stride);
        for (int r = 0; r < a.rowsAlloc; r++) padOk = padOk && a.px[(size_t)r * a.stride + a.cols] == a.px[(size_t)r * a.stride + a.cols - 1];
        check(padOk, "view: the padding copies the last column and row 0");
        for (int i = 0; i < 600; i++) {   // incrementally across many wraps, and the seam row stays a copy of row 0
            push(-1);
            a.update(lv.data(), W, H, wr, R, have, lut, 1, 0, W, 1000, 2, full, dirty);
        }
        b.update(lv.data(), W, H, wr, R, have, lut, 1, 0, W, 1000, 2, full, dirty);
        check(a.px == b.px, "view: after wrapping, rows one by one == a rebuild (padding too)");
        check(a.update(lv.data(), W, H, wr, R, have, lut, 1, 1000, 1200, 1000, 2, full, dirty) && full && a.cols == 200, "view zoomed in: no more columns than the source");
        check(a.update(lv.data(), W, H, wr, R, have, lut, 2, 1000, 1200, 1000, 2, full, dirty) && full, "view: a new colour table rebuilds");
    }
    if (!fails) printf("plot_util ok\n");
    return fails ? 1 : 0;
}
