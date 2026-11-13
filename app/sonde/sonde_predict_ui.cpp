// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Radiosonde landing prediction 4.0 in the app (core: dect2/sonde/sonde_predict.h, docs/SONDE-PREDICT-4.0.md): one tracker per sonde fed
// with every frame, the prediction refreshed live (about once a second for the selected sonde, with its ensemble), the terrain tiles and
// the wind forecast. Nothing goes on the network unless "Download terrain and wind forecasts" is on and Offline mode is off; terrain
// tiles already on disk (the propagation tool's folder) are used either way.
#include "app.h"
#include "../adsb/adsb_map.h"
#include "dect2/prop/prop_terrain.h"
#include "dect2/sonde/sonde_map_overlay.h"
#include "dect2/sonde/sonde_predict.h"
#include "dect2/sonde/sonde_tel.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

const dect2::SondePrediction* sondePredict40(const dect2::SondeInfo& s, bool selected, const dect2::tawhiri::Result* tw);
bool sondePredict40On();
void sondePredict40Draw(ImDrawList* dl, const dect2::SondePrediction& p);
void sondePredict40Advanced();
void sondePredict40Network();

namespace {

struct Opts {
    bool loaded = false;
    bool on = true, net = false, terrain = true, ascent = true, fit = true, kalman = true, drawMembers = true;
    int source = 0;              // 0 Open-Meteo GFS, 1 the SondeHub (Tawhiri) prediction, 2 none
    int members = 200;
    float approachS = 60, windChange = 1.0f, corrLen = 600;
};
Opts& O() {
    static Opts o;
    if (!o.loaded) {
        o.loaded = true;
        plat::Prefs& p = plat::prefs();
        o.on = p.getB("sondePred40", true); o.net = p.getB("sondePredNet", false); o.terrain = p.getB("sondePredTerrain", true);
        o.ascent = p.getB("sondePredAscent", true); o.fit = p.getB("sondePredFit", true); o.kalman = p.getB("sondePredKalman", true);
        o.drawMembers = p.getB("sondePredDots", true);
        o.source = std::max(0, std::min(2, (int)p.getI("sondePredSource", 0)));
        o.members = std::max(0, std::min(1000, (int)p.getI("sondePredMembers", 200)));
        o.approachS = (float)std::max(30.0, std::min(120.0, p.getD("sondePredApproachS", 60)));
        o.windChange = (float)std::max(0.0, std::min(5.0, p.getD("sondePredWindChange", 1.0)));
        o.corrLen = (float)std::max(100.0, std::min(5000.0, p.getD("sondePredCorrLen", 600)));
    }
    return o;
}
void save() {
    const Opts& o = O(); plat::Prefs& p = plat::prefs();
    p.setB("sondePred40", o.on); p.setB("sondePredNet", o.net); p.setB("sondePredTerrain", o.terrain); p.setB("sondePredAscent", o.ascent);
    p.setB("sondePredFit", o.fit); p.setB("sondePredKalman", o.kalman); p.setB("sondePredDots", o.drawMembers); p.setI("sondePredSource", o.source);
    p.setI("sondePredMembers", o.members); p.setD("sondePredApproachS", o.approachS); p.setD("sondePredWindChange", o.windChange); p.setD("sondePredCorrLen", o.corrLen);
}
bool netAllowed() { return O().net && !plat::prefs().getB("offlineMode", false); }

// ---- terrain: SRTM tiles from the propagation tool's folder, downloaded on demand when allowed
namespace fs = std::filesystem;
fs::path P(const std::string& x) { return fs::path(reinterpret_cast<const char8_t*>(x.c_str())); }
struct TerrainCache {
    std::mutex mu;
    std::shared_ptr<const dect2::prop::Terrain> t;
    std::set<std::string> tried;      // tiles loaded, missing or failed (asked once per run)
    std::atomic<bool> busy{false};
    std::thread th;
    std::string msg;
};
TerrainCache& T() { static TerrainCache* c = new TerrainCache(); return *c; }   // never destroyed: the thread may run at exit
void wantTerrain(double lat, double lon) {
    TerrainCache& c = T();
    if (c.busy.load()) return;
    std::vector<std::pair<int, int>> need;
    {
        std::lock_guard<std::mutex> l(c.mu);
        for (const auto& t : dect2::prop::demTilesFor(lat, lon, 20, NAN, NAN, 4)) {
            const std::string n = dect2::prop::demTileName(t.first, t.second) + (netAllowed() ? "+net" : "");
            if (c.tried.insert(n).second) need.push_back(t);
        }
    }
    if (need.empty()) return;
    if (c.th.joinable()) c.th.join();
    c.busy = true;
    const bool net = netAllowed();
    c.th = std::thread([need, net] {
        TerrainCache& c = T();
        const std::string dir = plat::dataDir() + "/terrain";
        std::error_code ec;
        fs::create_directories(P(dir), ec);
        dect2::prop::Terrain add;
        int got = 0, dl = 0;
        for (const auto& t : need) {
            const std::string name = dect2::prop::demTileName(t.first, t.second);
            std::string f = dir + "/" + name + ".hgt.gz";
            if (!(fs::exists(P(f), ec) && fs::file_size(P(f), ec) > 1000)) {
                const std::string plain = dir + "/" + name + ".hgt";
                if (fs::exists(P(plain), ec)) f = plain;
                else if (net) {
                    { std::lock_guard<std::mutex> l(c.mu); c.msg = "downloading terrain " + name + "..."; }
                    if (!plat::fetchUrl(dect2::prop::demTileUrl(t.first, t.second), f, "OnAir")) { fs::remove(P(f), ec); continue; }
                    dl++;
                } else continue;
            }
            std::string err;
            if (dect2::prop::loadElevationFile(f, add, err)) got++;
        }
        std::lock_guard<std::mutex> l(c.mu);
        if (got) {
            auto merged = std::make_shared<dect2::prop::Terrain>(c.t ? *c.t : dect2::prop::Terrain());
            for (auto& tile : add.tiles) merged->tiles.push_back(std::move(tile));
            c.t = merged;
        }
        char b[160];
        std::snprintf(b, sizeof b, "terrain: %d tile(s) in use%s", c.t ? (int)c.t->tiles.size() : 0, dl ? " (downloaded now)" : "");
        c.msg = (c.t && !c.t->empty()) ? b : (net ? "terrain: no tile here (open sea, or the download failed)" : "terrain: no tile on disk (turn on the download, or add SRTM files in the propagation tool)");
        c.busy = false;
    });
}

// ---- wind forecast
dect2::SondeWindFetcher& fetcher() {
    static dect2::SondeWindFetcher* f = [] { auto* x = new dect2::SondeWindFetcher(); x->cacheDir = plat::dataDir() + "/sonde-wind"; return x; }();
    return *f;
}

struct Entry {
    dect2::SondeLandTracker tr;
    double lastUnix = -1;
    double predAt = -1e9;
    size_t predFixes = 0;
    dect2::SondePrediction pred;
    std::vector<std::pair<double, double>> fcPts;   // the forecast points asked for this flight (kept so the cache key stays)
    double fcT = 0;
    dect2::sondepred::ForecastWinds fc, tawh;
    bool fcOk = false;
};
std::map<std::string, Entry>& E() { static std::map<std::string, Entry>* m = new std::map<std::string, Entry>(); return *m; }

double distKm(double a, double b, double c, double d) {
    const double k = 3.14159265358979323846 / 180;
    return std::hypot((d - b) * k * 6371.0 * std::cos(a * k), (c - a) * k * 6371.0);
}
}  // namespace

bool sondePredict40On() { return O().on; }

const dect2::SondePrediction* sondePredict40(const dect2::SondeInfo& s, bool selected, const dect2::tawhiri::Result* tw) {
    const Opts& o = O();
    if (!s.hasPos || s.serial.empty()) return nullptr;
    auto& m = E();
    if (m.size() > 40 && !m.count(s.serial)) m.erase(m.begin());
    Entry& e = m[s.serial];
    const double tNow = s.hasTime ? s.unixTime : (double)time(nullptr);
    if (e.tr.size() == 0 && s.trackIncluded && !s.track.empty() && (s.track.front().unixT != 0) == s.hasTime) e.tr.seed(s.track);
    if (tNow > e.lastUnix) { e.tr.addFix(tNow, s.lat, s.lon, s.altM, s.hasTemp ? s.tempC : -999); e.lastUnix = tNow; }
    const double now = ImGui::GetTime();
    const bool fresh = e.tr.size() != e.predFixes;   // a new frame since the last prediction
    if (!fresh || now - e.predAt < (selected ? 1.0 : 3.0)) return e.pred.valid ? &e.pred : nullptr;
    // the environment
    dect2::SondePredictEnv env;
    std::shared_ptr<const dect2::prop::Terrain> terr;
    if (o.terrain) {
        const double la = e.pred.valid ? e.pred.lat : s.lat, lo = e.pred.valid ? e.pred.lon : s.lon;
        wantTerrain(la, lo);
        { std::lock_guard<std::mutex> l(T().mu); terr = T().t; }
        if (terr && !terr->empty()) env.ground = [terr](double la2, double lo2, double& h) { return terr->heightAt(la2, lo2, h); };
    }
    if (tw && tw->ok) {   // the SondeHub prediction's path as winds (kept: later calls may come without it)
        std::vector<dect2::sondepred::PathPoint> p;
        for (const auto& q : tw->path) p.push_back({q.lat, q.lon, q.altM, q.unixT});
        e.tawh = dect2::sondepred::windsFromPath(p);
    }
    if (o.source == 0 && netAllowed()) {
        const double la = e.pred.valid ? e.pred.lat : s.lat, lo = e.pred.valid ? e.pred.lon : s.lon;
        if (e.fcPts.empty() || distKm(e.fcPts.back().first, e.fcPts.back().second, la, lo) > 40 || std::fabs(tNow - e.fcT) > 3 * 3600) {
            e.fcPts = dect2::sondepred::forecastPoints(s.lat, s.lon, la, lo); e.fcT = tNow; e.fcOk = false;
        }
        if (!e.fcOk) { fetcher().request(e.fcPts, e.fcT, now); e.fcOk = fetcher().get(e.fcPts, e.fcT, e.fc); }
        if (e.fcOk) env.forecast = &e.fc;
    } else if (o.source == 1 && !e.tawh.empty()) env.forecast = &e.tawh;
    dect2::SondePredictOpts po;
    po.useAscent = o.ascent; po.useFit = o.fit; po.useKalman = o.kalman; po.useTerrain = o.terrain;
    po.approachWindowS = o.approachS; po.windChangeMsPerH = o.windChange; po.driftCorrLenM = o.corrLen;
    po.members = selected ? o.members : 0;
    // RS41, RS92, M10 and MRZ (ECEF) send heights above the WGS 84 ellipsoid, the terrain is above the geoid; DFM and iMet send mean sea level
    po.altEllipsoid = s.type == "RS41" || s.type == "RS92" || s.type == "M10" || (s.type == "MRZ" && s.subtype.find("lat/lon") == std::string::npos);
    e.pred = e.tr.predict(po, env);
    e.predAt = now; e.predFixes = e.tr.size();
    return e.pred.valid ? &e.pred : nullptr;
}

void sondePredict40Draw(ImDrawList* dl, const dect2::SondePrediction& p) {
    if (!p.valid) return;
    const double k = 3.14159265358979323846 / 180, R = 6371000.0;
    auto at = [&](double de, double dn) { return adsbmap::project(p.lat + dn / R / k, p.lon + de / (R * std::cos(p.lat * k)) / k); };
    const ImU32 cDots = IM_COL32(255, 120, 90, 70), c50 = IM_COL32(255, 210, 90, 230), c95 = IM_COL32(255, 150, 70, 200), cEl = IM_COL32(120, 200, 255, 220);
    if (O().drawMembers) for (const auto& m : p.members) dl->AddCircleFilled(adsbmap::project(m.first, m.second), 1.5f * gUi, cDots, 6);
    if (!p.path.empty()) {   // the nominal ground track: a thin line
        ImVec2 prev = adsbmap::project(p.path.front().first, p.path.front().second);
        for (size_t i = 1; i < p.path.size(); i++) { const ImVec2 q = adsbmap::project(p.path[i].first, p.path[i].second); dl->AddLine(prev, q, IM_COL32(255, 120, 90, 120), 1.f * gUi); prev = q; }
    }
    if (p.hasSpread) {
        ImVec2 pts[73];
        auto circle = [&](double r, ImU32 col, float w) {
            for (int i = 0; i <= 72; i++) { const double a = i * 5 * k; pts[i] = at(r * std::sin(a), r * std::cos(a)); }
            dl->AddPolyline(pts, 73, col, 0, w * gUi);
        };
        circle(p.r50M, c50, 1.6f); circle(p.r95M, c95, 1.2f);
        const double ang = p.ellAngleDeg * k, s95 = 2.4477;   // the 95 % ellipse of a 2-D normal
        for (int i = 0; i <= 72; i++) {
            const double a = i * 5 * k, al = s95 * p.ellMajorM * std::cos(a), cr = s95 * p.ellMinorM * std::sin(a);
            pts[i] = at(al * std::sin(ang) + cr * std::cos(ang), al * std::cos(ang) - cr * std::sin(ang));
        }
        dl->AddPolyline(pts, 73, cEl, 0, 1.4f * gUi);
    }
    const ImVec2 e = at(0, 0);
    const float r = 6 * gUi;
    dl->AddLine(ImVec2(e.x - r, e.y - r), ImVec2(e.x + r, e.y + r), IM_COL32(255, 80, 70, 255), 2.f * gUi);
    dl->AddLine(ImVec2(e.x - r, e.y + r), ImVec2(e.x + r, e.y - r), IM_COL32(255, 80, 70, 255), 2.f * gUi);
    char b[128];
    const int s = (int)std::max(0.0, p.secondsLeft);
    if (p.hasSpread) std::snprintf(b, sizeof b, "lands in %d:%02d, 50 %% within %.0f m, 95 %% within %.0f m", s / 60, s % 60, p.r50M, p.r95M);
    else std::snprintf(b, sizeof b, "lands in %d:%02d", s / 60, s % 60);
    dl->AddText(ImVec2(e.x + 9 * gUi, e.y + 4 * gUi), IM_COL32(255, 215, 120, 240), b);
}

void sondePredict40Network() {
    Opts& o = O();
    if (ImGui::Checkbox(tr("Download terrain and wind forecasts for the landing prediction##sondepnet"), &o.net)) save();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Off by default. When on, the landing prediction may fetch:\n- elevation tiles (1 x 1 degree SRTM, s3.amazonaws.com/elevation-tiles-prod, 10 to 25 MB each, kept in the terrain folder),\n- pressure-level winds of the GFS model for up to three points near the flight (api.open-meteo.com; it receives those points, rounded to 0.25 degrees, and the hour), at most every 2 minutes, kept in the sonde-wind folder.\nNothing else is sent. Offline mode (Settings) turns all of it off."));
    if (o.net && plat::prefs().getB("offlineMode", false)) ImGui::TextDisabled("%s", tr("Offline mode is on: nothing is downloaded."));
}

void sondePredict40Advanced() {
    Opts& o = O();
    bool ch = false;
    ch |= ImGui::Checkbox(tr("landing prediction 4.0##sp40"), &o.on);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The wind the sonde measured on the way up, the fall rate fitted to the descent, a Kalman filter on the drift near the ground,\nthe terrain and (opt-in) a wind forecast, with an uncertainty ellipse. Off: the 3.9 estimate (constant drift, sea-level ground)."));
    if (!o.on) { if (ch) save(); return; }
    ImGui::Indent();
    ch |= ImGui::Checkbox(tr("ascent winds##sp40a"), &o.ascent); ImGui::SameLine();
    ch |= ImGui::Checkbox(tr("fitted fall rate##sp40f"), &o.fit); ImGui::SameLine();
    ch |= ImGui::Checkbox(tr("drift filter##sp40k"), &o.kalman);
    ch |= ImGui::Checkbox(tr("terrain##sp40t"), &o.terrain); ImGui::SameLine();
    ch |= ImGui::Checkbox(tr("ensemble dots##sp40d"), &o.drawMembers);
    const char* src[] = {"Open-Meteo GFS", "SondeHub prediction (Tawhiri)", "none"};
    ImGui::SetNextItemWidth(std::min(190 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::Combo(tr("wind forecast##sp40s"), &o.source, src, 3)) ch = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("For the layers the ascent did not measure (above the highest point heard, or all of them when the ascent was missed).\nOpen-Meteo needs the download switch in \"me / prediction\"; the SondeHub prediction needs the online flight prediction there."));
    ImGui::SetNextItemWidth(std::min(120 * gUi, ImGui::GetContentRegionAvail().x));
    ch |= ImGui::SliderFloat(tr("final approach window##sp40w"), &o.approachS, 30, 120, "%.0f s");
    ImGui::SetNextItemWidth(std::min(120 * gUi, ImGui::GetContentRegionAvail().x));
    ch |= ImGui::SliderFloat(tr("drift reach below##sp40c"), &o.corrLen, 100, 3000, "%.0f m");
    ImGui::SetNextItemWidth(std::min(120 * gUi, ImGui::GetContentRegionAvail().x));
    ch |= ImGui::SliderFloat(tr("wind change since ascent##sp40g"), &o.windChange, 0, 5, "%.1f m/s/h");
    ImGui::SetNextItemWidth(std::min(120 * gUi, ImGui::GetContentRegionAvail().x));
    ch |= ImGui::SliderInt(tr("ensemble members##sp40m"), &o.members, 0, 1000);
    {
        std::lock_guard<std::mutex> l(T().mu);
        if (!T().msg.empty()) ImGui::TextDisabled("%s", T().msg.c_str());
    }
    const std::string st = fetcher().status();
    if (!st.empty()) ImGui::TextDisabled("%s", st.c_str());
    ImGui::Unindent();
    if (ch) save();
}
