// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Played list and broadcast events: see played_ui.h. Core: dect2/dab/played_log.h.
#include "app.h"
#include "dab/played_ui.h"
#include "dect2/automation/engine.h"
#include "dect2/dab/played_log.h"
#include "i18n/i18n.h"
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace {
namespace fs = std::filesystem;
namespace au = dect2::automation;
struct Pl {
    bool loaded = false;
    dect2::PlayedLog log;
    char find[64] = {};
    std::string note;
    std::map<std::string, std::string> lastText;   // per source+station: the last text sent as broadcast.text
    std::map<std::string, bool> lastTa;
};
Pl P;

fs::path dir() { return fs::path(reinterpret_cast<const char8_t*>((plat::dataDir() + "/played").c_str())); }
std::string month(int64_t t) { const time_t tt = (time_t)t; struct tm l = *localtime(&tt); char b[16]; std::strftime(b, sizeof b, "%Y-%m", &l); return b; }
void load() {
    if (P.loaded) return;
    P.loaded = true;
    const int64_t now = (int64_t)time(nullptr);
    for (int64_t t : {now - 32 * 86400, now}) {   // this month and the last one
        std::ifstream f(dir() / ("played-" + month(t) + ".csv"), std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        for (const auto& p : dect2::playedFromCsv(ss.str())) P.log.add(p);
    }
}
void append(const dect2::PlayedItem& p) {
    std::error_code ec;
    fs::create_directories(dir(), ec);
    const fs::path fp = dir() / ("played-" + month(p.utc) + ".csv");
    const bool fresh = !fs::exists(fp, ec);
    std::string csv = dect2::playedCsv({p});
    if (!fresh) csv = csv.substr(csv.find('\n') + 1);
    std::ofstream o(fp, std::ios::binary | std::ios::app);
    o << csv;
}
std::string clock(int64_t t) { const time_t tt = (time_t)t; struct tm l = *localtime(&tt); char b[24]; std::strftime(b, sizeof b, "%d.%m %H:%M", &l); return b; }
}  // namespace

void playedNote(const char* source, const std::string& station, double freqMhz, const std::string& artist, const std::string& title,
                const std::string& text, const std::string& pty, bool ta) {
    load();
    const int64_t now = (int64_t)time(nullptr);
    if (P.log.note(source, station, freqMhz, artist, title, now)) {
        append(P.log.items().back());
        if (au::wants(au::FBroadcast)) {
            au::Event ev("broadcast.now_playing");
            ev.at((double)now, source, freqMhz * 1e6);
            ev.str("source", source).str("station", station).str("artist", artist).str("title", title).str("pty", pty).num("freq_mhz", freqMhz);
            au::emit(std::move(ev));
        }
    }
    const std::string key = std::string(source) + "|" + station;
    if ((text != P.lastText[key] && !text.empty()) || ta != P.lastTa[key]) {
        const bool taStart = ta && !P.lastTa[key];
        P.lastText[key] = text; P.lastTa[key] = ta;
        if (P.lastText.size() > 500) { P.lastText.clear(); P.lastTa.clear(); }
        if (au::wants(au::FBroadcast)) {
            au::Event ev("broadcast.text");
            ev.at((double)now, source, freqMhz * 1e6);
            ev.str("source", source).str("station", station).str("text", text).str("artist", artist).str("title", title).str("pty", pty).flag("ta", ta).flag("ta_start", taStart).num("freq_mhz", freqMhz);
            au::emit(std::move(ev));
        }
    }
}

void playedPanel(App& a, const char* id) {
    (void)a;
    load();
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(220 * gUi);
    ImGui::InputTextWithHint("##find", tr("Search artist, title, station"), P.find, sizeof P.find);
    const auto rows = P.log.find(P.find, 2000);
    ImGui::SameLine();
    if (ImGui::Button(tr("Save CSV..."))) {
        const std::string p = saveFileDialog("played.csv");
        std::vector<dect2::PlayedItem> v(rows.rbegin(), rows.rend());
        if (!p.empty()) { std::ofstream o(fs::path(reinterpret_cast<const char8_t*>(p.c_str())), std::ios::binary); o << dect2::playedCsv(v); P.note = o ? tr("saved") : tr("could not write the file"); }
    }
    ImGui::SameLine();
    ImGui::TextDisabled(tr("%zu songs (kept in %s)"), rows.size(), (plat::dataDir() + "/played").c_str());
    if (!P.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", P.note.c_str()); }
    if (ImGui::BeginTable("##pl", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, std::max(80.f * gUi, ImGui::GetContentRegionAvail().y)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Time")); ImGui::TableSetupColumn(tr("Station")); ImGui::TableSetupColumn(tr("Artist")); ImGui::TableSetupColumn(tr("Title"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)rows.size());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                const auto& r = rows[(size_t)i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(r.utc).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%s %s", r.source.c_str(), r.station.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.artist.empty() ? "-" : r.artist.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.title.empty() ? "-" : r.title.c_str());
            }
        ImGui::EndTable();
    }
    if (rows.empty()) ImGui::TextDisabled("%s", tr("Nothing yet: stations that tag their songs (RadioText+ on FM, DL Plus on DAB) fill this list."));
    ImGui::PopID();
}
