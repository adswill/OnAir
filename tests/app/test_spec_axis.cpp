// The shared frequency axis of the spectrum and the waterfall (app/gfx/spec_axis.h) and the colour additions (app/gfx/colormap.h).
#include "../../app/gfx/spec_axis.h"
#include "../../app/gfx/colormap.h"
#include "../../app/gfx/wf_view.h"
#include <cstdio>
#include <vector>
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }
static bool near(double a, double b, double e = 1e-9) { return std::fabs(a - b) <= e; }

int main() {
    using namespace specaxis;
    // #270: the spectrum (tick labels at its bottom) and the waterfall (none) are flush: the same width, so the same pixels per Hz
    check(rightOverhang(true, true, 2.f) == rightOverhang(false, true, 2.f), "flush plots: same right margin with or without x labels");
    check(rightOverhang(true, false, 2.f) == 24.f && rightOverhang(false, false, 2.f) == 0.f, "a free plot keeps room for its last label");
    // a bin's centre and the picture's edge: bin i's colour is drawn around the trace point of bin i, for every FFT size and history width
    const double c = 100.0, fs = 2.4;
    for (size_t n : {4096u, 8192u, 65536u, 262144u}) {
        for (int cols : {8192, 16384, 65536}) {
            const double lo = imageLo(c, fs, n);
            // column x shows bins [x*n/cols, ...): for n >= cols the middle of those bins, for n < cols the bin x*n/cols
            for (int x : {0, cols / 3, cols - 1}) {
                if (n >= (size_t)cols) {   // column x: the middle of its k bins
                    const size_t k = n / cols;
                    check(near(lo + (x + 0.5) * fs / cols, binCentre(c, fs, n, (double)x * k + (k - 1) / 2.0)), "waterfall column sits under its bins");
                } else {   // bin b fills r columns: the middle of them
                    const int r = cols / (int)n, b = (int)((size_t)x * n / cols);
                    check(near(lo + (b * r + r / 2.0) * fs / cols, binCentre(c, fs, n, b)), "waterfall columns sit under their bin");
                }
            }
        }
    }
    check(near(binCentre(c, fs, 8192, 4096), c), "the middle bin is the centre (DC)");
    // the columns a view shows
    int c0, c1;
    visibleCols(imageLo(c, fs, 8192), imageLo(c, fs, 8192) + fs, imageLo(c, fs, 8192), fs, 8192, c0, c1);
    check(c0 == 0 && c1 == 8192, "the whole band: every column");
    visibleCols(c - 0.1, c + 0.1, imageLo(c, fs, 8192), fs, 8192, c0, c1);
    check(c0 < c1 && c1 - c0 <= (int)std::ceil(0.2 / fs * 8192) + 2, "a zoomed view: about its share of the columns");
    visibleCols(c - 10, c + 10, imageLo(c, fs, 8192), fs, 8192, c0, c1);
    check(c0 == 0 && c1 == 8192, "clamped to the history");
    // a retune moves the history so the old rows line up with the new axis
    check(retuneShift(100.0, 100.3, 2.4, 8192) == -1024, "up by an eighth of the band: an eighth to the left");
    check(retuneShift(100.0, 99.7, 2.4, 8192) == 1024, "down: to the right");
    check(retuneShift(100.0, 150.0, 2.4, 8192) == -8192 && retuneShift(100.0, 50.0, 2.4, 8192) == 8192, "far: all of it");
    check(retuneShift(100.0, 100.0, 2.4, 8192) == 0 && retuneShift(100, 101, 0, 8192) == 0, "no move / no rate");
    {
        std::vector<uint8_t> r = {1, 2, 3, 4, 5};
        shiftRow(r.data(), 5, 2, 0);
        check(r == std::vector<uint8_t>({0, 0, 1, 2, 3}), "shift right");
        r = {1, 2, 3, 4, 5}; shiftRow(r.data(), 5, -2, 9);
        check(r == std::vector<uint8_t>({3, 4, 5, 9, 9}), "shift left");
        r = {1, 2, 3}; shiftRow(r.data(), 3, 7, 0);
        check(r == std::vector<uint8_t>({0, 0, 0}), "shift past the edge clears");
    }
    // pixels and frequencies, both plots the same
    check(near(pxToMhz(150, 100, 200, 99, 101), 99.5) && near(mhzToPx(99.5, 100, 200, 99, 101), 150), "pixel <-> MHz");
    check(near(pxToMhz(mhzToPx(100.123, 7, 999, 99, 101), 7, 999, 99, 101), 100.123), "round trip");
    // the band-edge drag (#274)
    check(edgeUnder(103, 100, 200, 5) == 0 && edgeUnder(198, 100, 200, 5) == 1 && edgeUnder(150, 100, 200, 5) == -1, "edge under the mouse");
    check(edgeUnder(101, 100, 104, 5) == 0 && edgeUnder(103.5, 100, 104, 5) == 1, "close edges: the nearer");
    check(near(widthFromEdge(100.0, 100.09, 0.1, 0.3, 0.01), 0.18), "width: twice the distance, on the step");
    check(near(widthFromEdge(100.0, 100.5, 0.1, 0.3, 0.01), 0.3) && near(widthFromEdge(100.0, 100.0, 0.1, 0.3, 0.01), 0.1), "width clamps");
    // the history width (#243)
    check(historyCols(0, 65536) == 8192, "standard width");
    check(historyCols(32768, 65536) == 32768 && historyCols(262144, 262144) == 65536, "finer, with a cap");
    check(historyCols(65536, 16384) == 16384 && historyCols(65536, 4096) == 8192, "no wider than the FFT, never under 8192");
    check(historyCols(-1, 32768) == 32768 && historyCols(-1, 262144) == 65536 && historyCols(-1, 4096) == 8192, "auto: as fine as the FFT, with the cap");
    // frame pacing (#246)
    Pace p; p.running = true; p.focused = false; p.liveWaterfall = true;
    check(near(frameInterval(p), 1.0 / 30), "unfocused live waterfall keeps the spectrum's rate");
    p.liveWaterfall = false;
    check(near(frameInterval(p), 1.0 / 15), "unfocused, waterfall paused: saves CPU as before");
    p.shown = false;
    check(frameInterval(p) >= 0.1, "hidden: slow");
    p.shot = true;
    check(frameInterval(p) == 0, "--shot: unpaced");
    check(missedRows(0, 5) == 0 && missedRows(5, 6) == 0 && missedRows(5, 8) == 2 && missedRows(5, 100) == 8 && missedRows(9, 3) == 0, "missed spectrum frames");
    // the waterfall view keeps working for a wider history
    {
        const int W = 32768, H = 16;
        std::vector<uint8_t> lv((size_t)W * H, 0);
        for (int r = 0; r < H; r++) lv[(size_t)r * W + 20000] = 200;
        uint32_t lut[256]; for (int i = 0; i < 256; i++) lut[i] = 0xFF000000u | (uint32_t)i;
        plt::WfView v; bool full; std::vector<int> d;
        check(v.update(lv.data(), W, H, 0, H - 1, H, lut, 1, 0, W, 1000, 1, full, d) && full, "view of a wide history");
        int hits = 0; for (int x = 0; x < v.cols; x++) if ((v.px[x] & 255) == 200) hits++;
        check(hits == 1, "a single-column signal is kept when squeezed");
    }
    // markers
    {
        const std::vector<double> m = parseMarkers("100.5, 433.92,junk,,-3,145.8");
        check(m.size() == 3 && near(m[0], 100.5) && near(m[1], 433.92) && near(m[2], 145.8), "markers parse, bad ones skipped");
        check(parseMarkers(markersText(m)) == m, "markers round trip");
        std::string many; for (int i = 1; i <= 30; i++) many += std::to_string(i) + ",";
        check((int)parseMarkers(many).size() == kMaxMarkers, "markers capped");
        check(markerNear(m, 433.9, 0.05) == 1 && markerNear(m, 300, 1) == -1, "nearest marker");
    }
    // colours
    check(cmap::levelTransform(0.3f, 0, 1, 1) == 0.3f, "brightness 0, contrast 1, gamma 1: unchanged");
    {
        const uint32_t red = 0xFF2030E0u;   // 0xAABBGGRR
        check(cmap::saturate(red, 1.f) == red, "colour intensity 1 leaves the colour");
        const uint32_t g = cmap::saturate(red, 0.f);
        check(((g >> 16) & 0xFF) == ((g >> 8) & 0xFF) && ((g >> 8) & 0xFF) == (g & 0xFF) && (g >> 24) == 0xFF, "colour intensity 0 is grey, opaque");
        const uint32_t v = cmap::saturate(red, 2.f);
        check((v & 0xFF) >= (red & 0xFF) && ((v >> 16) & 0xFF) <= ((red >> 16) & 0xFF), "colour intensity 2 pushes the channels apart");
    }
    check(near(cmap::levelTransform(0.3f, 0.1f, 1, 1), 0.4, 1e-6) && near(cmap::levelTransform(0.75f, 0, 2, 1), 1.0, 1e-6) && near(cmap::levelTransform(0.5f, 0, 3, 1), 0.5, 1e-6), "brightness and contrast");
    check(cmap::levelTransform(2.f, 0.5f, 4, 0.5f) == 1.f && cmap::levelTransform(-1.f, -0.5f, 4, 2) == 0.f, "clamped");
    cmap::Gradient g;
    check(g.parse("1:ffffff,0:000000"), "parse");
    check(g.n == 2 && g.pos[0] == 0 && g.rgb[1][0] == 255, "sorted by position");
    check(g.at(0) == 0xFF000000u && g.at(1) == 0xFFFFFFFFu && ((g.at(0.5f) & 255) == 128 || (g.at(0.5f) & 255) == 127), "blends");
    cmap::Gradient h; check(h.parse(g.text()) && h.text() == g.text(), "text round trip");
    check(!h.parse("junk") && !h.parse("0:zzzzzz,1:ffffff") && h.n == 2, "bad text keeps the gradient");
    for (const auto& pr : cmap::kGradientPresets) { cmap::Gradient x; check(x.parse(pr.stops) && x.n >= 2, "preset parses"); }
    check(cmap::color(cmap::SdrClassic, 0) == (0xFF000000u | (32u << 16)), "SDR++ classic starts dark blue");
    check(cmap::color(cmap::Magma, 1) == (0xFF000000u | (191u << 16) | (253u << 8) | 252u), "magma end");
    check(std::string(cmap::name(cmap::Custom)) == "Custom" && std::string(cmap::name(99)) == "Classic", "names");
    check(cmap::colourBlindSafe(cmap::Cividis) && !cmap::colourBlindSafe(cmap::Turbo), "colour-blind tags");
    check(cmap::traceRgb(0, false)[0] > 0.9f && cmap::traceRgb(0, true)[0] < 0.3f && cmap::traceRgb(99, false) == cmap::traceRgb(0, false), "trace presets");
    // Click-to-tune (Discord, 80 m view): a tone's waterfall column must stay under the spectrum's peak after the centre moves, with and
    // without a tune offset, for a click inside the band and for a jump of a whole band.
    for (double off : {0.0, 0.0125}) {
        const double fs2 = 10.0, c2 = 3.66 + off, tone = 3.455;
        const int cols = 8192;
        auto colOf = [&](double cen, double f) { return (int)std::lround((f - imageLo(cen, fs2, cols)) / fs2 * cols - 0.5); };
        std::vector<uint8_t> row(cols, 0);
        row[colOf(c2, tone)] = 200;
        for (double newC : {3.66 + off, 3.84, 3.45 + off, 3.46, 3.9 + 2 * off}) {
            std::vector<uint8_t> r = row;
            const int s = retuneShift(c2, newC, fs2, cols);
            shiftRow(r.data(), cols, s, 0);
            const double histC = c2 - (double)s * fs2 / cols;   // what the moved row stands for (Waterfall::follow)
            int at = -1; for (int i = 0; i < cols; i++) if (r[i]) at = i;
            if (at >= 0) {
                check(near(binCentre(histC, fs2, cols, at), tone, fs2 / cols * 1.01), "tone stays under its frequency after a retune");
                check(std::abs(at - colOf(newC, tone)) <= 1, "tone column == spectrum bin column of the new axis");
            }
            const double fx = mhzToPx(tone, 100, 900, newC - 0.2, newC + 0.2);
            check(near(pxToMhz(fx, 100, 900, newC - 0.2, newC + 0.2), tone, 1e-9), "px/mhz round trip");
        }
        std::vector<uint8_t> r = row; shiftRow(r.data(), cols, retuneShift(c2, c2 + 50, fs2, cols), 0);
        bool any = false; for (auto v : r) any |= v != 0;
        check(!any, "a jump of a whole band clears the row");
    }
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("spec_axis: all passed\n");
    return 0;
}
