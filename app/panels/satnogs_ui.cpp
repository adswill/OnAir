// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SatNOGS transmitters (View > SatNOGS transmitters): the public SatNOGS DB list of satellite transmitters (read only, no login, nothing is
// sent), cached on disk for a day, with filters (alive, band, mode, name), a Tune button per row that sets the radio to the downlink (and starts the
// OnAir mode that fits the SatNOGS mode, where there is one) and, when the pass planner has TLEs for the satellite, its next pass for the
// receiver's position.
#include "../app.h"
#include "dect2/common/diag.h"
#include "../ui/typography.h"
#include "dect2/geo/passes.h"
#include "dect2/net/satnogs.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
fs::path fsPath(const std::string& p) { return fs::path(reinterpret_cast<const char8_t*>(p.c_str())); }

const char* const kTxUrl = "https://db.satnogs.org/api/transmitters/?format=json";
const char* const kSatUrl = "https://db.satnogs.org/api/satellites/?format=json";

std::string dir() { return plat::dataDir() + "/satnogs"; }
double nowSec() { return (double)std::time(nullptr); }
std::string readAll(const std::string& p) {
    std::ifstream in(fsPath(p), std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct NextPass { bool done = false; bool has = false; dect2::Pass p; double computedAt = 0, lat = 1e9, lon = 1e9; };

struct State {
    bool loaded = false;
    std::vector<dect2::SatnogsTx> all;
    std::vector<int> shown;                  // indexes into all
    dect2::SatnogsFilter f;
    char modeBuf[32] = "", textBuf[48] = "";
    float minMhz = 0, maxMhz = 0;
    bool dirty = true;
    double fetchedAt = 0;
    std::string status;
    std::map<int, dect2::Tle> tles;
    bool tlesLoaded = false;
    std::map<int, NextPass> next;
};
State S;

std::atomic<int> gDl{0};          // 0 idle, 1 working, 2 finished
std::atomic<bool> gDlOk{false};
std::mutex gMu;
std::string gMsg;

void loadCache() {
    std::map<int, std::string> names;
    dect2::parseSatnogsSatellites(readAll(dir() + "/satellites.json"), names);
    S.all.clear();
    dect2::parseSatnogsTransmitters(readAll(dir() + "/transmitters.json"), names, S.all);
    std::stable_sort(S.all.begin(), S.all.end(), [](const dect2::SatnogsTx& a, const dect2::SatnogsTx& b) { return a.satName < b.satName; });
    std::error_code ec;
    const auto t = fs::last_write_time(fsPath(dir() + "/transmitters.json"), ec);
    S.fetchedAt = ec || S.all.empty() ? 0 : (double)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - (double)std::chrono::duration_cast<std::chrono::seconds>(
        fs::file_time_type::clock::now() - t).count();
    S.dirty = true;
    S.next.clear();
}

void startDownload() {
    if (gDl.load() == 1) return;
    gDl = 1; gDlOk = false;
    S.status = tr("Downloading...");
    std::thread([] {
        const std::string d = dir();
        std::error_code ec;
        fs::create_directories(fsPath(d), ec);
        const std::string ua = "OnAir/" ONAIR_VERSION " (SatNOGS DB read-only list)";
        bool ok = false;
        std::string msg;
        // each list goes to a .part file and replaces the cached one only when it parses
        if (!plat::fetchUrl(kSatUrl, d + "/satellites.part", ua)) { DIAG(184534, "The SatNOGS satellite list could not be downloaded, so the satellite data stays as it was."); msg = "could not download the satellite list"; }
        else if (!plat::fetchUrl(kTxUrl, d + "/transmitters.part", ua)) { DIAG(774747, "The SatNOGS transmitter list could not be downloaded, so the satellite data stays as it was."); msg = "could not download the transmitter list"; }
        else {
            std::map<int, std::string> names;
            std::vector<dect2::SatnogsTx> tx;
            const int ns = dect2::parseSatnogsSatellites(readAll(d + "/satellites.part"), names);
            const int nt = dect2::parseSatnogsTransmitters(readAll(d + "/transmitters.part"), names, tx);
            if (ns == 0 || nt == 0) { DIAG(601452, "The SatNOGS answer held %d satellites and %d transmitters, so it was not used and the old lists stay.", ns, nt); msg = "the answer was not a SatNOGS list"; }
            else {
                fs::rename(fsPath(d + "/satellites.part"), fsPath(d + "/satellites.json"), ec);
                fs::rename(fsPath(d + "/transmitters.part"), fsPath(d + "/transmitters.json"), ec);
                ok = !ec;
                msg = ok ? std::to_string(nt) + " transmitters" : "could not save the lists";
            }
        }
        fs::remove(fsPath(d + "/satellites.part"), ec);
        fs::remove(fsPath(d + "/transmitters.part"), ec);
        { std::lock_guard<std::mutex> lk(gMu); gMsg = msg; }
        gDlOk = ok;
        gDl = 2;
    }).detach();
}

void pollDownload() {
    if (gDl.load() != 2) return;
    gDl = 0;
    std::string m;
    { std::lock_guard<std::mutex> lk(gMu); m = gMsg; }
    if (gDlOk.load()) { loadCache(); S.status = m; }
    else S.status = std::string(tr("Download failed")) + ": " + m;
}

void loadTles() {
    S.tlesLoaded = true;
    S.tles.clear();
    for (const char* f : {"weather.txt", "stations.txt", "amateur.txt", "keplerpass.txt", "pasted.txt"}) {
        std::error_code ec;
        const std::string p = plat::dataDir() + "/tle/" + f;
        if (!fs::exists(fsPath(p), ec)) continue;
        for (const dect2::Tle& t : dect2::parseTleText(readAll(p))) S.tles[t.norad] = t;
    }
}

const NextPass& nextPassFor(int norad, double now) {
    NextPass& n = S.next[norad];
    double lat, lon;
    receiverLocation(lat, lon);
    if (n.done && now - n.computedAt < 600 && n.lat == lat && n.lon == lon) return n;
    n = NextPass{};
    n.done = true; n.computedAt = now; n.lat = lat; n.lon = lon;
    auto it = S.tles.find(norad);
    if (it == S.tles.end()) return n;
    dect2::Sgp4 sg;
    if (!sg.init(it->second)) return n;
    dect2::Observer o; o.latDeg = lat; o.lonDeg = lon; o.altKm = receiverAltitudeM() / 1000;
    const auto ps = dect2::findPasses(sg, o, now, now + 24 * 3600, 5);
    if (!ps.empty()) { n.has = true; n.p = ps.front(); }
    return n;
}

std::string clock(double t) {
    char b[32];
    const std::time_t tt = (std::time_t)t;
    std::tm lt = *std::localtime(&tt);
    std::strftime(b, sizeof b, "%H:%M", &lt);
    return b;
}

void tuneTo(App& a, const dect2::SatnogsTx& t) {
    const int fam = dect2::satnogsFamily(t.mode, t.baud);
    if (fam && a.family != fam) selectMode(a, fam);
    a.freqMhz = t.downHz / 1e6;
    if (a.engine.running()) { a.tune.centerHz = a.freqMhz * 1e6; a.engine.retune(a.tune); }
}

void rebuild() {
    S.f.modeText = S.modeBuf; S.f.text = S.textBuf; S.f.minMhz = S.minMhz; S.f.maxMhz = S.maxMhz;
    S.shown.clear();
    for (size_t i = 0; i < S.all.size(); i++)
        if (S.all[i].downHz > 0 && dect2::satnogsMatches(S.all[i], S.f)) S.shown.push_back((int)i);
    S.dirty = false;
}

void drawWindow(App& a) {
    firstWindowSize(860 * gUi, 520 * gUi);
    if (!ImGui::Begin(trw("SatNOGS transmitters"), &a.satnogsOpen)) { ImGui::End(); return; }
    const double now = nowSec();
    if (ImGui::Button(tr("Refresh"))) startDownload();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Downloads the public SatNOGS DB lists (db.satnogs.org, read only, nothing is sent). Cached for 24 hours."));
    ImGui::SameLine();
    if (S.fetchedAt > 0) ImGui::TextDisabled("%s %d h", tr("list age"), (int)((now - S.fetchedAt) / 3600));
    else ImGui::TextDisabled("%s", tr("no list yet: press Refresh"));
    if (!S.status.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", S.status.c_str()); }
    bool ch = false;
    ch |= ImGui::Checkbox(tr("Alive only"), &S.f.aliveOnly);
    ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); ch |= ImGui::InputFloat(tr("from MHz"), &S.minMhz, 0, 0, "%.3f");
    ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); ch |= ImGui::InputFloat(tr("to MHz"), &S.maxMhz, 0, 0, "%.3f");
    ImGui::SetNextItemWidth(120 * gUi); ch |= ImGui::InputText(tr("Mode"), S.modeBuf, sizeof S.modeBuf);
    ImGui::SameLine(); ImGui::SetNextItemWidth(160 * gUi); ch |= ImGui::InputText(tr("Satellite"), S.textBuf, sizeof S.textBuf);
    if (ch) S.dirty = true;
    if (S.dirty) rebuild();
    if (!S.tlesLoaded) loadTles();
    ImGui::TextDisabled("%d / %d", (int)S.shown.size(), (int)S.all.size());
    if (!receiverLocationSet()) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("receiver position not set: passes are for a placeholder")); }
    if (ImGui::BeginTable("satnogs", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Satellite"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("NORAD", ImGuiTableColumnFlags_WidthFixed, 60 * gUi);
        ImGui::TableSetupColumn(tr("Downlink MHz"), ImGuiTableColumnFlags_WidthFixed, 95 * gUi);
        ImGui::TableSetupColumn(tr("Mode"), ImGuiTableColumnFlags_WidthFixed, 90 * gUi);
        ImGui::TableSetupColumn(tr("Baud"), ImGuiTableColumnFlags_WidthFixed, 55 * gUi);
        ImGui::TableSetupColumn(tr("Next pass"), ImGuiTableColumnFlags_WidthFixed, 110 * gUi);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60 * gUi);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)S.shown.size());
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                const dect2::SatnogsTx& t = S.all[S.shown[r]];
                ImGui::TableNextRow();
                ImGui::PushID(r);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(t.satName.c_str());
                if (ImGui::IsItemHovered() && !t.description.empty()) ImGui::SetTooltip("%s\n%s", t.description.c_str(), t.alive ? "alive" : t.status.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%d", t.norad);
                ImGui::TableNextColumn(); ImGui::Text("%.4f", t.downHz / 1e6);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(t.mode.c_str());
                ImGui::TableNextColumn(); if (t.baud > 0) ImGui::Text("%g", t.baud);
                ImGui::TableNextColumn();
                const NextPass& np = nextPassFor(t.norad, now);
                if (np.has) ImGui::Text("%s  %.0f\xC2\xB0", clock(np.p.aos).c_str(), np.p.maxElDeg);
                else ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                if (ImGui::SmallButton(tr("Tune"))) tuneTo(a, t);
                if (ImGui::IsItemHovered()) {
                    const int fam = dect2::satnogsFamily(t.mode, t.baud);
                    ImGui::SetTooltip("%s", fam ? tr("Sets the frequency and opens the OnAir mode that fits this mode") : tr("Sets the frequency (OnAir has no decoder for this mode)"));
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

}

void satnogsWindow(App& a) {
    if (!S.loaded) { S.loaded = true; loadCache(); }
    pollDownload();
    if (!a.satnogsOpen) return;
    if (dect2::satnogsCacheStale(nowSec(), S.fetchedAt) && gDl.load() == 0 && S.status.empty() && S.all.empty()) S.status = tr("Press Refresh to download the list");
    drawWindow(a);
}
