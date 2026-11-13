// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AIS vessel log: every MMSI ever heard with its last name and type, unique vessels per day and per hour, the farthest per class, kept on
// disk (core: dect2/common/seen_store.h; bands are the classes: 0 class A, 1 class B, 2 the others).
#include "app.h"
#include "ais/ais_seen_ui.h"
#include "dect2/ais/ais_tel.h"
#include "dect2/common/seen_store.h"
#include "i18n/i18n.h"
#include "adsb/records_ui.h"
#include "adsb/digest_ui.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace {
namespace fs = std::filesystem;
struct St {
    bool loaded = false;
    dect2::SeenStore st;
    std::map<uint32_t, uint32_t> lastMsgs;
    std::map<uint32_t, std::pair<double, double>> lastPos;
    double lastSave = 0;
    uint64_t lastSeq = 0;
    char find[48] = {};
    std::string note;
};
St G;

bool live(const App& a) { return a.rx.standard == 15; }   // as ais_ui.cpp
int tzOff() {
    const time_t t = time(nullptr);
    struct tm l = *localtime(&t), g = *gmtime(&t);
    l.tm_isdst = 0; g.tm_isdst = 0;
    return (int)difftime(mktime(&l), mktime(&g));
}
fs::path file() { return fs::path(reinterpret_cast<const char8_t*>((plat::dataDir() + "/stats").c_str())) / "ais-seen.txt"; }
void save() {
    std::error_code ec;
    fs::create_directories(file().parent_path(), ec);
    const fs::path tmp = file().string() + ".tmp";
    { std::ofstream o(tmp, std::ios::binary | std::ios::trunc); o << G.st.serialize(); if (!o) return; }
    fs::rename(tmp, file(), ec);
    G.st.clearDirty();
}
void load() {
    if (G.loaded) return;
    G.loaded = true;
    digestRegister("2ais", [] {
        const dect2::SeenDay* t = G.st.today((int64_t)time(nullptr));
        if (!t || !t->unique) return std::string();
        char b[120]; std::snprintf(b, sizeof b, "%u vessels (%u new), farthest %.0f km", t->unique, t->fresh, t->maxKm);
        return std::string(b);
    });
    G.st.setTzOffset(tzOff());
    std::ifstream f(file(), std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    G.st.parse(ss.str());
    G.st.setTzOffset(tzOff());
}
double distKm(double la1, double lo1, double la2, double lo2, double& brg) {
    const double r = 3.14159265358979 / 180, p1 = la1 * r, p2 = la2 * r, dl = (lo2 - lo1) * r;
    const double a = std::sin((p2 - p1) / 2) * std::sin((p2 - p1) / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    brg = std::fmod(std::atan2(std::sin(dl) * std::cos(p2), std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl)) / r + 360, 360);
    return 6371.0 * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}
}  // namespace

void aisSeenTick(App& a) {
    load();
    const double now = ImGui::GetTime();
    if (G.st.dirty() && now - G.lastSave > 300) { G.lastSave = now; save(); }
    if (!live(a)) { if (G.st.dirty() && G.lastSeq) { save(); G.lastSeq = 0; } return; }
    if (a.rx.seq == G.lastSeq) return;
    G.lastSeq = a.rx.seq;
    const int64_t utc = (int64_t)time(nullptr);
    double rl = 0, ro = 0;
    const bool home = receiverLocationSet();
    if (home) receiverLocation(rl, ro);
    for (const auto& v : a.rx.ais.vessels) {
        if (v.ageSec > 10 || !v.mmsi) continue;
        const std::string id = std::to_string(v.mmsi);
        G.st.seen(id, utc, v.name, v.shipType >= 0 ? dect2::aisShipTypeText(v.shipType) : std::string());
        uint32_t& m = G.lastMsgs[v.mmsi];
        if (v.messages > m) G.st.messages(utc, v.messages - m);
        m = v.messages;
        if (v.hasPos && home) {
            auto& lp = G.lastPos[v.mmsi];
            if (lp.first != v.lat || lp.second != v.lon) {
                lp = {v.lat, v.lon};
                double brg = 0;
                const double km = distKm(rl, ro, v.lat, v.lon, brg);
                G.st.position(id, utc, km, brg, v.cls == dect2::AIS_CLASS_A ? 0 : v.cls == dect2::AIS_CLASS_B ? 1 : 2);
                if (!v.lrPosition && v.cls != dect2::AIS_CLASS_BASE) recordsOffer("AIS", "", km, v.name.empty() ? id : v.name + " " + id, v.cls == dect2::AIS_CLASS_A ? "class A" : v.cls == dect2::AIS_CLASS_B ? "class B" : "");
            }
        }
    }
    if (G.lastMsgs.size() > 20000) { G.lastMsgs.clear(); G.lastPos.clear(); }
}

void aisSeenView(App& a) {
    (void)a;
    load();
    const int64_t utc = (int64_t)time(nullptr);
    const dect2::SeenDay* t = G.st.today(utc);
    ImGui::Text(tr("Vessels ever heard: %zu   today: %u, of them new: %u   farthest today: %.0f km"), G.st.uniqueEver(), t ? t->unique : 0u, t ? t->fresh : 0u, t ? t->maxKm : 0.0);
    ImGui::TextDisabled(tr("Farthest ever: class A %.0f km (%s), class B %.0f km (%s), others %.0f km (%s)"), G.st.farthestKm(0), G.st.farthestId(0).c_str(), G.st.farthestKm(1),
                        G.st.farthestId(1).c_str(), G.st.farthestKm(2), G.st.farthestId(2).c_str());
    if (!receiverLocationSet()) ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.25f, 1), "%s", tr("Set the receiver position for distances (Tuner, Locate)."));
    if (ImGui::Button(tr("Days CSV...##aisseen"))) {
        const std::string p = saveFileDialog("ais-days.csv");
        if (!p.empty()) { std::ofstream o(fs::path(reinterpret_cast<const char8_t*>(p.c_str())), std::ios::binary); o << dect2::seenDaysCsv(G.st); G.note = o ? tr("saved") : tr("could not write the file"); }
    }
    if (!G.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", G.note.c_str()); }
    recordsPanel();
    const auto& d = G.st.days();
    if (d.empty()) { ImGui::TextDisabled("%s", tr("No days yet: the log fills while the receiver runs.")); return; }
    const float w = ImGui::GetContentRegionAvail().x * 0.49f, h = 64 * gUi;
    std::vector<float> uq, hr;
    for (size_t i = d.size() > 30 ? d.size() - 30 : 0; i < d.size(); i++) uq.push_back((float)d[i].unique);
    for (uint32_t v : (t ? t->hours : d.back().hours)) hr.push_back((float)v);
    ImGui::PlotHistogram("##aissu", uq.data(), (int)uq.size(), 0, tr("unique vessels per day (30 days)"), 0, FLT_MAX, ImVec2(w, h));
    ImGui::SameLine();
    ImGui::PlotHistogram("##aissh", hr.data(), (int)hr.size(), 0, tr("vessels per hour today (local, 0-23)"), 0, FLT_MAX, ImVec2(w, h));
    // all-time list: name, type, first and last day
    ImGui::SetNextItemWidth(220 * gUi);
    ImGui::InputTextWithHint("##aisseenf", tr("Find MMSI, name or type"), G.find, sizeof G.find);
    std::string q = G.find;
    for (char& c : q) c = (char)std::tolower((unsigned char)c);
    std::vector<std::pair<const std::string*, const dect2::SeenId*>> rows;
    for (const auto& kv : G.st.ids()) {
        if (!q.empty()) {
            std::string h2 = kv.first + " " + kv.second.name + " " + kv.second.kind;
            for (char& c : h2) c = (char)std::tolower((unsigned char)c);
            if (h2.find(q) == std::string::npos) continue;
        }
        rows.push_back({&kv.first, &kv.second});
    }
    std::sort(rows.begin(), rows.end(), [](const auto& x, const auto& y) { return x.second->lastUtc > y.second->lastUtc; });
    ImGui::SameLine(); ImGui::TextDisabled(tr("%zu vessels"), rows.size());
    if (!ImGui::BeginTable("##aisseenl", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("MMSI"); ImGui::TableSetupColumn(tr("Name")); ImGui::TableSetupColumn(tr("Type")); ImGui::TableSetupColumn(tr("First heard"));
    ImGui::TableSetupColumn(tr("Last heard")); ImGui::TableSetupColumn(tr("Days"), ImGuiTableColumnFlags_WidthStretch);
    ImGuiListClipper clip;
    clip.Begin((int)rows.size());
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const auto& r = rows[(size_t)i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.first->c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.second->name.empty() ? "-" : r.second->name.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.second->kind.empty() ? "-" : r.second->kind.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.second->firstDay.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.second->lastDay.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%u", r.second->days);
        }
    ImGui::EndTable();
}
