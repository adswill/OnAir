// GSM-R screens: the base station table (ARFCN, BSIC, operator code, location area, cell identity, level, last seen), the channel lists
// of System Information 1 and 2, the carriers seen in the spectrum, a band scan for radios that cannot take the whole railway band at
// once, and the log. Only what base stations broadcast to every mobile is shown.
#include "app.h"
#include "state/results_export.h"
#include "dect2/gsmr/gsmr_rx.h"
#include "dect2/gsmr/gsmr_si.h"
#include "dect2/gsmr/gsmr_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;                 // 0 cells, 1 channel lists, 2 carriers, 3 log
    int sel = -1;                 // selected cell (index in cells)
    bool railway = true, publicBand = false, dcsBand = false;
    bool scanning = false;
    std::vector<double> plan;     // centres, MHz
    size_t step = 0;
    double stepAt = 0;
    float dwell = 7.0f;           // a capture is 2.2 s, and the search takes about as long again
};
State S;

bool live(const App& a) { return a.rx.standard == 40; }

std::string ago(int64_t t) {
    if (t <= 0) return "-";
    const long d = (long)(std::time(nullptr) - t);
    char b[32];
    if (d < 60) snprintf(b, sizeof b, "%ld s", std::max(0L, d)); else if (d < 3600) snprintf(b, sizeof b, "%ld min", d / 60); else snprintf(b, sizeof b, "%ld h", d / 3600);
    return b;
}

std::string opText(const dect2::GsmrCell& c) {
    if (!c.hasLai) return "-";
    char b[96];
    snprintf(b, sizeof b, "%03d-%0*d", c.mcc, c.mncDigits, c.mnc);
    return c.op.empty() ? std::string(b) : std::string(b) + " " + c.op;
}

std::string listText(const std::vector<int>& v) {
    std::string s;
    for (int n : v) s += std::to_string(n) + " ";
    return s.empty() ? "-" : s;
}

void makePlan(const App& a) {
    S.plan.clear();
    const double span = std::max(1.0, a.tune.sampleRate / 1e6 * 0.75);   // the part of the sample band searched whole
    auto add = [&](double lo, double hi) { for (double c = lo + span / 2; c - span / 2 < hi; c += span) S.plan.push_back(std::round(c * 10) / 10); };
    if (S.railway) add(921.0, 925.0);
    if (S.publicBand) add(925.0, 960.0);
    if (S.dcsBand) add(1805.0, 1880.0);
}

void scanBar(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Bands");
    flowNext(6 * gUi);
    ImGui::Checkbox("GSM-R 921 - 925##gb0", &S.railway);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The railway allocation: ARFCN 955 - 973, 921.2 - 924.8 MHz");
    flowNext(6 * gUi);
    ImGui::Checkbox("GSM 900 925 - 960##gb1", &S.publicBand);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The public GSM 900 downlink: the same broadcast, the same decoder");
    flowNext(6 * gUi);
    ImGui::Checkbox("DCS 1800 1805 - 1880##gb2", &S.dcsBand);
    flowNext(10 * gUi);
    if (!S.scanning) {
        if (runningButton(a, tr("Scan"), true)) { makePlan(a); S.step = 0; S.stepAt = 0; S.scanning = !S.plan.empty(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Step the radio over the chosen bands; every carrier in each step is searched. Receive only.");
    } else {
        if (ImGui::SmallButton("Stop scan")) S.scanning = false;
        ImGui::SameLine(); ImGui::TextDisabled("%zu / %zu", S.step + 1, S.plan.size());
    }
    flowNext(10 * gUi);
    if (ImGui::SmallButton("Clear")) a.engine.gsmr().clearCells();
    flowNext(10 * gUi);
    {
        static std::string note;
        resexp::Table t; t.cols = {"time", "frequency_hz", "mode", "operator", "arfcn", "bsic", "lac", "cell_id", "band", "level_db", "snr_db"};
        const std::string now = resexp::stamp();
        for (const auto& c : a.rx.gsmr.cells)
            t.add({now, resexp::num(c.freqHz, 0), c.railway ? "GSM-R" : c.freqHz > 1.7e9 ? "DCS 1800" : "GSM 900", opText(c), c.arfcn >= 0 ? std::to_string(c.arfcn) : "", std::to_string(c.ncc) + "-" + std::to_string(c.bcc),
                   c.hasLai ? std::to_string(c.lac) : "", c.hasSi3 ? std::to_string(c.cellId) : "", c.railway ? "GSM-R" : c.freqHz > 1.7e9 ? "DCS 1800" : "GSM 900",
                   resexp::num(c.levelDb, 0), resexp::num(c.snrDb, 0)});
        resexp::saveButton(t, "gsmr", note, "svgsmr");
        if (!note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", note.c_str()); }
    }
    flowEnd();
}

void cellTable(App& a) {
    const dect2::GsmrTelemetry& t = a.rx.gsmr;
    const bool wide = ImGui::GetContentRegionAvail().x > 900 * gUi;
    const int nc = wide ? 10 : 6;
    const ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV;
    if (!ImGui::BeginTable("##gcells", nc, fl, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    if (wide) for (const char* h : {"Operator", "ARFCN", "MHz", "BSIC", "LAC", "Cell", "Band", "Level", "SNR", "Seen"}) ImGui::TableSetupColumn(h);
    else for (const char* h : {"Operator", "ARFCN", "BSIC", "LAC", "Cell", "Level"}) ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < t.cells.size(); i++) {
        const auto& c = t.cells[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        char l[128]; snprintf(l, sizeof l, "%s##g%zu", opText(c).c_str(), i);
        if (ImGui::Selectable(l, S.sel == (int)i, ImGuiSelectableFlags_SpanAllColumns)) S.sel = (int)i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s, %.1f MHz, BSIC %d-%d, offset %+.1f kHz, seen %s ago", opText(c).c_str(), c.freqHz / 1e6, c.ncc, c.bcc, c.cfoHz / 1e3, ago(c.lastWall).c_str());
        ImGui::TableNextColumn(); if (c.arfcn >= 0) ImGui::Text("%d", c.arfcn); else ImGui::TextDisabled("-");
        if (wide) { ImGui::TableNextColumn(); ImGui::Text("%.1f", c.freqHz / 1e6); }
        ImGui::TableNextColumn(); ImGui::Text("%d-%d", c.ncc, c.bcc);
        ImGui::TableNextColumn(); if (c.hasLai) ImGui::Text("%u", c.lac); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (c.hasSi3) ImGui::Text("%u", c.cellId); else ImGui::TextDisabled("-");
        if (wide) { ImGui::TableNextColumn(); ImGui::TextUnformatted(c.railway ? "GSM-R" : c.freqHz > 1.7e9 ? "DCS 1800" : "GSM 900"); }
        ImGui::TableNextColumn(); ImGui::Text("%.0f", c.levelDb);
        if (wide) { ImGui::TableNextColumn(); ImGui::Text("%.0f", c.snrDb); }
        if (wide) { ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(c.lastWall).c_str()); }
    }
    ImGui::EndTable();
}

void detail(App& a) {
    ImGui::PushTextWrapPos(0);
    struct Pop { ~Pop() { ImGui::PopTextWrapPos(); } } pop_;
    const dect2::GsmrTelemetry& t = a.rx.gsmr;
    if (S.sel < 0 || S.sel >= (int)t.cells.size()) { ImGui::TextDisabled("select a base station"); return; }
    const auto& c = t.cells[(size_t)S.sel];
    ImGui::Text("ARFCN %d (%.1f MHz)%s, BSIC NCC %d BCC %d, frame number %d", c.arfcn, c.freqHz / 1e6, c.railway ? ", GSM-R band" : "", c.ncc, c.bcc, c.fn);
    ImGui::Text("level %.1f dBFS, SNR %.1f dB, offset %+.0f Hz, %llu SI messages, %llu failed", c.levelDb, c.snrDb, c.cfoHz, (unsigned long long)c.siMsgs, (unsigned long long)c.siBad);
    if (c.hasLai) ImGui::Text("Location area: %s (%s), LAC %u (0x%04X)", opText(c).c_str(), dect2::gsmrCountry(c.mcc), c.lac, c.lac);
    if (c.hasSi3) {
        ImGui::Text("SI3: cell identity %u, IMSI attach %s, CCCH-CONF %d, AG blocks reserved %d, paging multiframes %d, T3212 %d min",
                    c.cellId, c.attach ? "allowed" : "not allowed", c.ccchConf, c.agBlks, c.pagMfrms, c.t3212 * 6);
        ImGui::Text("radio link timeout %d SACCH blocks, DTX %d, reselect hysteresis %d dB, access level min %d dBm, max TX power level %d%s%s",
                    (c.rlTimeout + 1) * 4, c.dtx, c.reselHystDb, c.rxLevAccessMin - 110, c.msTxPwrMax, c.cellBar ? ", cell barred" : "", c.acBarred ? ", some access classes barred" : "");
    }
    std::string ty; for (int v : c.typesSeen) { ty += dect2::gsmr::siName(v); ty += " "; }
    ImGui::Text("messages read: %s", ty.empty() ? "-" : ty.c_str());
}

void listsView(App& a) {
    const dect2::GsmrTelemetry& t = a.rx.gsmr;
    if (!ImGui::BeginChild("##glists")) { ImGui::EndChild(); return; }
    bool any = false;
    for (const auto& c : t.cells) {
        if (!c.hasSi1 && !c.hasSi2) continue;
        any = true;
        char h[96]; snprintf(h, sizeof h, "ARFCN %d %s##l%d_%d", c.arfcn, opText(c).c_str(), c.arfcn, c.bsic);
        if (!ImGui::TreeNode(h)) continue;
        if (c.hasSi1) ImGui::Text("cell allocation (SI1): %s", listText(c.cellAlloc).c_str());
        if (c.hasSi2) {
            ImGui::Text("BCCH neighbours (SI2): %s", listText(c.neigh).c_str());
            ImGui::Text("neighbour NCC permitted: 0x%02X", c.nccPermitted);
        }
        ImGui::TreePop();
    }
    if (!any) ImGui::TextDisabled("no channel list read yet (SI1 / SI2)");
    ImGui::EndChild();
}

void carrierView(App& a) {
    const dect2::GsmrTelemetry& t = a.rx.gsmr;
    if (t.carriers.empty()) { ImGui::TextDisabled("no carriers in the last block"); return; }
    for (const auto& c : t.carriers)
        ImGui::BulletText("%.2f MHz (ARFCN %d), %.0f dB over the floor, %s", c.freqHz / 1e6, dect2::gsmrHzToArfcn(c.freqHz), c.levelDb, c.synced ? "BCCH synchronised" : "no synchronisation");
}

void logView(App& a) {
    for (const auto& e : a.rx.gsmr.events) ImGui::TextUnformatted(e.text.c_str());
    if (a.rx.gsmr.events.empty()) ImGui::TextDisabled("nothing yet");
}

void tick(App& a) {
    if (!S.scanning) return;
    if (!a.engine.running()) { S.scanning = false; return; }
    const double now = ImGui::GetTime();
    if (S.stepAt == 0 || now - S.stepAt >= S.dwell) {
        if (S.stepAt != 0) S.step++;
        if (S.step >= S.plan.size()) { S.scanning = false; return; }
        tuneFreq(a, S.plan[S.step]);
        S.stepAt = now;
    }
}

void tab(App& a) {
    scanBar(a);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Receive only: the public information every GSM / GSM-R base station broadcasts to all mobiles (identity, operator code, location area). Nothing addressed to a mobile is decoded.");
    ImGui::PopTextWrapPos();
    subNav("gsmrv", S.view, {"Cells", "Channel lists", "Carriers", "Log"});
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to find the railway cells around you"); return; }
    if (S.view == 0) {
        const float h = ImGui::GetContentRegionAvail().y;
        if (ImGui::BeginChild("##gct", ImVec2(0, h * 0.62f))) cellTable(a);
        ImGui::EndChild();
        detail(a);
    } else if (S.view == 1) listsView(a);
    else if (S.view == 2) carrierView(a);
    else logView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    for (const auto& c : a.rx.gsmr.cells) ImGui::Text("%4d %-12s %.0f", c.arfcn, opText(c).c_str(), c.levelDb);
    if (a.rx.gsmr.cells.empty()) ImGui::TextDisabled("none yet");
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::GsmrTelemetry& t = a.rx.gsmr;
    ImGui::Text("%s", dect2::gsmrSummary(t).c_str());
    ImGui::Text("%llu blocks searched, %llu SI messages, %llu failed", (unsigned long long)t.blocks, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    ImGui::Text("input %.2f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    if (t.inputRate < 3.5e6) ImGui::TextDisabled("%.1f MHz of the band at a time: use Scan for the rest", t.inputRate / 1e6 * 0.8);
}

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::gsmrSummary(a.rx.gsmr).c_str() : (a.engine.running() ? "starting" : "stopped"));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "GSM-R";
    if (live(a)) l2 = dect2::gsmrSummary(a.rx.gsmr);
}

} // namespace

extern const ModeUi kGsmrUi;
const ModeUi kGsmrUi = {
    .sideTitle = "GSM-R",
    .tabName = "Cells",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
