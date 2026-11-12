// 4.0 display resolution (docs/WATERFALL-4.0.md), the screen side: the pixel-exact mapping of history columns and bins onto pixels (a
// one-column signal lands in exactly one pixel at any zoom and pan; coarse columns become sharp steps with no level made up), the zoom
// history beside the main one (rows shift on a pan of the same shape, start again on a new shape or a retune, missing rows fall back to the
// main history), the pixel-exact waterfall view (incremental == rebuilt, zoom rows used only where the view is inside the zoom band), and
// the resolution table the doc quotes (Hz per pixel before and after at several zoom levels).
#include "gfx/spec_pipe.h"
#include "gfx/wf_view.h"
#include "gfx/zoom_detail.h"
#include "dect2/dsp/spectrum.h"
#include "dect2/dsp/zoom_spectrum.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }

int main() {
    std::mt19937 rng(42);
    // ---- columns onto pixels: peak preservation ------------------------------------------------------------------------------------
    {
        bool once = true, inView = true;
        std::vector<uint8_t> row(65536, 10), out(1500);
        for (int t = 0; t < 3000; t++) {
            const int cols = 1 << (10 + rng() % 7);   // 1024 .. 65536
            row.assign((size_t)cols, 10);
            const int spike = (int)(rng() % (unsigned)cols);
            row[(size_t)spike] = 250;
            const double lo = 100.0, hi = 102.4, cw = (hi - lo) / cols;
            const double span = (hi - lo) * std::pow(10.0, -(double)(rng() % 3000) / 1000.0);   // the whole band down to 1/1000 of it
            const double c = lo + (spike + 0.5) * cw + ((double)(rng() % 1000) / 1000.0 - 0.5) * span * 0.9;
            const int px = 300 + (int)(rng() % 1200);
            const int set = spipe::pixelLevels(row.data(), cols, lo, hi, c - span / 2, c + span / 2, px, out.data(), 0);
            int hits = 0;
            for (int x = 0; x < px; x++) hits += out[(size_t)x] == 250;
            const double colsInView = span / cw;
            if (colsInView >= px) { if (hits != 1) once = false; }        // finer than pixels: exactly one pixel
            else if (hits < 1) once = false;                              // coarser: the column fills its pixels (at least one)
            if (set <= 0) inView = false;
        }
        check(once, "a one-column spike lands in exactly one pixel when columns are finer, and is never lost when coarser");
        check(inView, "views inside the row set pixels");
    }
    {   // coarse columns: sharp steps (only the columns' own levels, in runs)
        const uint8_t row[4] = {0, 80, 160, 240};
        std::vector<uint8_t> out(1000);
        spipe::pixelLevels(row, 4, 0, 4, 0, 4, 1000, out.data(), 7);
        bool only = true; int runs = 1;
        for (int x = 0; x < 1000; x++) { only = only && (out[(size_t)x] % 80 == 0); if (x && out[(size_t)x] != out[(size_t)x - 1]) runs++; }
        check(only && runs == 4 && out[249] == 0 && out[250] == 80 && out[999] == 240, "4 columns on 1000 pixels: four sharp steps at the column edges");
        spipe::pixelLevels(row, 4, 0, 4, -1, 5, 600, out.data(), 7);
        check(out[0] == 7 && out[599] == 7 && out[300] == 160, "outside the row: the fill");
    }
    {   // the dB version for the trace
        std::vector<float> y(4096, -100.f);
        y[1234] = -20.f;
        std::vector<float> out(700);
        spipe::pixelPeaks(y.data(), y.size(), 0.0, 1.0, -0.5, 4095.5, 700, out.data());
        int hits = 0; for (float v : out) hits += v == -20.f;
        check(hits == 1, "trace: a one-bin peak in exactly one pixel");
        spipe::pixelPeaks(y.data(), y.size(), 0.0, 1.0, 1230, 1240, 700, out.data());
        hits = 0; for (float v : out) hits += v == -20.f;
        check(hits >= 60 && hits <= 80, "trace zoomed in: the bin is a flat step about 70 pixels wide");
        spipe::pixelPeaks(y.data(), y.size(), 0.0, 1.0, -100, -50, 10, out.data());
        check(std::isnan(out[0]) && std::isnan(out[9]), "trace outside the bins: no value");
    }

    // ---- the zoom history ------------------------------------------------------------------------------------------------------------
    const int H = 64;
    auto frame = [](const dect2::zoomfft::Plan& p, int peakBin, uint64_t seq) {
        dect2::ZoomFrame f; f.plan = p; f.seq = seq;
        f.dbfs.assign(p.bins, -120.f);
        if (peakBin >= 0) f.dbfs[(size_t)peakBin] = -30.f;
        return f;
    };
    {
        dect2::zoomfft::Plan p = dect2::zoomfft::make(2.4e6, 300e3, 40e3, 1000); p.id = 1;
        zdet::ZoomDetail z;
        const double rc = 100.0;
        z.take(frame(p, 1000, 1), rc, 0.f, 1.0, 1.f, 1.f, 0.f, H);
        check(z.plan.id == 1 && z.W == (int)p.bins && z.fresh(1.0) && !z.fresh(2.0), "first frame: a history of the plan's bins");
        check(z.covers(rc + (300e3 - 15e3) / 1e6, rc + (300e3 + 15e3) / 1e6) && !z.covers(rc, rc + 0.5), "covers the view inside the usable band only");
        check(std::fabs(z.x0Mhz() + (double)(p.bins / 2) * z.dxMhz() - (rc + p.offsetHz / 1e6)) < 1e-12, "bin N/2 is the zoom centre");
        z.fitRange(false, -110, -40, 3, 80, 0, 0, 1.0);
        long long rows = 0;
        z.sync(rows, 0, 0.0, true, 1.0);   // the first call only notes the count
        rows = 5; z.sync(rows, 59, 0.0, true, 1.0);
        int okN = 0; for (uint8_t o : z.ok) okN += o;
        check(okN == 5 && z.ok[59] && z.ok[63] && !z.ok[0], "rows written beside the main ring's newest five (ring rows 59 .. 63)");
        check(z.lv[(size_t)59 * z.W + 1000] > z.lv[(size_t)59 * z.W + 900], "the peak is in the row");
        rows = 6; z.sync(rows, 58, 0.0, true, 3.0);
        check(!z.ok[58], "no fresh frame: the row is marked missing (the main row shows)");
        // a pan of the same shape: whole bins
        dect2::zoomfft::Plan q = dect2::zoomfft::make(2.4e6, 300e3 + 100 * p.binHz, 40e3, 1000); q.id = 2;
        check(q.sameShape(p) && std::fabs((q.offsetHz - p.offsetHz) / p.binHz - 100) < 1e-6, "the pan plan: the same shape, 100 bins on");
        const int gen0 = z.gen;
        z.take(frame(q, 900, 2), rc, 0.f, 3.1, 1.f, 1.f, 0.f, H);
        check(z.ok[59] && z.gen != gen0 && z.lv[(size_t)59 * z.W + 900] == z.lv[(size_t)59 * z.W + 900] && z.lv[(size_t)59 * z.W + 900] > z.lv[(size_t)59 * z.W + 800],
              "a pan: the rows stay and move by 100 columns (the peak at column 1000 is now at 900)");
        // a new shape: the rows start again
        dect2::zoomfft::Plan r = dect2::zoomfft::make(2.4e6, 300e3, 8e3, 1000); r.id = 3;
        z.take(frame(r, -1, 3), rc, 0.f, 3.2, 1.f, 1.f, 0.f, H);
        okN = 0; for (uint8_t o : z.ok) okN += o;
        check(okN == 0 && z.W == (int)r.bins, "another shape: no zoom rows (the main history shows them)");
        // a retune of the main history
        rows = 8; z.sync(rows, 56, 0.0, true, 3.2);
        check(z.ok[56] && z.ok[57], "rows again");
        rows = 9; z.sync(rows, 55, 0.5, true, 3.2);
        check(!z.ok[56] && !z.ok[57] && z.ok[55], "a retune clears the old zoom rows (they no longer line up)");
        z.off();
        okN = 0; for (uint8_t o : z.ok) okN += o;
        check(!z.plan.on && okN == 0 && !z.fresh(3.2), "off: nothing shown from the zoom");
    }

    // ---- the pixel-exact view ----------------------------------------------------------------------------------------------------
    {
        const int W = 65536;
        std::vector<uint8_t> lv((size_t)W * H, 10);
        for (int r = 0; r < H; r++) lv[(size_t)r * W + 40000] = 200;
        uint32_t lut[256]; for (int i = 0; i < 256; i++) lut[i] = 0xFF000000u | (uint32_t)i;
        plt::WfSrc m; m.lv = lv.data(); m.W = W; m.lo = 99.0; m.hi = 101.4;
        plt::WfView a, b; bool full = false; std::vector<int> dirty;
        check(a.updateExact(m, nullptr, H, 0, H - 1, H, lut, 1, 99.0, 101.4, 1500, 1, full, dirty) && full && a.exact && a.cols == 1500, "exact view built");
        int hits = 0; for (int x = 0; x < a.cols; x++) hits += (a.px[(size_t)x] & 255) == 200;
        check(hits == 1, "exact view: the one-column signal in one pixel (65536 columns on 1500 pixels)");
        const double c = 99.0 + 40000.5 * 2.4 / W;
        check(a.updateExact(m, nullptr, H, 0, H - 1, H, lut, 1, c - 0.0005, c + 0.0005, 1500, 1, full, dirty) && full, "a zoom rebuilds");
        hits = 0; for (int x = 0; x < a.cols; x++) hits += (a.px[(size_t)x] & 255) == 200;
        check(hits > 45 && hits < 65, "zoomed in 2400x: the 36.6 Hz column is a sharp block of ~55 pixels");
        bool sharp = true; for (int x = 0; x < a.cols; x++) { const uint32_t l = a.px[(size_t)x] & 255; sharp = sharp && (l == 200 || l == 10); }
        check(sharp, "zoomed in: no level between the two (no blur)");
        check(!a.updateExact(m, nullptr, H, 0, H - 1, H, lut, 1, c - 0.0005, c + 0.0005, 1500, 1, full, dirty), "nothing new: nothing done");
        // the zoom history: finer rows in the newest half
        zdet::ZoomDetail z;
        dect2::zoomfft::Plan p = dect2::zoomfft::make(2.4e6, (c - 100.2) * 1e6, 3000, 1500); p.id = 1;
        const double rc = 100.2;
        dect2::ZoomFrame f; f.plan = p; f.seq = 1; f.dbfs.assign(p.bins, -120.f);
        z.take(f, rc, 0.f, 1.0, 1.f, 1.f, 0.f, H);
        const size_t kb = (size_t)std::lround((c + 0.0002 - z.x0Mhz()) / z.dxMhz());
        f.dbfs[kb] = -20.f; f.seq = 2;
        z.take(f, rc, 0.f, 1.0, 1.f, 1.f, 0.f, H);
        z.fitRange(false, -120, -10, 3, 80, 0, 0, 1.0);
        z.sync(0, 0, 0.0, true, 1.0);
        z.sync(H / 2, 0, 0.0, true, 1.0);   // ring rows 0 .. 31 (the newest at 0)
        const plt::WfSrc d = z.src();
        check(d.lv && d.W == (int)p.bins, "the zoom history as a source");
        plt::WfView v;
        v.updateExact(m, &d, H, 0, H - 1, H, lut, 1, c - 0.0005, c + 0.0005, 1500, 1, full, dirty);
        check(v.detOn, "the view inside the zoom band uses the zoom rows");
        auto rowHas = [&](const plt::WfView& w, long long g, int level) { const uint32_t* o = w.px.data() + (size_t)w.rowOf(g) * w.stride; int n = 0; for (int x = 0; x < w.cols; x++) n += (int)((o[x] & 255) >= (uint32_t)level); return n; };
        check(rowHas(v, H - 1, 150) >= 1 && rowHas(v, H - 1, 150) < 20, "the newest row: the zoom tone, a few pixels wide (finer than the main column)");
        check(rowHas(v, 0, 190) > 30, "the oldest row (no zoom row): the main history's column");
        plt::WfView wv;
        wv.updateExact(m, &d, H, 0, H - 1, H, lut, 1, 99.0, 101.4, 1500, 1, full, dirty);
        check(!wv.detOn, "the whole band is outside the zoom band: the main history only");
        // incremental == rebuilt, with the zoom rows changing
        z.sync(H, 0, 0.0, true, 1.0);   // the zoom history catches up with the main ring's 64 rows
        std::vector<uint8_t> lv2 = lv;
        int wr = 0; long long R = H - 1;
        for (int i = 0; i < 40; i++) {
            wr = (wr + H - 1) % H; R++;
            std::fill(lv2.begin() + (size_t)wr * W, lv2.begin() + (size_t)(wr + 1) * W, (uint8_t)(20 + i));
            plt::WfSrc m2 = m; m2.lv = lv2.data();
            z.sync(R + 1, wr, 0.0, i % 3 != 0, 1.0);
            const plt::WfSrc d2 = z.src();
            v.updateExact(m2, &d2, H, wr, R, H, lut, 1, c - 0.0005, c + 0.0005, 1500, 2, full, dirty);
        }
        plt::WfView fresh;
        plt::WfSrc m2 = m; m2.lv = lv2.data(); const plt::WfSrc d2 = z.src();
        fresh.updateExact(m2, &d2, H, wr, R, H, lut, 1, c - 0.0005, c + 0.0005, 1500, 2, full, dirty);
        check(v.px == fresh.px, "the exact view row by row == rebuilt");
        v.mode = plt::WfView::LowPass;
        check(v.updateExact(m2, &d2, H, wr, R, H, lut, 1, c - 0.0005, c + 0.0005, 1500, 2, full, dirty) && full, "the low-pass on the pixel rows rebuilds");
    }

    // ---- the resolution table for the doc ----------------------------------------------------------------------------------------
    {
        printf("  resolution (Hz per pixel column drawn: before = the 3.9 history column, at least the FFT bin; after = the zoom FFT bin, or the full-band bin when fine enough):\n");
        for (double fs : {2.4e6, 10e6, 20e6}) {
            const size_t nAuto = dect2::SpectrumAnalyzer::autoSize(fs);
            for (double span : {fs, fs / 10, 100e3, 20e3, 5e3, 1e3}) {
                const int px = 1920;
                const double pxHz = span / px;
                const double before = fs / (double)std::min<size_t>(nAuto, 65536);   // the 3.9 history: a column per bin, 64k at most
                const dect2::zoomfft::Plan p = dect2::zoomfft::update(dect2::zoomfft::Plan(), fs, nAuto, 0, span, px);
                const double after = p.on ? p.binHz : before;
                printf("    %5.1f Msps, %8.0f Hz over %d px (%.2f Hz a pixel): before %.2f Hz a bin (%.2f px a bin), after %.2f Hz a bin%s\n",
                       fs / 1e6, span, px, pxHz, before, pxHz > 0 ? before / pxHz : 0, after, p.on ? " (zoom FFT)" : "");
                if (p.on) check(after <= pxHz * (1 + 1e-9), "after: a bin per pixel or finer");
            }
        }
    }
    printf(fails ? "%d FAILED\n" : "zoom_detail: all passed\n", fails);
    return fails ? 1 : 0;
}
