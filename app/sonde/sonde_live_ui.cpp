// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Live Google Earth...": the selected sonde as a KML file that is rewritten every 5 seconds, and once a small NetworkLink KML that makes
// Google Earth reload it. Both go to the exports folder (data folder / exports). The KML is built by core (dect2/sonde/sonde_kml.h).
#include "app.h"
#include "dect2/engine/updater.h"
#include "dect2/sonde/sonde_kml.h"
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>

void sondeLiveKmlControls(const dect2::SondeInfo* s);
void sondeLiveKmlTick(const dect2::SondeInfo* sel);
bool sondePredictedCourse(const dect2::SondeInfo& s, std::vector<dect2::sondecourse::Point>& out);   // sonde_ui.cpp

namespace {
constexpr int kRefreshS = 5;
struct Lv {
    bool loaded = false, on = false, haveLast = false;
    double lastWriteT = -1e9;
    std::string note;
    dect2::SondeInfo last;
};
Lv& L() { static Lv* l = new Lv(); return *l; }
double steady() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::filesystem::path P(const std::string& u8) { return std::filesystem::path(reinterpret_cast<const char8_t*>(u8.c_str())); }
std::string folder() { return plat::dataDir() + "/exports"; }
std::string liveFile() { return folder() + "/onair-sonde-live.kml"; }
std::string linkFile() { return folder() + "/OnAir-sonde-network-link.kml"; }

bool writeFile(const std::string& path, const std::string& body) {   // through a temporary file, so Google Earth never reads half of it
    const std::string tmp = path + ".tmp";
    { std::ofstream o(P(tmp), std::ios::binary); o << body; if (!o) return false; }
    std::error_code ec;
    std::filesystem::rename(P(tmp), P(path), ec);
    if (ec) { std::filesystem::remove(P(path), ec); std::filesystem::rename(P(tmp), P(path), ec); }
    return !ec;
}
bool writeLive(const dect2::SondeInfo& s) {
    std::vector<dect2::sondecourse::Point> course;
    sondePredictedCourse(s, course);
    return writeFile(liveFile(), dect2::sondeLiveKml(s, course, (double)std::time(nullptr)));
}
void load() {
    Lv& l = L();
    if (l.loaded) return;
    l.loaded = true;
    l.on = plat::prefs().getB("sondeLiveKmlOn", false);
}
}  // namespace

// every frame from the sonde screens' tick: the live file every 5 s for the selected sonde, while switched on
void sondeLiveKmlTick(const dect2::SondeInfo* sel) {
    load();
    Lv& l = L();
    if (!sel) return;
    const double now = steady();
    if (now - l.lastWriteT < kRefreshS) return;
    l.lastWriteT = now;
    l.last = *sel; l.haveLast = true;   // copied at most every 5 s (the track can hold 600 points)
    if (!l.on || !sel->hasPos) return;
    if (!writeLive(*sel)) l.note = tr("could not write the live KML into the exports folder");
}

void sondeLiveKmlControls(const dect2::SondeInfo* s) {
    load();
    Lv& l = L();
    if (s) { l.last = *s; l.haveLast = true; }
    ImGui::PushID("sondelivekml");
    if (ImGui::SmallButton(tr("Live Google Earth..."))) {
        std::error_code ec;
        std::filesystem::create_directories(P(folder()), ec);
        const bool a = writeFile(linkFile(), dect2::sondeNetworkLinkKml(dect2::fileUrlOfPath(liveFile()), kRefreshS, "OnAir sonde (live)"));
        const bool b = !l.haveLast || !l.last.hasPos || writeLive(l.last);
        l.on = a && b; l.lastWriteT = steady();
        plat::prefs().setB("sondeLiveKmlOn", l.on);
        l.note = l.on ? std::string(tr("files written to ")) + folder() : std::string(tr("could not write into ")) + folder();
        if (l.on) dect2::openUrl(folder());
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Keeps the selected sonde in a KML file and rewrites it every 5 seconds, with the path drawn down to the ground, the burst, the position now and the predicted course. Google Earth reloads it by itself after you open the link file once."));
    if (l.on) {
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("stop live KML"))) { l.on = false; plat::prefs().setB("sondeLiveKmlOn", false); l.note.clear(); }
    }
    ImGui::TextDisabled("%s", tr("Open OnAir-sonde-network-link.kml in Google Earth once."));
    if (!l.note.empty()) ImGui::TextDisabled("%s", l.note.c_str());
    ImGui::PopID();
}
