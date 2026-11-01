// Our own plotting on top of Dear ImGui's draw lists. See plot.h.
#include "plot.h"
#include "plot_util.h"
#include "plot_batch.h"
#include "spec_axis.h"
#include "spec_look.h"
#include "../ui/touch.h"
#include "tune_drag.h"
#include "imgui_internal.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace plt {

namespace {

struct AxisSetup {
    double min = 0, max = 1;
    bool haveLimits = false;
    Cond cond = Cond_Once;
    int flags = 0;
    std::string label, fmt;
    double unit = 1;   // the labels show value * unit
    Scale scale = Scale_Linear;
    std::vector<double> tickPos;
    std::vector<std::string> tickLab;
    double* linkMin = nullptr; double* linkMax = nullptr;
    double cmin = -std::numeric_limits<double>::infinity(), cmax = std::numeric_limits<double>::infinity();   // pan and zoom stay inside these
    double fitMin = std::numeric_limits<double>::infinity(), fitMax = -std::numeric_limits<double>::infinity();
};

// what is remembered between frames for one plot
struct State {
    bool init = false;
    double lim[2][2] = {{0, 1}, {0, 1}};
    bool fitValid[2] = {false, false};
    double fit[2][2] = {{0, 1}, {0, 1}};
    double tgt[2][2] = {{0, 1}, {0, 1}}, last[2][2] = {{0, 1}, {0, 1}};   // an eased wheel zoom: where the limits are heading, and where this plot left them
    bool hasTgt[2] = {false, false};
};
std::unordered_map<ImGuiID, State> gStates;

struct Subplots {
    bool active = false;
    int rows = 1, cols = 1, next = 0;
    ImVec2 min, size;
};

struct Ctx {
    bool active = false;
    ImGuiID id = 0;
    ImDrawList* dl = nullptr;
    int flags = 0;
    ImRect frame, plot;
    AxisSetup ax[2];
    State* st = nullptr;
    bool setupDone = false;
    bool hovered = false, held = false;
    double lo[2] = {0, 1}, hi[2] = {1, 1};   // limits in use
    double tlo[2] = {0, 0}, thi[2] = {1, 1};   // the same after the scale (log10)
    int autoColor = 0;
    ImVec2 afterCursor;
    bool inSubplot = false;
    bool noXLabel = false;   // too short for the x axis label: it is left out
    bool noYTicks = false;   // too narrow for the y tick labels: left out
    Look look;               // SetNextPlotLook (off: the usual look)
    double zoomMarker = std::numeric_limits<double>::quiet_NaN(); float zoomMarkerPx = 0;   // the tuner marker the wheel zoom keeps still when the pointer is near it
    int wheelAxes = -1;      // SetNextWheelAxes (-1: the usual rule)
    bool lookStrip = false;  // the x labels are on the look's strip
};
Look gNextLook;
int gNextWheel = -1;
double gNextMarker = std::numeric_limits<double>::quiet_NaN(); float gNextMarkerPx = 0;   // SetNextZoomMarker
Ctx C;
Subplots S;
Style gStyle;
std::vector<std::vector<ImVec4>> gMaps;
std::vector<int> gMapStack;

const ImVec4 kDeep[] = {ImVec4(0.298f, 0.447f, 0.690f, 1), ImVec4(0.867f, 0.518f, 0.322f, 1), ImVec4(0.333f, 0.659f, 0.408f, 1),
                        ImVec4(0.769f, 0.306f, 0.322f, 1), ImVec4(0.506f, 0.447f, 0.702f, 1), ImVec4(0.576f, 0.471f, 0.376f, 1),
                        ImVec4(0.855f, 0.545f, 0.765f, 1), ImVec4(0.549f, 0.549f, 0.549f, 1)};

ImVec4 pick(const ImVec4& c) {   // automatic colour: the next of the palette
    if (c.w >= 0) return c;
    return gStyle.AutoColors[C.autoColor++ % 8];
}

double tr(int a, double v) {   // data value to the axis' own scale
    if (C.ax[a].scale == Scale_Log10) return std::log10(std::max(v, 1e-30));
    return v;
}
float px(double x) {
    const double t = (tr(0, x) - C.tlo[0]) / (C.thi[0] - C.tlo[0]);
    return (float)(C.plot.Min.x + t * C.plot.GetWidth());
}
float py(double y) {
    const double t = (tr(1, y) - C.tlo[1]) / (C.thi[1] - C.tlo[1]);
    return (float)(C.plot.Max.y - t * C.plot.GetHeight());
}
double fromPx(float x) { return C.lo[0] + (x - C.plot.Min.x) / C.plot.GetWidth() * (C.hi[0] - C.lo[0]); }
double fromPy(float y) {
    const double t = (C.plot.Max.y - y) / C.plot.GetHeight();
    const double v = C.tlo[1] + t * (C.thi[1] - C.tlo[1]);
    return C.ax[1].scale == Scale_Log10 ? std::pow(10.0, v) : v;
}

// the step of a scale: 1, 2 or 5 times a power of ten, near the wish
double niceStep(double range, double wanted) {
    if (!(range > 0) || wanted < 1) return 1;
    const double raw = range / wanted;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double n = raw / mag;
    return (n < 1.5 ? 1 : n < 3 ? 2 : n < 7 ? 5 : 10) * mag;
}

std::string formatTick(int a, double v, double step) {
    char b[64];
    const std::string& fmt = C.ax[a].fmt;
    if (fmt == "%freq") return formatFreqTick(v, step);   // MHz with the decimals the step needs
    if (!fmt.empty()) { snprintf(b, sizeof b, fmt.c_str(), v); return b; }
    if (C.ax[a].scale == Scale_Log10) { snprintf(b, sizeof b, "%g", v); return b; }
    const int dec = std::max(0, (int)std::ceil(-std::log10(std::max(step, 1e-12)) - 1e-9));
    if (std::fabs(v) < step * 1e-6) v = 0;
    snprintf(b, sizeof b, "%.*f", std::min(dec, 8), v);
    return b;
}

struct Tick { double v; std::string label; };
// the look's label font and size, and the width of a digit in it
ImFont* lookFont() { return C.look.font ? C.look.font : ImGui::GetFont(); }
float lookFontSize() { return C.look.fontSize > 0 ? C.look.fontSize : ImGui::GetFontSize(); }
float lookCharPx() { return lookFont()->CalcTextSizeA(lookFontSize(), FLT_MAX, 0, "0").x; }
bool lookFreqAxis(int a) { return C.look.on && a == 0 && C.ax[0].fmt == "%freq" && C.ax[0].tickPos.empty() && C.ax[0].scale == Scale_Linear; }
// the frequency ticks of a look (majors and minors): the same for every plot of the same width, range and look
std::vector<slook::Tick> lookTicks() {
    const float cp = lookCharPx(), sc = gStyle.Scale;
    return slook::freqTicks(C.lo[0], C.hi[0], C.plot.GetWidth(), cp, std::max(28.f * sc, 3.f * cp), C.look.labelMode, C.look.ref, 6.0 * sc);
}
std::vector<Tick> makeTicks(int a, double pixels, double minPixelsPerTick) {
    std::vector<Tick> out;
    const AxisSetup& A = C.ax[a];
    if (lookFreqAxis(a)) {
        for (const auto& t : lookTicks()) if (t.major) out.push_back({t.mhz, t.label});
        return out;
    }
    if (!A.tickPos.empty()) {
        for (size_t i = 0; i < A.tickPos.size(); i++) out.push_back({A.tickPos[i], i < A.tickLab.size() ? A.tickLab[i] : std::string()});
        return out;
    }
    if (A.scale == Scale_Log10) {
        for (int e = (int)std::floor(C.tlo[a]); e <= (int)std::ceil(C.thi[a]); e++) {
            const double v = std::pow(10.0, e);
            if (e >= C.tlo[a] - 1e-9 && e <= C.thi[a] + 1e-9) out.push_back({v, formatTick(a, v, 1)});
        }
        return out;
    }
    // nice steps in the units of the labels
    const double u = A.unit, lo = std::min(C.lo[a] * u, C.hi[a] * u), hi = std::max(C.lo[a] * u, C.hi[a] * u);
    const double step = niceStep(hi - lo, std::max(2.0, pixels / minPixelsPerTick));
    if (!(step > 0) || !std::isfinite(step)) return out;
    for (double v = std::ceil(lo / step - 1e-9) * step; v <= hi + step * 1e-9 && out.size() < 200; v += step) out.push_back({v / u, formatTick(a, v, step)});
    return out;
}

// keep [a, b] of axis ax inside the constraints: no wider than they are, and shifted back in
static double gPanOverflowX = 0;
void clampAxis(int ax, double& a, double& b) {
    const double lo = C.ax[ax].cmin, hi = C.ax[ax].cmax;
    if (!std::isfinite(lo) || !std::isfinite(hi) || !(hi > lo)) return;
    double span = b - a;
    if (span > hi - lo) { a = lo; b = hi; return; }
    if (a < lo) { a = lo; b = lo + span; }
    if (b > hi) { b = hi; a = hi - span; }
}

void setLimitsFromSetup() {
    for (int a = 0; a < 2; a++) {
        AxisSetup& A = C.ax[a];
        double* l = C.st->lim[a];
        const bool fit = (A.flags & AxisFlags_AutoFit) != 0;
        if (A.haveLimits && (A.cond == Cond_Always || !C.st->init)) { l[0] = A.min; l[1] = A.max; }
        else if (!A.haveLimits && !C.st->init) { l[0] = 0; l[1] = 1; }
        if (fit && C.st->fitValid[a] && !(A.haveLimits && A.cond == Cond_Always)) { l[0] = C.st->fit[a][0]; l[1] = C.st->fit[a][1]; }
        if (A.linkMin && A.linkMax) {
            // first frame / double-click reset: only seed the shared axis when it has no valid range yet (keep slider or wheel zoom)
            if (!C.st->init && A.haveLimits && *A.linkMax <= *A.linkMin) { *A.linkMin = A.min; *A.linkMax = A.max; }
            if (*A.linkMax > *A.linkMin) { l[0] = *A.linkMin; l[1] = *A.linkMax; }
        }
        if (!(l[1] > l[0])) l[1] = l[0] + 1;
    }
    C.st->init = true;
}

void finishSetup() {
    if (C.setupDone) return;
    C.setupDone = true;
    setLimitsFromSetup();
    const float sc = gStyle.Scale;
    const float pad = gStyle.Pad * sc, tickLen = gStyle.TickLen * sc;
    const float th = ImGui::GetTextLineHeight();
    for (int a = 0; a < 2; a++) { C.lo[a] = C.st->lim[a][0]; C.hi[a] = C.st->lim[a][1]; }
    for (int a = 0; a < 2; a++) {
        C.tlo[a] = tr(a, C.lo[a]); C.thi[a] = tr(a, C.hi[a]);
        if (!(C.thi[a] > C.tlo[a])) C.thi[a] = C.tlo[a] + 1;
    }

    // margins: the room for the tick labels and the x label
    C.lookStrip = C.look.on && C.look.strip && !(C.ax[0].flags & AxisFlags_NoTickLabels);
    const bool xLab = !C.lookStrip && !(C.ax[0].flags & (AxisFlags_NoTickLabels | AxisFlags_Inside)), yLab = !(C.ax[1].flags & (AxisFlags_NoTickLabels | AxisFlags_Inside));
    const float edge = (C.flags & Flags_Flush) ? 0.f : pad;
    float bottom = edge, left = edge, right = edge, top = edge;
    if (xLab) bottom += th + tickLen * 0.5f;
    if (C.lookStrip) bottom += LookStripHeight(C.look);
    if (!C.ax[0].label.empty()) bottom += th + pad * 0.5f;
    if (!C.ax[0].label.empty() && (C.frame.GetHeight() - bottom - top < 3.5f * th || ImGui::CalcTextSize(C.ax[0].label.c_str()).x > C.frame.GetWidth())) { bottom -= th + pad * 0.5f; C.noXLabel = true; }   // a short (or narrow) plot keeps its room for the trace
    if (yLab) {
        // width of the y labels: measured on the ticks of a provisional plot height
        const auto ticks = makeTicks(1, std::max(10.0f, C.frame.GetHeight() - bottom - top), th * 3.0);
        float w = 0;
        for (const auto& t : ticks) w = std::max(w, ImGui::CalcTextSize(t.label.c_str()).x);
        left += w + tickLen * 0.5f + pad;
    }
    if (yLab && C.frame.GetWidth() - left - right - (xLab ? 12 * sc : 0) < 4.f * th) { left = edge; C.noYTicks = true; }   // a narrow plot keeps its room for the trace
    right += specaxis::rightOverhang(xLab, (C.flags & Flags_Flush) != 0, sc);   // the last x label sticks out to the right (not on the flush spectrum over the waterfall: #270)
    C.plot = ImRect(ImVec2(C.frame.Min.x + left, C.frame.Min.y + top), ImVec2(C.frame.Max.x - right, C.frame.Max.y - bottom));
    if (C.plot.GetWidth() < 20) C.plot.Max.x = C.plot.Min.x + 20;
    if (C.plot.GetHeight() < 20) C.plot.Max.y = C.plot.Min.y + 20;

    // equal scales: the same number of pixels per unit on both axes
    if (C.flags & Flags_Equal) {
        const double ppx = C.plot.GetWidth() / (C.hi[0] - C.lo[0]), ppy = C.plot.GetHeight() / (C.hi[1] - C.lo[1]);
        const double pp = std::min(ppx, ppy);
        const double cx = (C.lo[0] + C.hi[0]) * 0.5, cy = (C.lo[1] + C.hi[1]) * 0.5;
        C.lo[0] = cx - C.plot.GetWidth() / pp * 0.5; C.hi[0] = cx + C.plot.GetWidth() / pp * 0.5;
        C.lo[1] = cy - C.plot.GetHeight() / pp * 0.5; C.hi[1] = cy + C.plot.GetHeight() / pp * 0.5;
        for (int a = 0; a < 2; a++) { C.tlo[a] = C.lo[a]; C.thi[a] = C.hi[a]; }
    }

    // the mouse: panning (drag), zooming (wheel), back to the start (double click)
    const ImGuiID bid = ImGui::GetID("##plot");
    if (C.flags & Flags_Overlay) ImGui::SetNextItemAllowOverlap();
    ImGui::ItemAdd(C.plot, bid);
    bool hov = false, held = false;
    ImGui::ButtonBehavior(C.plot, bid, &hov, &held, ImGuiButtonFlags_MouseButtonLeft);
    C.hovered = hov; C.held = held;
    const bool movable[2] = {!(C.ax[0].haveLimits && C.ax[0].cond == Cond_Always), !(C.ax[1].haveLimits && C.ax[1].cond == Cond_Always)};
    ImGuiIO& io = ImGui::GetIO();
    if (!(C.flags & (Flags_Equal | Flags_NoPan)) && (movable[0] || movable[1])) {
        const bool smooth = (C.flags & Flags_SmoothZoom) != 0;
        for (int a = 0; a < 2; a++)   // somebody else moved the limits (a drag, a plot sharing the axis, a reset): the eased zoom gives way
            if (C.st->hasTgt[a] && (std::fabs(C.st->lim[a][0] - C.st->last[a][0]) > 1e-12 || std::fabs(C.st->lim[a][1] - C.st->last[a][1]) > 1e-12)) C.st->hasTgt[a] = false;
        gPanOverflowX = 0;
        double intendedLo = 0; bool draggedX = false;
        if (held && !(C.flags & Flags_NoDrag) && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
            if (movable[0] && C.ax[0].scale == Scale_Linear) { const double d = -io.MouseDelta.x / C.plot.GetWidth() * (C.hi[0] - C.lo[0]); C.st->lim[0][0] += d; C.st->lim[0][1] += d; intendedLo = C.st->lim[0][0]; draggedX = true; }
            if (movable[1] && C.ax[1].scale == Scale_Linear) { const double d = io.MouseDelta.y / C.plot.GetHeight() * (C.hi[1] - C.lo[1]); C.st->lim[1][0] += d; C.st->lim[1][1] += d; }
        }
        // wheel: both axes; Ctrl = frequency (x) only; Shift = amplitude (y) only (macOS turns Shift+wheel into a sideways wheel)
        const float wheel = io.MouseWheel != 0 ? io.MouseWheel : (io.KeyShift ? io.MouseWheelH : 0.f);
        const bool zx = C.wheelAxes >= 0 ? (C.wheelAxes & 1) != 0 : (!io.KeyShift || io.KeyCtrl), zy = C.wheelAxes >= 0 ? (C.wheelAxes & 2) != 0 : (!io.KeyCtrl || io.KeyShift);
        if (hov && wheel != 0 && (zx || zy) && !((C.flags & Flags_NoDrag) && C.wheelAxes < 0)) {
            ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY); ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);
            const double f = std::pow(0.9, wheel);
            const double mx = (io.MousePos.x - C.plot.Min.x) / C.plot.GetWidth(), my = 1.0 - (io.MousePos.y - C.plot.Min.y) / C.plot.GetHeight();
            for (int a = 0; a < 2; a++) {
                if (!((a == 0 ? zx : zy) && movable[a] && C.ax[a].scale == Scale_Linear)) continue;
                const double m = a == 0 ? mx : my;
                const double b0 = smooth && C.st->hasTgt[a] ? C.st->tgt[a][0] : C.st->lim[a][0], b1 = smooth && C.st->hasTgt[a] ? C.st->tgt[a][1] : C.st->lim[a][1];
                const double r = b1 - b0;
                double p = b0 + m * r, mm = m;   // the point under the mouse stays under it (of the view the zoom is heading for)
                if (a == 0 && std::isfinite(C.zoomMarker) && C.zoomMarkerPx > 0) {   // near the tuner marker: the marker stays still instead
                    p = tunedrag::zoomAnchor(p, C.zoomMarker, b0, b1, C.plot.GetWidth(), C.zoomMarkerPx);
                    mm = (p - b0) / r;
                }
                double n0 = p - mm * r * f, n1 = p + (1 - mm) * r * f;
                clampAxis(a, n0, n1);
                if (smooth) { C.st->tgt[a][0] = n0; C.st->tgt[a][1] = n1; C.st->hasTgt[a] = true; }
                else { C.st->lim[a][0] = n0; C.st->lim[a][1] = n1; }
            }
        }
        // touch: two fingers pinch (zoom about their centre) and pan together (ui/touch.h; a one-finger drag is the pan above)
        if (touch::on() && hov && touch::gesture().pinching) {
            const touch::GestureFrame& g = touch::gesture();
            const double f = 1.0 / std::max(0.2f, std::min(5.f, g.scale));
            const double mx = (g.cx - C.plot.Min.x) / C.plot.GetWidth(), my = 1.0 - (g.cy - C.plot.Min.y) / C.plot.GetHeight();
            for (int a = 0; a < 2; a++) {
                if (!(movable[a] && C.ax[a].scale == Scale_Linear)) continue;
                const double m = a == 0 ? mx : my, r = C.st->lim[a][1] - C.st->lim[a][0], p = C.st->lim[a][0] + m * r;
                const double pan = a == 0 ? -g.panDx / C.plot.GetWidth() * r * f : g.panDy / C.plot.GetHeight() * r * f;
                double n0 = p - m * r * f + pan, n1 = p + (1 - m) * r * f + pan;
                clampAxis(a, n0, n1);
                C.st->lim[a][0] = n0; C.st->lim[a][1] = n1; C.st->hasTgt[a] = false;
            }
        }
        if (smooth) {   // ease towards the target: about 80 ms to cover most of the way, whatever the frame rate
            const double k = 1.0 - std::exp(-std::min(0.1, (double)io.DeltaTime) * 22.0);
            for (int a = 0; a < 2; a++) if (C.st->hasTgt[a]) {
                for (int e = 0; e < 2; e++) C.st->lim[a][e] += (C.st->tgt[a][e] - C.st->lim[a][e]) * k;
                const double span = C.st->tgt[a][1] - C.st->tgt[a][0];
                if (std::fabs(C.st->lim[a][0] - C.st->tgt[a][0]) + std::fabs(C.st->lim[a][1] - C.st->tgt[a][1]) < span * 1e-4) { C.st->lim[a][0] = C.st->tgt[a][0]; C.st->lim[a][1] = C.st->tgt[a][1]; C.st->hasTgt[a] = false; }
            }
        }
        for (int a = 0; a < 2; a++) if (movable[a] && C.ax[a].scale == Scale_Linear) {   // a drag or an unusual limit may not leave the allowed range either
            if (!C.st->hasTgt[a]) clampAxis(a, C.st->lim[a][0], C.st->lim[a][1]);
            C.st->last[a][0] = C.st->lim[a][0]; C.st->last[a][1] = C.st->lim[a][1];
        }
        if (draggedX) gPanOverflowX = intendedLo - C.st->lim[0][0];   // how far the drag went past the constraint (the caller may retune)
        if (hov && !(C.flags & Flags_NoDoubleReset) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) C.st->init = false;
        // draw this frame with the new limits (a plot sharing the axis reads them at once, so both must show the same range)
        for (int a = 0; a < 2; a++) if (movable[a] && C.ax[a].scale == Scale_Linear && C.st->init && C.st->lim[a][1] > C.st->lim[a][0]) {
            C.lo[a] = C.tlo[a] = C.st->lim[a][0]; C.hi[a] = C.thi[a] = C.st->lim[a][1];
        }
    }
    for (int a = 0; a < 2; a++) {   // a linked axis: the pan or zoom shows at once, so the plots that share it stay in step this frame
        const AxisSetup& A = C.ax[a];
        if (!A.linkMin || !A.linkMax) continue;
        *A.linkMin = C.st->lim[a][0]; *A.linkMax = C.st->lim[a][1];
        if (C.st->init && A.scale == Scale_Linear) { C.lo[a] = C.tlo[a] = C.st->lim[a][0]; C.hi[a] = C.thi[a] = C.st->lim[a][1]; }
    }
    if (hov) C.hovered = true;

    // background and grid, then the items inside a clip rectangle
    if (C.look.on) {   // a vertical gradient
        const ImU32 t = ImGui::ColorConvertFloat4ToU32(C.look.bgTop), b = ImGui::ColorConvertFloat4ToU32(C.look.bgBottom);
        C.dl->AddRectFilledMultiColor(C.plot.Min, C.plot.Max, t, t, b, b);
    } else C.dl->AddRectFilled(C.plot.Min, C.plot.Max, ImGui::ColorConvertFloat4ToU32(gStyle.PlotBg));
    const ImU32 grid = ImGui::ColorConvertFloat4ToU32(C.look.on ? C.look.grid : gStyle.Grid);
    for (int a = 0; a < 2; a++) {
        if (C.ax[a].flags & AxisFlags_NoGridLines) continue;
        if (C.look.on && a == 1 && !C.look.gridH) continue;
        const auto ticks = a == 0 ? makeTicks(0, C.plot.GetWidth(), 70 * sc) : makeTicks(1, C.plot.GetHeight(), th * 3.0);
        for (const auto& t : ticks) {
            if (a == 0) { const float x = std::floor(px(t.v)) + 0.5f; if (x >= C.plot.Min.x && x <= C.plot.Max.x) C.dl->AddLine(ImVec2(x, C.plot.Min.y), ImVec2(x, C.plot.Max.y), grid); }
            else { const float y = std::floor(py(t.v)) + 0.5f; if (y >= C.plot.Min.y && y <= C.plot.Max.y) C.dl->AddLine(ImVec2(C.plot.Min.x, y), ImVec2(C.plot.Max.x, y), grid); }
        }
    }
    if (C.flags & Flags_Equal) {   // constellation-like plots: the zero axes, a little stronger than the grid, with a tick at every grid step
        const ImU32 zc = ImGui::ColorConvertFloat4ToU32(ImVec4(gStyle.Grid.x, gStyle.Grid.y, gStyle.Grid.z, std::min(1.f, gStyle.Grid.w * 2.2f)));
        const float zx = std::floor(px(0)) + 0.5f, zy = std::floor(py(0)) + 0.5f, tl = 3 * sc;
        if (zx >= C.plot.Min.x && zx <= C.plot.Max.x) C.dl->AddLine(ImVec2(zx, C.plot.Min.y), ImVec2(zx, C.plot.Max.y), zc);
        if (zy >= C.plot.Min.y && zy <= C.plot.Max.y) C.dl->AddLine(ImVec2(C.plot.Min.x, zy), ImVec2(C.plot.Max.x, zy), zc);
        for (int a = 0; a < 2; a++)
            for (const auto& t : (a == 0 ? makeTicks(0, C.plot.GetWidth(), 50 * sc) : makeTicks(1, C.plot.GetHeight(), 50 * sc))) {
                if (a == 0) { const float x = std::floor(px(t.v)) + 0.5f; if (x > C.plot.Min.x && x < C.plot.Max.x && zy >= C.plot.Min.y && zy <= C.plot.Max.y) C.dl->AddLine(ImVec2(x, zy - tl), ImVec2(x, zy + tl), zc); }
                else { const float y = std::floor(py(t.v)) + 0.5f; if (y > C.plot.Min.y && y < C.plot.Max.y && zx >= C.plot.Min.x && zx <= C.plot.Max.x) C.dl->AddLine(ImVec2(zx - tl, y), ImVec2(zx + tl, y), zc); }
            }
    }
    C.dl->PushClipRect(C.plot.Min, C.plot.Max, true);
}

template <class T> inline double at(const T* p, int i, int stride) { return (double)*reinterpret_cast<const T*>(reinterpret_cast<const char*>(p) + (size_t)i * (stride ? stride : sizeof(T))); }

void fitData(double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    C.ax[0].fitMin = std::min(C.ax[0].fitMin, x); C.ax[0].fitMax = std::max(C.ax[0].fitMax, x);
    C.ax[1].fitMin = std::min(C.ax[1].fitMin, y); C.ax[1].fitMax = std::max(C.ax[1].fitMax, y);
}

void drawMarker(ImVec2 p, MarkerType m, float size, ImU32 fill, ImU32 line) {
    if (m == Marker_Circle) {
        const float r = size * 1.4f;   // the size is a radius here, as in the library this replaces
        if (r <= 1.0f) C.dl->AddRectFilled(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), fill);   // tiny dots as squares: cheaper
        else C.dl->AddCircleFilled(p, r, fill, 8);
    } else if (m == Marker_Cross) {
        const float r = size;
        C.dl->AddLine(ImVec2(p.x - r, p.y), ImVec2(p.x + r, p.y), line, 1.0f);
        C.dl->AddLine(ImVec2(p.x, p.y - r), ImVec2(p.x, p.y + r), line, 1.0f);
    }
}

// DECT2_PLOTSTATS=1: the CPU time of every plot (from BeginPlot to the end of EndPlot, so the data calls between are in it) added up per
// frame, and the number of plots, printed every 2 seconds on stderr. Dev tool for the plot speed work; costs two clock reads per plot when on.
struct PlotStats {
    bool on = getenv("DECT2_PLOTSTATS") != nullptr;
    std::chrono::steady_clock::time_point t0;
    double acc = 0, worst = 0, frameAcc = 0; int plots = 0, frames = 0, lastFrame = -1; double last = 0;
    void begin() { if (on) t0 = std::chrono::steady_clock::now(); }
    void end() {
        if (!on) return;
        const double d = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const int f = ImGui::GetFrameCount();
        if (f != lastFrame) { worst = std::max(worst, frameAcc); acc += frameAcc; if (lastFrame >= 0) frames++; frameAcc = 0; lastFrame = f; }
        frameAcc += d; plots++;
        const double now = ImGui::GetTime();
        if (now - last > 2.0 && frames > 0) {
            fprintf(stderr, "plotstats: %.3f ms avg / %.2f ms max per frame in plots (%d frames, %.1f plots/frame)\n", 1e3 * acc / frames, 1e3 * worst, frames, (double)plots / frames);
            acc = worst = 0; plots = frames = 0; last = now;
        }
    }
} gPlotStats;

} // namespace

Style& GetStyle() { return gStyle; }

void SetNextPlotLook(const Look& l) { gNextLook = l; }
void SetNextWheelAxes(int mask) { gNextWheel = mask; }
void SetNextZoomMarker(double x, float snapPx) { gNextMarker = x; gNextMarkerPx = snapPx; }
float LookStripHeight(const Look& l) {
    const float fs = l.fontSize > 0 ? l.fontSize : ImGui::GetFontSize();
    return std::ceil(fs * 1.75f);
}

bool BeginPlot(const char* id, ImVec2 size, int flags) {
    if (C.active) { gNextLook.on = false; gNextWheel = -1; gNextMarker = std::numeric_limits<double>::quiet_NaN(); return false; }
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) { gNextLook.on = false; gNextWheel = -1; gNextMarker = std::numeric_limits<double>::quiet_NaN(); return false; }
    C = Ctx();
    C.look = gNextLook; gNextLook.on = false;
    C.wheelAxes = gNextWheel; gNextWheel = -1;
    C.zoomMarker = gNextMarker; C.zoomMarkerPx = gNextMarkerPx; gNextMarker = std::numeric_limits<double>::quiet_NaN();
    C.id = ImGui::GetID(id);
    C.flags = flags;
    C.dl = ImGui::GetWindowDrawList();
    ImGui::PushID(id);
    C.st = &gStates[C.id];
    if (S.active) {
        const int cell = S.next++;
        const int r = cell / S.cols, c = cell % S.cols;
        const float cw = S.size.x / S.cols, ch = S.size.y / S.rows;
        C.frame = ImRect(ImVec2(S.min.x + c * cw, S.min.y + r * ch), ImVec2(S.min.x + (c + 1) * cw, S.min.y + (r + 1) * ch));
        C.inSubplot = true;
    } else {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 s = size;
        s.x = s.x > 0 ? s.x : std::max(60.f, avail.x + s.x);
        s.y = s.y > 0 ? s.y : std::max(60.f, avail.y + s.y);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        C.frame = ImRect(p, ImVec2(p.x + s.x, p.y + s.y));
        ImGui::ItemSize(s);
    }
    C.active = true;
    gPlotStats.begin();
    return true;
}

void SetupAxes(const char* xLabel, const char* yLabel, int xFlags, int yFlags) {
    if (!C.active) return;
    C.ax[0].label = xLabel ? xLabel : ""; C.ax[1].label = yLabel ? yLabel : "";
    C.ax[0].flags = xFlags; C.ax[1].flags = yFlags;
}
void SetupAxisLimits(Axis a, double mn, double mx, Cond cond) {
    if (!C.active) return;
    C.ax[a].min = mn; C.ax[a].max = mx; C.ax[a].cond = cond; C.ax[a].haveLimits = true;
}
void SetupAxisConstraints(Axis a, double lo, double hi) { if (C.active) { C.ax[a].cmin = lo; C.ax[a].cmax = hi; } }
void SetupAxisFormat(Axis a, const char* fmt) { if (C.active) C.ax[a].fmt = fmt ? fmt : ""; }
void SetupAxisUnit(Axis a, double unit) { if (C.active && unit > 0 && std::isfinite(unit)) C.ax[a].unit = unit; }
void SetupAxisScale(Axis a, Scale s) { if (C.active) C.ax[a].scale = s; }
void SetupAxisLinks(Axis a, double* mn, double* mx) { if (C.active) { C.ax[a].linkMin = mn; C.ax[a].linkMax = mx; } }
void SetupAxisTicks(Axis a, const double* v, int n, const char* const* labels) {
    if (!C.active) return;
    C.ax[a].tickPos.assign(v, v + n);
    C.ax[a].tickLab.clear();
    for (int i = 0; i < n; i++) C.ax[a].tickLab.push_back(labels && labels[i] ? labels[i] : "");
}

template <class T> void PlotLine(const char*, const T* xs, const T* ys, int n, const Spec& s) {
    if (!C.active) return;
    finishSetup();
    const ImVec4 col = pick(s.LineColor);
    const ImU32 u = ImGui::ColorConvertFloat4ToU32(col);
    const ImU32 uf = ImGui::ColorConvertFloat4ToU32(pick(s.MarkerFillColor.w >= 0 ? s.MarkerFillColor : col));
    const float wgt = std::max(1.3f, s.LineWeight) * gStyle.Scale;   // never thinner than 1.3: the same weight on every plot
    if (s.FillFade > 0 && n > 1) { Spec fs = s; fs.FillColor = ImVec4(col.x, col.y, col.z, 0.22f); fs.LineColor = ImVec4(0, 0, 0, 0); PlotShaded(nullptr, xs, ys, n, C.lo[1], fs); }
    static std::vector<ImVec2> pts;   // kept between calls: no allocation per line per frame
    pts.clear();
    pts.reserve((size_t)n);
    auto flush = [&] { if (pts.size() > 1) C.dl->AddPolyline(pts.data(), (int)pts.size(), u, 0, wgt); pts.clear(); };
    for (int i = 0; i < n; i++) {
        const double x = at(xs, i, s.Stride), y = at(ys, i, s.Stride);
        fitData(x, y);
        if (!std::isfinite(x) || !std::isfinite(y)) { flush(); continue; }   // a gap in the data is a gap in the line
        const ImVec2 p(px(x), py(y));
        if (s.Marker != Marker_None) drawMarker(p, s.Marker, s.MarkerSize * gStyle.Scale, uf, ImGui::ColorConvertFloat4ToU32(pick(s.MarkerLineColor.w >= 0 ? s.MarkerLineColor : col)));
        // ImGui joins two segments along the mean of their normals: where the line doubles back (the up-and-down of a decimated spectrum
        // on a steep edge) the two normals nearly cancel and the stroke there collapses to nothing or juts out far. So a step of no length
        // is left out, and at a turn of more than 90 degrees the line is ended and a new one started from the same point.
        if (!pts.empty()) {
            const ImVec2 d2(p.x - pts.back().x, p.y - pts.back().y);
            if (d2.x * d2.x + d2.y * d2.y < 1e-4f) continue;
            if (pts.size() >= 2) {
                const ImVec2 d1(pts.back().x - pts[pts.size() - 2].x, pts.back().y - pts[pts.size() - 2].y);
                if (d1.x * d2.x + d1.y * d2.y < 0) { const ImVec2 q = pts.back(); flush(); pts.push_back(q); }
            }
        }
        pts.push_back(p);
    }
    flush();
}
template <class T> void PlotLine(const char* label, const T* ys, int n, const Spec& s) {
    if (!C.active) return;
    std::vector<double> xs((size_t)n);
    for (int i = 0; i < n; i++) xs[(size_t)i] = i;
    std::vector<double> y((size_t)n);
    for (int i = 0; i < n; i++) y[(size_t)i] = at(ys, i, s.Stride);
    Spec t = s; t.Stride = 0;
    PlotLine(label, xs.data(), y.data(), n, t);
}

template <class T> void PlotScatter(const char*, const T* xs, const T* ys, int n, const Spec& s) {
    if (!C.active) return;
    finishSetup();
    ImVec4 col = pick(s.MarkerFillColor.w >= 0 ? s.MarkerFillColor : s.LineColor);
    if (n > 600 && s.MarkerSize < 3.f) col.w *= scatterAlphaFactor(n);   // a dense cloud: each dot fainter, so where dots pile up the colour builds up (persistence)
    const ImU32 fill = ImGui::ColorConvertFloat4ToU32(col);
    const ImU32 line = ImGui::ColorConvertFloat4ToU32(s.MarkerLineColor.w >= 0 ? ImVec4(s.MarkerLineColor.x, s.MarkerLineColor.y, s.MarkerLineColor.z, s.MarkerLineColor.w * col.w / std::max(1e-3f, pick(s.MarkerFillColor.w >= 0 ? s.MarkerFillColor : s.LineColor).w)) : col);
    const MarkerType m = s.Marker == Marker_None ? Marker_Circle : s.Marker;
    // Round dots larger than a pixel go through one batch (all the same colour and size, so the order does not matter and the pixels
    // are those of one AddCircleFilled each: see plot_batch.h); the rest are drawn one by one as before.
    const float size = s.MarkerSize * gStyle.Scale;
    const bool batch = m == Marker_Circle && size * 1.4f > 1.0f;
    static std::vector<ImVec2> dots; static DotBatch batcher;
    dots.clear();
    for (int i = 0; i < n; i++) {
        const double x = at(xs, i, s.Stride), y = at(ys, i, s.Stride);
        fitData(x, y);
        if (!std::isfinite(x) || !std::isfinite(y)) continue;
        const ImVec2 p(px(x), py(y));
        if (p.x < C.plot.Min.x - 8 || p.x > C.plot.Max.x + 8 || p.y < C.plot.Min.y - 8 || p.y > C.plot.Max.y + 8) continue;
        if (batch) dots.push_back(p); else drawMarker(p, m, size, fill, line);
    }
    if (batch) batcher.draw(C.dl, dots.data(), (int)dots.size(), size * 1.4f, fill);
}

template <class T> void PlotScatter(const char* label, const T* ys, int n, double xScale, double xStart, const Spec& s) {
    if (!C.active) return;
    std::vector<double> xs((size_t)n), y((size_t)n);
    for (int i = 0; i < n; i++) { xs[(size_t)i] = xStart + i * xScale; y[(size_t)i] = at(ys, i, s.Stride); }
    Spec t = s; t.Stride = 0;
    PlotScatter(label, xs.data(), y.data(), n, t);
}

template <class T> void PlotBars(const char*, const T* xs, const T* ys, int n, double width, const Spec& s) {
    if (!C.active) return;
    finishSetup();
    const ImU32 fill = ImGui::ColorConvertFloat4ToU32(pick(s.FillColor));
    const double base = C.ax[1].scale == Scale_Log10 ? C.lo[1] : 0.0;
    for (int i = 0; i < n; i++) {
        const double x = at(xs, i, 0), y = at(ys, i, 0);
        fitData(x, y); fitData(x, base);
        if (!std::isfinite(x) || !std::isfinite(y) || (C.ax[1].scale == Scale_Log10 && y <= 0)) continue;
        const float x0 = px(x - width * 0.5), x1 = px(x + width * 0.5);
        C.dl->AddRectFilled(ImVec2(std::min(x0, x1), std::min(py(y), py(base))), ImVec2(std::max(x0, x1) - (std::fabs(x1 - x0) > 2 ? 1.0f : 0.0f), std::max(py(y), py(base))), fill, std::fabs(x1 - x0) > 6 ? 1.5f : 0.f, ImDrawFlags_RoundCornersTop);
    }
    (void)s;
}

template <class T> void PlotShaded(const char*, const T* xs, const T* ys, int n, double yRef, const Spec& s) {
    if (!C.active) return;
    finishSetup();
    const ImVec4 fc = pick(s.FillColor);
    const ImU32 fill = ImGui::ColorConvertFloat4ToU32(fc);
    const ImU32 fillLow = ImGui::ColorConvertFloat4ToU32(ImVec4(fc.x, fc.y, fc.z, fc.w * fillBottomAlpha(s.FillFade)));
    const ImDrawListFlags keep = C.dl->Flags;
    if (n > 400) C.dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;   // a trace of thousands of columns: two plain triangles each, without the feathered edge
    for (int i = 0; i + 1 < n; i++) {
        const double x0 = at(xs, i, s.Stride), x1 = at(xs, i + 1, s.Stride), y0 = at(ys, i, s.Stride), y1 = at(ys, i + 1, s.Stride);
        fitData(x0, y0); fitData(x1, y1);
        if (!std::isfinite(x0 + x1 + y0 + y1)) continue;
        const ImVec2 q[4] = {ImVec2(px(x0), py(y0)), ImVec2(px(x1), py(y1)), ImVec2(px(x1), py(yRef)), ImVec2(px(x0), py(yRef))};
        if (s.FillFade > 0) {   // a vertical gradient: the quad as two triangles, the bottom corners at the faded alpha (no feathered edge)
            C.dl->PrimReserve(6, 4);
            const ImVec2 uv = C.dl->_Data->TexUvWhitePixel;
            const unsigned base = C.dl->_VtxCurrentIdx;
            for (int k = 0; k < 4; k++) C.dl->PrimWriteVtx(q[k], uv, k < 2 ? fill : fillLow);
            const ImDrawIdx ix[6] = {0, 1, 2, 0, 2, 3};
            for (int k = 0; k < 6; k++) C.dl->PrimWriteIdx((ImDrawIdx)(base + ix[k]));
            continue;
        }
        C.dl->AddConvexPolyFilled(q, 4, fill);
    }
    C.dl->Flags = keep;
}

template <class T> void PlotInfLines(const char*, const T* values, int n, const Spec& s) {
    if (!C.active) return;
    finishSetup();
    const ImU32 u = ImGui::ColorConvertFloat4ToU32(pick(s.LineColor));
    const float wgt = std::max(0.5f, s.LineWeight) * gStyle.Scale;
    for (int i = 0; i < n; i++) {
        const double v = at(values, i, 0);
        if (!std::isfinite(v)) continue;
        if (s.Flags & InfLines_Horizontal) { const float y = py(v); C.dl->AddLine(ImVec2(C.plot.Min.x, y), ImVec2(C.plot.Max.x, y), u, wgt); }
        else { const float x = px(v); C.dl->AddLine(ImVec2(x, C.plot.Min.y), ImVec2(x, C.plot.Max.y), u, wgt); }
    }
}

void PlotImage(const char*, ImTextureID tex, Point bmin, Point bmax, ImVec2 uv0, ImVec2 uv1) {
    if (!C.active) return;
    finishSetup();
    C.dl->AddImage(tex, ImVec2(px(bmin.x), py(bmax.y)), ImVec2(px(bmax.x), py(bmin.y)), uv0, uv1);
}

static ImVec4 sampleMap(double t) {
    const std::vector<ImVec4>* m = gMapStack.empty() ? nullptr : &gMaps[(size_t)gMapStack.back()];
    t = std::min(1.0, std::max(0.0, t));
    if (!m || m->empty()) return ImVec4((float)t, (float)t, (float)t, 1);
    const double f = t * ((double)m->size() - 1);
    const size_t i = std::min((size_t)f, m->size() - 1), j = std::min(i + 1, m->size() - 1);
    const float k = (float)(f - (double)i);
    const ImVec4 a = (*m)[i], b = (*m)[j];
    return ImVec4(a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k, a.z + (b.z - a.z) * k, a.w + (b.w - a.w) * k);
}

void PlotHeatmap(const char*, const float* v, int rows, int cols, double smin, double smax, const char*, Point bmin, Point bmax) {
    if (!C.active || rows <= 0 || cols <= 0) return;
    finishSetup();
    const double dx = (bmax.x - bmin.x) / cols, dy = (bmax.y - bmin.y) / rows;
    const double range = smax > smin ? smax - smin : 1;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            const float x0 = px(bmin.x + c * dx), x1 = px(bmin.x + (c + 1) * dx);
            const float y0 = py(bmax.y - r * dy), y1 = py(bmax.y - (r + 1) * dy);
            C.dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1 + 0.5f, y1 + 0.5f), ImGui::ColorConvertFloat4ToU32(sampleMap((v[r * cols + c] - smin) / range)));
        }
}

void PlotText(const char* text, double x, double y, ImVec2 off) {
    if (!C.active) return;
    finishSetup();
    const ImVec2 sz = ImGui::CalcTextSize(text);
    const ImVec2 p(px(x) + off.x * 1.0f - sz.x * 0.5f, py(y) + off.y - sz.y * 0.5f);
    C.dl->AddText(p, ImGui::ColorConvertFloat4ToU32(gStyle.Text), text);
}

void PlotVBandGradient(double x0, double x1, ImVec4 top, ImVec4 bottom) {
    if (!C.active) return;
    finishSetup();
    if (!std::isfinite(x0 + x1)) return;
    float a = px(std::min(x0, x1)), b = px(std::max(x0, x1));
    if (b - a < 1) { const float m = 0.5f * (a + b); a = m - 0.5f; b = m + 0.5f; }
    const ImU32 t = ImGui::ColorConvertFloat4ToU32(top), u = ImGui::ColorConvertFloat4ToU32(bottom);
    C.dl->AddRectFilledMultiColor(ImVec2(a, C.plot.Min.y), ImVec2(b, C.plot.Max.y), t, t, u, u);
}

void PlotVBand(const char*, double x0, double x1, const Spec& s) {
    if (!C.active) return;
    finishSetup();
    if (!std::isfinite(x0 + x1)) return;
    float a = px(std::min(x0, x1)), b = px(std::max(x0, x1));
    if (b - a < 1) { const float m = 0.5f * (a + b); a = m - 0.5f; b = m + 0.5f; }
    C.dl->AddRectFilled(ImVec2(a, C.plot.Min.y), ImVec2(b, C.plot.Max.y), ImGui::ColorConvertFloat4ToU32(pick(s.FillColor)));
}

int AddColormap(const char*, const ImVec4* colors, int n) {
    gMaps.emplace_back(colors, colors + n);
    return (int)gMaps.size() - 1;
}
void PushColormap(int map) { gMapStack.push_back(map); }
void PopColormap() { if (!gMapStack.empty()) gMapStack.pop_back(); }

Point GetPlotMousePos() {
    const ImVec2 m = ImGui::GetIO().MousePos;
    return Point(fromPx(m.x), fromPy(m.y));
}
bool IsPlotHovered() { return C.active && C.hovered; }
double PanOverflowX() { return gPanOverflowX; }
bool IsPlotHeld() { return C.active && C.held; }
ImVec2 GetPlotPos() { if (C.active) finishSetup(); return C.plot.Min; }
ImVec2 GetPlotSize() { if (C.active) finishSetup(); return C.plot.GetSize(); }
Rect GetPlotLimits() {
    Rect r;
    r.X.Min = C.lo[0]; r.X.Max = C.hi[0]; r.Y.Min = C.lo[1]; r.Y.Max = C.hi[1];
    return r;
}

void EndPlot() {
    if (!C.active) return;
    struct Timed { ~Timed() { gPlotStats.end(); } } timed;
    finishSetup();
    if (C.look.on && (C.look.overV.w > 0 || C.look.overH.w > 0)) {   // the grid over the items (the waterfall's picture)
        if (C.look.overV.w > 0) {
            const ImU32 g = ImGui::ColorConvertFloat4ToU32(C.look.overV);
            for (const auto& t : makeTicks(0, C.plot.GetWidth(), 70 * gStyle.Scale)) {
                const float x = std::floor(px(t.v)) + 0.5f;
                if (x >= C.plot.Min.x && x <= C.plot.Max.x) C.dl->AddLine(ImVec2(x, C.plot.Min.y), ImVec2(x, C.plot.Max.y), g);
            }
        }
        if (C.look.overH.w > 0) {
            const ImU32 g = ImGui::ColorConvertFloat4ToU32(C.look.overH);
            for (const auto& t : makeTicks(1, C.plot.GetHeight(), ImGui::GetTextLineHeight() * 3.0)) {
                const float y = std::floor(py(t.v)) + 0.5f;
                if (y > C.plot.Min.y + 1 && y < C.plot.Max.y - 1) C.dl->AddLine(ImVec2(C.plot.Min.x, y), ImVec2(C.plot.Max.x, y), g);
            }
        }
    }
    C.dl->PopClipRect();
    const float sc = gStyle.Scale, th = ImGui::GetTextLineHeight(), tickLen = gStyle.TickLen * sc, pad = gStyle.Pad * sc;
    const ImU32 txt = ImGui::ColorConvertFloat4ToU32(gStyle.Text);
    // tick marks and labels outside the plot area
    if (C.lookStrip) {   // the look's strip: minor and major ticks at its top, the labels big and centred under their ticks
        const float y0 = C.plot.Max.y, h = LookStripHeight(C.look), x0 = C.frame.Min.x, x1 = C.frame.Max.x;
        C.dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + h), ImGui::ColorConvertFloat4ToU32(C.look.stripBg));
        ImFont* font = lookFont();
        const float fs = lookFontSize();
        const ImVec4 lc = C.look.label;
        const ImU32 lab = ImGui::ColorConvertFloat4ToU32(lc), maj = ImGui::ColorConvertFloat4ToU32(ImVec4(lc.x, lc.y, lc.z, lc.w * 0.55f)),
                    mnr = ImGui::ColorConvertFloat4ToU32(ImVec4(lc.x, lc.y, lc.z, lc.w * 0.28f));
        float stripEnd = -FLT_MAX;
        for (const auto& t : lookTicks()) {
            const float x = std::floor(px(t.mhz)) + 0.5f;
            if (x < C.plot.Min.x || x > C.plot.Max.x) continue;
            C.dl->AddLine(ImVec2(x, y0), ImVec2(x, y0 + (t.major ? 5.f : 2.5f) * sc), t.major ? maj : mnr);
            if (!t.major || t.label.empty()) continue;
            const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, t.label.c_str());
            // never clipped by the panel: a label that would cross its edge is shifted inside (dropped when that would touch the label before it)
            const float tx = std::max(x0 + 2 * sc, std::min(x1 - 2 * sc - ts.x, x - ts.x * 0.5f));
            if (tx < stripEnd + 4 * sc) continue;
            stripEnd = tx + ts.x;
            C.dl->AddText(font, fs, ImVec2(std::round(tx), std::round(y0 + (h - ts.y) * 0.5f + 1.5f * sc)), lab, t.label.c_str());
        }
    }
    for (int a = 0; a < 2; a++) {
        if ((C.ax[a].flags & AxisFlags_NoTickLabels) || (a == 1 && C.noYTicks) || (a == 0 && C.lookStrip)) continue;
        const auto ticks = a == 0 ? makeTicks(0, C.plot.GetWidth(), 70 * sc) : makeTicks(1, C.plot.GetHeight(), th * 3.0);
        float lastEnd = -FLT_MAX;   // a narrow plot: an x label that would touch the one before it is left out (the tick stays)
        for (const auto& t : ticks) {
            if (a == 0) {
                const float x = px(t.v);
                if (x < C.plot.Min.x - 1 || x > C.plot.Max.x + 1) continue;
                C.dl->AddLine(ImVec2(std::floor(x) + 0.5f, C.plot.Max.y), ImVec2(std::floor(x) + 0.5f, C.plot.Max.y + tickLen * 0.5f), txt);
                const ImVec2 s = ImGui::CalcTextSize(t.label.c_str());
                const float lx = std::max(C.frame.Min.x + 2 * sc, std::min(C.frame.Max.x - 2 * sc - s.x, x - s.x * 0.5f));   // a label that would leave the panel is shifted inside (#230)
                if (lx < lastEnd + 4 * sc) continue;
                lastEnd = lx + s.x;
                C.dl->AddText(ImVec2(lx, C.plot.Max.y + tickLen * 0.5f), txt, t.label.c_str());
            } else {
                const float y = py(t.v);
                if (y < C.plot.Min.y - 1 || y > C.plot.Max.y + 1) continue;
                C.dl->AddLine(ImVec2(C.plot.Min.x - tickLen * 0.5f, std::floor(y) + 0.5f), ImVec2(C.plot.Min.x, std::floor(y) + 0.5f), txt);
                const ImVec2 s = ImGui::CalcTextSize(t.label.c_str());
                C.dl->AddText(ImVec2(C.plot.Min.x - tickLen * 0.5f - pad * 0.5f - s.x, y - s.y * 0.5f), txt, t.label.c_str());
            }
        }
    }
    // tick labels inside the plot: small and faint, x along the bottom edge, y on the left (clear of the y label and the x labels)
    if ((C.ax[0].flags | C.ax[1].flags) & AxisFlags_Inside) {
        ImFont* font = ImGui::GetFont();
        const float fs = ImGui::GetFontSize() * 0.85f, lh = fs, m = 4 * sc;
        const ImVec4 tc = C.look.on ? C.look.label : gStyle.Text;
        const ImU32 faint = ImGui::ColorConvertFloat4ToU32(ImVec4(tc.x, tc.y, tc.z, C.look.on ? 0.62f : 0.65f));
        const bool xIn = (C.ax[0].flags & AxisFlags_Inside) && !(C.ax[0].flags & AxisFlags_NoTickLabels);
        if (xIn) {
            float lastEnd = C.plot.Min.x;
            for (const auto& t : makeTicks(0, C.plot.GetWidth(), 70 * sc)) {
                const float x = px(t.v);
                const ImVec2 s = font->CalcTextSizeA(fs, FLT_MAX, 0, t.label.c_str());
                if (x - s.x * 0.5f < lastEnd + m || x + s.x * 0.5f > C.plot.Max.x - m) continue;
                lastEnd = x + s.x * 0.5f;
                C.dl->AddText(font, fs, ImVec2(x - s.x * 0.5f, C.plot.Max.y - lh - 2 * sc), faint, t.label.c_str());
            }
        }
        if ((C.ax[1].flags & AxisFlags_Inside) && !(C.ax[1].flags & AxisFlags_NoTickLabels)) {
            const float top = C.plot.Min.y + (C.ax[1].label.empty() ? 0 : th + 3 * sc), bot = C.plot.Max.y - (xIn ? lh + 2 * sc : 0);
            for (const auto& t : makeTicks(1, C.plot.GetHeight(), th * 3.0)) {
                const float y = py(t.v);
                if (y - lh * 0.5f < top + 1 || y + lh * 0.5f > bot - 1) continue;
                C.dl->AddText(font, fs, ImVec2(C.plot.Min.x + m, y - lh * 0.5f), faint, t.label.c_str());
            }
        }
    }
    if (!C.ax[0].label.empty() && !C.noXLabel) {
        const ImVec2 s = ImGui::CalcTextSize(C.ax[0].label.c_str());
        C.dl->AddText(ImVec2(C.plot.Min.x + (C.plot.GetWidth() - s.x) * 0.5f, C.frame.Max.y - th - pad * 0.5f), txt, C.ax[0].label.c_str());
    }
    // the y label sits inside the plot, in the top left corner: no rotated text needed
    if (!C.ax[1].label.empty() && C.plot.GetHeight() >= 2.5f * th && C.plot.GetWidth() >= ImGui::CalcTextSize(C.ax[1].label.c_str()).x + 12 * sc) C.dl->AddText(ImVec2(C.plot.Min.x + 6 * sc, C.plot.Min.y + 3 * sc), ImGui::ColorConvertFloat4ToU32(ImVec4(gStyle.Text.x, gStyle.Text.y, gStyle.Text.z, 0.8f)), C.ax[1].label.c_str());
    {
        const ImU32 bc = ImGui::ColorConvertFloat4ToU32(gStyle.Border);
        if (!(C.flags & (Flags_OpenTop | Flags_OpenBottom))) C.dl->AddRect(C.plot.Min, C.plot.Max, bc);
        else {   // a plot stacked on another: no line on the shared edge
            const ImVec2 a(C.plot.Min.x + 0.5f, C.plot.Min.y + 0.5f), b(C.plot.Max.x - 0.5f, C.plot.Max.y - 0.5f);
            C.dl->AddLine(ImVec2(a.x, a.y), ImVec2(a.x, b.y + 0.5f), bc);
            C.dl->AddLine(ImVec2(b.x, a.y), ImVec2(b.x, b.y + 0.5f), bc);
            if (!(C.flags & Flags_OpenTop)) C.dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x + 0.5f, a.y), bc);
            if (!(C.flags & Flags_OpenBottom)) C.dl->AddLine(ImVec2(a.x, b.y), ImVec2(b.x + 0.5f, b.y), bc);
        }
    }
    // the position of the mouse
    if (C.hovered && !(C.flags & Flags_NoMouseText) && !C.look.on) {
        const Point m = GetPlotMousePos();
        char b[64];
        snprintf(b, sizeof b, "%.4g, %.4g", m.x * C.ax[0].unit, m.y * C.ax[1].unit);
        const ImVec2 s = ImGui::CalcTextSize(b);
        const float lift = (C.ax[0].flags & AxisFlags_Inside) ? th : 0.f;   // above the inside x labels
        C.dl->AddText(ImVec2(C.plot.Max.x - s.x - 5 * sc, C.plot.Max.y - s.y - 3 * sc - lift), txt, b);
    }
    // what was plotted decides the limits of an automatically fitted axis, from the next frame on
    for (int a = 0; a < 2; a++) {
        const AxisSetup& A = C.ax[a];
        if ((A.flags & AxisFlags_AutoFit) && std::isfinite(A.fitMin) && std::isfinite(A.fitMax)) {
            double lo = A.fitMin, hi = A.fitMax;
            if (!(hi > lo)) { lo -= 0.5; hi += 0.5; }
            const double m = (hi - lo) * 0.05;
            C.st->fit[a][0] = lo - m; C.st->fit[a][1] = hi + m; C.st->fitValid[a] = true;
        }
    }
    ImGui::PopID();
    C.active = false;
}

bool BeginSubplots(const char*, int rows, int cols, ImVec2 size, int) {
    if (S.active) return false;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 s = size;
    s.x = s.x > 0 ? s.x : std::max(60.f, avail.x + s.x);
    s.y = s.y > 0 ? s.y : std::max(60.f, avail.y + s.y);
    S.active = true; S.rows = std::max(1, rows); S.cols = std::max(1, cols); S.next = 0;
    S.min = ImGui::GetCursorScreenPos(); S.size = s;
    ImGui::ItemSize(s);
    return true;
}
void EndSubplots() { S.active = false; }

// the templates for the data types the application uses
#define PLT_INSTANTIATE(T) \
    template void PlotLine<T>(const char*, const T*, const T*, int, const Spec&); \
    template void PlotLine<T>(const char*, const T*, int, const Spec&); \
    template void PlotScatter<T>(const char*, const T*, const T*, int, const Spec&); \
    template void PlotScatter<T>(const char*, const T*, int, double, double, const Spec&); \
    template void PlotBars<T>(const char*, const T*, const T*, int, double, const Spec&); \
    template void PlotShaded<T>(const char*, const T*, const T*, int, double, const Spec&); \
    template void PlotInfLines<T>(const char*, const T*, int, const Spec&);
PLT_INSTANTIATE(float)
PLT_INSTANTIATE(double)

} // namespace plt
