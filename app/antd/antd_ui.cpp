// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tools > Antenna designer: a wire antenna modeller in the spirit of the NEC-based programs. Pick a template (or import a NEC deck, or draw
// wires in the 3D view), see the impedance and SWR across a band, the radiation pattern in 3D and as azimuth / elevation cuts, the gain,
// front to back, beamwidth, take-off angle and efficiency, the current on every wire; trim or optimise the dimensions; calculators for the
// feed side; export the pattern for Tools > Propagation (docs/ANTENNA.md). The solver is core/include/dect2/antd/antd.h (method of moments).
#include "antd_ui.h"
#include "../gfx/colormap.h"
#include "dect2/common/u8file.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace {
namespace ad = dect2::antd;
using ad::cd;
using ad::Vec3;
using antdui::Units;

struct Results {
    bool valid = false;
    ad::Model m;
    std::vector<ad::SweepPoint> sweep;
    double fHz = 0, z0 = 50;
    ad::Solution sol;
    ad::Pattern pat;
    ad::Figures fig;
    std::vector<double> az, el;
    std::vector<std::string> warn;
    double ms = 0;
    bool haveConv = false;
    ad::Convergence conv;
};

struct Opt {
    std::vector<char> vary;
    std::vector<double> lo, hi;
    ad::OptGoal goal;
    bool goalInit = false;
    int maxEvals = 150;
    std::thread th;
    std::atomic<bool> busy{false}, stop{false};
    std::mutex mu;
    std::string log;
    bool haveResult = false, isTrim = false;
    ad::OptResult res;
    std::vector<ad::TParam> trimmed;
    ~Opt() { stop = true; if (th.joinable()) th.join(); }
};

struct State {
    bool loaded = false;
    std::string tpl = "dipole";
    std::vector<ad::TParam> params;
    double designMHz = 14.2;
    ad::Model model;
    ad::Ground ground;
    bool custom = false;
    std::string note, msg;
    Units u;
    double z0 = 50;
    double f0MHz = 14.0, f1MHz = 14.35, fSelMHz = 14.2;
    int nPts = 31;
    bool sweepGain = false;
    // view
    float yaw = 0.75f, pitch = 0.42f, zoom = 1.f;
    ImVec2 pan{0, 0};
    bool showPat = true, showCur = true, showSeg = false, showGrid = true;
    int selWire = -1, dragEnd = -1;
    double gridM = 0.1;
    bool snap = true;
    // the worker
    std::mutex mu;
    std::thread th;
    std::atomic<bool> busy{false}, cancel{false};
    Results res;
    bool dirty = true, sweepDirty = true, wantConv = false;
    double lastEdit = -1;
    Opt opt;
    int resTab = 0;
    // add-wire fields
    double nw[7] = {0, 0, 1, 0, 0, 2, 0.001};
    double moveBy[3] = {0, 0, 0}, arrStep[3] = {1, 0, 0};
    int arrN = 1;
    char name[96] = "";
    ~State() { cancel = true; if (th.joinable()) th.join(); }
};
State S;

double now() { return ImGui::GetTime(); }
void touch(bool sweepToo = true) { S.dirty = true; if (sweepToo) S.sweepDirty = true; S.lastEdit = now(); if (S.busy) S.cancel = true; }
void modelEdited() { S.custom = true; touch(); }

std::string antennaDir() {
    const std::string d = plat::dataDir() + "/antennas";
    std::error_code ec;
    std::filesystem::create_directories(dect2::u8file(d), ec);
    return d;
}

bool writeText(const std::string& path, const std::string& s) {
    FILE* f = dect2::u8fopen(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(s.data(), 1, s.size(), f) == s.size();
    std::fclose(f);
    return ok;
}
bool readText(const std::string& path, std::string& s) {
    FILE* f = dect2::u8fopen(path, "rb");
    if (!f) return false;
    s.clear();
    char b[65536]; size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    std::fclose(f);
    return true;
}

// the sweep range around a design frequency: the band preset holding it, else +-5 %
void rangeFor(double fMHz) {
    for (const auto& b : ad::bandPresets())
        if (fMHz * 1e6 >= b.f0 && fMHz * 1e6 <= b.f1 && (b.f1 - b.f0) < 0.5 * b.f0) {
            const double w = b.f1 - b.f0;
            S.f0MHz = (b.f0 - 0.1 * w) / 1e6; S.f1MHz = (b.f1 + 0.1 * w) / 1e6;
            return;
        }
    S.f0MHz = fMHz * 0.95; S.f1MHz = fMHz * 1.05;
}

void buildFromTemplate() {
    S.model = ad::buildTemplate(S.tpl, S.designMHz * 1e6, S.params, &S.note, &S.ground);
    if (S.name[0]) S.model.name = S.name;
    S.custom = false;
    touch();
}

void chooseTemplate(const std::string& id, bool keepRange = false) {
    S.tpl = id;
    S.params = ad::templateParams(id, S.designMHz * 1e6);
    const ad::Model m = ad::buildTemplate(id, S.designMHz * 1e6, S.params, &S.note);
    S.ground = m.ground;
    if (const ad::TemplateInfo* t = ad::findTemplate(id)) { S.z0 = t->z0; std::snprintf(S.name, sizeof S.name, "%s", t->name.c_str()); }
    if (!keepRange) { rangeFor(S.designMHz); S.fSelMHz = S.designMHz; }
    S.opt.vary.clear();
    S.selWire = -1;
    buildFromTemplate();
}

void save() {
    auto& pf = plat::prefs();
    pf.setS("antd_tpl", S.tpl); pf.setD("antd_f", S.designMHz); pf.setI("antd_units", S.u.sys); pf.setD("antd_z0", S.z0);
    pf.setD("antd_f0", S.f0MHz); pf.setD("antd_f1", S.f1MHz); pf.setD("antd_fs", S.fSelMHz); pf.setI("antd_n", S.nPts);
    pf.setI("antd_gnd", (int)S.ground.type); pf.setD("antd_er", S.ground.epsr); pf.setD("antd_sig", S.ground.sigma);
    pf.setB("antd_custom", S.custom);
    std::string ps;
    for (const auto& p : S.params) { char b[96]; std::snprintf(b, sizeof b, "%s=%.9g;", p.key.c_str(), p.value); ps += b; }
    pf.setS("antd_params", ps);
    if (S.custom) writeText(antennaDir() + "/last.nec", ad::writeNec(S.model, S.f0MHz * 1e6, S.f1MHz * 1e6, S.nPts));
}

void load() {
    if (S.loaded) return;
    S.loaded = true;
    auto& pf = plat::prefs();
    S.designMHz = pf.getD("antd_f", 14.2);
    S.u.sys = (int)pf.getI("antd_units", 0);
    const std::string id = pf.getS("antd_tpl", "dipole");
    chooseTemplate(ad::findTemplate(id) ? id : "dipole");
    S.z0 = pf.getD("antd_z0", S.z0);
    if (pf.has("antd_f0")) { S.f0MHz = pf.getD("antd_f0", S.f0MHz); S.f1MHz = pf.getD("antd_f1", S.f1MHz); S.fSelMHz = pf.getD("antd_fs", S.fSelMHz); }
    S.nPts = (int)pf.getI("antd_n", S.nPts);
    if (pf.has("antd_gnd")) {
        S.ground.type = (ad::GroundType)std::min(2L, std::max(0L, pf.getI("antd_gnd", 0)));
        S.ground.epsr = pf.getD("antd_er", 13); S.ground.sigma = pf.getD("antd_sig", 0.005);
    }
    // the parameters the user had
    std::stringstream ss(pf.getS("antd_params", ""));
    std::string kv;
    while (std::getline(ss, kv, ';')) {
        const size_t e = kv.find('=');
        if (e == std::string::npos) continue;
        for (auto& p : S.params) if (p.key == kv.substr(0, e)) p.value = std::atof(kv.c_str() + e + 1);
    }
    buildFromTemplate();
    if (pf.getB("antd_custom", false)) {
        std::string t, err;
        ad::Model m;
        if (readText(antennaDir() + "/last.nec", t) && ad::readNec(t, m, nullptr, nullptr, nullptr, &err)) {
            S.model = m; S.custom = true; S.ground = m.ground;
            std::snprintf(S.name, sizeof S.name, "%s", m.name.c_str());
        }
    }
    touch();
}

// ---- the worker ------------------------------------------------------------------------------------------------------------------------------
void schedule() {
    if (S.busy) return;
    if (S.th.joinable()) S.th.join();
    if (!S.dirty || now() - S.lastEdit < 0.2) return;
    S.dirty = false;
    const bool doSweep = S.sweepDirty;
    S.sweepDirty = false;
    ad::Model m = S.model;
    m.ground = S.ground;
    const double f0 = std::min(S.f0MHz, S.f1MHz) * 1e6, f1 = std::max(S.f0MHz, S.f1MHz) * 1e6, fs = S.fSelMHz * 1e6, z0 = S.z0;
    const int n = std::max(2, std::min(401, S.nPts));
    const bool gain = S.sweepGain, conv = S.wantConv;
    S.wantConv = false;
    std::vector<ad::SweepPoint> oldSweep;
    if (!doSweep) { std::lock_guard<std::mutex> lk(S.mu); oldSweep = S.res.sweep; }
    S.cancel = false;
    S.busy = true;
    S.th = std::thread([=, old = std::move(oldSweep)]() mutable {
        const auto t0 = std::chrono::steady_clock::now();
        Results r;
        r.m = m; r.fHz = fs; r.z0 = z0;
        r.warn = ad::checkModel(m, std::max(f1, fs));
        r.sweep = doSweep ? ad::sweep(m, f0, f1, n, z0, gain, 0, [] { return S.cancel.load(); }) : std::move(old);
        if (!S.cancel) {
            // one mesh for the band and the selected frequency (the sweep's), so the currents line up
            const ad::Mesh mesh = ad::buildMesh(m, std::max(f1, fs));
            r.sol = ad::solveMesh(m, mesh, fs);
            if (r.sol.ok) {
                r.pat = ad::pattern(m, r.sol, 5, 5);
                r.fig = ad::figures(m, r.sol, r.pat);
                r.az = ad::azimuthCut(m, r.sol, r.fig.elMaxDeg, 1);
                r.el = ad::elevationCut(m, r.sol, r.fig.azMaxDeg, 1);
            }
            if (conv) { r.conv = ad::convergence(m, fs); r.haveConv = true; }
            r.valid = true;
        }
        r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (r.valid) { std::lock_guard<std::mutex> lk(S.mu); S.res = std::move(r); }
        else { S.dirty = true; S.sweepDirty = S.sweepDirty || doSweep; }
        S.busy = false;
    });
}

// ---- the 3D view -----------------------------------------------------------------------------------------------------------------------------
struct Cam {
    Vec3 centre, R, U, D;   // right, up, toward the viewer
    float scale = 1;
    ImVec2 c;
    ImVec2 to(const Vec3& p) const { const Vec3 q = p - centre; return ImVec2(c.x + scale * (float)q.dot(R), c.y - scale * (float)q.dot(U)); }
};

Cam camera(const ImVec2& p0, const ImVec2& sz, const ad::Model& m) {
    Cam c;
    Vec3 lo(1e30, 1e30, 1e30), hi(-1e30, -1e30, -1e30);
    for (const auto& w : m.wires)
        for (const Vec3& p : {w.a, w.b}) {
            lo = Vec3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
            hi = Vec3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
        }
    if (m.wires.empty()) { lo = Vec3(-1, -1, 0); hi = Vec3(1, 1, 1); }
    c.centre = (lo + hi) * 0.5;
    const double ext = std::max((hi - lo).norm(), 1e-3);
    const double y = S.yaw, p = S.pitch;
    c.R = Vec3(-std::sin(y), std::cos(y), 0);
    c.U = Vec3(-std::sin(p) * std::cos(y), -std::sin(p) * std::sin(y), std::cos(p));
    c.D = Vec3(std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p));
    c.scale = (float)(std::min(sz.x, sz.y) * 0.62 / ext) * S.zoom;
    c.c = ImVec2(p0.x + sz.x / 2 + S.pan.x, p0.y + sz.y / 2 + S.pan.y);
    return c;
}

double snapv(double v) { return S.snap && S.gridM > 0 ? std::round(v / S.gridM) * S.gridM : v; }

float distSeg(ImVec2 p, ImVec2 a, ImVec2 b) {
    const float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2 : 0;
    t = std::min(1.f, std::max(0.f, t));
    const float ex = a.x + t * dx - p.x, ey = a.y + t * dy - p.y;
    return std::sqrt(ex * ex + ey * ey);
}

void view3d(float height) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 sz(std::max(200.f, ImGui::GetContentRegionAvail().x), height);
    ImGui::InvisibleButton("##ant3d", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), true);
    dl->AddRectFilled(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), ImGui::GetColorU32(ImVec4(0.025f, 0.027f, 0.03f, 1)), 4 * gUi);
    ad::Model& m = S.model;
    Cam cam = camera(p0, sz, m);
    ImGuiIO& io = ImGui::GetIO();
    // the ground plane / grid
    const bool gnd = S.ground.type != ad::GroundType::Free;
    {
        const double ext = ad::modelExtent(m);
        double step = S.gridM > 0 ? S.gridM : 0.1;
        while (ext / step > 24) step *= 2;
        while (ext / step < 6) step /= 2;
        const int n = 12;
        const double z = gnd ? 0.0 : std::min(cam.centre.z, [&] { double lo = 1e30; for (auto& w : m.wires) lo = std::min({lo, w.a.z, w.b.z}); return lo; }());
        const double gx = std::round(cam.centre.x / step) * step, gy = std::round(cam.centre.y / step) * step;
        const ImU32 gc = ImGui::GetColorU32(gnd ? ImVec4(0.45f, 0.36f, 0.22f, 0.35f) : ImVec4(0.5f, 0.55f, 0.6f, 0.13f));
        if (S.showGrid || gnd)
            for (int i = -n; i <= n; ++i) {
                dl->AddLine(cam.to(Vec3(gx + i * step, gy - n * step, z)), cam.to(Vec3(gx + i * step, gy + n * step, z)), gc);
                dl->AddLine(cam.to(Vec3(gx - n * step, gy + i * step, z)), cam.to(Vec3(gx + n * step, gy + i * step, z)), gc);
            }
    }
    // the pattern surface, around the first feed point
    std::lock_guard<std::mutex> lk(S.mu);
    const Results& r = S.res;
    const bool fresh = r.valid && !S.dirty && !S.busy;
    if (S.showPat && r.valid && r.sol.ok && r.pat.nTh > 1 && r.pat.maxDbi > -998) {
        Vec3 o = cam.centre;
        if (!r.m.sources.empty() && r.m.sources[0].wire >= 0 && r.m.sources[0].wire < (int)r.m.wires.size() && r.m.sources[0].line < 0) {
            const ad::Wire& w = r.m.wires[r.m.sources[0].wire];
            o = w.a + (w.b - w.a) * r.m.sources[0].pos;
        }
        const double R0 = ad::modelExtent(r.m) * 0.75, range = 30;
        const ad::Pattern& p = r.pat;
        auto pt = [&](int i, int j, float& v) {
            const double g = p.at(i, j);
            v = (float)std::max(0.0, (g - p.maxDbi + range) / range);
            const double th = p.thAt(i) * ad::kPi / 180, ph = p.phAt(j) * ad::kPi / 180;
            return cam.to(o + Vec3(std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th)) * (R0 * v));
        };
        for (int i = 0; i < p.nTh; ++i)
            for (int j = 0; j + 1 < p.nPh; ++j) {
                float v0, v1, v2;
                const ImVec2 a = pt(i, j, v0), b = pt(i, j + 1, v1);
                const ImU32 c = (cmap::color(cmap::Turbo, 0.25f + 0.75f * 0.5f * (v0 + v1)) & 0x00FFFFFF) | 0x90000000;
                if (i % 2 == 0) dl->AddLine(a, b, c);
                if (j % 2 == 0 && i + 1 < p.nTh) {
                    const ImVec2 d = pt(i + 1, j, v2);
                    dl->AddLine(a, d, (cmap::color(cmap::Turbo, 0.25f + 0.75f * 0.5f * (v0 + v2)) & 0x00FFFFFF) | 0x90000000);
                }
            }
    }
    // wires (with the current when the solution is current)
    double imax = 0;
    if (fresh && S.showCur && r.sol.ok) for (const auto& c : r.sol.segI) imax = std::max({imax, std::abs(c.ia), std::abs(c.ib)});
    const ImU32 wcol = ImGui::GetColorU32(ImVec4(0.72f, 0.76f, 0.8f, 1)), sel = ImGui::GetColorU32(pal::accent());
    if (imax > 0) {
        for (size_t i = 0; i < r.sol.mesh.segs.size(); ++i) {
            const ad::Segment& sg = r.sol.mesh.segs[i];
            const float v = (float)(0.5 * (std::abs(r.sol.segI[i].ia) + std::abs(r.sol.segI[i].ib)) / imax);
            dl->AddLine(cam.to(sg.a), cam.to(sg.b), cmap::color(cmap::Turbo, 0.15f + 0.85f * v), (sg.wire == S.selWire ? 3.5f : 2.5f) * gUi);
        }
        if (S.selWire >= 0 && S.selWire < (int)m.wires.size())
            dl->AddLine(cam.to(m.wires[S.selWire].a), cam.to(m.wires[S.selWire].b), (sel & 0x00FFFFFF) | 0x60000000, 7 * gUi);
    } else {
        for (int i = 0; i < (int)m.wires.size(); ++i)
            dl->AddLine(cam.to(m.wires[i].a), cam.to(m.wires[i].b), i == S.selWire ? sel : wcol, (i == S.selWire ? 3.f : 2.f) * gUi);
    }
    if (S.showSeg && r.valid)
        for (const auto& sg : r.sol.mesh.segs) dl->AddCircleFilled(cam.to(sg.a), 1.8f * gUi, ImGui::GetColorU32(ImVec4(0.9f, 0.9f, 0.9f, 0.6f)));
    // feeds, loads, lines
    auto wirePt = [&](int w, double pos) { return m.wires[w].a + (m.wires[w].b - m.wires[w].a) * pos; };
    for (const auto& s : m.sources)
        if (s.line < 0 && s.wire >= 0 && s.wire < (int)m.wires.size()) dl->AddCircleFilled(cam.to(wirePt(s.wire, s.pos)), 5 * gUi, IM_COL32(230, 70, 60, 255));
    for (const auto& l : m.loads)
        if (l.wire >= 0 && l.wire < (int)m.wires.size()) {
            const ImVec2 q = cam.to(wirePt(l.wire, l.pos));
            dl->AddRectFilled(ImVec2(q.x - 4 * gUi, q.y - 4 * gUi), ImVec2(q.x + 4 * gUi, q.y + 4 * gUi), IM_COL32(235, 170, 60, 255));
        }
    for (const auto& t : m.lines) {
        if (t.wire1 < 0 || t.wire1 >= (int)m.wires.size()) continue;
        const ImVec2 a = cam.to(wirePt(t.wire1, t.pos1));
        bool driven = false;
        for (const auto& s : m.sources) if (s.line >= 0 && s.line < (int)m.lines.size() && &m.lines[s.line] == &t) driven = true;
        ImVec2 b;
        if (driven) { b = ImVec2(a.x, a.y + 40 * gUi); dl->AddCircleFilled(b, 5 * gUi, IM_COL32(230, 70, 60, 255)); }
        else if (t.wire2 >= 0 && t.wire2 < (int)m.wires.size()) b = cam.to(wirePt(t.wire2, t.pos2));
        else continue;
        dl->AddLine(a, b, IM_COL32(80, 200, 220, 200), 1.5f * gUi);
    }
    // the selected wire's ends: drag to move (Shift: up / down), snapped to the grid
    if (S.selWire >= 0 && S.selWire < (int)m.wires.size()) {
        ad::Wire& w = m.wires[S.selWire];
        for (int e = 0; e < 2; ++e) dl->AddCircle(cam.to(e ? w.b : w.a), 6 * gUi, sel, 0, 2 * gUi);
    }
    // the axes in a corner
    {
        Cam ax = cam; ax.centre = Vec3(0, 0, 0); ax.c = ImVec2(p0.x + 34 * gUi, p0.y + sz.y - 34 * gUi); ax.scale = 22 * gUi;
        const ImU32 cx = IM_COL32(220, 90, 80, 255), cy = IM_COL32(110, 200, 110, 255), cz = IM_COL32(100, 150, 240, 255);
        dl->AddLine(ax.c, ax.to(Vec3(1, 0, 0)), cx, 2); dl->AddText(ax.to(Vec3(1.25, 0, 0)), cx, "x");
        dl->AddLine(ax.c, ax.to(Vec3(0, 1, 0)), cy, 2); dl->AddText(ax.to(Vec3(0, 1.25, 0)), cy, "y");
        dl->AddLine(ax.c, ax.to(Vec3(0, 0, 1)), cz, 2); dl->AddText(ax.to(Vec3(0, 0, 1.25)), cz, "z");
    }
    if (imax > 0) {
        char b[64]; std::snprintf(b, sizeof b, "current: max %.3g mA (1 V feed)", imax * 1000);
        dl->AddText(ImVec2(p0.x + 8 * gUi, p0.y + 6 * gUi), ImGui::GetColorU32(ImVec4(0.7f, 0.72f, 0.75f, 1)), b);
    }
    dl->PopClipRect();
    // mouse: wheel zooms, left drag orbits (or moves a selected end), right / middle drag pans, a click selects a wire
    if (hov && io.MouseWheel != 0) S.zoom = std::min(40.f, std::max(0.05f, S.zoom * std::pow(1.15f, io.MouseWheel)));
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        S.dragEnd = -1;
        if (S.selWire >= 0 && S.selWire < (int)m.wires.size()) {
            for (int e = 0; e < 2; ++e) {
                const ImVec2 q = cam.to(e ? m.wires[S.selWire].b : m.wires[S.selWire].a);
                if (std::hypot(q.x - io.MousePos.x, q.y - io.MousePos.y) < 8 * gUi) S.dragEnd = e;
            }
        }
        if (S.dragEnd < 0) {
            int best = -1; float bd = 7 * gUi;
            for (int i = 0; i < (int)m.wires.size(); ++i) {
                const float d = distSeg(io.MousePos, cam.to(m.wires[i].a), cam.to(m.wires[i].b));
                if (d < bd) { bd = d; best = i; }
            }
            if (best >= 0) S.selWire = best;
        }
    }
    if (act && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.f)) {
        if (S.dragEnd >= 0 && S.selWire >= 0 && S.selWire < (int)m.wires.size()) {
            ad::Wire& w = m.wires[S.selWire];
            Vec3& p = S.dragEnd ? w.b : w.a;
            const Vec3 old = p;
            const Vec3 base = cam.centre + cam.R * ((io.MousePos.x - cam.c.x) / cam.scale) + cam.U * ((cam.c.y - io.MousePos.y) / cam.scale);
            if (io.KeyShift) {   // up / down only
                const Vec3 q = old - cam.centre;
                const double z = (((cam.c.y - io.MousePos.y) / cam.scale) - q.x * cam.U.x - q.y * cam.U.y) / std::max(0.05, cam.U.z);
                p.z = snapv(cam.centre.z + z);
            } else if (std::fabs(cam.D.z) > 0.08) {   // on the horizontal plane through the end
                const double t = (old.z - base.z) / cam.D.z;
                p.x = snapv(base.x + cam.D.x * t); p.y = snapv(base.y + cam.D.y * t);
            }
            // ends joined to this one move with it
            for (int i = 0; i < (int)m.wires.size(); ++i) {
                if (i == S.selWire) continue;
                if ((m.wires[i].a - old).norm() < 1e-6) m.wires[i].a = p;
                if ((m.wires[i].b - old).norm() < 1e-6) m.wires[i].b = p;
            }
            if ((p - old).norm() > 0) modelEdited();
        } else {
            S.yaw -= io.MouseDelta.x * 0.01f;
            S.pitch = std::min(1.55f, std::max(-1.55f, S.pitch + io.MouseDelta.y * 0.01f));
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) S.dragEnd = -1;
    if (hov && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) { S.pan.x += io.MouseDelta.x; S.pan.y += io.MouseDelta.y; }
    if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { S.zoom = 1; S.pan = ImVec2(0, 0); }
    if (hov && S.dragEnd < 0) ImGui::SetItemTooltip("Drag: orbit; wheel: zoom; right drag: pan; double click: reset.\nClick a wire to select it, then drag its end circles (Shift: up / down).");
}

// ---- inputs ----------------------------------------------------------------------------------------------------------------------------------
bool lenInput(const char* label, double* m, double lo = -1e9, double hi = 1e9) {
    double v = S.u.len(*m);
    ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::InputDouble(label, &v, 0, 0, "%.4f")) { *m = std::min(hi, std::max(lo, S.u.lenIn(v))); return true; }
    return false;
}

bool paramSlider(ad::TParam& p) {
    double v = p.value, lo = p.lo, hi = p.hi;
    const char* unit = "";
    if (p.kind == ad::PKind::Length) { v = S.u.len(v); lo = S.u.len(lo); hi = S.u.len(hi); unit = S.u.lenUnit(); }
    char fmt[32];
    switch (p.kind) {
    case ad::PKind::Length: std::snprintf(fmt, sizeof fmt, "%%.4f %s", unit); break;
    case ad::PKind::Count: std::snprintf(fmt, sizeof fmt, "%%.0f"); break;
    case ad::PKind::Angle: std::snprintf(fmt, sizeof fmt, "%%.1f deg"); break;
    case ad::PKind::Ohm: std::snprintf(fmt, sizeof fmt, "%%.0f ohm"); break;
    case ad::PKind::Pf: std::snprintf(fmt, sizeof fmt, "%%.2f pF"); break;
    case ad::PKind::Freq: std::snprintf(fmt, sizeof fmt, "%%.3f MHz"); break;
    default: std::snprintf(fmt, sizeof fmt, "%%.3f"); break;
    }
    ImGui::SetNextItemWidth(-140 * gUi);
    bool ch = ImGui::SliderScalar(("##p" + p.key).c_str(), ImGuiDataType_Double, &v, &lo, &hi, fmt);
    ImGui::SameLine();
    ImGui::TextUnformatted(p.label.c_str());
    if (!p.tip.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.tip.c_str());
    if (ch) {
        if (p.kind == ad::PKind::Length) v = S.u.lenIn(v);
        if (p.kind == ad::PKind::Count) v = std::round(v);
        p.value = v;
    }
    return ch;
}

// ---- the left panel: design, wires, optimise, files -------------------------------------------------------------------------------------
void startTrim() {
    const ad::TemplateInfo* t = ad::findTemplate(S.tpl);
    if (!t || t->trim.empty() || S.opt.busy) return;
    if (S.opt.th.joinable()) S.opt.th.join();
    S.opt.busy = true; S.opt.isTrim = true; S.opt.haveResult = false;
    std::vector<ad::TParam> p = S.params;
    const std::string id = S.tpl, key = t->trim;
    const double f = S.fSelMHz * 1e6, z0 = S.z0;
    const ad::Ground g = S.ground;
    { std::lock_guard<std::mutex> lk(S.opt.mu); S.opt.log = "trimming " + key + " for resonance at " + std::to_string(S.fSelMHz).substr(0, 8) + " MHz..."; }
    S.opt.th = std::thread([=]() mutable {
        cd z;
        const bool ok = ad::trimParam(id, f, p, key, &g, z0, &z);
        std::lock_guard<std::mutex> lk(S.opt.mu);
        S.opt.trimmed = p;
        char b[160];
        std::snprintf(b, sizeof b, ok ? "trimmed: Z = %.1f %+.1fj ohm, SWR %.2f" : "could not trim", z.real(), z.imag(), ad::vswr(z, z0));
        S.opt.log = b;
        S.opt.haveResult = ok;
        S.opt.busy = false;
    });
}

void designTab() {
    const ad::TemplateInfo* cur = ad::findTemplate(S.tpl);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##tpl", cur ? cur->name.c_str() : "template")) {
        for (const auto& t : ad::templates()) {
            if (ImGui::Selectable(t.name.c_str(), t.id == S.tpl)) chooseTemplate(t.id);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t.blurb.c_str());
        }
        ImGui::EndCombo();
    }
    if (cur) ImGui::TextWrapped("%s", cur->blurb.c_str());
    if (S.custom) {
        ImGui::TextColored(pal::warnAmber(), "Hand-edited: the template sliders no longer apply.");
        if (ImGui::Button("Back to the template")) buildFromTemplate();
        return;
    }
    bool ch = false;
    for (auto& p : S.params) ch |= paramSlider(p);
    if (ch) buildFromTemplate();
    if (!S.note.empty()) ImGui::TextDisabled("%s", S.note.c_str());
    if (ImGui::Button("Defaults")) chooseTemplate(S.tpl, true);
    ImGui::SameLine();
    if (cur && !cur->trim.empty()) {
        ImGui::BeginDisabled(S.opt.busy);
        if (ImGui::Button("Trim to resonance")) startTrim();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Move the main dimension until the feed reactance is zero at the selected frequency");
    }
    if (S.opt.isTrim) {
        std::lock_guard<std::mutex> lk(S.opt.mu);
        ImGui::TextDisabled("%s", S.opt.log.c_str());
        if (S.opt.haveResult && !S.opt.busy) { S.params = S.opt.trimmed; S.opt.haveResult = false; buildFromTemplate(); }
    }
}

void wiresTab() {
    ad::Model& m = S.model;
    ImGui::Checkbox("Snap", &S.snap); ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    double g = S.u.len(S.gridM);
    if (ImGui::InputDouble(S.u.sys ? "grid (ft)" : "grid (m)", &g, 0, 0, "%.3f")) S.gridM = std::max(1e-4, S.u.lenIn(g));
    if (ImGui::BeginTable("##wires", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(-1, 150 * gUi))) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 26 * gUi);
        ImGui::TableSetupColumn(S.u.sys ? "length ft" : "length m");
        ImGui::TableSetupColumn(S.u.sys ? "radius in" : "radius mm");
        ImGui::TableSetupColumn("segs");
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)m.wires.size(); ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char b[16]; std::snprintf(b, sizeof b, "%d", i + 1);
            if (ImGui::Selectable(b, i == S.selWire, ImGuiSelectableFlags_SpanAllColumns)) S.selWire = i;
            ImGui::TableNextColumn(); ImGui::Text("%.4f", S.u.len(ad::wireLength(m.wires[i])));
            ImGui::TableNextColumn(); ImGui::Text("%.3f", S.u.rad(m.wires[i].radius));
            ImGui::TableNextColumn();
            if (m.wires[i].segments > 0) ImGui::Text("%d", m.wires[i].segments); else ImGui::TextDisabled("auto");
        }
        ImGui::EndTable();
    }
    bool ch = false;
    if (S.selWire >= 0 && S.selWire < (int)m.wires.size()) {
        ad::Wire& w = m.wires[S.selWire];
        ImGui::Text("Wire %d", S.selWire + 1);
        ImGui::PushID(S.selWire);
        double* v[6] = {&w.a.x, &w.a.y, &w.a.z, &w.b.x, &w.b.y, &w.b.z};
        const char* lb[6] = {"x1", "y1", "z1", "x2", "y2", "z2"};
        for (int k = 0; k < 6; ++k) {
            if (k % 3) ImGui::SameLine();
            double x = S.u.len(*v[k]);
            ImGui::SetNextItemWidth(78 * gUi);
            if (ImGui::DragScalar(lb[k], ImGuiDataType_Double, &x, (float)S.u.len(S.gridM) * 0.2f, nullptr, nullptr, "%.3f")) {
                const Vec3 olda = w.a, oldb = w.b;
                *v[k] = snapv(S.u.lenIn(x));
                // the ends joined to the moved one follow
                for (int i = 0; i < (int)m.wires.size(); ++i) {
                    if (i == S.selWire) continue;
                    const Vec3& old = k < 3 ? olda : oldb; const Vec3& nw = k < 3 ? w.a : w.b;
                    if ((m.wires[i].a - old).norm() < 1e-6) m.wires[i].a = nw;
                    if ((m.wires[i].b - old).norm() < 1e-6) m.wires[i].b = nw;
                }
                ch = true;
            }
        }
        double rr = S.u.rad(w.radius);
        ImGui::SetNextItemWidth(90 * gUi);
        if (ImGui::InputDouble(S.u.sys ? "radius (in)" : "radius (mm)", &rr, 0, 0, "%.3f")) { w.radius = std::max(1e-6, S.u.radIn(rr)); ch = true; }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70 * gUi);
        if (ImGui::InputInt("segments (0 auto)", &w.segments)) { w.segments = std::max(0, std::min(2000, w.segments)); ch = true; }
        const char* mats[] = {"perfect", "copper", "aluminium", "brass", "steel"};
        const double sig[] = {0, 5.8e7, 3.5e7, 1.5e7, 1.0e6};
        int mi = 0;
        for (int k = 1; k < 5; ++k) if (std::fabs(w.conductivity - sig[k]) < 1) mi = k;
        ImGui::SetNextItemWidth(120 * gUi);
        if (ImGui::Combo("conductor", &mi, mats, 5)) { w.conductivity = sig[mi]; ch = true; }
        ImGui::PopID();
        if (ImGui::Button("Feed here")) {
            m.sources.clear(); ad::Source s; s.wire = S.selWire; s.pos = 0.5; m.sources.push_back(s); ch = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Add load")) { ad::Load l; l.wire = S.selWire; m.loads.push_back(l); ch = true; }
        ImGui::SameLine();
        if (ImGui::Button("Delete wire")) {
            const int d = S.selWire;
            m.wires.erase(m.wires.begin() + d);
            auto fix = [&](int& wi) { if (wi == d) wi = -1; else if (wi > d) --wi; };
            for (auto& s : m.sources) fix(s.wire);
            for (auto& l : m.loads) fix(l.wire);
            for (auto& t : m.lines) { fix(t.wire1); fix(t.wire2); }
            m.sources.erase(std::remove_if(m.sources.begin(), m.sources.end(), [](const ad::Source& s) { return s.line < 0 && s.wire < 0; }), m.sources.end());
            m.loads.erase(std::remove_if(m.loads.begin(), m.loads.end(), [](const ad::Load& l) { return l.wire < 0; }), m.loads.end());
            S.selWire = -1; ch = true;
        }
    }
    if (ImGui::CollapsingHeader("Add a wire")) {
        const char* lb[6] = {"x1##n", "y1##n", "z1##n", "x2##n", "y2##n", "z2##n"};
        for (int k = 0; k < 6; ++k) { if (k % 3) ImGui::SameLine(); lenInput(lb[k], &S.nw[k]); }
        double rr = S.u.rad(S.nw[6]);
        ImGui::SetNextItemWidth(90 * gUi);
        if (ImGui::InputDouble(S.u.sys ? "radius (in)##n" : "radius (mm)##n", &rr, 0, 0, "%.3f")) S.nw[6] = std::max(1e-6, S.u.radIn(rr));
        ImGui::SameLine();
        if (ImGui::Button("Add")) {
            ad::Wire w; w.a = Vec3(S.nw[0], S.nw[1], S.nw[2]); w.b = Vec3(S.nw[3], S.nw[4], S.nw[5]); w.radius = S.nw[6];
            if (ad::wireLength(w) > 0) { m.wires.push_back(w); S.selWire = (int)m.wires.size() - 1; ch = true; }
            else S.msg = "a wire needs two different ends";
        }
        if (S.selWire >= 0 && S.selWire < (int)m.wires.size()) {
            ImGui::SameLine();
            if (ImGui::Button("From the selected end")) {
                const ad::Wire& s = m.wires[S.selWire];
                S.nw[0] = s.b.x; S.nw[1] = s.b.y; S.nw[2] = s.b.z;
                S.nw[3] = s.b.x + (s.b.x - s.a.x); S.nw[4] = s.b.y + (s.b.y - s.a.y); S.nw[5] = s.b.z + (s.b.z - s.a.z); S.nw[6] = s.radius;
            }
        }
    }
    if (ImGui::CollapsingHeader("Move, mirror, array")) {
        lenInput("dx##mv", &S.moveBy[0]); ImGui::SameLine(); lenInput("dy##mv", &S.moveBy[1]); ImGui::SameLine(); lenInput("dz##mv", &S.moveBy[2]);
        if (ImGui::Button("Move all")) { ad::translate(m, Vec3(S.moveBy[0], S.moveBy[1], S.moveBy[2])); ch = true; }
        ImGui::SameLine();
        if (ImGui::Button("Move selected") && S.selWire >= 0 && S.selWire < (int)m.wires.size()) {
            m.wires[S.selWire].a = m.wires[S.selWire].a + Vec3(S.moveBy[0], S.moveBy[1], S.moveBy[2]);
            m.wires[S.selWire].b = m.wires[S.selWire].b + Vec3(S.moveBy[0], S.moveBy[1], S.moveBy[2]);
            ch = true;
        }
        if (ImGui::Button("Mirror copy in x")) { ad::mirror(m, 0); ch = true; } ImGui::SameLine();
        if (ImGui::Button("in y")) { ad::mirror(m, 1); ch = true; } ImGui::SameLine();
        if (ImGui::Button("in z")) { ad::mirror(m, 2); ch = true; }
        if (ImGui::Button("Rotate 90 deg about z")) { ad::rotateZ(m, 90); ch = true; }
        lenInput("step x##ar", &S.arrStep[0]); ImGui::SameLine(); lenInput("y##ar", &S.arrStep[1]); ImGui::SameLine(); lenInput("z##ar", &S.arrStep[2]);
        ImGui::SetNextItemWidth(80 * gUi);
        ImGui::InputInt("copies", &S.arrN); S.arrN = std::max(1, std::min(64, S.arrN));
        ImGui::SameLine();
        if (ImGui::Button("Array the selected") && S.selWire >= 0) { ad::arrayCopy(m, {S.selWire}, S.arrN, Vec3(S.arrStep[0], S.arrStep[1], S.arrStep[2])); ch = true; }
        ImGui::SameLine();
        if (ImGui::Button("Array all")) {
            std::vector<int> all; for (int i = 0; i < (int)m.wires.size(); ++i) all.push_back(i);
            ad::arrayCopy(m, all, S.arrN, Vec3(S.arrStep[0], S.arrStep[1], S.arrStep[2])); ch = true;
        }
    }
    if (ImGui::CollapsingHeader("Feeds, loads, lines", ImGuiTreeNodeFlags_DefaultOpen)) {
        const int nwires = (int)m.wires.size();
        for (size_t i = 0; i < m.sources.size(); ++i) {
            ad::Source& s = m.sources[i];
            ImGui::PushID((int)i);
            if (s.line >= 0) ImGui::Text("Feed %d: drives line %d", (int)i + 1, s.line + 1);
            else {
                int w = s.wire + 1;
                ImGui::SetNextItemWidth(70 * gUi);
                if (ImGui::InputInt("wire##s", &w)) { s.wire = std::min(nwires, std::max(1, w)) - 1; ch = true; }
                ImGui::SameLine();
                float pos = (float)s.pos;
                ImGui::SetNextItemWidth(110 * gUi);
                if (ImGui::SliderFloat("along##s", &pos, 0, 1, "%.3f")) { s.pos = pos; ch = true; }
            }
            ImGui::SameLine();
            double mag = std::abs(s.volts), ph = std::arg(s.volts) * 180 / ad::kPi;
            ImGui::SetNextItemWidth(60 * gUi);
            bool vc = ImGui::InputDouble("V##s", &mag, 0, 0, "%.2f");
            ImGui::SameLine(); ImGui::SetNextItemWidth(60 * gUi);
            vc |= ImGui::InputDouble("deg##s", &ph, 0, 0, "%.0f");
            if (vc) { s.volts = std::polar(std::max(mag, 1e-6), ph * ad::kPi / 180); ch = true; }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) { m.sources.erase(m.sources.begin() + i); ch = true; ImGui::PopID(); break; }
            ImGui::PopID();
        }
        if (ImGui::SmallButton("+ feed") && nwires > 0) { ad::Source s; s.wire = std::max(0, S.selWire); m.sources.push_back(s); ch = true; }
        for (size_t i = 0; i < m.loads.size(); ++i) {
            ad::Load& l = m.loads[i];
            ImGui::PushID(1000 + (int)i);
            int w = l.wire + 1;
            ImGui::SetNextItemWidth(70 * gUi);
            if (ImGui::InputInt("wire##l", &w)) { l.wire = std::min(nwires, std::max(1, w)) - 1; ch = true; }
            ImGui::SameLine();
            float pos = (float)l.pos;
            ImGui::SetNextItemWidth(90 * gUi);
            if (ImGui::SliderFloat("along##l", &pos, 0, 1, "%.3f")) { l.pos = pos; ch = true; }
            ImGui::SameLine();
            int t = (int)l.type;
            const char* tn[] = {"R + jX", "series RLC", "parallel RLC"};
            ImGui::SetNextItemWidth(110 * gUi);
            if (ImGui::Combo("##lt", &t, tn, 3)) { l.type = (ad::LoadType)t; ch = true; }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) { m.loads.erase(m.loads.begin() + i); ch = true; ImGui::PopID(); break; }
            ImGui::SetNextItemWidth(70 * gUi);
            ch |= ImGui::InputDouble("R##l", &l.r, 0, 0, "%.2f");
            if (l.type == ad::LoadType::RX) { ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); ch |= ImGui::InputDouble("X##l", &l.x, 0, 0, "%.2f"); }
            else {
                double uh = l.l * 1e6, pf = l.c * 1e12;
                ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi);
                if (ImGui::InputDouble("uH##l", &uh, 0, 0, "%.4g")) { l.l = std::max(0.0, uh) * 1e-6; ch = true; }
                ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi);
                if (ImGui::InputDouble("pF##l", &pf, 0, 0, "%.4g")) { l.c = std::max(0.0, pf) * 1e-12; ch = true; }
            }
            ImGui::PopID();
        }
        for (size_t i = 0; i < m.lines.size(); ++i) {
            ad::TLine& t = m.lines[i];
            ImGui::PushID(2000 + (int)i);
            ImGui::Text("Line %d: wire %d at %.2f to %s", (int)i + 1, t.wire1 + 1, t.pos1, [&] {
                for (const auto& s : m.sources) if (s.line == (int)i) return std::string("the feed");
                char b[48]; std::snprintf(b, sizeof b, "wire %d at %.2f", t.wire2 + 1, t.pos2); return std::string(b);
            }().c_str());
            ImGui::SetNextItemWidth(70 * gUi); ch |= ImGui::InputDouble("Z0##t", &t.z0, 0, 0, "%.0f");
            ImGui::SameLine(); ch |= lenInput("length##t", &t.length, 0);
            ImGui::SameLine(); ImGui::SetNextItemWidth(60 * gUi); ch |= ImGui::InputDouble("VF##t", &t.vf, 0, 0, "%.2f");
            ImGui::SameLine(); ch |= ImGui::Checkbox("crossed##t", &t.crossed);
            t.vf = std::min(1.0, std::max(0.05, t.vf)); t.z0 = std::max(1.0, t.z0);
            ImGui::PopID();
        }
    }
    if (ch) modelEdited();
}

void optimiseTab() {
    Opt& o = S.opt;
    if (S.custom) { ImGui::TextWrapped("The optimiser varies a template's parameters. This model was edited by hand: go back to a template to optimise."); return; }
    if (o.vary.size() != S.params.size()) {
        o.vary.assign(S.params.size(), 0); o.lo.clear(); o.hi.clear();
        for (const auto& p : S.params) {
            const double span = p.kind == ad::PKind::Count ? 1.0 : 0.15 * std::fabs(p.value) + 1e-9;
            o.lo.push_back(std::max(p.lo, p.value - span)); o.hi.push_back(std::min(p.hi, p.value + span));
        }
        if (const ad::TemplateInfo* t = ad::findTemplate(S.tpl))
            for (size_t i = 0; i < S.params.size(); ++i) if (S.params[i].key == t->trim) o.vary[i] = 1;
    }
    if (!o.goalInit) { o.goalInit = true; o.goal.f0Hz = S.f0MHz * 1e6; o.goal.f1Hz = S.f1MHz * 1e6; }
    ImGui::TextDisabled("Vary (between min and max):");
    for (size_t i = 0; i < S.params.size(); ++i) {
        ad::TParam& p = S.params[i];
        ImGui::PushID((int)i);
        bool v = o.vary[i];
        if (ImGui::Checkbox(p.label.c_str(), &v)) o.vary[i] = v;
        if (v) {
            const bool len = p.kind == ad::PKind::Length;
            double lo = len ? S.u.len(o.lo[i]) : o.lo[i], hi = len ? S.u.len(o.hi[i]) : o.hi[i];
            ImGui::SameLine(); ImGui::SetNextItemWidth(80 * gUi);
            if (ImGui::InputDouble("min", &lo, 0, 0, "%.4g")) o.lo[i] = std::max(p.lo, len ? S.u.lenIn(lo) : lo);
            ImGui::SameLine(); ImGui::SetNextItemWidth(80 * gUi);
            if (ImGui::InputDouble("max", &hi, 0, 0, "%.4g")) o.hi[i] = std::min(p.hi, len ? S.u.lenIn(hi) : hi);
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::TextDisabled("Goals:");
    double a = o.goal.f0Hz / 1e6, b = o.goal.f1Hz / 1e6;
    ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::InputDouble("from MHz", &a, 0, 0, "%.4f")) o.goal.f0Hz = a * 1e6;
    ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::InputDouble("to MHz", &b, 0, 0, "%.4f")) o.goal.f1Hz = b * 1e6;
    ImGui::SameLine();
    if (ImGui::SmallButton("= sweep")) { o.goal.f0Hz = S.f0MHz * 1e6; o.goal.f1Hz = S.f1MHz * 1e6; }
    ImGui::SetNextItemWidth(60 * gUi); ImGui::InputInt("points", &o.goal.points); o.goal.points = std::max(1, std::min(9, o.goal.points));
    ImGui::SetNextItemWidth(80 * gUi); ImGui::InputDouble("SWR below", &o.goal.swrTarget, 0, 0, "%.2f");
    ImGui::SameLine(); ImGui::SetNextItemWidth(60 * gUi); ImGui::InputDouble("weight##sw", &o.goal.swrWeight, 0, 0, "%.1f");
    ImGui::SetNextItemWidth(80 * gUi); ImGui::InputDouble("gain dBi", &o.goal.gainTarget, 0, 0, "%.1f");
    ImGui::SameLine(); ImGui::SetNextItemWidth(60 * gUi); ImGui::InputDouble("weight##g", &o.goal.gainWeight, 0, 0, "%.1f");
    ImGui::SetNextItemWidth(80 * gUi); ImGui::InputDouble("F/B dB", &o.goal.fbTarget, 0, 0, "%.1f");
    ImGui::SameLine(); ImGui::SetNextItemWidth(60 * gUi); ImGui::InputDouble("weight##fb", &o.goal.fbWeight, 0, 0, "%.1f");
    ImGui::SetNextItemWidth(80 * gUi); ImGui::InputDouble("at elevation (deg)", &o.goal.elevationDeg, 0, 0, "%.0f");
    ImGui::SetNextItemWidth(80 * gUi); ImGui::InputInt("max solves", &o.maxEvals); o.maxEvals = std::max(10, std::min(5000, o.maxEvals));
    ImGui::TextDisabled("Gain and F/B are toward +x (phi 0), where the directional templates point.");
    if (o.busy && !o.isTrim) {
        if (ImGui::Button("Stop")) o.stop = true;
    } else {
        ImGui::BeginDisabled(o.busy);
        if (ImGui::Button("Optimise")) {
            std::vector<ad::OptVar> vars;
            for (size_t i = 0; i < S.params.size(); ++i) if (o.vary[i]) vars.push_back({S.params[i].key, o.lo[i], o.hi[i]});
            if (vars.empty()) S.msg = "tick at least one parameter to vary";
            else {
                if (o.th.joinable()) o.th.join();
                o.busy = true; o.stop = false; o.isTrim = false; o.haveResult = false;
                o.goal.z0 = S.z0;
                { std::lock_guard<std::mutex> lk(o.mu); o.log.clear(); }
                const std::vector<ad::TParam> start = S.params;
                const std::string id = S.tpl; const double fd = S.designMHz * 1e6; const ad::Ground g = S.ground; const ad::OptGoal goal = o.goal; const int me = o.maxEvals;
                o.th = std::thread([=] {
                    Opt& o = S.opt;
                    ad::OptResult r = ad::optimise(id, fd, start, vars, goal, g, me, [](const ad::OptStep& st) {
                        Opt& o = S.opt;
                        char b[160];
                        std::snprintf(b, sizeof b, "step %d: cost %.4f  SWR %.2f%s\n", st.iter, st.cost, st.swr,
                                      st.gain > -998 ? ("  gain " + std::to_string(st.gain).substr(0, 5) + " dBi  F/B " + std::to_string(st.fb).substr(0, 5)).c_str() : "");
                        std::lock_guard<std::mutex> lk(o.mu);
                        o.log += b;
                        if (o.log.size() > 6000) o.log.erase(0, o.log.size() - 6000);
                        return !o.stop.load();
                    });
                    std::lock_guard<std::mutex> lk(o.mu);
                    o.res = r; o.haveResult = r.ok;
                    char b[200];
                    std::snprintf(b, sizeof b, "%s after %d solves: worst SWR %.2f%s\n", r.ok ? "done" : r.msg.c_str(), r.evals, r.worstSwr,
                                  r.gainDbi > -998 ? ("  gain " + std::to_string(r.gainDbi).substr(0, 5) + " dBi").c_str() : "");
                    o.log += b;
                    o.busy = false;
                });
            }
        }
        ImGui::EndDisabled();
    }
    if (!o.isTrim) {
        std::lock_guard<std::mutex> lk(o.mu);
        if (o.haveResult && !o.busy) {
            ImGui::SameLine();
            if (ImGui::Button("Apply the result")) { S.params = o.res.params; buildFromTemplate(); o.haveResult = false; }
        }
        ImGui::BeginChild("##optlog", ImVec2(-1, 120 * gUi), ImGuiChildFlags_Borders);
        ImGui::TextUnformatted(o.log.c_str());
        if (o.busy) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
}

std::string safeName(std::string s) {
    for (char& c : s) if (!std::isalnum((unsigned char)c) && c != '-' && c != '_') c = '_';
    if (s.empty()) s = "antenna";
    return s;
}

void filesTab() {
    ImGui::SetNextItemWidth(-80 * gUi);
    if (ImGui::InputText("name", S.name, sizeof S.name)) S.model.name = S.name;
    if (ImGui::Button("Open NEC deck...")) {
        const std::string p = openFileDialog();
        std::string t, err;
        ad::Model m; double f0 = 0, f1 = 0; int nf = 0;
        if (!p.empty()) {
            if (!readText(p, t)) S.msg = "could not read " + p;
            else if (!ad::readNec(t, m, &f0, &f1, &nf, &err)) S.msg = "not read: " + err;
            else {
                S.model = m; S.ground = m.ground; S.custom = true; S.selWire = -1;
                std::snprintf(S.name, sizeof S.name, "%s", m.name.c_str());
                if (f0 > 0) { S.designMHz = S.fSelMHz = (f1 > f0 ? 0.5 * (f0 + f1) : f0) / 1e6; if (f1 > f0) { S.f0MHz = f0 / 1e6; S.f1MHz = f1 / 1e6; S.nPts = std::max(3, std::min(201, nf)); } else rangeFor(S.designMHz); }
                S.msg = "opened " + p + " (" + std::to_string(m.wires.size()) + " wires)";
                touch();
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save NEC deck...")) {
        const std::string p = saveFileDialog((safeName(S.name) + ".nec").c_str());
        if (!p.empty()) S.msg = writeText(p, ad::writeNec(S.model, S.f0MHz * 1e6, S.f1MHz * 1e6, S.nPts)) ? "saved " + p : "could not write " + p;
    }
    ImGui::TextDisabled("NEC-2 cards: GW GA GH GS GM GE GN EX LD TL FR (other cards are not read).");
    ImGui::Separator();
    std::lock_guard<std::mutex> lk(S.mu);
    const bool have = S.res.valid && S.res.sol.ok;
    ImGui::BeginDisabled(!have);
    if (ImGui::Button("Export the pattern for Propagation")) {
        ad::Model m = S.res.m; m.name = S.name;
        const std::string p = antennaDir() + "/" + safeName(S.name) + ".csv";
        S.msg = writeText(p, ad::writePatternCsv(m, S.res.sol, 5, 5)) ? "saved " + p + " (Tools > Propagation reads this folder)" : "could not write " + p;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The pattern at the selected frequency, 5-degree grid, compass azimuth (+y = north, +x = east); docs/ANTENNA.md");
    ImGui::SameLine();
    if (ImGui::Button("Pattern as...")) {
        const std::string p = saveFileDialog((safeName(S.name) + ".csv").c_str());
        ad::Model m = S.res.m; m.name = S.name;
        if (!p.empty()) S.msg = writeText(p, ad::writePatternCsv(m, S.res.sol, 5, 5)) ? "saved " + p : "could not write " + p;
    }
    if (ImGui::Button("Sweep table...")) {
        const std::string p = saveFileDialog((safeName(S.name) + "-sweep.csv").c_str());
        if (!p.empty()) S.msg = writeText(p, ad::writeSweepCsv(S.res.sweep, S.res.z0)) ? "saved " + p : "could not write " + p;
    }
    ImGui::SameLine();
    if (ImGui::Button("Currents table...")) {
        const std::string p = saveFileDialog((safeName(S.name) + "-currents.csv").c_str());
        if (!p.empty()) S.msg = writeText(p, ad::writeCurrentsCsv(S.res.m, S.res.sol)) ? "saved " + p : "could not write " + p;
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Pattern files go to %s", antennaDir().c_str());
}

// ---- results ---------------------------------------------------------------------------------------------------------------------------------
void sweepCharts(const Results& r) {
    std::vector<double> f, swr, R, X;
    for (const auto& p : r.sweep) if (p.ok) { f.push_back(p.fHz / 1e6); swr.push_back(std::min(p.swr, 10.0)); R.push_back(p.z.real()); X.push_back(p.z.imag()); }
    if (f.size() < 2) { ImGui::TextDisabled("no sweep yet"); return; }
    const float h = std::max(120.f, (ImGui::GetContentRegionAvail().y - 8 * gUi) / 2);
    auto bandMarks = [&] {
        for (const auto& b : ad::bandPresets()) {
            if (b.f1 / 1e6 < f.front() || b.f0 / 1e6 > f.back()) continue;
            plt::Spec s; s.FillColor = ImVec4(0.4f, 0.7f, 0.5f, 0.10f);
            plt::PlotVBand(b.name, b.f0 / 1e6, b.f1 / 1e6, s);
        }
        plt::Spec v; v.LineColor = pal::warnAmber(); v.LineWeight = 1;
        const double fs = S.fSelMHz;
        plt::PlotInfLines("##fsel", &fs, 1, v);
    };
    auto clickSets = [&] {
        if (plt::IsPlotHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            S.fSelMHz = std::min(f.back(), std::max(f.front(), plt::GetPlotMousePos().x));
            touch(false);
        }
    };
    if (plt::BeginPlot("##swr", ImVec2(-1, h), plt::Flags_NoLegend)) {
        char yl[32]; std::snprintf(yl, sizeof yl, "SWR (%g ohm)", r.z0);
        plt::SetupAxes("MHz", yl);
        plt::SetupAxisLimits(plt::X1, f.front(), f.back(), plt::Cond_Always);
        plt::SetupAxisLimits(plt::Y1, 1, 10, plt::Cond_Always);
        bandMarks();
        const double two = 2.0;
        plt::Spec g; g.LineColor = ImVec4(0.6f, 0.6f, 0.6f, 0.4f); g.Flags = plt::InfLines_Horizontal;
        plt::PlotInfLines("##2", &two, 1, g);
        plt::Spec s; s.LineColor = pal::accent(); s.LineWeight = 2;
        plt::PlotLine("SWR", f.data(), swr.data(), (int)f.size(), s);
        clickSets();
        plt::EndPlot();
    }
    if (plt::BeginPlot("##rx", ImVec2(-1, h))) {
        plt::SetupAxes("MHz", "ohm");
        plt::SetupAxisLimits(plt::X1, f.front(), f.back(), plt::Cond_Always);
        double lo = 0, hi = 0;
        for (size_t i = 0; i < f.size(); ++i) { lo = std::min({lo, R[i], X[i]}); hi = std::max({hi, R[i], X[i]}); }
        lo = std::max(lo, -3 * std::max(r.z0, 50.0) * 10); hi = std::min(hi, 3 * std::max(r.z0, 50.0) * 10);
        plt::SetupAxisLimits(plt::Y1, lo - 0.05 * (hi - lo + 1), hi + 0.05 * (hi - lo + 1), plt::Cond_Always);
        bandMarks();
        plt::Spec a; a.LineColor = ImVec4(0.30f, 0.62f, 0.95f, 1); a.LineWeight = 2;
        plt::PlotLine("R", f.data(), R.data(), (int)f.size(), a);
        plt::Spec b; b.LineColor = ImVec4(0.92f, 0.52f, 0.32f, 1); b.LineWeight = 2;
        plt::PlotLine("X", f.data(), X.data(), (int)f.size(), b);
        clickSets();
        plt::EndPlot();
    }
}

void summary(const Results& r) {
    if (!r.sol.ok) { ImGui::TextColored(pal::badRed(), "%s", r.sol.error.c_str()); return; }
    const cd z = r.sol.zin;
    const ad::Mismatch mm = ad::fromZ(z, r.z0);
    const ad::Figures& f = r.fig;
    const bool gnd = r.m.ground.type != ad::GroundType::Free;
    if (ImGui::BeginTable("##sum", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        auto row = [](const char* k, const char* fmt, auto... v) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k); ImGui::TableNextColumn(); ImGui::Text(fmt, v...);
        };
        row("Frequency", "%.4f MHz", r.fHz / 1e6);
        row("Impedance", "%.2f %+.2fj ohm", z.real(), z.imag());
        row("SWR", "%.3f against %g ohm (return loss %.1f dB, %.1f %% reflected)", mm.swr, r.z0, mm.returnLossDb, mm.reflectedPct);
        if (r.sol.srcZ.size() > 1)
            for (size_t i = 1; i < r.sol.srcZ.size(); ++i) row("  feed", "%d: %.2f %+.2fj ohm", (int)i + 1, r.sol.srcZ[i].real(), r.sol.srcZ[i].imag());
        row("Gain", "%.2f dBi  (%.2f dBd)", f.maxDbi, f.maxDbd);
        row("Direction", "phi %.0f deg (from +x, counter-clockwise), elevation %.0f deg", f.azMaxDeg, f.elMaxDeg);
        row("Front to back", "%.1f dB   front to side %.1f dB", f.fbDb, f.fsDb);
        row("Beamwidth", "%s azimuth, %s elevation", f.bwAzDeg >= 359 ? "omni" : (std::to_string((int)std::lround(f.bwAzDeg)) + " deg").c_str(),
            f.bwElDeg >= 179 ? "wide" : (std::to_string((int)std::lround(f.bwElDeg)) + " deg").c_str());
        if (gnd) row("Take-off angle", "%.0f deg", f.takeoffDeg);
        row("Efficiency", "%.1f %%%s", 100 * r.pat.efficiency, gnd && r.m.ground.type == ad::GroundType::Real ? " (ground loss included)" : "");
        row("Directivity", "%.2f dBi", r.pat.directivityDbi);
        if (r.sol.pLoss > 0) row("Loss in loads / wires", "%.1f %% of the input", 100 * r.sol.pLoss / std::max(r.sol.pIn, 1e-30));
        row("Model", "%d wires, %d segments, %d unknowns; %.0f ms", (int)r.m.wires.size(), (int)r.sol.mesh.segs.size(), (int)r.sol.mesh.basis.size(), r.ms);
        if (r.haveConv) row("Convergence", "%s: %.2f %% change from %d to %d segments", r.conv.converged ? "converged" : "NOT converged", r.conv.changePct, r.conv.n1, r.conv.n2);
        ImGui::EndTable();
    }
    if (ImGui::SmallButton("Check convergence")) { S.wantConv = true; touch(false); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Solve again with twice the segments: below 2 %% change the segmentation is fine");
    for (const auto& w : r.warn) ImGui::TextColored(pal::warnAmber(), "%s", w.c_str());
}

void currentsChart(const Results& r) {
    if (!r.sol.ok || r.sol.segI.empty()) { ImGui::TextDisabled("no currents"); return; }
    std::vector<double> x, mag, ph;
    for (size_t i = 0; i < r.sol.segI.size(); ++i) {
        const cd I = 0.5 * (r.sol.segI[i].ia + r.sol.segI[i].ib);
        x.push_back((double)i + 1); mag.push_back(1000 * std::abs(I)); ph.push_back(std::arg(I) * 180 / ad::kPi);
    }
    const float h = std::max(120.f, (ImGui::GetContentRegionAvail().y - 8 * gUi) / 2);
    if (plt::BeginPlot("##cur", ImVec2(-1, h), plt::Flags_NoLegend)) {
        plt::SetupAxes("segment", "mA (1 V feed)", 0, plt::AxisFlags_AutoFit);
        plt::SetupAxisLimits(plt::X1, 0.5, x.size() + 0.5, plt::Cond_Always);
        plt::Spec s; s.FillColor = pal::accent(0.7f);
        plt::PlotBars("|I|", x.data(), mag.data(), (int)x.size(), 0.8, s);
        // wire boundaries
        std::vector<double> edges;
        for (size_t i = 1; i < r.sol.mesh.segs.size(); ++i) if (r.sol.mesh.segs[i].wire != r.sol.mesh.segs[i - 1].wire) edges.push_back(i + 0.5);
        plt::Spec e; e.LineColor = ImVec4(0.6f, 0.6f, 0.6f, 0.35f);
        if (!edges.empty()) plt::PlotInfLines("##w", edges.data(), (int)edges.size(), e);
        plt::EndPlot();
    }
    if (plt::BeginPlot("##curph", ImVec2(-1, h), plt::Flags_NoLegend)) {
        plt::SetupAxes("segment", "phase (deg)");
        plt::SetupAxisLimits(plt::X1, 0.5, x.size() + 0.5, plt::Cond_Always);
        plt::SetupAxisLimits(plt::Y1, -180, 180, plt::Cond_Always);
        plt::Spec s; s.Marker = plt::Marker_Circle; s.MarkerSize = 2.5f; s.LineColor = ImVec4(0.92f, 0.52f, 0.32f, 1);
        plt::PlotScatter("phase", x.data(), ph.data(), (int)x.size(), s);
        plt::EndPlot();
    }
}

void topBar() {
    bool ch = false;
    ImGui::SetNextItemWidth(110 * gUi);
    double d = S.designMHz;
    if (ImGui::InputDouble("design MHz", &d, 0, 0, "%.4f", ImGuiInputTextFlags_EnterReturnsTrue) && d > 0.001 && d < 100000) {
        S.designMHz = d; rangeFor(d); S.fSelMHz = d;
        if (!S.custom) { S.params = ad::templateParams(S.tpl, d * 1e6); S.opt.vary.clear(); buildFromTemplate(); }
        else touch();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Press Enter: the template's dimensions are worked out again for this frequency");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120 * gUi);
    if (ImGui::BeginCombo("##band", "band...")) {
        for (const auto& b : ad::bandPresets())
            if (ImGui::Selectable(b.name)) {
                S.f0MHz = b.f0 / 1e6; S.f1MHz = b.f1 / 1e6; S.fSelMHz = 0.5 * (S.f0MHz + S.f1MHz);
                const double w = S.f1MHz - S.f0MHz;
                if (w < 0.02 * S.fSelMHz) { S.f0MHz -= 0.5 * w + 0.01 * S.fSelMHz; S.f1MHz += 0.5 * w + 0.01 * S.fSelMHz; }
                S.designMHz = S.fSelMHz;
                if (!S.custom) { S.params = ad::templateParams(S.tpl, S.designMHz * 1e6); S.opt.vary.clear(); buildFromTemplate(); }
                else touch();
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi); ch |= ImGui::InputDouble("from", &S.f0MHz, 0, 0, "%.4f", ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); ch |= ImGui::InputDouble("to", &S.f1MHz, 0, 0, "%.4f", ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(); ImGui::SetNextItemWidth(60 * gUi);
    if (ImGui::InputInt("points", &S.nPts, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) { S.nPts = std::max(2, std::min(401, S.nPts)); ch = true; }
    ImGui::SameLine(); ImGui::SetNextItemWidth(80 * gUi);
    if (ImGui::InputDouble("at MHz", &S.fSelMHz, 0, 0, "%.4f", ImGuiInputTextFlags_EnterReturnsTrue)) touch(false);
    // second row: ground, reference impedance, units
    int gt = (int)S.ground.type;
    const char* gn[] = {"Free space", "Perfect ground", "Real ground"};
    ImGui::SetNextItemWidth(130 * gUi);
    if (ImGui::Combo("##gnd", &gt, gn, 3)) {
        S.ground.type = (ad::GroundType)gt;
        if (!S.custom) buildFromTemplate(); else { S.model.ground = S.ground; touch(); }
    }
    if (S.ground.type == ad::GroundType::Real) {
        ImGui::SameLine();
        static const struct { const char* n; double er, s; } kG[] = {{"sea water", 80, 5}, {"fresh water", 80, 0.001}, {"wet ground", 30, 0.02},
                                                                     {"average ground", 13, 0.005}, {"poor / dry ground", 5, 0.001}, {"city / rock", 3, 0.0001}};
        ImGui::SetNextItemWidth(140 * gUi);
        if (ImGui::BeginCombo("##gtype", "ground type...")) {
            for (const auto& g : kG) if (ImGui::Selectable(g.n)) { S.ground.epsr = g.er; S.ground.sigma = g.s; ch = true; }
            ImGui::EndCombo();
        }
        ImGui::SameLine(); ImGui::SetNextItemWidth(60 * gUi); ch |= ImGui::InputDouble("er", &S.ground.epsr, 0, 0, "%.0f");
        ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); ch |= ImGui::InputDouble("S/m", &S.ground.sigma, 0, 0, "%.4f");
        S.ground.epsr = std::max(1.0, S.ground.epsr); S.ground.sigma = std::max(0.0, S.ground.sigma);
    }
    ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi);
    if (ImGui::InputDouble("ohm ref.", &S.z0, 0, 0, "%.0f")) { S.z0 = std::max(1.0, S.z0); ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The reference impedance of the SWR (50 coax, 75 TV, 200 / 450 / 2450 with a balun or transformer)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    const char* un[] = {"metric", "imperial"};
    ImGui::Combo("##units", &S.u.sys, un, 2);
    ImGui::SameLine(); ch |= ImGui::Checkbox("gain in sweep", &S.sweepGain);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Also the best gain and front to back at every sweep point (slower)");
    if (ch) touch();
}

} // namespace

void antennaDesignerWindow(App& a) {
    if (!a.antdOpen) {
        if (S.th.joinable() && !S.busy) S.th.join();
        return;
    }
    load();
    firstWindowSize(1180 * gUi, 820 * gUi);
    if (!ImGui::Begin("Antenna designer", &a.antdOpen)) { ImGui::End(); return; }
    topBar();
    schedule();
    const float leftW = 400 * gUi;
    ImGui::BeginChild("##antleft", ImVec2(leftW, 0), ImGuiChildFlags_ResizeX);
    if (ImGui::BeginTabBar("##antl")) {
        if (ImGui::BeginTabItem("Design")) { designTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Wires")) { wiresTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Optimise")) { optimiseTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Calculators")) {
            double f0 = 0, f1 = 0; bool picked = false;
            antdui::calculatorsTab(S.fSelMHz, S.z0, S.u, &f0, &f1, &picked);
            if (picked) { S.f0MHz = f0; S.f1MHz = f1; S.fSelMHz = 0.5 * (f0 + f1); touch(); }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Files")) { filesTab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    if (!S.msg.empty()) ImGui::TextWrapped("%s", S.msg.c_str());
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##antright", ImVec2(0, 0));
    ImGui::Checkbox("pattern", &S.showPat); ImGui::SameLine(); ImGui::Checkbox("currents", &S.showCur); ImGui::SameLine();
    ImGui::Checkbox("segments", &S.showSeg); ImGui::SameLine(); ImGui::Checkbox("grid", &S.showGrid);
    ImGui::SameLine();
    if (S.busy) ImGui::TextDisabled("solving..."); else if (S.dirty) ImGui::TextDisabled("..."); else ImGui::TextDisabled(" ");
    const float viewH = std::max(220.f, ImGui::GetContentRegionAvail().y * 0.48f);
    view3d(viewH);
    if (ImGui::BeginTabBar("##antr")) {
        std::lock_guard<std::mutex> lk(S.mu);   // the worker only takes it to hand over a finished result
        const Results& r = S.res;
        if (ImGui::BeginTabItem("Summary")) { if (r.valid) summary(r); else ImGui::TextDisabled("computing..."); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("SWR / impedance")) { sweepCharts(r); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Pattern")) {
            if (r.valid && r.sol.ok) {
                const float s = std::min(ImGui::GetContentRegionAvail().x / 2 - 10 * gUi, ImGui::GetContentRegionAvail().y - 30 * gUi);
                char t[96];
                std::snprintf(t, sizeof t, "Azimuth at %.0f deg elevation (phi, 0 = +x)", r.fig.elMaxDeg);
                antdui::polarPlot("az", r.az, 1, true, false, std::max(160.f, s), t, r.fig.azMaxDeg);
                ImGui::SameLine();
                std::snprintf(t, sizeof t, "Elevation toward phi %.0f deg", r.fig.azMaxDeg);
                const bool gnd = r.m.ground.type != ad::GroundType::Free;
                antdui::polarPlot("el", r.el, 1, !gnd, false, std::max(160.f, s), t, r.fig.elMaxDeg);
            } else ImGui::TextDisabled("no pattern yet");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Currents")) { currentsChart(r); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    static bool savedOnce = false;
    static double lastSave = 0;
    if (!savedOnce || (S.lastEdit > lastSave && now() - S.lastEdit > 1.0)) { save(); savedOnce = true; lastSave = now(); }
    ImGui::End();
}
