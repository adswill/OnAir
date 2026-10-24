// The 3D waterfall in the waterfall's place (Waterfall popup > 3D view): the history as a height field, coloured with the waterfall's
// palette and levels. The pure parts (camera, decimation, mesh, pick) are in wf3d.h; here the input, the cache and the drawing.
//
// Cost: the mesh is made again only when a row comes in, the camera moves or the plot changes size; any other frame copies the cached
// screen-space vertices into the draw list (wf3d_draw.h). The mesh is sized to the plot (wf3d.h budget), smaller in Low power mode,
// which also makes it again at most 15 times a second. Measured in tests/app/test_wf3d.cpp.
//
// Mouse: drag orbits, Shift+drag / right drag / middle drag pans, wheel zooms, Ctrl+wheel sets the height, a click on the surface tunes,
// a double click goes back to the chosen angle. Keys (with the mouse over it): arrows orbit (Shift: pan), + and - zoom, 1-5 the angles,
// [ and ] the height, Space freezes, Home resets. Touch: one finger orbits, a pinch zooms and pans.
#include "../app.h"
#include "wf3d.h"
#include "wf3d_draw.h"
#include "spec_axis.h"
#include "../ui/touch.h"
#include <cmath>
#if __has_include("dect2/power/power.h")
#include "dect2/power/power.h"
static bool lowPowerNow() { return dect2::power::lowPower(); }   // View > Power > Low power mode (perf-power)
#else
static bool lowPowerNow() { return false; }
#endif

namespace {

void cameraKeys(w3d::State& s, float viewH) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || io.KeyCtrl || io.KeySuper || io.KeyAlt) return;
    w3d::Camera& c = s.cam;
    const float step = 6.f;   // pixels of drag a key press stands for, repeated while held
    auto held = [](ImGuiKey k) { return ImGui::IsKeyDown(k); };
    if (io.KeyShift) {
        if (held(ImGuiKey_LeftArrow)) c.pan(-step, 0, viewH);
        if (held(ImGuiKey_RightArrow)) c.pan(step, 0, viewH);
        if (held(ImGuiKey_UpArrow)) c.pan(0, -step, viewH);
        if (held(ImGuiKey_DownArrow)) c.pan(0, step, viewH);
    } else {
        if (held(ImGuiKey_LeftArrow)) c.orbit(-step, 0);
        if (held(ImGuiKey_RightArrow)) c.orbit(step, 0);
        if (held(ImGuiKey_UpArrow)) c.orbit(0, step);
        if (held(ImGuiKey_DownArrow)) c.orbit(0, -step);
    }
    if (held(ImGuiKey_Equal) || held(ImGuiKey_KeypadAdd)) c.zoom(0.15f);
    if (held(ImGuiKey_Minus) || held(ImGuiKey_KeypadSubtract)) c.zoom(-0.15f);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true)) s.heightScale = std::max(0.05f, s.heightScale / 1.1f);
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, true)) s.heightScale = std::min(1.5f, s.heightScale * 1.1f);
    static const ImGuiKey num[w3d::NumPresets] = {ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5};
    for (int i = 0; i < w3d::NumPresets; i++)
        if (ImGui::IsKeyPressed(num[i], false)) { s.preset = i; c = w3d::preset(i); }
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) c = w3d::preset(s.preset);
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) s.frozen = !s.frozen;
}

// a 1-2-5 step for about n labels over span
double niceStep(double span, int n) {
    const double raw = span / std::max(1, n), mag = std::pow(10.0, std::floor(std::log10(raw))), f = raw / mag;
    return (f < 1.5 ? 1 : f < 3 ? 2 : f < 7 ? 5 : 10) * mag;
}

void axisLabels(App& a, ImDrawList* dl, const w3d::State& s, const w3d::Proj& p, const w3d::Look& lk, ImVec2 o) {
    const w3d::Grid& g = s.grid;
    const w3d::State::Band& gMap = s.band;
    if (g.cols < 2) return;
    const ImU32 col = ImGui::GetColorU32(plt::GetStyle().Text);
    const double f0 = w3d::uToMhz(0, g.cols, gMap.c0, gMap.c1, gMap.W, gMap.imgLo, gMap.fs), f1 = w3d::uToMhz(1, g.cols, gMap.c0, gMap.c1, gMap.W, gMap.imgLo, gMap.fs);
    if (!(f1 > f0)) return;
    const double st = niceStep(f1 - f0, 5);
    const int dec = std::max(0, std::min(6, (int)std::ceil(-std::log10(st) - 1e-9)));
    for (double f = std::ceil(f0 / st) * st; f <= f1 + 1e-12; f += st) {
        const float u = (float)w3d::mhzToU(f, g.cols, gMap.c0, gMap.c1, gMap.W, gMap.imgLo, gMap.fs);
        float x, y, d;
        if (!w3d::project(p, w3d::v3(u - 0.5f, 0, lk.depth * 0.5f), x, y, d)) continue;
        char b[32]; snprintf(b, sizeof b, "%.*f", dec, f);
        const ImVec2 ts = ImGui::CalcTextSize(b);
        dl->AddText(ImVec2(o.x + x - ts.x / 2, o.y + y + 2 * gUi), col, b);
    }
    // the time along the left edge: now at the front, the oldest row at the back
    const double dt = a.wf.rowDt > 0 ? a.wf.rowDt : 1.0 / 30;
    const int group = std::max(1, s.key.group);
    for (int i = 0; i <= 2; i++) {
        const float v = i / 2.f;
        float x, y, d;
        if (!w3d::project(p, w3d::v3(-0.5f, 0, lk.depth * 0.5f - lk.depth * v), x, y, d)) continue;
        char b[32];
        if (i == 0) snprintf(b, sizeof b, "%s", tr("now"));
        else snprintf(b, sizeof b, "-%.0f s", v * (g.rows - 1) * group * dt);
        const ImVec2 ts = ImGui::CalcTextSize(b);
        dl->AddText(ImVec2(o.x + x - ts.x - 4 * gUi, o.y + y - ts.y / 2), col, b);
    }
}

// the tuned channel: a see-through plane standing at the frequency, and its width on the floor
void channelMarker(App& a, ImDrawList* dl, const w3d::State& s, const w3d::Proj& p, const w3d::Look& lk, ImVec2 o) {
    const w3d::Grid& g = s.grid;
    const w3d::State::Band& gMap = s.band;
    if (g.cols < 2) return;
    const float u = (float)w3d::mhzToU(a.freqMhz, g.cols, gMap.c0, gMap.c1, gMap.W, gMap.imgLo, gMap.fs);
    if (u < 0 || u > 1) return;
    const ChanWidth cw = channelWidth(a);
    const double fspan = w3d::uToMhz(1, g.cols, gMap.c0, gMap.c1, gMap.W, gMap.imgLo, gMap.fs) - w3d::uToMhz(0, g.cols, gMap.c0, gMap.c1, gMap.W, gMap.imgLo, gMap.fs);
    const float hw = fspan > 0 && cw.mhz > 0 ? (float)(cw.mhz / 2 / fspan) : 0.f;
    const float z0 = lk.depth * 0.5f, z1 = -lk.depth * 0.5f, top = lk.heightScale * 1.05f;
    auto pt = [&](float x, float y, float z, ImVec2& out) { float sx, sy, d; if (!w3d::project(p, w3d::v3(x, y, z), sx, sy, d)) return false; out = ImVec2(o.x + sx, o.y + sy); return true; };
    ImVec2 q[4];
    const float x = u - 0.5f;
    if (hw > 0) {   // the band on the floor
        const float xa = std::max(-0.5f, x - hw), xb = std::min(0.5f, x + hw);
        if (pt(xa, 0, z0, q[0]) && pt(xb, 0, z0, q[1]) && pt(xb, 0, z1, q[2]) && pt(xa, 0, z1, q[3])) dl->AddQuadFilled(q[0], q[1], q[2], q[3], IM_COL32(255, 80, 80, 40));
    }
    if (pt(x, 0, z0, q[0]) && pt(x, top, z0, q[1]) && pt(x, top, z1, q[2]) && pt(x, 0, z1, q[3])) {
        dl->AddQuadFilled(q[0], q[1], q[2], q[3], IM_COL32(255, 70, 70, 46));
        dl->AddLine(q[0], q[1], IM_COL32(255, 90, 90, 200), 1.5f * gUi);
        dl->AddLine(q[1], q[2], IM_COL32(255, 90, 90, 110), 1.f * gUi);
    }
}

} // namespace

void waterfall3dPlot(App& a, ImVec2 size) {
    w3d::State& s = a.wf.s3d;
    if (a.wf.paletteStale()) a.wf.palette();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 sz(size.x > 0 ? size.x : avail.x, size.y > 0 ? size.y : avail.y);
    sz.x = std::max(60.f, sz.x); sz.y = std::max(60.f, sz.y);
    ImGui::SetNextItemAllowOverlap();   // the corner buttons are drawn over it afterwards: they must get the clicks (else "3D" did nothing and the view could not be left)
    ImGui::InvisibleButton("##wf3d", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    if (hovered) { ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY); }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const double t0 = glfwGetTime();
    dl->AddRectFilled(o, ImVec2(o.x + sz.x, o.y + sz.y), ImGui::GetColorU32(plt::GetStyle().PlotBg));
    dl->PushClipRect(o, ImVec2(o.x + sz.x, o.y + sz.y), true);

    // ---- the data: the columns in view (the spectrum's frequency range), decimated to the budget
    const double fs = (a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate) / 1e6;
    const double x0 = specaxis::imageLo(radioCenterMhz(a), fs, a.smooth.empty() ? (size_t)a.wf.cols : a.smooth.size());
    const int W = a.wf.cols;
    double lo = 0, hi = 0;
    specFreqViewGet(lo, hi);
    if (!(hi > lo)) { lo = x0; hi = x0 + fs; }
    int c0 = 0, c1 = W;
    specaxis::visibleCols(lo, hi, x0, fs, W, c0, c1);
    const ImVec2 fbs = ImGui::GetIO().DisplayFramebufferScale;
    const bool lowPower = lowPowerNow();
    s.rows = std::max(64, std::min(1024, s.rows));
    const w3d::Budget b = w3d::budget(c1 - c0, sz.x * std::max(1.f, fbs.x), s.rows, lowPower);
    if (!s.frozen && !a.wf.paused && !a.wf.lv.empty() && a.wf.rows > 0 && fs > 0) {
        if (w3d::ingest(s, a.wf.lv.data(), W, Waterfall::H, a.wf.writeRow, a.wf.rows - 1, a.wf.filled, c0, c1, b.cols, 1024))
            s.band.c0 = s.srcC0, s.band.c1 = s.srcC1, s.band.W = W, s.band.imgLo = x0, s.band.fs = fs;
    }

    // ---- input
    ImGuiIO& io = ImGui::GetIO();
    w3d::Camera& cam = s.cam;
    const bool touchPinch = a.touchUi && hovered && touch::gesture().pinching;
    if (touchPinch) { const touch::GestureFrame& g = touch::gesture(); w3d::applyGesture(cam, true, g.scale, g.panDx, g.panDy, false, sz.y); }
    else if (active) {
        const ImVec2 d = io.MouseDelta;
        if (ImGui::IsMouseDragging(0, 2) && !io.KeyShift) cam.orbit(d.x, d.y);
        else if (ImGui::IsMouseDragging(0, 2) || ImGui::IsMouseDragging(1, 2) || ImGui::IsMouseDragging(2, 2)) cam.pan(d.x, d.y, sz.y);
    }
    if (hovered && io.MouseWheel != 0) {
        if (io.KeyCtrl || io.KeySuper) s.heightScale = std::min(1.5f, std::max(0.05f, s.heightScale * std::pow(1.1f, io.MouseWheel)));
        else cam.zoom(io.MouseWheel);
    }
    if (hovered && ImGui::IsMouseDoubleClicked(0)) cam = w3d::preset(s.preset);
    if (hovered) cameraKeys(s, sz.y);

    // ---- the mesh, made again only when something it depends on changed
    w3d::Look lk; lk.heightScale = s.heightScale; lk.shade = s.shade && !lowPower;
    lk.depth = std::min(1.f, std::max(0.45f, 2.2f * sz.y / sz.x));   // a wide, short plot: a shallower time axis, so the band gets the width
    // relative to the plot's corner (moving the window keeps the mesh); framed to fill the plot whatever its shape
    const w3d::Proj p = w3d::makeProj(cam, 0, 0, sz.x, sz.y, w3d::fitScale(cam, sz.x, sz.y, std::min(0.5f, lk.heightScale), lk.depth));
    w3d::Key k;
    k.rows = s.hist.count; k.lutGen = a.wf.lutGen; k.cols = s.hist.cols; k.gridRows = b.rows; k.group = b.group;
    k.vw = sz.x; k.vh = sz.y; k.yaw = cam.yaw; k.pitch = cam.pitch; k.dist = cam.dist; k.tx = cam.tx; k.ty = cam.ty; k.tz = cam.tz; k.hs = lk.heightScale;
    k.smooth = s.smooth; k.shade = lk.shade;
    bool rebuild = k != s.key;
    if (rebuild && lowPower && k.rows != s.key.rows) {   // Low power: new rows at most 15 times a second (a camera move is at once)
        w3d::Key onlyRows = s.key; onlyRows.rows = k.rows;
        static double lastT = 0;
        if (onlyRows == k && glfwGetTime() - lastT < 1.0 / 15) rebuild = false;
        else lastT = glfwGetTime();
    }
    if (rebuild && s.hist.have() > 0) {
        const double tb = glfwGetTime();
        if (k.rows != s.key.rows || k.cols != s.key.cols || k.gridRows != s.key.gridRows || k.group != s.key.group || k.smooth != s.key.smooth || k.lutGen != s.key.lutGen)
            w3d::buildGrid(s.hist, b.rows, b.group, s.smooth, s.grid, s.tmp);
        w3d::buildMesh(s.grid, p, lk, a.wf.lut, s.mesh);
        w3d::peakLine(s.grid, p, lk, false, s.ridge);
        s.key = k;
        s.lastBuildMs = 1e3 * (glfwGetTime() - tb);
    }

    // ---- draw: the ridge behind the surface when the back is far (the surface hides it), else over it
    const ImU32 ridgeCol = IM_COL32(255, 255, 255, 170);
    const bool ridgeFirst = w3d::backIsFar(p);
    if (s.peak && ridgeFirst) w3d::drawRidge(dl, s.ridge, o.x, o.y, ridgeCol, 1.4f * gUi);
    w3d::drawMesh(dl, s.mesh, o.x, o.y);
    if (s.peak && !ridgeFirst) w3d::drawRidge(dl, s.ridge, o.x, o.y, ridgeCol, 1.4f * gUi);
    if (s.marker) channelMarker(a, dl, s, p, lk, o);
    axisLabels(a, dl, s, p, lk, o);
    if (s.hist.have() == 0) {
        const char* t = tr("No spectrum yet: start a receiver");
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(o.x + (sz.x - ts.x) / 2, o.y + (sz.y - ts.y) / 2), ImGui::GetColorU32(plt::GetStyle().Text), t);
    }
    if (s.frozen) dl->AddText(ImVec2(o.x + 8 * gUi, o.y + 6 * gUi), IM_COL32(255, 200, 80, 230), tr("Frozen (Space)"));

    // ---- the mouse on the surface: what is under it, and a click tunes there
    if (hovered && !ImGui::IsMouseDown(0) && !ImGui::IsMouseDown(1) && !touchPinch) {
        const w3d::State::Band& gMap = s.band;
        const w3d::Hit h = w3d::pick(s.grid, p, lk, io.MousePos.x - o.x, io.MousePos.y - o.y);
        if (h.ok) {
            const double mhz = w3d::uToMhz(h.u, s.grid.cols, gMap.c0, gMap.c1, gMap.W, gMap.imgLo, gMap.fs);
            const double ago = h.v * (s.grid.rows - 1) * std::max(1, s.key.group) * (a.wf.rowDt > 0 ? a.wf.rowDt : 1.0 / 30);
            const double tune = a.family == 55 || a.fmScan.running || a.identifyTool ? NAN : clickTuneMhz(a, mhz);
            float sx, sy, d;
            if (w3d::project(p, h.p, sx, sy, d)) dl->AddCircle(ImVec2(o.x + sx, o.y + sy), 4 * gUi, IM_COL32(255, 80, 80, 220), 12, 1.5f * gUi);
            if (!std::isnan(tune)) ImGui::SetTooltip(tr("%.4f MHz, %.1f s ago: click to tune"), mhz, ago);
            else ImGui::SetTooltip(tr("%.4f MHz, %.1f s ago"), mhz, ago);
            const ImVec2 dd = ImGui::GetMouseDragDelta(0, 0);
            if (ImGui::IsMouseReleased(0) && std::fabs(dd.x) < 4 && std::fabs(dd.y) < 4 && !std::isnan(tune) && std::fabs(tune - a.freqMhz) > 1e-6)
                spectrumTune(a, tune);
        }
    }
    dl->PopClipRect();
    const double tAll = glfwGetTime() - t0;
    s.lastDrawMs = 1e3 * tAll;
    static const bool perf = getenv("DECT2_FPS") != nullptr;   // as the 2D plots: the CPU time on stderr every 2 seconds
    if (perf) {
        static double acc = 0, mx = 0, tp = glfwGetTime(); static int n = 0;
        acc += tAll; mx = std::max(mx, tAll); n++;
        if (glfwGetTime() - tp > 2.0) {
            fprintf(stderr, "wf3d: %.3f ms avg / %.2f ms max per frame, last build %.2f ms, %d x %d grid, %d quads%s\n", 1e3 * acc / n, 1e3 * mx, s.lastBuildMs, s.grid.cols, s.grid.rows, s.mesh.quads, lowPower ? " (low power)" : "");
            acc = mx = 0; n = 0; tp = glfwGetTime();
        }
    }
    {   // "2D" back to the flat spectrum and waterfall, and "Reset view" for the camera, left of the popup button
        const float bw = specCornerPopup(a, a.wf.paused ? "Paused###wfbtn" : "3D###wfbtn", "##wfpop", o, sz, 0, 40 * gUi, ImGui::GetTextLineHeight() + 6 * gUi, a.wf.paused || s.frozen,
                    tr("Waterfall: 3D view, colour map, intensity range, speed and pause"), wfDisplayPopup);
        const float m = 4 * gUi, fp = ImGui::GetStyle().FramePadding.x * 2;
        const std::string labs[2] = {std::string(tr("2D")) + "###wf3d2d", std::string(tr("Reset view")) + "###wf3dreset"};
        float used = bw;
        for (int i = 0; i < 2; i++) {
            const float w = ImGui::CalcTextSize(labs[i].c_str(), nullptr, true).x + fp, x = o.x + sz.x - used - w - m;
            if (bw <= 0 || x < o.x + 40 * gUi) break;
            const ImVec2 keep = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(ImVec2(x, o.y + m + ImGui::GetTextLineHeight() + 6 * gUi));
            if (ImGui::SmallButton(labs[i].c_str())) { if (i == 0) { s.on = false; savePrefs(a); } else { s.cam = w3d::preset(s.preset); savePrefs(a); } }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i == 0 ? tr("Back to the flat spectrum and waterfall (the waterfall history is kept)") : tr("Camera back to the chosen angle and zoom (double click does the same)"));
            ImGui::SetCursorScreenPos(keep);
            used += w + m;
        }
    }
}

void waterfall3dPopup(App& a) {
    w3d::State& s = a.wf.s3d;
    if (ImGui::Checkbox(tr("3D view"), &s.on)) savePrefs(a);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The history as a landscape: drag to orbit, Shift or right drag to pan, wheel to zoom, Ctrl+wheel for the height, click to tune"));
    if (!s.on) return;
    ImGui::SameLine(0, 8 * gUi);
    if (ImGui::Button(tr("Reset view##wf3dpop"))) { s.cam = w3d::preset(s.preset); savePrefs(a); }
    ImGui::SameLine(0, 8 * gUi);
    ImGui::Checkbox(tr("Freeze"), &s.frozen);
    for (int i = 0; i < w3d::NumPresets; i++) {
        if (i) ImGui::SameLine(0, 4 * gUi);
        if (ImGui::RadioButton(tr(w3d::presetName(i)), s.preset == i)) { s.preset = i; s.cam = w3d::preset(i); savePrefs(a); }
    }
    ImGui::SetNextItemWidth(170 * gUi);
    if (ImGui::SliderInt(tr("Rows"), &s.rows, 64, 1024)) s.rows = std::max(64, std::min(1024, s.rows));
    if (ImGui::IsItemDeactivatedAfterEdit()) savePrefs(a);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("How many rows of the history stand in the landscape (beyond 512 they build up while the view is open)"));
    ImGui::SetNextItemWidth(170 * gUi);
    ImGui::SliderFloat(tr("Height"), &s.heightScale, 0.05f, 1.5f, "%.2f", ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemDeactivatedAfterEdit()) savePrefs(a);
    bool ch = false;
    ch |= ImGui::Checkbox(tr("Smooth"), &s.smooth); ImGui::SameLine(0, 8 * gUi);
    ch |= ImGui::Checkbox(tr("Peak ridge"), &s.peak); ImGui::SameLine(0, 8 * gUi);
    ch |= ImGui::Checkbox(tr("Channel"), &s.marker); ImGui::SameLine(0, 8 * gUi);
    ch |= ImGui::Checkbox(tr("Shading"), &s.shade);
    if (ch) savePrefs(a);
    if (s.lastBuildMs > 0) ImGui::TextDisabled(tr("%d x %d grid, %d quads; last build %.1f ms%s"), s.grid.cols, s.grid.rows, s.mesh.quads, s.lastBuildMs, lowPowerNow() ? tr(" (low power)") : "");
}
