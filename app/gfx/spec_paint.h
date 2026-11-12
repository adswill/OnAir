// The drawing of the spectrum looks inside a plot (after plt::BeginPlot and the axis setup): the plot look, the passband, the trace with
// its fill and the waterfall picture. Shared by the app (plots.cpp) and the offscreen render harness (tools/spec_render.cpp), so the
// harness's pictures are the app's. Uses plt and ImGui only (no App).
#pragma once
#include "plot.h"
#include "spec_look.h"
#include "wf_view.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace spaint {

inline ImVec4 v4(const slook::Rgba& c, float alphaMul = 1.f) { return ImVec4(c.r, c.g, c.b, c.a * alphaMul); }

// the plt look of the spectrum (with its axis strip) or of the waterfall (the grid carried over the picture)
inline plt::Look plotLook(const slook::LookSet& L, bool spectrum, bool grid, ImFont* font, float fontSize, int labelMode, double ref) {
    plt::Look k;
    if (L.classic) return k;
    k.on = true;
    k.bgTop = v4(L.bgTop); k.bgBottom = v4(L.bgBottom);
    k.grid = v4(L.grid); k.gridH = L.gridH;
    k.font = font;
    k.fontSize = fontSize;
    k.labelMode = labelMode;
    k.ref = ref;
    k.label = v4(L.label); k.stripBg = v4(L.stripBg);
    if (spectrum) k.strip = true;
    else if (grid) {
        k.overV = ImVec4(L.grid.r, L.grid.g, L.grid.b, L.wfGridAlpha);
        k.overH = ImVec4(L.grid.r, L.grid.g, L.grid.b, L.wfGridHAlpha);
    }
    return k;
}

// the passband x0..x1 with its edges and the centre line (spectrum: the filled block; waterfall: the lines and the look's faint band)
inline void band(const slook::LookSet& L, double x0, double x1, double centre, bool spectrum, bool dragging) {
    if (x1 > x0) {
        if (spectrum) plt::PlotVBandGradient(x0, x1, v4(L.bandTop), v4(L.bandBottom));
        else if (L.wfBand.a > 0) plt::PlotVBandGradient(x0, x1, v4(L.wfBand), v4(L.wfBand));
        const double e[2] = {x0, x1};
        plt::Spec es; es.LineColor = v4(L.bandEdge, spectrum ? 1.f : L.wfLineAlpha); es.LineWeight = L.bandEdgeW * (dragging ? 2.f : 1.f);
        if (es.LineColor.w > 0) plt::PlotInfLines("bandedges", e, 2, es);
    }
    const double c[1] = {centre};
    plt::Spec cs; cs.LineColor = v4(L.centre, spectrum ? 1.f : L.wfLineAlpha); cs.LineWeight = L.centreW;
    if (cs.LineColor.w > 0) plt::PlotInfLines("centre", c, 1, cs);
}

// One point per pixel column of the view vis.X over px columns from n bins at x0 + i * dx (slook::decimate: the highest bin of the
// column, interpolated between bins when zoomed in), smoothed by `smooth` [1 2 1] passes. ex / ey: the points (kept between frames).
inline bool traceXY(const float* y, size_t n, double x0, double dx, const plt::Rect& vis, int px, int interp, bool useMin, int smooth,
                    std::vector<double>& ex, std::vector<double>& ey) {
    static std::vector<float> t;
    ex.clear(); ey.clear();
    if (!slook::decimate(y, n, x0, dx, vis.X.Min, vis.X.Max, px, interp, useMin, t)) return false;
    slook::smoothPasses(t, smooth);
    const double cw = (vis.X.Max - vis.X.Min) / px;
    for (int c = 0; c < px; c++) { ex.push_back(vis.X.Min + (c + 0.5) * cw); ey.push_back(t[(size_t)c]); }
    return true;
}

// The trace with its fill down to yMin: the fill's alpha fillTop at the trace fading to fillBottom at the bottom
inline void trace(const std::vector<double>& ex, const std::vector<double>& ey, ImVec4 c, float weight, bool fill, float fillTop, float fillBottom, double yMin) {
    if (ex.empty()) return;
    if (fill) {
        const float ft = std::max(0.02f, fillTop);
        plt::Spec fs; fs.FillColor = ImVec4(c.x, c.y, c.z, ft); fs.FillFade = std::max(0.001f, 1.f - fillBottom / ft); fs.LineColor = ImVec4(0, 0, 0, 0);
        plt::PlotShaded("fill", ex.data(), ey.data(), (int)ex.size(), yMin, fs);
    }
    plt::Spec ss; ss.LineColor = c; ss.LineWeight = weight;
    plt::PlotLine("spectrum", ex.data(), ey.data(), (int)ex.size(), ss);
}

// The waterfall picture from the view texture (plt::WfView v, uploaded as tex of texW x H texels): the frequency span x0..x1 of the W
// history columns, R the newest row's number, frac the part of a row due since it came (the picture slides down by it).
inline void wfImage(const plt::WfView& v, ImTextureID tex, int texW, int H, double x0, double x1, int W, long long rows, double frac) {
    if (v.cols <= 0 || rows <= 0) return;
    const double vx0 = v.exact ? v.vLo : x0 + (x1 - x0) * v.c0 / W, vx1 = v.exact ? v.vHi : x0 + (x1 - x0) * v.c1 / W;   // exact: the view is the pixels
    const float u1 = (float)v.cols / texW;
    const long long R = rows - 1, gN = R / v.k;
    const int w = v.rowOf(gN);
    // row r takes y from -(R - r) - 1 - frac to -(R - r) - frac: the newest group's top is that of its last row (still to come while it
    // fills: above the plot, clipped), the older groups below it, the ring's rows w..ring-1 then 0..w-1
    const double top = -(double)(R - (gN * v.k + v.k - 1)) - frac, midY = top - (double)(v.ring - w) * v.k;
    plt::PlotImage("a", tex, plt::Point(vx0, midY), plt::Point(vx1, top), ImVec2(0, (float)w / H), ImVec2(u1, (float)v.ring / H));
    if (w > 0) plt::PlotImage("b", tex, plt::Point(vx0, midY - (double)w * v.k), plt::Point(vx1, midY), ImVec2(0, 0), ImVec2(u1, (float)w / H));
    const float vNew = (w + 0.5f) / H;   // the gap above (a whole group in): the newest group, stretched
    if (top < 0) plt::PlotImage("top", tex, plt::Point(vx0, top), plt::Point(vx1, 0), ImVec2(0, vNew), ImVec2(u1, vNew));
}

} // namespace spaint
