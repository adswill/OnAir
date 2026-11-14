// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The airport search of the airband panel (see airport_ui.h).
#include "dect2/common/thread_name.h"
#include "dect2/common/diag.h"
#include "airband/airport_ui.h"
#include "ui/ease_logic.h"
#include "app.h"
#include "memory/memory_options.h"
#include "platform/platform.h"
#include "ui/ui_common.h"
#include "dect2/engine/updater.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct State {
    char query[64] = "";
    std::string lastQuery;
    bool nearMode = false;
    float radiusKm = 50;
    std::vector<dect2::AirportHit> hits;
    int sel = -1;                                   // index in the table of the selected airport
    std::vector<dect2::AirbandGroup> groups;        // of the selected airport's channels
    std::vector<dect2::AirbandChannel> chans;       // of the selected airport
    int skipped = 0;
    std::string note;
    bool noteBad = false;
    // the table in use
    std::unique_ptr<dect2::AirportDb> user;
    bool userTried = false;
    std::string userInfo;                           // "imported 12570 airports, 28420 frequencies" for the data line
    // download
    std::atomic<int> job{0};                        // 0 idle, 1 working, 2 done, 3 failed
    std::mutex mu;
    std::string jobMsg;
    std::unique_ptr<dect2::AirportDb> fresh;
    std::thread th;
    bool open = false;
};
State S;
struct Joiner { ~Joiner() { if (S.th.joinable()) S.th.join(); } } gJoiner;   // a download running at exit is waited for (it gives up after seconds)

std::string dirPath() { return plat::dataDir() + "/airports"; }

void loadUser() {
    if (S.userTried) return;
    S.userTried = true;
    const std::string p = dirPath() + "/airports.tsv";
    std::error_code ec;
    if (!fs::exists(fs::u8path(p), ec)) return;
    std::ifstream f(fs::u8path(p), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    auto db = std::make_unique<dect2::AirportDb>();
    const auto r = db->parseTsv(ss.str());
    if (r.airports < 1000) return;   // damaged: the built-in table stays
    S.userInfo = std::to_string(r.airports) + " airports, " + std::to_string(r.freqs) + " frequencies";
    S.user = std::move(db);
}

bool station(double& lat, double& lon) {
    if (!receiverLocationSet()) return false;
    lat = plat::prefs().getD("adsbLat", 0); lon = plat::prefs().getD("adsbLon", 0);
    return true;
}

void say(const std::string& s, bool bad = false) { S.note = s; S.noteBad = bad; }

std::string fm(const char* f, const std::string& a) {
    std::vector<char> b(a.size() + 400);
    snprintf(b.data(), b.size(), f, a.c_str());
    return b.data();
}

std::string readAll(const std::string& p) {
    std::ifstream f(fs::u8path(p), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Both CSV files read, checked, converted and saved as the user's table. Not on the interface thread when it comes from the web.
bool installCsv(const std::string& apFile, const std::string& frFile, std::string& msg, std::unique_ptr<dect2::AirportDb>& out) {
    std::string e1, e2;
    const std::string h1 = dect2::sha256File(apFile, &e1), h2 = dect2::sha256File(frFile, &e2);
    if (h1.empty() || h2.empty()) { msg = "cannot read the files"; return false; }
    auto db = std::make_unique<dect2::AirportDb>();
    const auto r = db->importOurAirports(readAll(apFile), readAll(frFile));
    if (r.airports < 1000) { msg = "not usable: only " + std::to_string(r.airports) + " airports found (are these the OurAirports airports.csv and airport-frequencies.csv?)"; return false; }
    std::error_code ec;
    fs::create_directories(fs::u8path(dirPath()), ec);
    const std::string tmp = dirPath() + "/airports.tsv.part", dst = dirPath() + "/airports.tsv";
    {
        std::ofstream o(fs::u8path(tmp), std::ios::binary);
        o << "# Imported by OnAir from OurAirports (public domain). airports.csv sha256 " << h1 << "  airport-frequencies.csv sha256 " << h2 << "\n" << db->toTsv();
        if (!o) { DIAG(403092, "Writing the imported airport list %s failed (the folder may be read-only or the disk full), so the import was dropped.", tmp.c_str()); msg = "cannot write " + dst; return false; }
    }
    fs::rename(fs::u8path(tmp), fs::u8path(dst), ec);
    if (ec) { msg = "cannot write " + dst; return false; }
    msg = std::to_string(r.airports) + " airports, " + std::to_string(r.freqs) + " frequencies. airports.csv SHA-256 " + h1 + ", airport-frequencies.csv SHA-256 " + h2;
    out = std::move(db);
    return true;
}

void startDownload() {
    if (S.job.load() == 1) return;
    if (S.th.joinable()) S.th.join();
    S.job = 1;
    S.th = dect2::namedThread("onair-airport", [] {
        const std::string d = dirPath() + "/download";
        std::error_code ec;
        fs::create_directories(fs::u8path(d), ec);
        const std::string a = d + "/airports.csv", f = d + "/airport-frequencies.csv";
        std::string msg;
        std::unique_ptr<dect2::AirportDb> db;
        bool ok = plat::fetchUrl("https://davidmegginson.github.io/ourairports-data/airports.csv", a, "OnAir") &&
                  plat::fetchUrl("https://davidmegginson.github.io/ourairports-data/airport-frequencies.csv", f, "OnAir");
        if (!ok) msg = "the download did not work (offline, or too slow): download the two files yourself and use Import CSV";
        else ok = installCsv(a, f, msg, db);
        fs::remove(fs::u8path(a), ec); fs::remove(fs::u8path(f), ec); fs::remove(fs::u8path(d), ec);
        { std::lock_guard<std::mutex> lk(S.mu); S.jobMsg = msg; S.fresh = std::move(db); }
        S.job = ok ? 2 : 3;
    });
}

}

// The offline-packs window downloaded the two OurAirports CSV files: convert them like a download of this section does (called on the pack's thread)
bool airportImportPackFiles(const std::string& apFile, const std::string& frFile, std::string& msg) {
    if (S.job.load() == 1) { msg = "the airport section is downloading too"; return false; }
    std::unique_ptr<dect2::AirportDb> db;
    const bool ok = installCsv(apFile, frFile, msg, db);
    { std::lock_guard<std::mutex> lk(S.mu); S.jobMsg = msg; S.fresh = std::move(db); }
    S.job = ok ? 2 : 3;
    return ok;
}

namespace {
void pollDownload() {
    const int j = S.job.load();
    if (j != 2 && j != 3) return;
    if (S.th.joinable()) S.th.join();
    std::lock_guard<std::mutex> lk(S.mu);
    if (j == 2 && S.fresh) {
        S.user = std::move(S.fresh);
        S.userTried = true;
        S.userInfo = std::to_string(S.user->size()) + " airports";
        S.hits.clear(); S.sel = -1; S.lastQuery = "\x01";
        say(fm(tr("Airport data updated: %s"), S.jobMsg));
    } else say(fm(tr("Airport data not updated: %s"), S.jobMsg), true);
    S.job = 0;
}

void search() {
    const dect2::AirportDb& db = airportDb();
    double me[2];
    const bool have = station(me[0], me[1]);
    if (S.nearMode) {
        if (!have) { S.hits.clear(); return; }
        S.hits = db.nearby(me[0], me[1], S.radiusKm, 200, true);
    } else {
        S.hits = db.search(S.query, 30, have ? me : nullptr, false);
    }
}

// the channels of the selected airport and their captures
void select(App& a, int idx, bool tune) {
    const dect2::AirportDb& db = airportDb();
    if (idx < 0 || (size_t)idx >= db.size()) return;
    S.sel = idx;
    S.chans = dect2::airportAirbandChannels(db.all()[(size_t)idx], &S.skipped);
    const double rate = a.engine.running() && a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate;
    std::vector<double> f;
    for (const auto& c : S.chans) f.push_back(c.freqHz);
    S.groups = dect2::airbandPlanGroups(f, dect2::airbandUsableSpanHz(rate));
    if (tune && !S.groups.empty()) {
        size_t best = 0;
        for (size_t g = 1; g < S.groups.size(); g++) if (S.groups[g].members.size() > S.groups[best].members.size()) best = g;
        airportTuneCentre(a, S.groups[best].centerHz);
    }
}

void saveMemorySet(const dect2::Airport& ap) {
    dect2::MemoryBank& b = memoryBank();
    int added = 0, dup = 0;
    for (const auto& c : S.chans) {
        dect2::Memory m;
        m.name = c.label.empty() ? dect2::airbandName(c.freqHz, c.is833) : c.label;
        m.freqHz = (uint64_t)std::llround(c.freqHz);
        m.mode = "airband";
        m.group = ap.icao + " " + ap.name;
        m.tags = "airport," + ap.icao;
        m.notes = std::string("channel ") + dect2::airbandName(c.freqHz, c.is833) + (c.is833 ? " (8.33 kHz)" : " (25 kHz)");
        const auto r = b.add(m, dect2::DupPolicy::Skip);
        if (r.what == dect2::MemoryAdd::Added) added++; else if (r.what == dect2::MemoryAdd::Duplicate) dup++;
    }
    memoryChanged();
    char buf[160];
    snprintf(buf, sizeof buf, tr("Memory set %s: %d added, %d already there."), ap.icao.c_str(), added, dup);
    say(buf);
}

} // namespace

const dect2::AirportDb& airportDb() {
    loadUser();
    return S.user ? *S.user : dect2::AirportDb::builtin();
}

std::string airportLabelFor(double freqHz) {
    double me[2];
    if (!station(me[0], me[1])) return "";
    const dect2::AirportDb& db = airportDb();
    const std::string nm = dect2::airbandName(freqHz, std::fabs(freqHz / 25000.0 - std::round(freqHz / 25000.0)) * 25000.0 > 10);   // "118.505": the table's kHz
    int whole = 0, frac = 0;
    if (sscanf(nm.c_str(), "%d.%d", &whole, &frac) != 2) return "";
    const int want = whole * 1000 + frac;
    for (const auto& h : db.nearby(me[0], me[1], 150, 40, true)) {
        const dect2::Airport& ap = db.all()[h.index];
        for (const auto& f : ap.freqs) if (f.khz == want) return ap.icao + " " + dect2::airportTypeName(f.type);
    }
    return "";
}

void airportTuneCentre(App& a, double centreHz) {
    const double dial = std::round((centreHz - dect2::airbandTuning().tuneOffsetHz) / 25e3) * 25e3;
    a.freqMhz = dial / 1e6;
    a.tune.centerHz = dial;
    if (a.engine.running()) { a.engine.retuneReset(a.tune); a.peak.clear(); }
    savePrefs(a);
}

bool airportLoadNearest(App& a, const std::function<void(const std::vector<dect2::AirbandChannel>&, bool)>& apply, std::string& what) {
    const dect2::AirportDb& db = airportDb();
    double me[2];
    if (!receiverLocationSet() || !station(me[0], me[1])) return false;
    for (const auto& hit : db.nearby(me[0], me[1], 300, 60, true)) {
        if (db.all()[hit.index].airbandCount() == 0) continue;
        select(a, (int)hit.index, true);
        if (S.chans.empty()) continue;
        apply(S.chans, true);
        char b[160]; snprintf(b, sizeof b, tr("Loaded %s, the nearest airport (%.0f km): %zu channels."), db.all()[hit.index].name.c_str(), hit.km, S.chans.size());
        what = b;
        return true;
    }
    return false;
}

bool airportListenAt(App& a, double lat, double lon, int altFt, const std::function<void(const std::vector<dect2::AirbandChannel>&, bool)>& apply, std::string& what) {
    const dect2::AirportDb& db = airportDb();
    for (const auto& hit : db.nearby(lat, lon, 400, 60, true)) {
        const dect2::Airport& ap = db.all()[hit.index];
        if (ap.airbandCount() == 0) continue;
        select(a, (int)hit.index, false);
        if (S.chans.empty()) continue;
        apply(S.chans, true);
        std::string typeUsed; int khz = 0;
        for (const std::string& t : ease::listenTypes(altFt, hit.km)) {
            for (const auto& f : ap.freqs) if (f.airband() && f.type == t) { typeUsed = t; khz = f.khz; break; }
            if (khz) break;
        }
        int member = -1;
        if (khz) for (size_t k = 0; k < S.chans.size(); k++) if (std::fabs(S.chans[k].freqHz - khz * 1e3) < 5000) member = (int)k;
        size_t best = 0;
        for (size_t g = 0; g < S.groups.size(); g++) if (member >= 0 && std::find(S.groups[g].members.begin(), S.groups[g].members.end(), member) != S.groups[g].members.end()) { best = g; break; }
        if (!S.groups.empty()) airportTuneCentre(a, S.groups[best].centerHz);
        char b[200];
        if (khz) snprintf(b, sizeof b, tr("Listening to %s %s, %.3f MHz (best guess from the aircraft's height and position)."), ap.icao.c_str(), typeUsed.c_str(), khz / 1000.0);
        else snprintf(b, sizeof b, tr("Listening to %s: all its channels."), ap.icao.c_str());
        what = b;
        return true;
    }
    return false;
}

void airportSection(App& a, const std::function<void(const std::vector<dect2::AirbandChannel>&, bool)>& apply) {
    pollDownload();
    const dect2::AirportDb& db = airportDb();
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    S.open = ImGui::CollapsingHeader(trw("Airports##airportsec"));
    if (!S.open) return;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Airport"));
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(220 * gUi);
    const bool changed = ImGui::InputTextWithHint("##aptq", tr("EGLL, LHR, Heathrow, Vilnius ..."), S.query, sizeof S.query);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("ICAO or IATA code, name or city. Pick an airport to load all its frequencies as channels."));
    flowNext(10 * gUi);
    double me[2];
    const bool have = station(me[0], me[1]);
    if (ImGui::Checkbox(tr("Near me##aptnear"), &S.nearMode)) S.lastQuery = "\x01";
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", have ? tr("Airports within the radius of your station position (Settings), nearest first.") : tr("Set your station position first (Settings or the ADS-B map): then the airports around you are listed."));
    if (S.nearMode) {
        ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(120 * gUi);
        if (ImGui::SliderFloat("##aptrad", &S.radiusKm, 5.f, 500.f, "%.0f km")) S.lastQuery = "\x01";
    }
    flowEnd();
    const std::string key = S.nearMode ? std::string("near") + std::to_string((int)S.radiusKm) : std::string(S.query);
    if (changed || key != S.lastQuery) { S.lastQuery = key; search(); }
    if (S.nearMode && !have) ImGui::TextColored(pal::warnAmber(), "%s", tr("Your station position is not set."));
    if (!S.nearMode && S.query[0] == 0) ImGui::TextDisabled("%s", tr("Type a code, a name or a city."));
    else if (S.hits.empty() && (have || !S.nearMode)) ImGui::TextDisabled("%s", tr("No airport found."));
    if (!S.hits.empty()) {
        const float h = std::min(180.f * gUi, (S.hits.size() + 1) * ImGui::GetFrameHeightWithSpacing());
        if (ImGui::BeginTable("##aptres", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, h))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(tr("Code")); ImGui::TableSetupColumn(tr("Airport"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(tr("Place")); ImGui::TableSetupColumn(tr("km")); ImGui::TableSetupColumn(tr("Freq."));
            ImGui::TableHeadersRow();
            for (const auto& hit : S.hits) {
                const dect2::Airport& ap = db.all()[hit.index];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char lb[64];
                snprintf(lb, sizeof lb, "%s%s%s##apt%zu", ap.icao.c_str(), ap.iata.empty() ? "" : " ", ap.iata.c_str(), hit.index);
                ImGui::PushFont(a.mono, 0);
                if (ImGui::Selectable(lb, S.sel == (int)hit.index, ImGuiSelectableFlags_SpanAllColumns)) select(a, (int)hit.index, false);
                ImGui::PopFont();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ap.name.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%s%s%s", ap.city.c_str(), ap.city.empty() ? "" : ", ", ap.country.c_str());
                ImGui::TableNextColumn(); if (hit.km >= 0) ImGui::Text("%.0f", hit.km); else ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); ImGui::Text("%zu", ap.airbandCount());
            }
            ImGui::EndTable();
        }
    }
    if (S.sel >= 0 && (size_t)S.sel < db.size()) {
        const dect2::Airport& ap = db.all()[(size_t)S.sel];
        ImGui::Spacing();
        ImGui::Text("%s%s%s  %s", ap.icao.c_str(), ap.iata.empty() ? "" : " / ", ap.iata.c_str(), ap.name.c_str());
        ImGui::SameLine(0, 8 * gUi);
        ImGui::TextDisabled("%.3f %.3f, %d ft", ap.latDeg, ap.lonDeg, ap.elevFt);
        if (S.chans.empty()) {
            ImGui::TextDisabled("%s", tr("This airport has no frequency in the airband in the table."));
        } else {
            std::string line;
            for (const auto& c : S.chans) {
                if (!line.empty()) line += "   ";
                line += c.label.substr(c.label.find(' ') == std::string::npos ? 0 : c.label.find(' ') + 1) + " " + dect2::airbandName(c.freqHz, c.is833);
            }
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(line.c_str());
            ImGui::PopTextWrapPos();
            if (S.skipped > 0) ImGui::TextDisabled(tr("%d more outside the airband (not received here)."), S.skipped);
            if (ImGui::Button(tr("Load channels##aptload"))) { apply(S.chans, true); select(a, S.sel, true); say(fm(tr("Loaded %s"), ap.icao)); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Replace the channel list with this airport's frequencies and tune the radio to hold as many as the sample rate allows."));
            flowNext(6 * gUi);
            if (ImGui::Button(tr("Add to list##aptadd"))) { apply(S.chans, false); select(a, S.sel, false); say(fm(tr("Added %s"), ap.icao)); }
            flowNext(6 * gUi);
            if (ImGui::Button(tr("Save as memory set##aptmem"))) saveMemorySet(ap);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("One memory per channel, in the group of this airport (Memory panel)."));
            flowEnd();
            if (S.groups.size() > 1) {
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled(tr("The frequencies spread over %.1f MHz; the radio takes in %.1f MHz at once, so they come in %zu captures. Tune to each:"),
                                    (S.groups.back().hiHz - S.groups.front().loHz) / 1e6, dect2::airbandUsableSpanHz(a.engine.running() && a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate) / 1e6, S.groups.size());
                ImGui::PopTextWrapPos();
                for (size_t g = 0; g < S.groups.size(); g++) {
                    char b[96];
                    snprintf(b, sizeof b, "%.3f - %.3f MHz (%zu)##aptg%zu", S.groups[g].loHz / 1e6, S.groups[g].hiHz / 1e6, S.groups[g].members.size(), g);
                    if (ImGui::Button(b)) airportTuneCentre(a, S.groups[g].centerHz);
                    flowNext(4 * gUi);
                }
                flowEnd();
            }
        }
    }
    if (!S.note.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(S.noteBad ? pal::warnAmber() : pal::okGreen(), "%s", S.note.c_str()); ImGui::PopTextWrapPos(); }
    // the data
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s %zu airports. %s", S.user ? tr("Imported table:") : tr("Built-in table (OurAirports, public domain):"), db.size(), tr("Community data: can be out of date. Never use it for navigation."));
    ImGui::PopTextWrapPos();
    const bool busy = S.job.load() == 1;
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(busy ? tr("Downloading ...##aptweb") : tr("Refresh from the web##aptweb"))) startDownload();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Downloads airports.csv and airport-frequencies.csv from https://davidmegginson.github.io/ourairports-data/ (about 14 MB) when you click, never by itself. The SHA-256 of what came is shown."));
    flowNext(6 * gUi);
    if (ImGui::Button(tr("Import CSV ...##aptimp"))) {
        const std::string p1 = openFileDialog();
        if (!p1.empty()) {
            const std::string p2 = openFileDialog();
            if (!p2.empty()) {
                std::string msg;
                std::unique_ptr<dect2::AirportDb> d;
                // the files may be picked in either order
                bool ok = installCsv(p1, p2, msg, d);
                if (!ok) ok = installCsv(p2, p1, msg, d);
                if (ok) { S.user = std::move(d); S.userTried = true; S.hits.clear(); S.sel = -1; S.lastQuery = "\x01"; say(fm(tr("Imported: %s"), msg)); }
                else say(fm(tr("Import failed: %s"), msg), true);
            }
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Pick airports.csv, then airport-frequencies.csv (OurAirports format)."));
    if (S.user) {
        flowNext(6 * gUi);
        if (ImGui::Button(tr("Back to built-in##aptback"))) {
            std::error_code ec;
            fs::remove(fs::u8path(dirPath() + "/airports.tsv"), ec);
            S.user.reset(); S.hits.clear(); S.sel = -1; S.lastQuery = "\x01"; say(tr("Using the built-in table again."));
        }
    }
    flowEnd();
    ImGui::Separator();
}
