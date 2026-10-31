// The waterfall's history (levels per row and column), its colour table and its intensity range: no ImGui calls, no App, so the offscreen
// render harness (tools/spec_render.cpp) runs the very same code as the app. Moved out of app.h.
#pragma once
#include "gfx.h"
#include "colormap.h"
#include "spec_axis.h"
#include "spec_look.h"
#include "spec_pipe.h"
#include "wf_view.h"
#include "wf3d.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// ------------------------------------------------------------------ waterfall texture

struct Waterfall {
    static constexpr int W = 8192, H = 512;   // W: the standard history width (and the view texture's); cols may be finer (#243)
    static constexpr int kMaxCols = 65536;    // the cap of the finer history: 32 MB of levels
    enum Map { Classic = cmap::Classic, Viridis = cmap::Viridis, Inferno = cmap::Inferno, Grey = cmap::Grey, Turbo = cmap::Turbo, Cividis = cmap::Cividis,   // Cividis: made to read the same with red-green colour blindness
               Magma = cmap::Magma, SdrClassic = cmap::SdrClassic, Ice = cmap::Ice, Fire = cmap::Fire, NightRed = cmap::NightRed, HighContrast = cmap::HighContrast,
               Custom = cmap::Custom, NavyHeat = cmap::NavyHeat, BlueGreen = cmap::BlueGreen, Amber = cmap::Amber, Paper = cmap::Paper, NumMaps = cmap::NumMaps };
    static const char* mapName(int m) { return cmap::name(m); }
    gfx::Image* img = nullptr;       // the history as a texture at the standard width (other waterfalls draw it; not written for a finer history)
    uint32_t lut[256] = {};
    int builtMap = -1, lutGen = 0;   // lutGen: counts the colour table rebuilds (and the history moves: the view is made again)
    int cols = W;                    // the history width: W, or finer (detail)
    int detail = -1;                 // the wanted width (-1: as fine as the FFT, the default; 0: W); never wider than the FFT gives (specaxis::historyCols)
    std::vector<uint8_t> lv;         // the history as levels (0..255, the index into lut), cols x H: the view is made from it
    long long rows = 0;              // rows written since the start; the newest is number rows - 1, at writeRow
    gfx::Image* viewImg = nullptr;   // the history at the size it is shown (the main waterfall; made on first use, W x H)
    plt::WfView view;
    w3d::State s3d;                  // the 3D view of the history (wf3d.h, drawn by wf3d_plot.cpp): its settings, camera and cached mesh
    float gamma = 1.f, builtGamma = 1.f;   // contrast: the level ^ gamma picks the colour (below 1 lifts weak signals)
    float brightness = 0.f, contrast = 1.f, builtBright = 0.f, builtContrast = 1.f;   // the level shifted and stretched before the gamma
    int builtCustom = 0;             // the custom gradient's version the table was made with
    static int& customGen() { static int g = 0; return g; }   // bumped when the custom gradient changes
    double histC = NAN, histFs = 0;  // the band the rows were taken from (MHz): a retune moves them along (follow)
    std::vector<uint32_t> row;      // scratch for the row being written (kept: no allocation per frame)
    std::vector<float> acc, floorScratch;   // the highest level per bin since the last row; the samples of the noise floor estimate
    bool accEmpty = true;
    int writeRow = 0; // newest row; older rows follow at writeRow+1 ... wrapping
    int filled = 0;
    double pushT = 0; // when the newest row came in (glfwGetTime)
    double rowDt = 1.0 / 30; // steady seconds per row, for the time labels and the scrolling
    double stamps[128] = {};
    int nStamps = 0;
    float minDb = -100, maxDb = -32;
    int map = NavyHeat;      // the colour map (Map); -1: the one that goes with the interface theme
    bool autoTrack = true;   // the intensity range follows the noise floor (on by default: the floor always sits in the dark end of the map)
    float autoBelow = 4.f, autoSpan = 55.f;   // the auto range: it starts autoBelow dB under the noise floor and is autoSpan dB wide
    spipe::FloorTracker floorTrack;           // the noise floor, followed with a 2 s time constant
    double viewU0 = 0, viewU1 = 1;            // the part of the band in view (0..1): its floor is the one the colours follow
    float rowTau = 0.15f;                     // the rows' time constant (seconds; 0: each row its own frame)
    std::vector<float> rowAvg, rowPow;
    double lastOfferT = 0;
    bool rowAverage = false; // slow rows: the average of the frames in between instead of the highest level (#237)
    bool clockLabels = false;// the time scale shows the clock time of the rows instead of "seconds ago" (#237)
    int accN = 0;            // frames summed into acc (rowAverage)
    std::vector<float> accOut;
    float rowsPerSec = 0;    // 0: one row per spectrum frame (about 30 a second); else this many rows a second (down to one a minute), each the highest level of the frames in between
    size_t fastBins = 0;     // bins of a fast row (fast(): the history width follows it, not the spectrum frame)
    double fastSent = -1;    // the fast row rate last told to the engine (0: off)
    bool paused = false;     // no new rows: the picture stays and can be scrolled back
    double nextRowT = 0;
    double trackT = 0;

    // average over the last 128 rows, and move only when that drifts by more than 2%: the labels stay put
    void stamp(double t) {
        const int N = 128;
        if (nStamps >= N) {
            const double dt = (t - stamps[nStamps % N]) / N;
            if (dt > 0 && std::fabs(dt - rowDt) > 0.02 * rowDt) rowDt = dt;
        } else if (nStamps >= 8) rowDt = (t - stamps[0]) / nStamps;   // starting up: whatever we have
        stamps[nStamps % N] = t;
        nStamps++;
        pushT = t;
    }
    bool slow() const { return rowsPerSec > 0.001f && rowsPerSec < 28.f; }   // rows come at a set, slower pace
    bool fast() const { return rowsPerSec >= 28.f; }   // fast rows (CW): the engine makes a row every 1/rowsPerSec seconds, the screen draws them in batches
    bool timeKnown() const { return nStamps >= 128 || slow() || fast(); }
    void setRate(float r) { rowsPerSec = r; nStamps = 0; nextRowT = 0; accEmpty = true; rowDt = slow() || fast() ? 1.0 / r : 1.0 / 30; }

    static int& themeMode() { static int m = 0; return m; }   // the interface theme's waterfall choice (pal::wfMode(), set by the app each frame)
    int effMap() const { return map >= 0 && map < NumMaps ? map : themeMode() == 5 ? Paper : themeMode() == 4 ? Amber : themeMode() == 3 ? Magma : themeMode() == 2 ? Turbo : themeMode() == 1 ? Grey : Classic; }
    static int& curMap() { static int m = 0; return m; }   // the map in use, for the other waterfalls (modes with their own picture) that colour with jet()
    static uint32_t jet(float v) { return color(curMap(), v); }
    bool paletteStale() const {
        return effMap() != builtMap || gamma != builtGamma || brightness != builtBright || contrast != builtContrast || (builtMap == Custom && builtCustom != customGen());
    }
    void palette() {   // after the map, the level controls or the interface palette changed
        if (builtMap >= 0 && builtMap != effMap()) slook::floorPlace(effMap(), autoBelow, autoSpan);   // a new map: its own place for the noise floor
        builtMap = effMap(); curMap() = builtMap;
        for (int i = 0; i < 256; i++) lut[i] = color(builtMap, cmap::levelTransform(i / 255.0f, brightness, contrast, gamma));
        builtGamma = gamma; builtBright = brightness; builtContrast = contrast; builtCustom = customGen(); lutGen++;
    }
    void init(gfx::Backend* gfx) {
        img = gfx->createImage(W, H, 0xFF000000u);
        row.assign(W, 0);
        lv.assign((size_t)cols * H, 0);
        palette();
    }
    static uint32_t color(int map, float v) { return cmap::color(map, v); }
    // a new history width: the rows start again (black)
    void setCols(int c) {
        c = std::max(W, std::min(kMaxCols, c));
        if (c == cols && lv.size() == (size_t)c * H) return;
        cols = c;
        lv.assign((size_t)cols * H, 0);
        filled = 0; accEmpty = true; lutGen++;
    }
    // The radio moved from the band the rows were taken from: move them along so each signal stays under its frequency (#270); a new
    // sample rate starts them again. The view is made again (lutGen).
    void follow(double centreMhz, double fsMhz) {
        if (!(fsMhz > 0) || !std::isfinite(centreMhz)) return;
        if (histFs != fsMhz || !std::isfinite(histC)) {
            if (histFs > 0 && histFs != fsMhz && filled > 0) { std::fill(lv.begin(), lv.end(), 0); filled = 0; lutGen++; }
            histC = centreMhz; histFs = fsMhz; return;
        }
        const int s = specaxis::retuneShift(histC, centreMhz, fsMhz, cols);
        if (s == 0) return;
        if (s >= cols || s <= -cols) { histC = centreMhz; std::fill(lv.begin(), lv.end(), 0); filled = 0; lutGen++; return; }
        rowAvg.clear(); rowPow.clear();   // the averaged row belongs to the old band
        histC -= (double)s * fsMhz / cols;   // what the moved rows now stand for (a part of a column stays as the remainder)
        for (int r = 0; r < H; r++) specaxis::shiftRow(lv.data() + (size_t)r * cols, cols, s, 0);
        lutGen++;
    }

    // One spectrum frame arrives: it becomes a row of the history, now or (rows at a set pace) when the next row is due. True when a row was written.
    // extra: frames the screen missed (it drew slower than they came, #246): the row is repeated for them, so the time scale stays true.
    bool offer(const std::vector<float>& dbfs, double now, int extra = 0) {
        if (paused || dbfs.empty()) return false;
        if (paletteStale()) palette();
        const float* d = dbfs.data();
        if (rowTau > 0.001f) {   // a short average down the rows (in dB): the noise keeps its grain but each speck lasts a few rows, as on the references
            const float dt = lastOfferT > 0 ? (float)std::min(0.5, std::max(0.001, now - lastOfferT)) : 1.f / 30;
            if (rowAvg.size() != dbfs.size()) { rowAvg.resize(dbfs.size()); rowPow.clear(); }
            spipe::powerEma(rowPow, d, rowAvg.data(), dbfs.size(), spipe::alphaFor(dt, rowTau));   // in power, as the trace
            d = rowAvg.data();
        }
        lastOfferT = now;
        if (slow()) {   // the highest level of every bin since the last row, so a short burst is not missed
            if (acc.size() != dbfs.size() || accEmpty) { acc.assign(d, d + dbfs.size()); accEmpty = false; accN = 1; }
            else if (rowAverage) { for (size_t i = 0; i < dbfs.size(); i++) acc[i] += d[i]; accN++; }
            else for (size_t i = 0; i < dbfs.size(); i++) acc[i] = std::max(acc[i], d[i]);
            if (now < nextRowT) return false;
            const double dt = 1.0 / rowsPerSec;
            nextRowT = now - nextRowT > dt ? now + dt : nextRowT + dt;
            accEmpty = true;
            d = acc.data();
            if (rowAverage && accN > 1) {
                accOut.resize(acc.size());
                const float k = 1.f / (float)accN;
                for (size_t i = 0; i < acc.size(); i++) accOut[i] = acc[i] * k;
                d = accOut.data();
            }
            extra = 0;
        }
        for (int i = 0; i <= extra; i++) writeFrom(d, dbfs.size());
        return true;
    }
    // One fast row from the engine (fast(): rows a second, each from its own short transform): written as it is, no pacing and no average down
    // the rows (rowTau would smear a 60 ms Morse dot over several rows).
    bool offerFast(const float* d, size_t n) {
        if (paused || n == 0) return false;
        if (paletteStale()) palette();
        writeFrom(d, n);
        return true;
    }
    // the bins of the frame onto the history columns (the highest of those that share a column), the levels onto the colour table
    void writeFrom(const float* d, size_t n) {
        if (lv.size() != (size_t)cols * H) lv.assign((size_t)cols * H, 0);
        writeRow = (writeRow + H - 1) % H;
        uint8_t* l = lv.data() + (size_t)writeRow * cols;
        const int C = cols;
        spipe::levelsRow(d, n, C, minDb, maxDb, l);
        if (img && C == W) {   // the standard-width texture (the other waterfalls draw it)
            if (row.size() != (size_t)W) row.assign(W, 0);
            uint32_t* r = row.data();
            for (int x = 0; x < W; x++) r[x] = lut[l[x]];
            img->update(0, writeRow, W, 1, r);
        }
        filled = std::min(H, filled + 1);
        rows++;
    }
    // The range from the averaged spectrum s: the noise floor (spipe::noiseFloor, followed over ~2 s) lands autoBelow dB over the map's dark
    // end and the map is autoSpan dB wide, so the noise always shows as a dark textured floor and strong signals reach the hot end.
    // instant: take the floor of this frame at once; else follow it (dt: seconds since the last call).
    void fitRange(const std::vector<float>& s, bool instant, float dt = 0.25f) {
        if (s.size() < 64) return;
        size_t i0 = (size_t)std::max(0.0, std::min(1.0, viewU0) * (double)s.size()), i1 = (size_t)std::max(0.0, std::min(1.0, viewU1) * (double)s.size());
        if (i1 < i0 + 32) { i0 = 0; i1 = s.size(); }   // too narrow a view for a floor: the whole band
        const float fl = spipe::noiseFloor(s.data() + i0, i1 - i0, floorScratch);
        if (instant) floorTrack.floor = NAN;
        const float f = floorTrack.update(fl, dt);
        if (!std::isfinite(f)) return;
        spipe::floorRange(f, autoBelow, autoSpan, minDb, maxDb);
    }
};

