// The zoom detail of the spectrum and the waterfall (4.0, docs/WATERFALL-4.0.md): the screen side of the engine's zoom spectrum
// (core dsp/zoom_spectrum.h). The plots ask for the view they show (request); each new zoom frame updates its own trace lines (the
// average in power, the max hold, on its real bins) and a waterfall history whose rows run parallel to the main history's ring (row i of
// one is the same moment as row i of the other), so the view can take each row from the zoom history where it has one and from the main
// history where it does not: rows from before a zoom show at the main history's resolution (sharp, never stretched), later rows at the zoom
// resolution. A pan that keeps the zoom's shape shifts the rows by whole bins; another shape or a retune starts the zoom rows again.
// Pure (no ImGui, no App): tests/app/test_zoom_detail.cpp.
#pragma once
#include "dect2/dsp/zoom_spectrum.h"
#include "spec_axis.h"
#include "spec_pipe.h"
#include "wf_view.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace zdet {

constexpr int kMaxCols = 65536;   // a zoom row's columns at most (a finer transform is reduced to this by the peak per column)

struct ZoomDetail {
    // what the plot shows (set each frame it is drawn): the view's centre from the capture centre, its width, its pixels
    double reqOffHz = 0, reqSpanHz = 0, reqT = -1;
    int reqPx = 0;
    void request(double visLoMhz, double visHiMhz, double captureCentreMhz, int px, double now) {
        reqOffHz = ((visLoMhz + visHiMhz) * 0.5 - captureCentreMhz) * 1e6;
        reqSpanHz = (visHiMhz - visLoMhz) * 1e6;
        reqPx = px; reqT = now;
    }
    bool wanted(double now) const { return reqT >= 0 && now - reqT < 0.5 && reqSpanHz > 0 && reqPx > 0; }   // the plot is being drawn

    // the frame and its lines (in the plots' unit: per Hz when chosen)
    dect2::zoomfft::Plan plan;
    double cMhz = NAN;   // the capture centre the plan's offset is from
    uint64_t lastSeq = 0;
    double frameT = -1;
    std::vector<float> view, smooth, pw, peak, rowAvg, rowPw, acc;
    bool accEmpty = true;

    // the history (H rows parallel to the main ring, W columns over [loMhz(), hiMhz()))
    int H = 0, W = 0, gen = 0;
    std::vector<uint8_t> lv, ok;
    double wfHistC = NAN;   // the main history's band centre when the rows were made (a retune there moves its rows: these start again)
    long long seenRows = -1;
    float minDb = -100, maxDb = -32;
    spipe::FloorTracker floor;
    std::vector<float> floorScratch;
    double trackT = 0;

    double x0Mhz() const { return cMhz + (plan.offsetHz - (double)(plan.bins / 2) * plan.binHz) / 1e6; }   // the centre of bin 0
    double dxMhz() const { return plan.binHz / 1e6; }
    double loMhz() const { return x0Mhz() - dxMhz() * 0.5; }                                             // the left edge of bin 0
    double hiMhz() const { return loMhz() + (double)plan.bins * dxMhz(); }
    double useLoMhz() const { return cMhz + plan.usableLo() / 1e6; }
    double useHiMhz() const { return cMhz + plan.usableHi() / 1e6; }
    bool fresh(double now, double maxAge = 0.5) const { return plan.on && frameT >= 0 && now - frameT <= maxAge && view.size() == plan.bins && std::isfinite(cMhz); }
    bool covers(double lo, double hi) const { return plan.on && std::isfinite(cMhz) && lo >= useLoMhz() - 1e-12 && hi <= useHiMhz() + 1e-12; }

    void clearRows() { std::fill(ok.begin(), ok.end(), (uint8_t)0); gen++; }
    void off() {
        if (!plan.on && view.empty()) return;
        plan = dect2::zoomfft::Plan(); view.clear(); smooth.clear(); pw.clear(); peak.clear(); rowAvg.clear(); rowPw.clear(); acc.clear();
        accEmpty = true; frameT = -1; cMhz = NAN; clearRows();
    }

    // A new zoom frame f (dBFS per bin) from the capture centred at captureMhz. perHzOff is subtracted (the per-Hz unit: 10 log10 binHz).
    // alpha / rowAlpha: the trace's and the rows' averages (1: none); holdFall: dB the max hold falls (0 with holdDecay off: it holds).
    void take(const dect2::ZoomFrame& f, double captureMhz, float perHzOff, double now, float alpha, float rowAlpha, float holdFall, int rowsH) {
        if (!f.plan.on || f.dbfs.size() != f.plan.bins || f.plan.bins < 2) return;
        lastSeq = f.seq;
        const size_t n = f.dbfs.size();
        if (rowsH > 0 && rowsH != H) { H = rowsH; ok.assign((size_t)H, 0); lv.clear(); W = 0; gen++; }
        const bool newPlan = f.plan.id != plan.id || !plan.on || captureMhz != cMhz;
        if (newPlan) {
            const bool shiftable = plan.on && f.plan.sameShape(plan) && captureMhz == cMhz && std::fabs(f.plan.rateHz - plan.rateHz) < 1e-9 * plan.rateHz && W > 0;
            if (shiftable) {   // a pan of the same shape: the rows move by whole bins (out[x] = in[x + s], in columns)
                const double sb = (f.plan.offsetHz - plan.offsetHz) / plan.binHz;
                const int s = (int)std::lround(sb * (double)W / (double)n);
                if (std::fabs(sb - std::round(sb)) > 1e-6 || s >= W || s <= -W) clearRows();
                else if (s != 0) { for (int r = 0; r < H; r++) specaxis::shiftRow(lv.data() + (size_t)r * W, W, -s, 0); gen++; }
            } else {
                W = (int)std::min<size_t>(n, (size_t)kMaxCols);
                lv.assign((size_t)W * (size_t)std::max(1, H), 0);
                clearRows();
            }
            plan = f.plan; cMhz = captureMhz;
            smooth.clear(); pw.clear(); peak.clear(); rowAvg.clear(); rowPw.clear(); accEmpty = true;
            floor.floor = NAN;
        }
        view.resize(n);
        for (size_t i = 0; i < n; i++) view[i] = f.dbfs[i] - perHzOff;
        if (smooth.size() != n) { smooth = view; pw.clear(); }
        spipe::powerEma(pw, view.data(), smooth.data(), n, alpha);
        if (peak.size() != n) peak = view;
        else for (size_t i = 0; i < n; i++) peak[i] = std::max(peak[i] - holdFall, view[i]);
        if (rowAvg.size() != n) { rowAvg = view; rowPw.clear(); }
        spipe::powerEma(rowPw, view.data(), rowAvg.data(), n, rowAlpha);
        if (accEmpty || acc.size() != n) { acc = rowAvg; accEmpty = false; }
        else for (size_t i = 0; i < n; i++) acc[i] = std::max(acc[i], rowAvg[i]);   // the highest since the last row (slow rows miss nothing)
        frameT = now;
    }

    // The colour range: the main waterfall's own one (manual), or the floor of the zoom trace over the view (automatic) placed as the main
    // waterfall places its own, so both histories have the noise at the same colour.
    void fitRange(bool autoTrack, float wfMin, float wfMax, float below, float span, double visLo, double visHi, double now) {
        if (!autoTrack || smooth.size() < 64) { minDb = wfMin; maxDb = wfMax; return; }
        if (now - trackT < 0.25 && std::isfinite(floor.floor)) return;
        const float dt = trackT > 0 ? (float)std::min(2.0, now - trackT) : 0.25f;
        trackT = now;
        long i0 = (long)std::floor((visLo - x0Mhz()) / dxMhz()), i1 = (long)std::ceil((visHi - x0Mhz()) / dxMhz());
        i0 = std::max(0L, i0); i1 = std::min((long)smooth.size(), i1);
        if (i1 < i0 + 32) { i0 = 0; i1 = (long)smooth.size(); }
        const float fl = floor.update(spipe::noiseFloor(smooth.data() + i0, (size_t)(i1 - i0), floorScratch), dt);
        if (std::isfinite(fl)) spipe::floorRange(fl, below, span, minDb, maxDb);
    }

    // After the main history wrote its rows: the same ring rows here, from the zoom frame when there is a fresh one (canWrite), else
    // marked as missing (the view then takes the main row). mainRows: the main history's row count; writeRow: its newest ring row;
    // histC: its band centre (a change: it was retuned, so the zoom rows no longer line up).
    void sync(long long mainRows, int writeRow, double histC, bool canWrite, double now) {
        if (H <= 0) return;
        if (histC != wfHistC) { if (std::isfinite(wfHistC)) clearRows(); wfHistC = histC; }
        if (seenRows < 0 || mainRows < seenRows) { seenRows = mainRows; return; }
        const long long d = std::min<long long>(mainRows - seenRows, H);
        seenRows = mainRows;
        // the highest since the last row; with no new frame since then the newest average again (a frame late by a row is not a gap)
        const std::vector<float>& from = accEmpty ? rowAvg : acc;
        const bool write = canWrite && fresh(now, 0.25) && W > 0 && lv.size() == (size_t)W * (size_t)H && from.size() == plan.bins;
        for (long long i = 0; i < d; i++) {
            const size_t r = (size_t)(((long long)writeRow + i) % H);
            if (write) { spipe::levelsRow(from.data(), from.size(), W, minDb, maxDb, lv.data() + r * (size_t)W); ok[r] = 1; }
            else ok[r] = 0;
        }
        if (d > 0 && write) accEmpty = true;
    }

    // the zoom history as a source for plt::WfView::updateExact
    plt::WfSrc src() const {
        plt::WfSrc s;
        if (!plan.on || W <= 0 || lv.size() != (size_t)W * (size_t)H) return s;
        s.lv = lv.data(); s.W = W; s.lo = loMhz(); s.hi = hiMhz(); s.ok = ok.data(); s.useLo = useLoMhz(); s.useHi = useHiMhz(); s.gen = gen;
        return s;
    }
};

}  // namespace zdet
