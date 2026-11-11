// FM: the census of the whole capture, following the station to its alternative frequencies, and the DX log. docs/SMART-fmam-3.7.md
#include "app.h"
#include "../ui/typography.h"
#include "dect2/fm/fm_af.h"
#include "dect2/fm/fm_dx.h"
#include "dect2/fm/fm_dxtools.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <sstream>

namespace {
dect2::FmAfFollow gAf;
dect2::FmDxLog gDx;
dect2::fmdx::PiWatch gPiWatch;                       // co-channel interference: two PIs alternating on one frequency
std::vector<dect2::fmdx::HeadingSample> gHeadings;   // antenna direction helper: level per heading
int gHeadingDeg = 0;
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
    if (live && fm.piCode) { seen(a.freqMhz, fm.piCode, fm.rds.ecc, fm.psName, fm.snrDb); gPiWatch.add(a.freqMhz, fm.piCode, fm.snrDb); }
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
    // Warnings from the census: stations that share a channel (two PIs alternate) and stations hidden next to a much stronger one.
    if (on && !rows.empty()) {
        for (const auto& c : gPiWatch.coChannel()) {
            std::string txt;
            for (const auto& q : c.pis) { char b[64]; snprintf(b, sizeof b, "%s%04X (%.0f%%, %.0f dB)", txt.empty() ? "" : " / ", q.pi, q.share * 100, q.snrDb); txt += b; }
            ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(1.f, 0.75f, 0.2f, 1.f), "%.1f MHz: co-channel interference, two stations alternate: %s", c.mhz, txt.c_str()); ImGui::PopTextWrapPos();
        }
        std::vector<dect2::fmdx::ListedStation> ls;
        for (const auto& r : rows) ls.push_back({r.mhz, r.levelDb});
        for (const auto& k : dect2::fmdx::adjacentRisk(ls)) {
            ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%.1f MHz sits %.0f kHz from a station %.0f dB stronger: expect splatter and poor RDS.", rows[(size_t)k.index].mhz, std::fabs(k.neighbourKhz), k.overDb); ImGui::PopTextWrapPos();
        }
    }
    // Antenna direction helper: turn the antenna, record the level at each heading, read the best lobe.
    if (typo::collapsingHeader(trw("Antenna direction"), 0)) {
        ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", tr("Turn the antenna to a heading, type it, press Add reading. After 4 or more headings that go around the compass the best direction is shown.")); ImGui::PopTextWrapPos();
        ImGui::SetNextItemWidth(90 * gUi); ImGui::InputInt(tr("heading (deg)"), &gHeadingDeg, 15, 45);
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Add reading"))) gHeadings.push_back({(double)gHeadingDeg, (double)a.rx.fm.levelDbfs});
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Clear"))) gHeadings.clear();
        const auto bh = dect2::fmdx::bestHeading(gHeadings);
        if (bh.ok) ImGui::Text("Best heading %.0f deg, %.1f dB over the average, lobe about %.0f deg wide.", bh.deg, bh.gainDb, bh.widthDeg);
        else if (!gHeadings.empty()) ImGui::TextDisabled("%zu readings; need 4 headings that go around the compass.", gHeadings.size());
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
