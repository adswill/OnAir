// Meteor-M pass report card: one row per pass (maximum elevation, SNR, % frames decoded, lines, gaps), compared with earlier passes at a similar
// elevation; kept in <data folder>/stats/meteor-passes.csv. Core: dect2/meteor/pass_report.h.
#include "app.h"
#include "panels/gallery.h"
#include "dect2/geo/passes.h"
#include "dect2/meteor/meteor_tel.h"
#include "dect2/meteor/pass_report.h"
#include "i18n/i18n.h"
#include "adsb/digest_ui.h"
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

void meteorPassTick(App& a);
void meteorPassPanel(App& a);

namespace {
namespace fs = std::filesystem;
struct Mp { bool loaded = false; dect2::PassTracker tr; std::vector<dect2::PassReport> hist; std::string last; uint64_t seq = 0; };
Mp M;
fs::path file() { return fs::path(reinterpret_cast<const char8_t*>((plat::dataDir() + "/stats").c_str())) / "meteor-passes.csv"; }
void load() {
    if (M.loaded) return;
    M.loaded = true;
    digestRegister("4meteor", [] {
        const time_t t = time(nullptr); struct tm l = *localtime(&t); l.tm_hour = l.tm_min = l.tm_sec = 0; const int64_t mid = (int64_t)mktime(&l);
        int n = 0, lines = 0, gaps = 0;
        for (const auto& p : M.hist) if (p.startUtc >= mid) { n++; lines += p.lines; gaps += p.gapLines; }
        if (!n) return std::string();
        char b[96]; std::snprintf(b, sizeof b, "%d Meteor passes, %d lines, %d lost", n, lines, gaps);
        return std::string(b);
    });
    std::ifstream f(file(), std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    M.hist = dect2::passFromCsv(ss.str());
}
void append(const dect2::PassReport& p) {
    std::error_code ec;
    fs::create_directories(file().parent_path(), ec);
    const bool fresh = !fs::exists(file(), ec);
    std::ofstream o(file(), std::ios::binary | std::ios::app);
    o << dect2::passCsv({p}, fresh);
}
void finishPass(dect2::PassReport p) {
    std::string name;
    const double mid = (p.startUtc + p.endUtc) / 2.0;
    if (const dect2::Sgp4* sat = passesFindSat("METEOR-M", mid, &name)) {
        p.satellite = name;
        if (receiverLocationSet()) {
            dect2::Observer o; receiverLocation(o.latDeg, o.lonDeg);
            for (int64_t t = p.startUtc - 300; t <= p.endUtc + 300; t += 10) { dect2::Look l; if (dect2::lookAt(*sat, o, (double)t, l)) p.maxElDeg = std::max(p.maxElDeg, l.elDeg); }
        }
    }
    const dect2::PassCompare c = dect2::passCompare(M.hist, p);
    char b[200];
    std::snprintf(b, sizeof b, "Last pass: %s, max %.0f deg, %.0f %% frames, %d lines%s%s", p.satellite.empty() ? "Meteor" : p.satellite.c_str(), p.maxElDeg, p.syncPct(), p.lines,
                  c.verdict.empty() ? "" : ", ", c.verdict.c_str());
    M.last = b;
    M.hist.push_back(p);
    append(p);
}
}  // namespace

void meteorPassTick(App& a) {
    load();
    const int64_t now = (int64_t)time(nullptr);
    const bool on = a.engine.running() && a.rx.standard == 60;
    dect2::PassReport p;
    if (!on) { if (M.tr.finish(now, p, 60, true)) finishPass(p); return; }
    const dect2::MeteorTelemetry& t = a.rx.meteor;
    if (t.seq == M.seq) { if (M.tr.finish(now, p)) finishPass(p); return; }
    M.seq = t.seq;
    int lines = 0, gaps = 0;
    if (t.image) {
        const dect2::MeteorImage& im = *t.image;
        lines = im.rows * 8;
        for (int r = 0; r < im.rows; r++) {
            bool any = false;
            for (int c = 0; c < 6 && !any; c++) if ((size_t)r < im.ch[(size_t)c].size() && im.ch[(size_t)c][(size_t)r]) any = true;
            if (!any) gaps += 8;
        }
    }
    M.tr.update(now, t.state, t.snrDb, t.blocksOk, t.blocksBad, t.packetsLost, lines, gaps);
    if (M.tr.finish(now, p)) finishPass(p);
}

void meteorPassPanel(App& a) {
    (void)a;
    load();
    if (!M.last.empty()) ImGui::TextDisabled("%s", M.last.c_str());
    if (!ImGui::CollapsingHeader(trw("Pass reports##metpass"))) return;
    if (M.hist.empty()) { ImGui::TextDisabled("%s", tr("No passes yet. A pass is written when frames stop for a minute.")); return; }
    if (!ImGui::BeginTable("##metpt", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, 200 * gUi))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Start")); ImGui::TableSetupColumn(tr("Satellite")); ImGui::TableSetupColumn(tr("Max el")); ImGui::TableSetupColumn("SNR");
    ImGui::TableSetupColumn(tr("Frames")); ImGui::TableSetupColumn(tr("Lines")); ImGui::TableSetupColumn(tr("Gaps")); ImGui::TableSetupColumn(tr("Against similar passes"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (auto it = M.hist.rbegin(); it != M.hist.rend(); ++it) {
        const auto& p = *it;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); { const time_t t = (time_t)p.startUtc; struct tm l = *localtime(&t); char b[24]; std::strftime(b, sizeof b, "%d.%m %H:%M", &l); ImGui::TextUnformatted(b); }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(p.satellite.empty() ? "-" : p.satellite.c_str());
        ImGui::TableNextColumn(); if (p.maxElDeg >= 0) ImGui::Text("%.0f", p.maxElDeg); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::Text("%.1f / %.1f", p.snrMeanDb, p.snrMaxDb);
        ImGui::TableNextColumn(); ImGui::Text("%.0f %%", p.syncPct());
        ImGui::TableNextColumn(); ImGui::Text("%d", p.lines);
        ImGui::TableNextColumn(); ImGui::Text("%d", p.gapLines);
        ImGui::TableNextColumn();
        const dect2::PassCompare c = dect2::passCompare(M.hist, p);
        if (c.similar) ImGui::TextDisabled("%s (%d passes: %.0f lines, %.0f %%)", c.verdict.c_str(), c.similar, c.meanLines, c.meanSync); else ImGui::TextDisabled("-");
    }
    ImGui::EndTable();
}
