// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Data packs window (View > Data packs): what can be downloaded to work without internet (map tiles of a region, satellite elements, airports,
// transmitter lists), with size, source, licence and the date of the last download. Every download starts with the user's click on that pack; nothing is
// fetched by itself. Progress, cancel (a started pack continues where it stopped), checksum check, delete, and a guard against a full disk.
// The logic is core/src/packs/packs.cpp; extra packs can be listed in <data folder>/packs/catalog.tsv.
#include "../app.h"
#include "../airband/airport_ui.h"
#include "../ui/ui_common.h"
#include "dect2/common/u8file.h"
#include "dect2/packs/packs.h"
#include <cmath>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>

using namespace dect2::packs;

namespace {

struct U {
    std::unique_ptr<PackManager> mgr;
    bool loaded = false;
    std::string note, userNote;
    bool noteBad = false;
    std::string lastId;
    PackManager::State lastState = PackManager::State::Idle;
    // the area of "around my station"
    double lat = 0, lon = 0, radiusKm = 50;
    int maxZ = 12;
    bool areaInit = false;
    std::string confirmDel;
    std::string filter;
} u;

std::string mb(uint64_t b) {
    char s[32];
    if (b >= (1u << 30)) snprintf(s, sizeof s, "%.1f GB", b / 1073741824.0);
    else if (b >= (1u << 20)) snprintf(s, sizeof s, "%.0f MB", b / 1048576.0);
    else snprintf(s, sizeof s, "%.0f KB", b / 1024.0);
    return s;
}

std::string dateOf(long long t) {
    time_t tt = (time_t)t;
    tm m{};
#ifdef _WIN32
    localtime_s(&m, &tt);
#else
    localtime_r(&tt, &m);
#endif
    char s[32];
    strftime(s, sizeof s, "%Y-%m-%d", &m);
    return s;
}

const char* categoryLabel(const std::string& c) {
    if (c == "Map tiles") return tr("Map tiles");
    if (c == "Satellites") return tr("Satellites");
    if (c == "Airports") return tr("Airports");
    if (c == "Transmitters") return tr("Transmitters");
    return c.c_str();
}

Pack hereArea() {
    Pack p;
    p.id = "tiles-here"; p.title = "Map tiles: around my station"; p.category = "Map tiles"; p.source = "OpenStreetMap tile servers";
    p.licence = "ODbL 1.0 (data), tile usage policy: small areas only"; p.attribution = "(c) OpenStreetMap contributors";
    const double dLat = u.radiusKm / 111.0, dLon = u.radiusKm / (111.0 * std::max(0.1, std::cos(u.lat * M_PI / 180)));
    p.south = std::max(-85.0, u.lat - dLat); p.north = std::min(85.0, u.lat + dLat);
    p.west = u.lon - dLon; p.east = u.lon + dLon;
    if (p.west < -180) p.west += 360;
    if (p.east > 180) p.east -= 360;
    p.minZoom = 0; p.maxZoom = u.maxZ;
    finishTilePack(p);
    return p;
}

void rebuildCatalog() {
    std::vector<Pack> c = builtinCatalog();
    if (u.areaInit) c.insert(c.begin(), hereArea());
    std::ifstream f(dect2::u8file(plat::dataDir() + "/packs/catalog.tsv"), std::ios::binary);
    if (f) {
        std::stringstream ss;
        ss << f.rdbuf();
        std::string err;
        for (Pack& p : parseCatalogTsv(ss.str(), &err)) c.push_back(std::move(p));
        u.userNote = err;
    }
    u.mgr->setCatalog(std::move(c));
}

void load() {
    if (u.loaded) return;
    u.loaded = true;
    u.mgr.reset(new PackManager(plat::dataDir(), plat::cacheDir()));
    u.mgr->setPostStep([](const Pack& p, std::string& msg) {
        if (p.post == "airports") {
            return airportImportPackFiles(plat::dataDir() + "/" + p.files[0].rel, plat::dataDir() + "/" + p.files[1].rel, msg);
        }
        return true;
    });
    u.lat = plat::prefs().getD("adsbLat", 0);
    u.lon = plat::prefs().getD("adsbLon", 0);
    u.radiusKm = plat::prefs().getD("packsRadiusKm", 50);
    u.maxZ = (int)plat::prefs().getI("packsMaxZoom", 12);
    u.areaInit = receiverLocationSet();
    rebuildCatalog();
}

void poll() {
    const auto st = u.mgr->state();
    if (st == u.lastState && u.mgr->activeId() == u.lastId) return;
    u.lastState = st;
    u.lastId = u.mgr->activeId();
    const Pack* p = u.mgr->find(u.lastId);
    const std::string t = p ? p->title : u.lastId;
    if (st == PackManager::State::Done) { u.note = std::string(tr("Downloaded: ")) + t; u.noteBad = false; }
    else if (st == PackManager::State::Failed) { u.note = t + ": " + u.mgr->error(); u.noteBad = true; }
    else if (st == PackManager::State::Cancelled) { u.note = std::string(tr("Stopped: ")) + t + tr(" (start it again to continue)"); u.noteBad = false; }
}

void table(float h) {
    const auto& cat = u.mgr->catalog();
    const bool busy = u.mgr->state() == PackManager::State::Downloading || u.mgr->state() == PackManager::State::Verifying;
    const std::string active = u.mgr->activeId();
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##packs", 6, tf, ImVec2(0, h))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Pack"), ImGuiTableColumnFlags_WidthStretch, 3.f);
    ImGui::TableSetupColumn(tr("Size"), ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
    ImGui::TableSetupColumn(tr("Source"), ImGuiTableColumnFlags_WidthStretch, 2.f);
    ImGui::TableSetupColumn(tr("Licence"), ImGuiTableColumnFlags_WidthStretch, 2.f);
    ImGui::TableSetupColumn(tr("Last updated"), ImGuiTableColumnFlags_WidthFixed, 100 * gUi);
    {   // as wide as the most it ever holds (Update, Check, Really delete, Keep), in this language: a fixed 250 px left most of it empty
        // at 150 % while the Pack column was cut off
        const ImGuiStyle& st = ImGui::GetStyle();
        float w = 0;
        for (const char* l : {tr("Update"), tr("Check"), tr("Really delete"), tr("Keep")}) w += ImGui::CalcTextSize(l).x + 2 * st.FramePadding.x;
        ImGui::TableSetupColumn(tr("Action"), ImGuiTableColumnFlags_WidthFixed, w + 3 * st.ItemSpacing.x + 4 * gUi);
    }
    ImGui::TableHeadersRow();
    std::string lastCat;
    for (const Pack& p : cat) {
        if (!u.filter.empty() && p.title.find(u.filter) == std::string::npos && p.category.find(u.filter) == std::string::npos) continue;
        if (p.category != lastCat) {
            lastCat = p.category;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.f, 1.f), "%s", categoryLabel(p.category));
        }
        const Installed in = u.mgr->installed(p.id);
        const bool mine = busy && active == p.id;
        ImGui::TableNextRow();
        ImGui::PushID(p.id.c_str());
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(p.title.c_str());
        if (p.tiles) ImGui::TextDisabled(tr("%ld tiles, zoom %d to %d"), p.tileCount, p.minZoom, p.maxZoom);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(in.ok && !mine ? mb(in.bytes).c_str() : (std::string("~") + mb(p.estBytes)).c_str());
        ImGui::TableSetColumnIndex(2);
        ImGui::TextWrapped("%s", p.source.c_str());
        ImGui::TableSetColumnIndex(3);
        ImGui::TextWrapped("%s", p.licence.c_str());
        if (ImGui::IsItemHovered() && !p.attribution.empty()) wrapTooltip(320 * gUi, (std::string(tr("Credit: ")) + p.attribution).c_str());
        ImGui::TableSetColumnIndex(4);
        if (in.ok) ImGui::TextUnformatted(dateOf(in.when).c_str()); else ImGui::TextDisabled("%s", tr("not downloaded"));
        ImGui::TableSetColumnIndex(5);
        if (mine) {
            const double tot = (double)std::max<long>(1, u.mgr->totalFiles());
            ImGui::ProgressBar((float)(u.mgr->doneFiles() / tot), ImVec2(110 * gUi, 0), (mb(u.mgr->doneBytes())).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Stop"))) u.mgr->cancel();
        } else {
            ImGui::BeginDisabled(busy || (p.tiles && p.tileCount <= 0));
            if (ImGui::SmallButton(in.ok ? tr("Update") : tr("Download"))) {
                std::string why;
                if (!u.mgr->start(p.id, &why)) { u.note = why; u.noteBad = true; }
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Starts the download now. Files that are already there and correct are kept."));
            if (in.ok) {
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("Check"))) {
                    const VerifyResult v = u.mgr->verify(p.id);
                    u.note = p.title + ": " + v.detail;
                    u.noteBad = v.bad || v.missing;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Checks every file against the checksum recorded at the download"));
                ImGui::SameLine();
                if (u.confirmDel == p.id) {
                    if (ImGui::SmallButton(tr("Really delete"))) { u.mgr->remove(p.id); u.confirmDel.clear(); u.note = std::string(tr("Deleted: ")) + p.title; u.noteBad = false; }
                    ImGui::SameLine();
                    if (ImGui::SmallButton(tr("Keep"))) u.confirmDel.clear();
                } else if (ImGui::SmallButton(tr("Delete"))) u.confirmDel = p.id;
            }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void body() {
    load();
    poll();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Download data once and use it without internet. Nothing is downloaded until you click Download on a pack."));
    ImGui::PopTextWrapPos();
    ImGui::TextDisabled(tr("Free disk space: %s"), mb(u.mgr->freeBytes()).c_str());
    ImGui::SameLine(0, 16 * gUi);
    ImGui::SetNextItemWidth(160 * gUi);
    char fb[64];
    snprintf(fb, sizeof fb, "%s", u.filter.c_str());
    if (ImGui::InputTextWithHint("##packfilter", tr("Search"), fb, sizeof fb)) u.filter = fb;
    // the area around the station
    if (ImGui::CollapsingHeader(trw("Map tiles around a position"))) {
        bool ch = false;
        ImGui::SetNextItemWidth(110 * gUi); ch |= ImGui::InputDouble(tr("Latitude"), &u.lat, 0, 0, "%.4f");
        ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi); ch |= ImGui::InputDouble(tr("Longitude"), &u.lon, 0, 0, "%.4f");
        ImGui::SetNextItemWidth(110 * gUi); ch |= ImGui::InputDouble(tr("Radius (km)"), &u.radiusKm, 0, 0, "%.0f");
        ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi); ch |= ImGui::InputInt(tr("Deepest zoom"), &u.maxZ, 1, 1);
        u.radiusKm = std::max(1.0, std::min(2000.0, u.radiusKm));
        u.maxZ = std::max(1, std::min(18, u.maxZ));
        if (ch) {
            u.areaInit = true;
            plat::prefs().setD("packsRadiusKm", u.radiusKm); plat::prefs().setI("packsMaxZoom", u.maxZ); plat::prefs().flush();
            if (u.mgr->state() != PackManager::State::Downloading) rebuildCatalog();
        }
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("The zoom is lowered by itself so the area stays within 4000 tiles (the OpenStreetMap tile policy does not allow bulk downloads)."));
        ImGui::PopTextWrapPos();
    }
    if (!u.userNote.empty()) ImGui::TextColored(pal::badRed(), "%s", u.userNote.c_str());
    if (!u.note.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(u.noteBad ? pal::badRed() : pal::okGreen(), "%s", u.note.c_str()); ImGui::PopTextWrapPos(); }
    // the credit line wraps in a narrow window (it ran off the right edge at 150 %): the table leaves room for as many lines as it takes
    const char* credit = tr("Map data (c) OpenStreetMap contributors. Satellite elements: CelesTrak. Airports: OurAirports (public domain). Details: Help > Credits.");
    const float creditH = ImGui::CalcTextSize(credit, nullptr, false, ImGui::GetContentRegionAvail().x).y;
    table(-(creditH + ImGui::GetStyle().ItemSpacing.y + 2 * gUi));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", credit);
    ImGui::PopTextWrapPos();
}

}  // namespace

void packsWindow(App& a) {
    if (!a.packsOpen) return;
    firstWindowSize(980 * gUi, 520 * gUi);
    if (!ImGui::Begin(trw("Data packs"), &a.packsOpen)) { ImGui::End(); return; }
    body();
    ImGui::End();
}
