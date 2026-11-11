// FM: RDS-TMC traffic messages in plain words with the user's own event list and location table, and the TMC log (dect2/fm/tmc_db.h,
// docs/TMC-PLAIN-3.9.md). The imported files are copied to the settings folder ("tmc") and read again at every start. No table ships with
// OnAir: without the files the messages keep their codes, with a "?" that says how to import them.
#include "app.h"
#include "../ui/typography.h"
#include "dect2/common/u8file.h"
#include "dect2/fm/tmc_db.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {
namespace td = dect2::tmcdb;
td::Database gDb;
td::Log gLog;
bool gInit = false;
std::string gMsg, gLang = "auto";
uint64_t gSeq = 0;

std::string tmcFolder() { return plat::dataDir() + "/tmc"; }

void reload() {
    gDb.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(dect2::u8file(tmcFolder()), ec)) return;
    td::loadFolder(gDb, tmcFolder());
}

void init() {
    if (gInit) return;
    gInit = true;
    plat::Prefs& p = plat::prefs();
    gLang = p.getS("tmcLang", "auto");
    gLog.setPath(p.getS("tmcLogPath", ""));
    gLog.setEnabled(p.getB("tmcLogOn", true));   // on by default: it writes once a path is set
    reload();
}

// Checks the file is a TMC list or table, then copies it into the settings folder and reads the folder again
bool importFile(const std::string& src, std::string& msg) {
    td::Database probe;
    const td::ImportReport r = probe.importFile(src);
    if (!r.ok) { msg = r.message; return false; }
    std::error_code ec;
    const std::filesystem::path dir = dect2::u8file(tmcFolder());
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path from = dect2::u8file(src);
    std::filesystem::path name = from.filename();
    std::string ext = name.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (ext != ".csv" && ext != ".txt" && ext != ".dat" && ext != ".tsv") name += ".csv";   // the folder loader reads these
    std::filesystem::copy_file(from, dir / name, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) { msg = std::string(tr("Could not copy the file into the settings folder")) + ": " + ec.message(); return false; }
    reload();
    msg = r.message;
    return true;
}

void importFolder(const std::string& src, std::string& msg) {
    std::error_code ec;
    int ok = 0, bad = 0;
    for (const auto& e : std::filesystem::directory_iterator(dect2::u8file(src), ec)) {
        if (!e.is_regular_file(ec)) continue;
        const std::u8string u = e.path().u8string();
        std::string m;
        if (importFile(std::string(u.begin(), u.end()), m)) ok++; else bad++;
    }
    char b[160];
    snprintf(b, sizeof b, tr("%d files imported, %d skipped (not TMC lists or tables)."), ok, bad);
    msg = b;
}

void hint() {
    if (!ImGui::IsItemHovered()) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
    ImGui::TextUnformatted(tr("The event text or the location is not in your TMC files. Location tables are national data and are not included: "
                              "get your country's event list and location table (CSV, or the TISA exchange files) from its TMC service, then use "
                              "\"TMC tables and log\" below, Import file or Import folder. They are kept in the settings folder."));
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

td::Station stationOf(const App& a) {
    const FmTelemetry& fm = a.rx.fm;
    td::Station st;
    st.freqMhz = a.freqMhz; st.pi = fm.piCode; st.ps = fm.psName; st.ltn = fm.tmc.ltn; st.sid = fm.tmc.sid;
    return st;
}
} // namespace

// Every frame in FM: the log takes the messages that are new (once per telemetry update)
void fmTmcTick(App& a) {
    init();
    const FmTelemetry& fm = a.rx.fm;
    if (!fm.tmc.seen || fm.seq == gSeq || !gLog.enabled()) return;
    gSeq = fm.seq;
    gLog.update(fm.tmcMessages, stationOf(a), &gDb, gLang);
}

void fmTmcPanel(App& a) {
    init();
    fmTmcTick(a);
    const FmTelemetry& fm = a.rx.fm;
    const bool haveDb = gDb.hasEvents() || gDb.hasLocations();
    ImGui::PushTextWrapPos(0);
    if (gDb.hasLocations())
        ImGui::TextDisabled(tr("Table %d%s. Locations from your table (%zu)."), fm.tmc.ltn, fm.tmc.mode == 1 ? tr(", enhanced mode") : "", gDb.locationCount());
    else {
        ImGui::TextDisabled(tr("Table %d%s. Locations are shown as codes: import your country's location table."), fm.tmc.ltn, fm.tmc.mode == 1 ? tr(", enhanced mode") : "");
        ImGui::SameLine(); ImGui::TextDisabled("(?)"); hint();
    }
    if (gDb.tableNumber() >= 0 && fm.tmc.ltn >= 0 && gDb.tableNumber() != fm.tmc.ltn)
        ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f), tr("Your location table is number %d, the station uses table %d: the names may be wrong."), gDb.tableNumber(), fm.tmc.ltn);
    ImGui::PopTextWrapPos();
    ImGui::BeginChild("##fmtmc", ImVec2(0, 170 * gUi), ImGuiChildFlags_Borders);
    if (fm.tmcMessages.empty()) ImGui::TextDisabled("%s", tr("No messages yet."));
    const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    for (size_t i = 0; i < fm.tmcMessages.size(); i++) {
        const TmcMessage& m = fm.tmcMessages[i];
        const td::Plain p = td::describe(m, haveDb ? &gDb : nullptr, gLang);
        ImGui::PushID((int)i);
        ImGui::PushTextWrapPos(0);
        ImGui::Text("%s", p.event.c_str());
        if (!p.eventKnown) { ImGui::SameLine(); ImGui::TextDisabled("(?)"); hint(); }
        for (const auto& s : p.details) ImGui::TextDisabled("  + %s", s.c_str());
        std::string where = p.road.empty() ? p.location : p.road + ", " + p.location;
        where += ", " + p.direction;
        ImGui::Text("%s", where.c_str());
        if (!p.locationKnown) { ImGui::SameLine(); ImGui::TextDisabled("(?)"); hint(); }
        std::string more;
        if (!p.extent.empty()) more = p.extent;
        if (!p.duration.empty()) more += (more.empty() ? "" : ", ") + p.duration;
        if (!p.diversion.empty()) more += (more.empty() ? "" : ", ") + std::string(tr("diversion advised"));
        if (!more.empty()) ImGui::TextDisabled("%s", more.c_str());
        char raw[96];
        snprintf(raw, sizeof raw, "event %d, location %d%s", m.event, m.location, m.negative ? ", negative" : "");
        ImGui::TextDisabled(tr("%s; received %d min ago, expires in %d min"), raw, (int)std::max(0.0, (now - m.received) / 60), (int)std::max(0.0, (m.expires - now) / 60));
        ImGui::PopTextWrapPos();
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (typo::collapsingHeader(tr("TMC tables and log"))) {   // the manual part
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled(tr("Your files: %zu events, %zu locations (in %s)."), gDb.eventCount(), gDb.locationCount(), tmcFolder().c_str());
        if (ImGui::Button(tr("Import file..."))) { const std::string f = plat::openFileDialog(); if (!f.empty()) importFile(f, gMsg); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("An event list or a location table (CSV with ; , or tab, or a TISA .DAT file); the columns are found on their own"));
        ImGui::SameLine();
        if (ImGui::Button(tr("Import folder..."))) { const std::string f = plat::openFolderDialog(); if (!f.empty()) importFolder(f, gMsg); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Every list and table in a folder, such as the TISA exchange files (POINTS, ROADS, SEGMENTS, NAMES, OFFSETS ...)"));
        ImGui::SameLine();
        if (ImGui::Button(tr("Remove all"))) {
            std::error_code ec;
            std::filesystem::remove_all(dect2::u8file(tmcFolder()), ec);
            reload();
            gMsg = tr("The TMC files were removed from the settings folder.");
        }
        if (!gMsg.empty()) ImGui::TextDisabled("%s", gMsg.c_str());
        const std::vector<std::string> langs = gDb.languages();
        if (!langs.empty()) {
            ImGui::SetNextItemWidth(120 * gUi);
            if (ImGui::BeginCombo(tr("Event language"), gLang == "auto" ? tr("automatic") : gLang.c_str())) {
                if (ImGui::Selectable(tr("automatic"), gLang == "auto")) { gLang = "auto"; plat::prefs().setS("tmcLang", gLang); }
                for (const std::string& l : langs) if (ImGui::Selectable(l.c_str(), gLang == l)) { gLang = l; plat::prefs().setS("tmcLang", gLang); }
                ImGui::EndCombo();
            }
        }
        bool on = gLog.on();
        if (ImGui::Checkbox(tr("Write a log (CSV and text)"), &on)) { gLog.setEnabled(on); plat::prefs().setB("tmcLogOn", on); }
        ImGui::SameLine();
        if (ImGui::Button(tr("Log file..."))) {
            std::string f = plat::saveFileDialog("tmc-log.csv");
            if (!f.empty()) {
                for (const char* ext : {".csv", ".txt", ".CSV", ".TXT"}) if (f.size() > 4 && f.compare(f.size() - 4, 4, ext) == 0) { f.resize(f.size() - 4); break; }
                gLog.setPath(f); gLog.setEnabled(true); gSeq = 0;
                plat::prefs().setS("tmcLogPath", f); plat::prefs().setB("tmcLogOn", true);
            }
        }
        if (gLog.path().empty()) ImGui::TextDisabled("%s", tr("No log file set: choose one and every new message is written to it."));
        else ImGui::TextDisabled(tr("Logging to %s.csv and .txt"), gLog.path().c_str());
        if (!gLog.lastError().empty()) ImGui::TextColored(ImVec4(1.f, 0.5f, 0.4f, 1.f), "%s", gLog.lastError().c_str());
        ImGui::PopTextWrapPos();
    }
}
