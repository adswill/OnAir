// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tools > Propagation: where a transmitter can be received. Two modes, chosen at the top:
//  Simple: where the transmitter is, what it is (a preset), frequency and power; a plain answer ("up to about 40 km"), a coloured map and a
//          good / fair / poor rating where you listen (a map click, or your location).
//  Advanced (prop_adv.cpp): every input of the model, the link budget term by term, sweeps, CSV, a second scenario to compare, terrain from
//          an elevation file you already have.
// The model is core/include/dect2/prop/ (docs/propagation.md). The map, ranges and sweeps are computed on a worker thread.
#include "prop_internal.h"
#include "dect2/common/u8file.h"
#include "dect2/geo/maidenhead.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <type_traits>

namespace propui {

State& st() { static State s; return s; }
pr::Scenario& cur() { State& S = st(); return S.sc[S.advanced && S.compare ? S.editing : 0]; }

std::shared_ptr<const Result> snapshot() {
    State& S = st();
    std::lock_guard<std::mutex> lk(S.mu);
    return S.result;
}

void helpMark(const char* text) {
    ImGui::SameLine(0, 4 * gUi);
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(380 * gUi); ImGui::TextUnformatted(text); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
}

static void labelText(const char* label, const char* help) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", label);
    if (help && ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(380 * gUi); ImGui::TextUnformatted(help); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
    if (st().labelCol > 0) ImGui::SameLine(st().labelCol); else ImGui::SameLine();
}

bool numField(const char* label, double* v, const char* unit, const char* help, float w, const char* fmt) {
    labelText(label, help);
    ImGui::SetNextItemWidth(w > 0 ? w : 90 * gUi);
    ImGui::PushID(label);
    const bool ch = ImGui::InputDouble("##v", v, 0, 0, fmt);
    ImGui::PopID();
    if (help) ImGui::SetItemTooltip("%s", help);
    if (unit && *unit) { ImGui::SameLine(); ImGui::TextDisabled("%s", unit); }
    return ch;
}

void enumCombo(const char* label, int* v, const char* items, const char* help, float w) {
    labelText(label, help);
    ImGui::SetNextItemWidth(w > 0 ? w : 150 * gUi);
    ImGui::PushID(label);
    ImGui::Combo("##c", v, items);
    ImGui::PopID();
    if (help) ImGui::SetItemTooltip("%s", help);
}

bool receiverPoint(double& lat, double& lon, std::string& label) {
    State& S = st();
    if (S.havePoint) { lat = S.pLat; lon = S.pLon; label = "the chosen point"; return true; }
    if (receiverLocationSet()) { receiverLocation(lat, lon); label = "your location"; return true; }
    return false;
}

void issuesUi(const pr::Scenario& s) {
    for (const pr::Issue& i : pr::validate(s)) {
        const ImVec4 c = i.level >= 2 ? pal::badRed() : i.level == 1 ? pal::warnAmber() : pal::grey();
        ImGui::PushStyleColor(ImGuiCol_Text, c);
        ImGui::TextWrapped("%s %s", i.level >= 2 ? "Fix:" : i.level == 1 ? "Check:" : "Note:", i.text.c_str());
        ImGui::PopStyleColor();
    }
}

namespace {

// ------------------------------------------------------------------ settings
template <class F> void visit(pr::Scenario& s, F&& f) {
    auto E = [&](const char* k, auto& e) { int v = (int)e; f(k, v); e = (std::remove_reference_t<decltype(e)>)v; };
    f("preset", s.preset); f("lat", s.txLat); f("lon", s.txLon); f("power", s.power); E("punit", s.powerUnit);
    f("txg", s.txGainDbi); f("feed", s.txFeederDb); f("txh", s.txHeightM); f("txaz", s.txAzDeg); f("pat", s.txPatternPath);
    f("f", s.fMHz); f("bw", s.bandwidthHz); f("snr", s.requiredSnrDb);
    f("rxg", s.rxGainDbi); f("rxfeed", s.rxFeederDb); f("rxh", s.rxHeightM);
    f("radio", s.radio); f("rgain", s.radioGainPct); f("nf", s.rxNfDb);
    f("pre", s.preamp); f("preg", s.preampGainDb); f("prenf", s.preampNfDb);
    E("txpol", s.txPol); E("rxpol", s.rxPol); E("env", s.env); f("atm", s.atmosphericNoise); E("gnd", s.ground); E("clut", s.rxClutter);
    E("pmodel", s.pathModel); E("hfmodel", s.hfModel); f("sfi", s.sfi); f("kidx", s.kIndex); f("rain", s.rainMmH); f("gas", s.gas);
    f("now", s.useNow); f("y", s.year); f("mo", s.month); f("d", s.day); f("h", s.hour); f("mi", s.minute);
    f("tpct", s.timePct); f("lpct", s.locPct); f("sigl", s.sigmaLocDb); f("dn", s.deltaN); f("n0", s.n0);   // 4.0
    f("hfleg", s.hfLegacy); f("r12", s.r12); f("dmode", s.decodeMode); f("dcond", s.decodeCondition);
}

std::string keyOf(pr::Scenario s) {
    std::string k;
    char b[64];
    visit(s, [&](const char*, auto& v) {
        using T = std::remove_reference_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) k += v;
        else if constexpr (std::is_same_v<T, double>) { snprintf(b, sizeof b, "%.17g", v); k += b; }
        else { snprintf(b, sizeof b, "%d", (int)v); k += b; }
        k += '|';
    });
    return k;
}

void nowUtc(pr::Scenario& s) {
    const std::time_t t = std::time(nullptr);
    std::tm g{};
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    s.year = g.tm_year + 1900; s.month = g.tm_mon + 1; s.day = g.tm_mday; s.hour = g.tm_hour; s.minute = g.tm_min;
}

std::string readText(const std::string& path, size_t maxBytes) {
    std::string out;
    FILE* f = dect2::u8fopen(path, "rb");
    if (!f) return out;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && out.size() < maxBytes) out.append(buf, n);
    std::fclose(f);
    return out;
}

void loadScenario(pr::Scenario& s, const char* prefix) {
    auto& pf = plat::prefs();
    const bool old = !pf.has((std::string(prefix) + "preset").c_str()) && pf.has((std::string(prefix) + "f").c_str());
    visit(s, [&](const char* k, auto& v) {
        const std::string key = std::string(prefix) + k;
        using T = std::remove_reference_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) v = pf.getS(key.c_str(), v);
        else if constexpr (std::is_same_v<T, double>) v = pf.getD(key.c_str(), v);
        else if constexpr (std::is_same_v<T, bool>) v = pf.getB(key.c_str(), v);
        else v = (int)pf.getI(key.c_str(), v);
    });
    if (old) s.preset = (int)pr::presets().size() - 1;   // settings from 0.2.9: their own numbers
    s.preset = std::clamp(s.preset, 0, (int)pr::presets().size() - 1);
    s.radio = std::clamp(s.radio, 0, (int)pr::radios().size() - 1);
    if (!std::isfinite(s.txLat) || std::fabs(s.txLat) > 90) s.txLat = 0;
    if (!std::isfinite(s.txLon) || std::fabs(s.txLon) > 180) s.txLon = 0;
}

void saveScenario(pr::Scenario s, const char* prefix) {
    auto& pf = plat::prefs();
    visit(s, [&](const char* k, auto& v) {
        const std::string key = std::string(prefix) + k;
        using T = std::remove_reference_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) pf.setS(key.c_str(), v);
        else if constexpr (std::is_same_v<T, double>) pf.setD(key.c_str(), v);
        else if constexpr (std::is_same_v<T, bool>) pf.setB(key.c_str(), v);
        else pf.setI(key.c_str(), v);
    });
}

void loadPattern(int i) {
    State& S = st();
    S.pattern[i].reset();
    S.patternMsg[i].clear();
    const std::string& path = S.sc[i].txPatternPath;
    if (path.empty()) return;
    const std::string text = readText(path, 1 << 20);
    auto ap = std::make_shared<pr::AntennaPattern>();
    std::string err;
    if (text.empty()) S.patternMsg[i] = "cannot read " + path;
    else if (!pr::parseAntennaPattern(text, *ap, err)) S.patternMsg[i] = err;
    else {
        const size_t sl = path.find_last_of("/\\");
        ap->name = sl == std::string::npos ? path : path.substr(sl + 1);
        S.pattern[i] = ap;
    }
}

void loadTerrainFiles() {
    State& S = st();
    auto t = std::make_shared<pr::Terrain>();
    std::string msgs;
    for (const std::string& f : S.terrainFiles) {
        std::string err;
        if (!pr::loadElevationFile(f, *t, err)) msgs += (msgs.empty() ? "" : "; ") + f + ": " + err;
    }
    S.terrain = t->empty() ? nullptr : std::shared_ptr<const pr::Terrain>(t);
    S.terrainMsg = msgs;
}

void load(App&) {
    State& S = st();
    if (S.loaded) return;
    S.loaded = true;
    auto& pf = plat::prefs();
    double rl, ro; receiverLocation(rl, ro);
    S.sc[0].txLat = rl; S.sc[0].txLon = ro;
    loadScenario(S.sc[0], "prop_");
    S.sc[1] = S.sc[0];
    if (pf.has("propB_f")) loadScenario(S.sc[1], "propB_");
    S.advanced = pf.getB("prop_adv", false);
    S.compare = pf.getB("prop_cmp", false);
    S.resolution = std::clamp((int)pf.getI("prop_res", 1), 0, 2);
    S.hfChartKm = pf.getD("prop_hfkm", 1000);
    S.budgetKm = pf.getD("prop_bkm", 10);
    const std::string tf = pf.getS("prop_terrain", "");
    for (size_t a = 0; a < tf.size();) {
        size_t b = tf.find('\n', a);
        if (b == std::string::npos) b = tf.size();
        if (b > a) S.terrainFiles.push_back(tf.substr(a, b - a));
        a = b + 1;
    }
    if (!S.terrainFiles.empty()) loadTerrainFiles();
    for (int i = 0; i < 2; i++) { if (S.sc[i].useNow) nowUtc(S.sc[i]); loadPattern(i); }
    S.map.lat = S.sc[0].txLat; S.map.lon = S.sc[0].txLon; S.map.zoom = (int)pf.getI("prop_zoom", 8);
}

std::string savedKey;
void saveIfChanged() {
    State& S = st();
    std::string k = keyOf(S.sc[0]) + "#" + keyOf(S.sc[1]);
    char b[96];
    snprintf(b, sizeof b, "#%d%d%d%.6g,%.6g,%d", S.advanced, S.compare, S.resolution, S.hfChartKm, S.budgetKm, S.map.zoom);
    k += b;
    for (const auto& f : S.terrainFiles) k += f + "\n";
    if (k == savedKey) return;
    savedKey = k;
    auto& pf = plat::prefs();
    saveScenario(S.sc[0], "prop_");
    saveScenario(S.sc[1], "propB_");
    pf.setB("prop_adv", S.advanced); pf.setB("prop_cmp", S.compare); pf.setI("prop_res", S.resolution); pf.setD("prop_hfkm", S.hfChartKm); pf.setD("prop_bkm", S.budgetKm);
    pf.setI("prop_zoom", S.map.zoom);
    std::string tf;
    for (const auto& f : S.terrainFiles) tf += f + "\n";
    pf.setS("prop_terrain", tf);
}

// ------------------------------------------------------------------ worker
void compute(Result& r, int n, std::atomic<bool>& cancel) {
    const int count = r.compare ? 2 : 1;
    double rad = 5;
    for (int i = 0; i < count; i++) {
        const pr::Params& p = r.p[i];
        if (p.terrain && p.fMHz >= 30) {
            // the range depends on the direction: try 8 bearings, report the median, remember the best
            std::vector<pr::Range> rs;
            for (int b = 0; b < 8 && !cancel; b++) rs.push_back(pr::findRange(p, b * 45.0, 400.0));
            std::sort(rs.begin(), rs.end(), [](const pr::Range& a, const pr::Range& c) { return a.maxKm < c.maxKm; });
            if (!rs.empty()) { r.range[i] = rs[rs.size() / 2]; r.bestKm[i] = rs.back().maxKm; }
        } else {
            r.range[i] = pr::findRange(p, r.sweepBearing);
            r.bestKm[i] = r.range[i].maxKm;
        }
        rad = std::max(rad, r.range[i].any ? std::max(r.bestKm[i], r.range[i].maxKm) * 1.25 : 60.0);
    }
    rad = std::min(rad, 6000.0);
    for (int i = 0; i < count && !cancel; i++) r.grid[i] = pr::computeGrid(r.p[i], rad, n, &cancel);
    if (!r.sweeps) return;
    for (int i = 0; i < count && !cancel; i++) {
        pr::Params p = r.p[i];
        p.terrain = nullptr;   // the charts use smooth earth: cheap everywhere, and not tied to one direction's hills
        r.sigVsDist[i] = pr::sweepSignalVsDistance(p, std::max(rad, 10.0), 60, r.sweepBearing);
        r.rangeVsPower[i] = pr::sweepRangeVsPower(p, 9, r.sweepBearing);
        if (p.fMHz < 30) {
            r.rangeVsFreq[i] = pr::sweepRangeVsFrequency(p, 1.8, 29.9, 14, r.sweepBearing);
            r.day[i] = pr::hfDay(p, std::max(r.sweepKm, 50.0), r.sweepBearing);
        } else r.rangeVsFreq[i] = pr::sweepRangeVsFrequency(p, std::max(30.0, p.fMHz / 3), std::min(100000.0, p.fMHz * 3), 12, r.sweepBearing);
    }
    // 4.0 views of scenario A, toward the chosen point (or north at the budget / HF chart distance)
    if (cancel) return;
    const pr::Params& a = r.p[0];
    if (a.fMHz >= 30) {
        r.lossByModel = pr::sweepLossByModel(a, std::max(rad, 10.0), 40, r.sweepBearing);
        r.profile = pr::profileView(a, r.tgtLat, r.tgtLon, 200);
    } else {
        r.lossByModel = pr::sweepLossByModel(a, std::max(rad, 10.0), 40, r.sweepBearing);
        std::vector<double> fr = pr::hf::hamBandsMHz();
        if (std::find_if(fr.begin(), fr.end(), [&](double x) { return std::fabs(x - a.fMHz) < 0.05; }) == fr.end()) {
            fr.push_back(a.fMHz);
            std::sort(fr.begin(), fr.end());
        }
        r.hfChart = pr::hfChartFor(a, r.tgtLat, r.tgtLon, fr);
        r.haveChart = true;
    }
}

std::string jobKey() {
    State& S = st();
    const bool cmp = S.advanced && S.compare;
    std::string k = keyOf(S.sc[0]);
    if (cmp) k += "#" + keyOf(S.sc[1]);
    char b[200];
    snprintf(b, sizeof b, "#%d%d%d%p%p%p%.6g,%.6g", S.advanced, cmp, S.resolution, (const void*)S.terrain.get(), (const void*)S.pattern[0].get(),
             (const void*)S.pattern[1].get(), S.hfChartKm, S.budgetKm);
    k += b;
    double la, lo; std::string lab;
    if (receiverPoint(la, lo, lab)) { snprintf(b, sizeof b, "#%.5f,%.5f", la, lo); k += b; }   // the sweeps point toward it; HF ranges by bearing
    return k;
}

void startJob() {
    State& S = st();
    if (S.th.joinable()) S.th.join();
    auto r = std::make_shared<Result>();
    r->terrain = S.terrain;
    r->compare = S.advanced && S.compare;
    for (int i = 0; i < (r->compare ? 2 : 1); i++) {
        r->pattern[i] = S.pattern[i];
        r->p[i] = pr::toParams(S.sc[i], r->terrain.get(), r->pattern[i].get());
    }
    r->sweeps = S.advanced;
    double la, lo; std::string lab;
    if (receiverPoint(la, lo, lab) && pr::greatCircleKm(r->p[0].txLat, r->p[0].txLon, la, lo) > 0.01) {
        r->sweepBearing = pr::bearingDeg(r->p[0].txLat, r->p[0].txLon, la, lo);
        r->sweepKm = pr::greatCircleKm(r->p[0].txLat, r->p[0].txLon, la, lo);
    } else r->sweepKm = S.hfChartKm;
    if (receiverPoint(la, lo, lab)) { r->tgtLat = la; r->tgtLon = lo; }
    else pr::destination(r->p[0].txLat, r->p[0].txLon, 0, r->p[0].fMHz < 30 ? S.hfChartKm : S.budgetKm, r->tgtLat, r->tgtLon);
    const int n = S.advanced ? (S.resolution == 0 ? 40 : S.resolution == 2 ? 140 : 80) : 60;
    S.cancel = false;
    S.busy = true;
    S.th = std::thread([r, n]() mutable {
        State& S2 = st();
        compute(*r, n, S2.cancel);
        if (!S2.cancel) { std::lock_guard<std::mutex> lk(S2.mu); S2.result = r; }
        S2.busy = false;
    });
}

void schedule() {
    State& S = st();
    const int count = S.advanced && S.compare ? 2 : 1;
    for (int i = 0; i < count; i++) if (pr::hasErrors(pr::validate(S.sc[i]))) return;   // nothing to compute until the inputs make sense
    const std::string k = jobKey();
    if (k == S.lastKey) return;
    if (S.busy) { S.cancel = true; return; }   // stop the old one; the next frame starts the new one
    S.lastKey = k;
    startJob();
}

// ------------------------------------------------------------------ map
ImU32 lerpCol(const int* a, const int* b, float t, int alpha) {
    auto L = [t](int x, int y) { return (int)(x + (y - x) * t); };
    return IM_COL32(L(a[0], b[0]), L(a[1], b[1]), L(a[2], b[2]), L(a[3], b[3]) * alpha / 255);
}

// the colour of a cell by its margin over the SNR needed: Simple uses three plain bands, Advanced a continuous scale
ImU32 marginColour(float m, bool simple, int alpha = 255) {
    if (simple) {
        if (m >= 10) return IM_COL32(70, 200, 90, 150 * alpha / 255);
        if (m >= 0) return IM_COL32(240, 200, 60, 150 * alpha / 255);
        if (m >= -10) return IM_COL32(220, 80, 70, (int)(110 * (1 + m / 10.0f)) * alpha / 255);
        return 0;
    }
    struct Stop { float m; int c[4]; };
    static const Stop s[] = {{-25, {40, 50, 120, 0}}, {-12, {70, 80, 170, 70}}, {0, {230, 90, 60, 150}}, {6, {245, 200, 60, 170}},
                             {15, {90, 210, 90, 175}}, {30, {60, 200, 220, 185}}, {45, {240, 240, 255, 200}}};
    const int n = (int)(sizeof s / sizeof s[0]);
    if (m <= s[0].m) return 0;
    if (m >= s[n - 1].m) return IM_COL32(s[n - 1].c[0], s[n - 1].c[1], s[n - 1].c[2], s[n - 1].c[3] * alpha / 255);
    for (int i = 1; i < n; i++)
        if (m <= s[i].m) return lerpCol(s[i - 1].c, s[i].c, (m - s[i - 1].m) / (s[i].m - s[i - 1].m), alpha);
    return 0;
}

void ring(ImDrawList* dl, double lat, double lon, double km, ImU32 col, bool dashed) {
    if (km <= 0.05) return;
    ImVec2 pts[97];
    for (int i = 0; i <= 96; i++) {
        double la, lo; pr::destination(lat, lon, i * 360.0 / 96, km, la, lo);
        pts[i] = adsbmap::project(la, lo);
    }
    if (!dashed) { dl->AddPolyline(pts, 97, col, ImDrawFlags_None, 2.0f * gUi); return; }
    for (int i = 0; i < 96; i += 2) dl->AddLine(pts[i], pts[i + 1], col, 2.0f * gUi);
}

void drawHeat(ImDrawList* dl, const pr::Grid& g, double needSnr, bool simple) {
    if (g.n < 2 || g.snr.empty()) return;
    for (int y = 0; y < g.n; y++) {
        const double la0 = g.lat0 + (g.lat1 - g.lat0) * y / g.n, la1 = g.lat0 + (g.lat1 - g.lat0) * (y + 1) / g.n;
        for (int x = 0; x < g.n; x++) {
            const ImU32 c = marginColour(g.snr[(size_t)y * g.n + x] - (float)needSnr, simple);
            if ((c >> 24) == 0) continue;
            const double lo0 = g.lon0 + (g.lon1 - g.lon0) * x / g.n, lo1 = g.lon0 + (g.lon1 - g.lon0) * (x + 1) / g.n;
            const ImVec2 a = adsbmap::project(la0, lo0), b = adsbmap::project(la1, lo1);
            dl->AddRectFilled(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x) + 1, std::max(a.y, b.y) + 1), c);
        }
    }
}

void legend(float x, float y, double needSnr, bool simple) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = 200 * gUi, h = 12 * gUi, lh = ImGui::GetTextLineHeight();
    dl->AddRectFilled(ImVec2(x - 6 * gUi, y - 6 * gUi), ImVec2(x + w + 6 * gUi, y + h + 6 * gUi + 2 * lh), IM_COL32(15, 18, 22, 210), 4 * gUi);
    const ImU32 tc = IM_COL32(210, 215, 220, 255);
    if (simple) {
        const char* nm[3] = {"poor", "fair", "good"};
        const float ms[3] = {-5, 5, 15};
        for (int i = 0; i < 3; i++) {
            const float x0 = x + w * i / 3.0f;
            dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + w / 3.0f - 2 * gUi, y + h), marginColour(ms[i], true) | IM_COL32(0, 0, 0, 255));
            dl->AddText(ImVec2(x0, y + h + 2 * gUi), tc, nm[i]);
        }
        dl->AddText(ImVec2(x, y + h + 2 * gUi + lh), IM_COL32(190, 196, 202, 255), "good: 10 dB to spare; ring: the range");
        return;
    }
    const float lo = -25, hi = 45;
    for (int i = 0; i < 70; i++) {
        const float m = lo + (hi - lo) * i / 69.0f;
        dl->AddRectFilled(ImVec2(x + w * i / 70.0f, y), ImVec2(x + w * (i + 1) / 70.0f + 1, y + h), marginColour(m, false));
    }
    const float zx = x + w * (0 - lo) / (hi - lo);
    dl->AddLine(ImVec2(zx, y - 3 * gUi), ImVec2(zx, y + h + 3 * gUi), IM_COL32(255, 255, 255, 230), 1.5f * gUi);
    char b[96];
    snprintf(b, sizeof b, "SNR %.0f dB", needSnr + lo);
    dl->AddText(ImVec2(x, y + h + 2 * gUi), tc, b);
    snprintf(b, sizeof b, "%.0f dB", needSnr + hi);
    dl->AddText(ImVec2(x + w - ImGui::CalcTextSize(b).x, y + h + 2 * gUi), tc, b);
    snprintf(b, sizeof b, "white line: %.0f dB needed", needSnr);
    dl->AddText(ImVec2(x, y + h + 2 * gUi + lh), IM_COL32(190, 196, 202, 255), b);
}

void pin(ImDrawList* dl, double lat, double lon, ImU32 c) {
    const ImVec2 q = adsbmap::project(lat, lon);
    dl->AddTriangleFilled(ImVec2(q.x - 4 * gUi, q.y - 10 * gUi), ImVec2(q.x + 4 * gUi, q.y - 10 * gUi), q, c);
    dl->AddCircleFilled(ImVec2(q.x, q.y - 13 * gUi), 6 * gUi, c);
    dl->AddCircleFilled(ImVec2(q.x, q.y - 13 * gUi), 2.2f * gUi, IM_COL32(255, 255, 255, 230));
}

// ------------------------------------------------------------------ Simple mode
void placeRow() {
    State& S = st();
    pr::Scenario& s = cur();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Transmitter");
    ImGui::SameLine();
    ImGui::Text("%.4f, %.4f  %s", s.txLat, s.txLon, dect2::toMaidenhead(s.txLat, s.txLon, 6).c_str());
    ImGui::SameLine(0, 10 * gUi);
    if (ImGui::SmallButton("Use my location")) {
        if (!receiverLocationSet()) S.msg = "your location is not set yet (the ADS-B window sets it)";
        else { receiverLocation(s.txLat, s.txLon); S.map.lat = s.txLat; S.map.lon = s.txLon; S.msg.clear(); }
    }
    ImGui::SameLine();
    if (S.placingTx) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.6f));
    if (ImGui::SmallButton(S.placingTx ? "Click the map..." : "Place on the map")) S.placingTx = !S.placingTx;
    if (S.placingTx) ImGui::PopStyleColor();
    ImGui::SetItemTooltip("%s", "The next click on the map moves the transmitter there. Other clicks choose where you listen.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170 * gUi);
    if (ImGui::InputTextWithHint("##propcoord", "or type a position, Enter", S.coordBuf, sizeof S.coordBuf, ImGuiInputTextFlags_EnterReturnsTrue)) {
        double la, lo;
        if (dect2::parseCoordinates(S.coordBuf, la, lo)) { s.txLat = la; s.txLon = lo; S.map.lat = la; S.map.lon = lo; S.msg.clear(); }
        else S.msg = "not a position (try 54.687, 25.280 or a locator like KO24pq)";
    }
    if (!S.msg.empty()) ImGui::TextColored(pal::warnAmber(), "%s", S.msg.c_str());
}

void simpleInputs() {
    State& S = st();
    pr::Scenario& s = S.sc[0];
    placeRow();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("What it is");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(190 * gUi);
    const auto& ps = pr::presets();
    if (ImGui::BeginCombo("##proppreset", ps[s.preset].name)) {
        for (int i = 0; i < (int)ps.size(); i++)
            if (ImGui::Selectable(ps[i].name, i == s.preset)) pr::applyPreset(s, i);
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, 12 * gUi);
    S.labelCol = 0;
    numField("Frequency", &s.fMHz, "MHz", "The carrier frequency in MHz (98.0 for an FM station, 145.5 for 2 m, 7.1 for 40 m).", 80 * gUi);
    ImGui::SameLine(0, 12 * gUi);
    numField("Power", &s.power, nullptr, "The transmitter's power (into its antenna cable). The preset's antenna and heights are used.", 70 * gUi);
    ImGui::SameLine();
    int u = (int)s.powerUnit;
    ImGui::SetNextItemWidth(58 * gUi);
    if (ImGui::Combo("##proppu", &u, "W\0dBW\0dBm\0kW\0")) s.powerUnit = (pr::PowerUnit)u;
    ImGui::TextDisabled("%s", ps[s.preset].note);
    // 4.0: decode with an OnAir mode (its measured threshold), one click
    const pr::ModeSnr* cm = s.decodeMode.empty() ? nullptr : pr::findModeThreshold(s.decodeMode);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Decode with");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(190 * gUi);
    if (ImGui::BeginCombo("##propdmode", cm ? cm->name : "(the preset's SNR)")) {
        if (ImGui::Selectable("(the preset's SNR)", !cm)) s.decodeMode.clear();
        for (const pr::ModeSnr& m : pr::modeThresholds())
            if (ImGui::Selectable(m.name, cm == &m)) s.decodeMode = m.id;
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s", "Answer 'will this OnAir mode decode here?': the SNR the mode needs on a real radio, as OnAir measured it, and its bandwidth.");
    if (cm) { ImGui::SameLine(); ImGui::TextDisabled("needs %.0f dB in %.4g kHz", pr::modeRequiredSnrDb(*cm, s.decodeCondition), cm->bwHz / 1e3); }
    issuesUi(s);
}

void simpleResult(const std::shared_ptr<const Result>& r) {
    if (pr::hasErrors(pr::validate(st().sc[0]))) { ImGui::TextColored(pal::badRed(), "Fix the input marked above to see the coverage."); return; }
    if (!r) { ImGui::TextDisabled("computing..."); return; }
    const pr::Params& p = r->p[0];
    double la, lo; std::string lab;
    const bool have = receiverPoint(la, lo, lab);
    pr::Point q;
    if (have) q = pr::evaluateAt(p, la, lo);
    const pr::Verdict v = pr::verdict(p, r->range[0], have ? &q : nullptr, lab.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, pal::heading());
    ImGui::TextWrapped("%s", v.headline.c_str());
    ImGui::PopStyleColor();
    if (p.terrain && r->bestKm[0] > r->range[0].maxKm * 1.2) {
        ImGui::SameLine();
        ImGui::TextDisabled("(hills shape it: up to %s in the best direction)", pr::kmText(r->bestKm[0]).c_str());
    }
    if (have) {
        const ImVec4 c = v.rating == 2 ? pal::okGreen() : v.rating == 1 ? pal::warnAmber() : pal::badRed();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(c.x, c.y, c.z, 0.35f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(c.x, c.y, c.z, 0.35f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(c.x, c.y, c.z, 0.35f));
        char chip[16];
        snprintf(chip, sizeof chip, "%s", v.rating == 2 ? "GOOD" : v.rating == 1 ? "FAIR" : "POOR");
        ImGui::SmallButton(chip);
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        ImGui::TextWrapped("%s", v.atRx.c_str());
        for (const std::string& t : v.reasons) { ImGui::Bullet(); ImGui::TextWrapped("%s", t.c_str()); }
    } else ImGui::TextDisabled("Click the map where you want to listen for a good / fair / poor rating there (or set your location in the ADS-B window).");
    ImGui::TextDisabled("%s", pr::modelName(p));
    helpMark((pr::modelLimits(p) + " Advanced mode shows every term, and takes an elevation file for hills.").c_str());
}

} // namespace

// ------------------------------------------------------------------ the map, both modes
void mapView(App&, float reserveBelow) {
    State& S = st();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, std::max(180 * gUi, ImGui::GetContentRegionAvail().y - reserveBelow));
    adsbmap::draw(S.map, size);
    ImVec2 click;
    if (adsbmap::clickedAt(click)) {
        double la, lo; adsbmap::unproject(click, la, lo); lo = std::fmod(lo + 540, 360) - 180;
        if (S.placingTx) { cur().txLat = la; cur().txLon = lo; S.placingTx = false; }
        else { S.havePoint = true; S.pLat = la; S.pLon = lo; }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    const std::shared_ptr<const Result> r = snapshot();
    const bool simple = !S.advanced;
    if (r) {
        const int show = r->compare ? std::clamp(S.show, 0, 1) : 0;
        drawHeat(dl, r->grid[show], r->p[show].requiredSnrDb, simple);
        for (int i = 0; i < (r->compare ? 2 : 1); i++) {
            if (r->p[i].terrain) continue;   // with hills the range is not a circle: the colours show it
            const ImU32 col = i == 0 ? IM_COL32(255, 255, 255, 230) : IM_COL32(110, 220, 255, 230);
            for (const auto& sp : r->range[i].spans) {
                ring(dl, r->p[i].txLat, r->p[i].txLon, sp.second, col, i == 1);
                if (sp.first > 0.1) ring(dl, r->p[i].txLat, r->p[i].txLon, sp.first, (col & 0x00ffffff) | IM_COL32(0, 0, 0, 140), i == 1);
            }
        }
    }
    if (S.advanced && S.compare) pin(dl, S.sc[1].txLat, S.sc[1].txLon, IM_COL32(110, 200, 255, 255));
    pin(dl, S.sc[0].txLat, S.sc[0].txLon, IM_COL32(240, 90, 80, 255));
    double la, lo; std::string lab;
    if (receiverPoint(la, lo, lab)) {
        const ImVec2 pp = adsbmap::project(la, lo);
        dl->AddCircle(pp, 6 * gUi, IM_COL32(255, 255, 255, 255), 16, 2 * gUi);
        dl->AddCircleFilled(pp, 2 * gUi, IM_COL32(255, 255, 255, 255));
    }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+##propz", ImVec2(26 * gUi, 0))) S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom in"));
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-##propz", ImVec2(26 * gUi, 0))) S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom out"));
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("TX##propc")) { S.map.lat = cur().txLat; S.map.lon = cur().txLon; }
    ImGui::SetItemTooltip("%s", "Centre the map on the transmitter");
    if (S.havePoint) {
        ImGui::SameLine(0, 6 * gUi);
        if (ImGui::Button("Clear point##propcp")) S.havePoint = false;
        ImGui::SetItemTooltip("%s", "Forget the clicked point (your location is used again when it is set)");
    }
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the transmitter or your location");
    if (S.busy) { ImGui::SameLine(0, 8 * gUi); ImGui::TextColored(pal::warnAmber(), "computing..."); }
    if (S.placingTx) { ImGui::SameLine(0, 8 * gUi); ImGui::TextColored(pal::warnAmber(), "click where the transmitter is"); }
    if (r) legend(p0.x + size.x - 214 * gUi, p0.y + size.y - 24 * gUi - 2 * ImGui::GetTextLineHeight(), r->p[r->compare ? std::clamp(S.show, 0, 1) : 0].requiredSnrDb, simple);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
    ImGui::Dummy(ImVec2(0, 0));
}

void loadPatternFor(int i) { loadPattern(i); }
void reloadTerrain() { loadTerrainFiles(); }
void placeRowUi() { placeRow(); }

} // namespace propui

void propagationWindow(App& a) {
    using namespace propui;
    State& S = st();
    if (!a.propOpen) {
        if (S.th.joinable() && !S.busy) S.th.join();
        return;
    }
    load(a);
    firstWindowSize(980 * gUi, 780 * gUi);
    if (!ImGui::Begin("Propagation", &a.propOpen)) { ImGui::End(); return; }
    for (auto& s : S.sc) if (s.useNow) nowUtc(s);
    // the mode toggle
    for (int m = 0; m < 2; m++) {
        const bool on = (m == 1) == S.advanced;
        if (m) ImGui::SameLine(0, 0);
        ImGui::PushStyleColor(ImGuiCol_Button, on ? pal::accent(0.55f) : ImVec4(0.16f, 0.17f, 0.18f, 1));
        if (ImGui::Button(m ? "Advanced" : "Simple", ImVec2(96 * gUi, 0))) S.advanced = m == 1;
        ImGui::PopStyleColor();
    }
    ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled("%s", S.advanced ? "every input, the link budget, sweeps, compare, terrain" : "where it can be heard, in plain words");
    if (S.advanced) {
        ImGui::SameLine(0, 14 * gUi);
        if (ImGui::Checkbox("Compare two scenarios", &S.compare) && S.compare && keyOf(S.sc[1]) == keyOf(S.sc[0])) S.editing = 1;
        ImGui::SetItemTooltip("%s", "Scenario B starts as a copy of A: change one thing and see the difference in the table, the sweeps and the rings (dashed).");
    }
    if (!S.advanced) {
        simpleInputs();
        schedule();
        const auto r = snapshot();
        simpleResult(r);
        mapView(a, 4 * gUi);
    } else {
        schedule();
        const auto r = snapshot();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const bool narrow = avail.x < 780 * gUi;   // phones and small windows: inputs above the results
        if (!narrow) {
            ImGui::BeginChild("##propin", ImVec2(390 * gUi, 0), ImGuiChildFlags_Borders);
            advancedInputs(a);
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("##propout", ImVec2(0, 0));
            advancedResults(a, r);
            ImGui::EndChild();
        } else {
            ImGui::BeginChild("##propin2", ImVec2(0, avail.y * 0.42f), ImGuiChildFlags_Borders);
            advancedInputs(a);
            ImGui::EndChild();
            ImGui::BeginChild("##propout2", ImVec2(0, 0));
            advancedResults(a, r);
            ImGui::EndChild();
        }
    }
    saveIfChanged();
    ImGui::End();
}
