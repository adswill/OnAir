// FM: the census of the whole capture, following the station to its alternative frequencies, and the DX log. docs/SMART-fmam-3.7.md
#include "app.h"
#include "../ui/typography.h"
#include "dect2/fm/fm_af.h"
#include "dect2/fm/fm_dx.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <sstream>

namespace {
dect2::FmAfFollow gAf;
dect2::FmDxLog gDx;
bool gDxLoaded = false, gAfAuto = false, gAfInit = false;
std::string gAfMsg;
double gLastObserve = 0, gLastSave = 0;

std::string dxPath() { return plat::dataDir() + "/fm_dx.tsv"; }
void dxLoad() {
    if (gDxLoaded) return;
    gDxLoaded = true;
    std::ifstream f(dxPath());
    if (f) { std::stringstream b; b << f.rdbuf(); gDx.load(b.str()); }
}
void dxSave() { std::ofstream f(dxPath()); if (f) f << gDx.serialize(); }
bool levelOf(App& a, double mhz, float& db) {
    if (a.spec.dbfs.empty()) return false;
    const double fs = a.engine.sampleRate(), off = mhz * 1e6 - a.engine.captureCentreHz();
    if (std::fabs(off) + 100e3 > fs * 0.46) return false;
    db = (float)fmChannelDb(a.spec, fs, off);
    return true;
}
}

void fmSmartTick(App& a) {
    if (!a.fmMode) return;
    dxLoad();
    if (!gAfInit) { gAfInit = true; gAfAuto = plat::prefs().getB("fmAfAuto", false); dect2::FmAfConfig c; c.autoFollow = gAfAuto; gAf.setConfig(c); }
    const double now = ImGui::GetTime();
    if (now - gLastObserve < 0.5) return;
    gLastObserve = now;
    const bool live = a.engine.running() && a.rx.standard == 6;
    const FmTelemetry& fm = a.rx.fm;
    if (live && fm.rdsSync && fm.piCode) {
        float level = fm.levelDbfs;
        levelOf(a, a.freqMhz, level);          // on the same scale as the AF levels when the spectrum has it
        auto cmd = gAf.update(now, a.freqMhz, fm.piCode, fm.snrDb, level, fm.rds.afMhz, [&](double m, float& d) { return levelOf(a, m, d); });
        if (cmd.type != dect2::FmAfCommand::None) { fmTune(a, cmd.mhz); gAfMsg = cmd.why; }
        else if (cmd.confirmed) gAfMsg = cmd.why;
    } else if (live) {
        auto cmd = gAf.update(now, a.freqMhz, 0, fm.snrDb, fm.levelDbfs, {}, {});
        if (cmd.type != dect2::FmAfCommand::None) { fmTune(a, cmd.mhz); gAfMsg = cmd.why; }
    }
    // the DX log: what is heard, from the station and the census
    const int64_t t = (int64_t)std::time(nullptr);
    auto seen = [&](double mhz, int pi, int ecc, const std::string& ps, float above) {
        const auto al = gDx.observe(t, mhz, pi, ecc, ps, above);
        if (al.fire) a.engine.log("FM DX: " + std::to_string(mhz) + " MHz " + ps + " from " + (al.certain ? al.country : al.likely) + (al.certain ? "" : " (likely)"));
    };
    if (live && fm.piCode) seen(a.freqMhz, fm.piCode, fm.rds.ecc, fm.psName, fm.snrDb);
    if (a.engine.fmCensusOn()) for (const auto& r : a.engine.fmCensusRows()) if (r.active && r.pi) seen(r.mhz, r.pi, r.ecc, r.ps, r.aboveDb);
    if (now - gLastSave > 30) { gLastSave = now; dxSave(); }
}

void fmAfPanel(App& a) {
    const FmTelemetry& fm = a.rx.fm;
    (void)fm;
    const auto& c = gAf.candidates();
    if (!c.empty() && ImGui::BeginTable("##fmaf", 3, ImGuiTableFlags_SizingFixedFit)) {
        for (const auto& x : c) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("%.1f MHz", x.mhz);
            ImGui::TableNextColumn();
            if (x.rejected) ImGui::TextDisabled("%s", tr("other station"));
            else if (x.inSpan) ImGui::Text("%.0f dB", x.levelDb); else ImGui::TextDisabled("%s", tr("outside the capture"));
            ImGui::TableNextColumn();
            ImGui::PushID((int)(x.mhz * 10));
            if (ImGui::SmallButton(tr("Listen"))) fmTune(a, x.mhz);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button(tr("Go to the strongest"))) gAf.requestBest();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Moves to the strongest alternative frequency and keeps it only if the programme code (PI) is the same."));
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Follow automatically"), &gAfAuto)) {
        dect2::FmAfConfig cfg = gAf.config(); cfg.autoFollow = gAfAuto; gAf.setConfig(cfg);
        plat::prefs().setB("fmAfAuto", gAfAuto);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("When this station gets weak, moves to a stronger frequency of the same programme, as a car radio does."));
    if (!gAfMsg.empty()) ImGui::TextDisabled("%s", gAfMsg.c_str());
}

void fmCensusPanel(App& a) {
    if (!typo::collapsingHeader(trw("Census: every station at once"), ImGuiTreeNodeFlags_DefaultOpen)) return;
    bool on = a.engine.fmCensusOn();
    const double rate = a.engine.sampleRate();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Reads every station inside the capture at the same time, without tuning. A wider capture covers more of the band."));
    ImGui::PopTextWrapPos();
    if (ImGui::Checkbox(tr("Run census"), &on)) a.engine.setFmCensus(on);
    if (a.engine.running() && rate < 1e6) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("needs a capture of at least 1 MHz")); }
    const auto rows = a.engine.fmCensusRows();
    if (on && !rows.empty() && ImGui::BeginTable("##fmcensus", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY, ImVec2(0, 220 * gUi))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("MHz")); ImGui::TableSetupColumn("PI"); ImGui::TableSetupColumn(tr("Name")); ImGui::TableSetupColumn(tr("Programme"));
        ImGui::TableSetupColumn(tr("Level")); ImGui::TableSetupColumn(tr("Signal")); ImGui::TableSetupColumn(tr("Text")); ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (const auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%.1f", r.mhz);
            ImGui::TableNextColumn(); if (r.pi) ImGui::Text("%04X", r.pi);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.ps.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.pty.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.0f dB", r.levelDb);
            ImGui::TableNextColumn(); ImGui::Text("%s%s%s%s", r.stereo ? tr("stereo") : tr("mono"), r.rds ? " RDS" : "", r.ta ? " TA" : "", r.active ? "" : " -");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.rt.c_str());
            ImGui::TableNextColumn();
            ImGui::PushID((int)(r.mhz * 10));
            if (ImGui::SmallButton(tr("Listen"))) fmTune(a, r.mhz);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    // the DX log, under the census
    if (!gDx.alerts().empty()) {
        char h[64]; snprintf(h, sizeof h, "%s (%zu)", tr("Distant stations"), gDx.alerts().size());
        if (typo::collapsingHeader(h, 0)) {
            for (auto it = gDx.alerts().rbegin(); it != gDx.alerts().rend(); ++it) {
                char t[32]; std::time_t tt = (std::time_t)it->at; std::strftime(t, sizeof t, "%H:%M", std::localtime(&tt));
                ImGui::Text("%s  %.1f MHz  %04X  %s  %s%s", t, it->mhz, it->pi, it->ps.c_str(), (it->certain ? it->country : it->likely).c_str(), it->certain ? "" : "?");
            }
            if (ImGui::SmallButton(tr("Save DX log (CSV)"))) { std::ofstream f(plat::dataDir() + "/fm_dx_log.csv"); if (f) { f << gDx.toCsv(); a.engine.log("FM DX log written to " + plat::dataDir() + "/fm_dx_log.csv"); } }
        }
    }
}
