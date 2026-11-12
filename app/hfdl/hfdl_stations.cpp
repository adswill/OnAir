// HFDL nearby ground stations (hfdl_stations.h): the opt-in background fetch of the hfdl.observer list with its cache, the position, the Nearby
// view (nearest first: distance, bearing, the active frequencies as tune buttons, when each list was updated, live or squitter) and the tune.
#include "hfdl_stations.h"
#include "app.h"
#include "ui/theme.h"
#include "platform/platform.h"
#include "dect2/geo/maidenhead.h"
#include "dect2/hfdl/hfdl_hop.h"
#include "dect2/hfdl/hfdl_learn.h"
#include "dect2/hfdl/hfdl_proto.h"
#include "dect2/hfdl/hfdl_tel.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#ifndef ONAIR_VERSION
#define ONAIR_VERSION "dev"
#endif

namespace hfdlst {

namespace {

constexpr const char* kDefaultUrl = "https://hfdl.observer/active.json";
constexpr double kMinGapSec = 120;          // never faster than this (the site's own client asks every 61 s; OnAir is a viewer, so slower)
constexpr double kFailGapSec = 600;         // after a failure

struct Feed {
    std::mutex mu;
    std::vector<dect2::HfdlLiveStation> st;
    double fetchedSec = 0;                  // UNIX seconds of the last good list
    double triedSec = 0;
    std::string err;
    std::string cacheText;                  // the last good answer, written to the cache by the UI thread
    bool cacheDirty = false;
    std::atomic<bool> busy{false};
};
Feed& feed() { static Feed* f = new Feed(); return *f; }   // never destroyed: a fetch may still run at exit

bool gCacheLoaded = false;
char gPosBuf[64] = "";
bool gPosLoaded = false;
double gWantedT = -1e9;                     // when the Nearby view or the automatic band choice last asked for the list

std::string cachePath() { return plat::dataDir() + "/hfdl-live.json"; }

double nowUnix() { return (double)std::time(nullptr); }

void loadCache() {
    if (gCacheLoaded) return;
    gCacheLoaded = true;
    std::ifstream f(cachePath(), std::ios::binary);
    if (!f) return;
    std::string line;
    std::getline(f, line);                  // first line: the UNIX time it was fetched
    const double t = atof(line.c_str());
    std::ostringstream ss; ss << f.rdbuf();
    std::vector<dect2::HfdlLiveStation> st;
    if (t > 0 && dect2::hfdlParseLiveList(ss.str(), t, st)) {
        Feed& F = feed();
        std::lock_guard<std::mutex> lk(F.mu);
        F.st = std::move(st); F.fetchedSec = t;
    }
}

void saveCacheIfDirty() {
    Feed& F = feed();
    std::string text; double t = 0;
    {
        std::lock_guard<std::mutex> lk(F.mu);
        if (!F.cacheDirty) return;
        F.cacheDirty = false;
        text.swap(F.cacheText); t = F.fetchedSec;
    }
    std::ofstream f(cachePath(), std::ios::binary | std::ios::trunc);
    f << (long long)t << "\n" << text;
}

void startFetch() {
    Feed& F = feed();
    bool expected = false;
    if (!F.busy.compare_exchange_strong(expected, true)) return;
    std::string url = plat::prefs().getS("hfdlLiveUrl", kDefaultUrl);
    if (url.empty()) url = kDefaultUrl;
    { std::lock_guard<std::mutex> lk(F.mu); F.triedSec = nowUnix(); }
    const std::string ua = std::string("OnAir/") + ONAIR_VERSION + " (HFDL ground-station list; +https://github.com/adswill/OnAir)";
    std::thread([url, ua] {
        Feed& G = feed();
        const plat::HttpResult r = plat::httpRequest("GET", url, "", ua, 15);
        std::vector<dect2::HfdlLiveStation> st;
        std::string err;
        const double t = nowUnix();
        bool ok = false;
        if (r.status == 200 && dect2::hfdlParseLiveList(r.body, t, st, &err)) ok = true;
        else if (r.status != 200) err = r.status ? "HTTP " + std::to_string(r.status) : (r.net == 1 ? "host not found" : r.net == 2 ? "no connection" : r.net == 3 ? "timed out" : r.detail.empty() ? "no answer" : r.detail);
        {
            std::lock_guard<std::mutex> lk(G.mu);
            if (ok) { G.st = std::move(st); G.fetchedSec = t; G.err.clear(); G.cacheText = r.body; G.cacheDirty = true; }
            else G.err = err;
        }
        G.busy.store(false);
    }).detach();
}

double refreshGapSec() { return std::max(kMinGapSec, 60.0 * (double)plat::prefs().getI("hfdlLiveMin", 5)); }

bool liveMode(const App& a) { return a.rx.standard == 36; }

void loadPos() {
    if (gPosLoaded) return;
    gPosLoaded = true;
    snprintf(gPosBuf, sizeof gPosBuf, "%s", plat::prefs().getS("hfdlMyPos", "").c_str());
}

bool parsePos(const std::string& s, double& lat, double& lon) {
    if (s.empty()) return false;
    if (dect2::fromMaidenhead(s, lat, lon)) return true;
    return dect2::parseCoordinates(s, lat, lon);
}

std::string agoText(double now, double then) {
    if (then <= 0) return "-";
    const double s = std::max(0.0, now - then);
    char b[32];
    if (s < 90) snprintf(b, sizeof b, "%.0f s ago", s); else if (s < 5400) snprintf(b, sizeof b, "%.0f min ago", s / 60); else snprintf(b, sizeof b, "%.1f h ago", s / 3600);
    return b;
}

const char* srcText(int s) {
    if (s == (dect2::kHfdlSrcLive | dect2::kHfdlSrcSquitter)) return "live + squitter";
    if (s == dect2::kHfdlSrcLive) return "live (hfdl.observer)";
    if (s == dect2::kHfdlSrcSquitter) return "squitter (heard here)";
    return "-";
}

}  // namespace

bool liveAllowed() { return plat::prefs().getB("hfdlLiveOn", false) && !plat::prefs().getB("offlineMode", false); }

void tick(App& a, bool wanted) {
    (void)a;
    loadCache();
    saveCacheIfDirty();
    const double t = ImGui::GetTime();
    if (wanted) gWantedT = t;
    if (!liveAllowed() || t - gWantedT > 30) return;   // only while someone looks at it or the band choice uses it
    Feed& F = feed();
    if (F.busy.load()) return;
    double fetched, tried; bool failed;
    { std::lock_guard<std::mutex> lk(F.mu); fetched = F.fetchedSec; tried = F.triedSec; failed = !F.err.empty(); }
    const double now = nowUnix();
    if (now - tried < (failed ? kFailGapSec : kMinGapSec)) return;
    if (now - fetched < refreshGapSec()) return;
    startFetch();
}

bool position(double& lat, double& lon, std::string* from) {
    loadPos();
    if (parsePos(gPosBuf, lat, lon)) { if (from) *from = "typed here"; return true; }
    if (receiverLocationSet()) { receiverLocation(lat, lon); if (from) *from = "station location"; return true; }
    return false;
}

std::vector<dect2::HfdlNearby> nearby(const App& a) {
    const double now = nowUnix();
    std::vector<dect2::HfdlLiveStation> live;
    if (plat::prefs().getB("hfdlLiveOn", false)) {   // the saved list is shown in Offline mode too (old data, no network); the merge drops it after an hour
        Feed& F = feed();
        std::lock_guard<std::mutex> lk(F.mu);
        live = F.st;
    }
    std::vector<dect2::HfdlHeardStation> heard;
    if (liveMode(a)) {
        const dect2::HfdlTelemetry& t = a.rx.hfdl;
        for (const auto& g : t.stations) {
            if (!g.heardDirect || g.activeKhz.empty() || g.lastHeardSec <= 0) continue;
            dect2::HfdlHeardStation h;
            h.id = g.id;
            h.heardSec = now - std::max(0.0, t.nowSec - g.lastHeardSec);   // receiver seconds to UNIX time
            h.activeKhz = dect2::HfdlFreqLearner::global().announced(g.id, g.activeKhz, now);   // the learner knows when a mask cannot be trusted
            heard.push_back(std::move(h));
        }
    }
    double lat = 0, lon = 0;
    const bool have = position(lat, lon);
    return dect2::hfdlNearby(live, heard, have, lat, lon, now);
}

std::vector<int> hintKhz(const App& a) { return dect2::hfdlHintKhz(nearby(a), plat::prefs().getD("hfdlHintKm", 6000.0), 3); }

void tuneKhz(App& a, int khz) {
    const int band = dect2::hfdlBandOfKhz(khz);
    const double rate = liveMode(a) && a.rx.hfdl.inputRate > 0 ? a.rx.hfdl.inputRate : 0;
    const double mhz = band >= 0 ? dect2::hfdlTunePointMhz(band, {khz}, rate) : (khz + dect2::kHfdlCarrierHz / 1e3) / 1e3;
    tuneFreq(a, mhz);
    if (!a.engine.running()) startReceiver(a);
}

void tunerLine(App& a) {
    const auto n = nearby(a);
    for (const auto& s : n) {
        if (s.freqs.empty()) continue;
        ImGui::AlignTextToFramePadding();
        if (s.km >= 0) ImGui::TextDisabled("Nearest active: %s, %.0f km", s.name.c_str(), s.km);
        else ImGui::TextDisabled("Active: %s", s.name.c_str());
        for (const auto& f : s.freqs) {
            ImGui::SameLine();
            char b[32]; snprintf(b, sizeof b, "%d##hfnt%d", f.khz, f.khz);
            if (ImGui::SmallButton(b)) tuneKhz(a, f.khz);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tune to %d kHz (%s)", f.khz, srcText(f.sources));
        }
        return;
    }
}

void panel(App& a) {
    loadPos();
    tick(a, true);
    const double now = nowUnix();
    // the switch (the same one as in Settings > Privacy)
    bool on = plat::prefs().getB("hfdlLiveOn", false);
    const bool offline = plat::prefs().getB("offlineMode", false);
    ImGui::BeginDisabled(offline);
    if (ImGui::Checkbox("Live list from hfdl.observer##hfdllive", &on)) { plat::prefs().setB("hfdlLiveOn", on); plat::prefs().flush(); }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Off (default): only the squitters this receiver hears. On: OnAir asks hfdl.observer for its list of active ground-station\n"
                          "frequencies (one HTTPS request at most every few minutes, only while this view is open or a band choice is automatic).\n"
                          "The site sees your IP address and the app version, nothing else. Also in Settings > Privacy. Off in Offline mode.");
    ImGui::SameLine(0, 14 * gUi);
    {
        Feed& F = feed();
        double fetched; std::string err; const bool busy = F.busy.load();
        { std::lock_guard<std::mutex> lk(F.mu); fetched = F.fetchedSec; err = F.err; }
        if (offline) ImGui::TextDisabled("Offline mode is on: squitters only%s", fetched > 0 ? " (and the saved list)" : "");
        else if (!on) ImGui::TextDisabled("Squitters only");
        else if (busy) ImGui::TextDisabled("asking hfdl.observer ...");
        else if (!err.empty()) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "list: %s (last good %s)", err.c_str(), agoText(now, fetched).c_str());
        else if (fetched > 0) ImGui::TextDisabled("list fetched %s", agoText(now, fetched).c_str());
    }
    // the position
    double lat = 0, lon = 0; std::string from;
    const bool have = position(lat, lon, &from);
    ImGui::AlignTextToFramePadding();
    if (have) ImGui::TextDisabled("From %.2f, %.2f (%s)", lat, lon, from.c_str()); else ImGui::TextDisabled("No position:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170 * gUi);
    if (ImGui::InputTextWithHint("##hfdlpos", "locator or lat, lon", gPosBuf, sizeof gPosBuf)) {
        double la, lo;
        if (!gPosBuf[0] || parsePos(gPosBuf, la, lo)) plat::prefs().setS("hfdlMyPos", gPosBuf);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Where to measure the distances from: a locator (KL55) or coordinates (25.2, 55.3).\nEmpty: the station location (View > Map).");
    if (gPosBuf[0]) { double la, lo; if (!parsePos(gPosBuf, la, lo)) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "not a position"); } }

    const auto n = nearby(a);
    if (n.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", liveMode(a) ? "No ground station heard yet and no live list. Each station sends its active frequencies every 32 s on each of them; turn on the live list to see them before one is heard."
                                              : "Start the receiver on an HFDL band to hear the squitters, or turn on the live list.");
        ImGui::PopTextWrapPos();
        return;
    }
    if (!ImGui::BeginTable("##hfdlnear", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV,
                           ImVec2(0, ImGui::GetContentRegionAvail().y - 2.4f * ImGui::GetTextLineHeightWithSpacing())))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Ground station"); ImGui::TableSetupColumn("Distance"); ImGui::TableSetupColumn("Bearing");
    ImGui::TableSetupColumn("Active now (kHz, click to tune)", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Updated"); ImGui::TableSetupColumn("Source");
    ImGui::TableHeadersRow();
    for (const auto& s : n) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%d %s", s.id, s.name.c_str());
        ImGui::TableNextColumn(); if (s.km >= 0) ImGui::Text("%.0f km", s.km); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (s.km >= 0) ImGui::Text("%.0f deg", s.bearingDeg); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        bool first = true;
        for (const auto& f : s.freqs) {
            if (!first) ImGui::SameLine();
            first = false;
            const bool inSpan = liveMode(a) && std::fabs(f.khz + 1.44 - a.rx.hfdl.centerHz / 1e3) < 0.4 * a.rx.hfdl.inputRate / 1e3;
            if (inSpan) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
            char b[40]; snprintf(b, sizeof b, "%d##hfn%d_%d", f.khz, s.id, f.khz);
            if (ImGui::SmallButton(b)) tuneKhz(a, f.khz);
            if (inSpan) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: tune to %d kHz%s\nListed by: %s", s.name.c_str(), f.khz, inSpan ? " (already in the span being decoded)" : "", srcText(f.sources));
        }
        if (s.freqs.empty()) ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(agoText(now, s.updatedSec).c_str());
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", srcText(s.source));
    }
    ImGui::EndTable();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Live list: hfdl.observer (community HFDL receivers, from squitters and aircraft performance reports; see airframes.io). "
                        "Frequencies change through the day; a station listed may still be out of reach. Lists older than an hour are not shown.");
    ImGui::PopTextWrapPos();
}

}  // namespace hfdlst
