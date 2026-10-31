#include "wx_overlay.h"
#include "../app.h"
#include "../adsb/adsb_map.h"
#include "gfx/gfx.h"
#include "platform/platform.h"
#include "dect2/geo/wx_overlay.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace wxoverlay {
namespace {

using namespace dect2;

struct State {
    bool loaded = false;
    bool on = true;
    float opacity = 0.75f;
    // advanced
    float swathKm = 2800.f, clockShift = 0.f, diskPct = 100.f;
    bool mirror = false;
    int geosSat = 0;                       // 0 GOES-East, 1 GOES-West, 2 custom
    float geosLon = -75.2f;
    // the picture
    enum { None, Swath, Geos } kind = None;
    uint64_t id = 0, version = 0;
    std::string label, status;
    std::vector<uint32_t> src; int srcW = 0, srcH = 0;
    SwathSpec spec; Sgp4 sat; std::string satName;
    bool dirty = false;
    double lastFeed = -1e9;
    GeoRaster ras;
    gfx::Image* tex = nullptr; int texW = 0, texH = 0;
    std::string msg;
};
State S;

double nowSec() { return (double)time(nullptr); }

void load() {
    if (S.loaded) return;
    S.loaded = true;
    S.on = plat::prefs().getB("wxOverlayOn", true);
    S.opacity = (float)plat::prefs().getD("wxOverlayOpacity", 0.75);
}

// the Meteor satellite the picture most likely came from: the listed Meteor with the highest elevation at the start (or the nearest)
bool pickMeteor(double tUnix, Sgp4& out, std::string& name) {
    std::vector<MapSatRef> v; passesMapSatellites(v);
    double la, lo; receiverLocation(la, lo);
    Observer o; o.latDeg = la; o.lonDeg = lo;
    double best = -1e9; bool any = false;
    for (const MapSatRef& r : v) {
        std::string n = r.name; for (char& c : n) c = (char)toupper((unsigned char)c);
        if (n.find("METEOR") == std::string::npos || !r.sgp) continue;
        Look l; if (!lookAt(*r.sgp, o, tUnix, l)) continue;
        if (l.elDeg > best) { best = l.elDeg; out = *r.sgp; name = r.name; any = true; }
    }
    return any;
}

void rebuild() {
    S.dirty = false;
    GeoRaster g; bool ok = false;
    if (S.kind == State::Swath) {
        S.spec.swathKm = S.swathKm; S.spec.timeOffsetSec = S.clockShift; S.spec.xToLeft = !S.mirror;
        ok = projectSwath(S.sat, S.spec, S.src.data(), S.srcH, 1536, MapProj::Mercator, g);
    } else if (S.kind == State::Geos) {
        const double lon = S.geosSat == 0 ? -75.2 : S.geosSat == 1 ? -137.2 : S.geosLon;
        ok = projectGeosDisk(S.src.data(), S.srcW, S.srcH, lon, S.srcW * S.diskPct / 100.0, 1536, MapProj::Mercator, g);
    }
    if (!ok || !gGfx) { S.ras = GeoRaster(); return; }
    if (!S.tex || S.texW != g.w || S.texH != g.h) { delete S.tex; S.tex = gGfx->createImage(g.w, g.h, 0u); S.texW = g.w; S.texH = g.h; }
    if (S.tex) S.tex->update(0, 0, g.w, g.h, g.px.data());
    S.ras = std::move(g);
    S.ras.px.clear(); S.ras.px.shrink_to_fit();   // the pixels live in the texture; the box is what the map needs
}

std::string stamp() {
    char ts[32]; const time_t tt = time(nullptr); struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &tt);
#else
    localtime_r(&tt, &lt);
#endif
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &lt);
    return ts;
}

void exportFiles() {
    GeoRaster g; bool ok = false;
    if (S.kind == State::Swath) { SwathSpec sp = S.spec; ok = projectSwath(S.sat, sp, S.src.data(), S.srcH, 3000, MapProj::PlateCarree, g); }
    else if (S.kind == State::Geos) {
        const double lon = S.geosSat == 0 ? -75.2 : S.geosSat == 1 ? -137.2 : S.geosLon;
        ok = projectGeosDisk(S.src.data(), S.srcW, S.srcH, lon, S.srcW * S.diskPct / 100.0, 3000, MapProj::PlateCarree, g);
    }
    if (!ok) { S.msg = "nothing to export yet"; return; }
    const std::string dir = plat::dataDir() + "/weather-overlay";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(dir.c_str())), ec);
    const std::string base = dir + "/" + stamp() + (S.kind == State::Swath ? "-meteor" : "-goes");
    std::vector<uint8_t> tif;
    bool good = geoTiffRgba(g, tif);
    if (good) { std::ofstream o(std::filesystem::path(reinterpret_cast<const char8_t*>((base + ".tif").c_str())), std::ios::binary); o.write((const char*)tif.data(), (std::streamsize)tif.size()); good = (bool)o; }
    const std::vector<uint8_t> rgba = rasterBytes(g);
    const bool png = gfx::writePng((base + ".png").c_str(), rgba.data(), g.w, g.h);
    const std::string kml = kmlGroundOverlay(S.label, std::filesystem::path(base + ".png").filename().string(), g, S.opacity);
    { std::ofstream o(std::filesystem::path(reinterpret_cast<const char8_t*>((base + ".kml").c_str()))); o << kml; }
    S.msg = good && png ? "saved " + base + ".tif / .kml / .png" : "could not write the files in " + dir;
}

}

bool hasPicture() { return S.kind != State::None && S.ras.w > 0; }

bool wanted(const App& a, double minGapSec) {
    load();
    return a.mapOpen && S.on && nowSec() - S.lastFeed >= minGapSec;
}

void setSwath(uint64_t imageId, const std::string& label, int width, int lines, const std::vector<uint32_t>& px, double lps) {
    load();
    S.lastFeed = nowSec();
    if (lines < 16 || (int)px.size() < width * lines) return;
    if (S.kind != State::Swath || S.id != imageId) {
        S.kind = State::Swath; S.id = imageId; S.label = label;
        S.spec = SwathSpec(); S.spec.width = width; S.spec.linesPerSec = lps;
        S.spec.startUnix = nowSec() - lines / lps;
        if (!pickMeteor(S.spec.startUnix + lines / lps / 2, S.sat, S.satName)) { S.kind = State::None; S.status = "no Meteor-M orbit: update the TLEs in View > Passes"; S.ras = GeoRaster(); return; }
        S.status = "orbit of " + S.satName;
    }
    S.src = px; S.srcW = width; S.srcH = lines; S.dirty = true;
    rebuild();
}

void setGeos(uint64_t productId, uint64_t version, const std::string& label, int w, int h, const std::vector<uint32_t>& px) {
    load();
    S.lastFeed = nowSec();
    if (w < 8 || h < 8 || (int)px.size() < w * h) return;
    if (S.kind == State::Geos && S.id == productId && S.version == version) return;
    S.kind = State::Geos; S.id = productId; S.version = version; S.label = label; S.status = "full disk";
    S.src = px; S.srcW = w; S.srcH = h;
    rebuild();
}

void draw(ImDrawList* dl) {
    load();
    if (!S.on || !hasPicture() || !S.tex) return;
    const ImVec2 a = adsbmap::project(S.ras.north, S.ras.west), b = adsbmap::project(S.ras.south, S.ras.west);
    const float perDeg = adsbmap::project(0, 1).x - adsbmap::project(0, 0).x;   // the short way round: a degree is far smaller than half the globe
    const float w = (float)(S.ras.east - S.ras.west) * perDeg;
    if (perDeg <= 0) return;
    const ImU32 col = IM_COL32(255, 255, 255, (int)(std::max(0.f, std::min(1.f, S.opacity)) * 255));
    dl->AddImage(S.tex->texture(), a, ImVec2(a.x + w, b.y), ImVec2(0, 0), ImVec2(1, 1), col);
}

void controls(App& a, float x, float y, float width) {
    load();
    if (S.kind == State::None && S.status.empty()) return;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::BeginGroup();
    ImGui::PushItemWidth(std::max(80.f, width * 0.5f));
    if (ImGui::Checkbox(tr("Weather picture"), &S.on)) { plat::prefs().setB("wxOverlayOn", S.on); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The Meteor-M or GOES picture being received, placed on the map from the satellite's orbit"));
    if (S.on && hasPicture()) {
        ImGui::SameLine();
        if (ImGui::SliderFloat("##wxop", &S.opacity, 0.f, 1.f, tr("opacity %.2f"))) { plat::prefs().setD("wxOverlayOpacity", S.opacity); }
        if (ImGui::SmallButton(tr("Export GeoTIFF + KML"))) exportFiles();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Writes the picture as a GeoTIFF (EPSG:4326), a KML ground overlay and the PNG into <data folder>/weather-overlay"));
        if (ImGui::TreeNode(tr("Advanced##wxadv"))) {
            bool ch = false;
            if (S.kind == State::Swath) {
                ch |= ImGui::SliderFloat(tr("swath km"), &S.swathKm, 1000.f, 3500.f, "%.0f");
                ch |= ImGui::SliderFloat(tr("clock shift s"), &S.clockShift, -90.f, 90.f, "%.1f");
                ch |= ImGui::Checkbox(tr("mirror left/right"), &S.mirror);
                ImGui::TextDisabled("%s", S.status.c_str());
            } else {
                const char* const sats[] = {"GOES-East 75.2 W", "GOES-West 137.2 W", "other longitude"};
                ch |= ImGui::Combo("##wxgs", &S.geosSat, sats, 3);
                if (S.geosSat == 2) ch |= ImGui::SliderFloat(tr("longitude E"), &S.geosLon, -180.f, 180.f, "%.1f");
                ch |= ImGui::SliderFloat(tr("disk width %"), &S.diskPct, 60.f, 120.f, "%.0f");
            }
            if (ch && S.kind != State::None) rebuild();
            ImGui::TreePop();
        }
    } else if (!S.status.empty()) ImGui::TextDisabled("%s", S.status.c_str());
    if (!S.msg.empty()) ImGui::TextDisabled("%s", S.msg.c_str());
    ImGui::PopItemWidth();
    ImGui::EndGroup();
}

}
