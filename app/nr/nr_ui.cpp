// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 5G NR cells screens: the cell table (band, GSCN, SS_REF, PCI, SSB spacing, beams, MIB, SS-RSRP / RSRQ / SINR, last seen), the beams of
// each cell (SSB index and RSRP), a band scan over the NR sync raster, the map (the receiver; tower positions need the cell identity of
// SIB1, which is not decoded) and the log. Only what cells broadcast to every phone is shown.
#include "app.h"
#include "state/results_export.h"
#include "../adsb/adsb_map.h"
#include "dect2/cell/cell_filter.h"
#include "dect2/nr/nr_phy.h"
#include "dect2/nr/nr_rx.h"
#include "dect2/nr/nr_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

constexpr int kNb = 22;
const int kScanBands[kNb] = {78, 77, 28, 20, 8, 3, 1, 7, 38, 40, 41, 79, 48, 5, 2, 25, 12, 71, 66, 34, 39, 50};

struct State {
    int view = adsbmap::devView(0);   // 0 cells, 1 beams, 2 map, 3 log
    int sel = -1;
    bool bands[kNb] = {true, false, true, true};   // n78, n28, n20
    bool custom = false;          // a custom range instead of (with) the bands
    float customLo = 3400, customHi = 3800;   // MHz
    bool scanning = false;
    std::vector<double> plan;     // centres, MHz
    size_t step = 0;
    double stepAt = 0;
    float dwell = 1.0f;
    uint64_t blocksAt = 0;
    adsbmap::View map;
    bool fitted = false;
    char hide[48] = {};           // "Hide bands": NR band numbers left out of the list and the export (#164)
    float minRsrp = -120;         // dBFS; at the bottom of the slider: no limit
    bool mibOnly = false;
};
State S;

dect2::NrFilter filterNow() {
    dect2::NrFilter f;
    f.hideBands = dect2::parseBandList(S.hide);
    f.minRsrpDb = S.minRsrp <= -120 ? -200.f : S.minRsrp;
    f.mibOnly = S.mibOnly;
    return f;
}
bool live(const App& a) { return a.rx.standard == 58; }

std::string ago(int64_t t) {
    if (t <= 0) return "-";
    const long d = (long)(std::time(nullptr) - t);
    char b[32];
    if (d < 60) snprintf(b, sizeof b, "%ld s", std::max(0L, d)); else if (d < 3600) snprintf(b, sizeof b, "%ld min", d / 60); else snprintf(b, sizeof b, "%ld h", d / 3600);
    return b;
}

std::string bandText(const dect2::NrCell& c) { if (!c.band) return "?"; char b[16]; snprintf(b, sizeof b, "n%d", c.band); return b; }

void makePlan(const App& a) {
    S.plan.clear();
    const double fs = a.tune.sampleRate / 1e6;
    auto add = [&](double lo, double hi, int scs) {   // centres one searched span apart; the receiver tries every GSCN inside each
        const double span = std::max(0.5, fs * 0.92 - 0.24 * scs - 0.4);   // the part of the sample band whose raster points are searched
        for (double c = lo + span / 2; c - span / 2 <= hi; c += span) {
            const double r = std::round(c * 1000) / 1000;
            bool dup = false;
            for (double p : S.plan) if (std::fabs(p - r) < span / 2) dup = true;
            if (!dup) S.plan.push_back(r);
        }
    };
    for (int i = 0; i < kNb; i++) {
        if (!S.bands[i]) continue;
        for (const auto& b : dect2::nr::nrBands()) {
            if (b.band != kScanBands[i]) continue;
            if (fs * 1e6 < 256e3 * b.scsKhz * 0.97) continue;   // this rate cannot hold the band's SSB
            add(dect2::nr::gscnToHz(b.gscnLo) / 1e6, dect2::nr::gscnToHz(b.gscnHi) / 1e6, b.scsKhz);
        }
    }
    // a custom range: the GSCN points in it (38.101-1 5.4.3), stepped for the wider 30 kHz SSB so both spacings fit in every step
    if (S.custom && S.customHi > S.customLo && fs * 1e6 >= 256e3 * 15 * 0.97) {
        const int g0 = dect2::nr::hzToGscn(S.customLo * 1e6), g1 = dect2::nr::hzToGscn(S.customHi * 1e6);
        if (g0 > 0 && g1 >= g0) add(dect2::nr::gscnToHz(g0) / 1e6, dect2::nr::gscnToHz(g1) / 1e6, fs * 1e6 >= 256e3 * 30 * 0.97 ? 30 : 15);
    }
}

void scanBar(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Bands");
    for (int i = 0; i < kNb; i++) {
        flowNext(6 * gUi);
        char l[16]; snprintf(l, sizeof l, "n%d##nb%d", kScanBands[i], i);
        ImGui::Checkbox(l, &S.bands[i]);
        if (ImGui::IsItemHovered()) if (const dect2::nr::NrBand* b = dect2::nr::nrBandInfo(kScanBands[i]))
            ImGui::SetTooltip("n%d: %.0f - %.0f MHz (%s), %d kHz SSB, GSCN %d - %d", b->band, b->dlLowMhz, b->dlHighMhz, b->region, b->scsKhz, b->gscnLo, b->gscnHi);
    }
    flowNext(10 * gUi);
    ImGui::Checkbox("Custom##nbc", &S.custom);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Also scan a range of your own, start and end in MHz, over the NR sync raster (GSCN)");
    if (S.custom) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); ImGui::InputFloat("##nblo", &S.customLo, 0, 0, "%.1f");
        ImGui::SameLine(); ImGui::TextUnformatted("-"); ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); ImGui::InputFloat("MHz##nbhi", &S.customHi, 0, 0, "%.1f");
        S.customLo = std::clamp(S.customLo, 1.0f, 7250.0f); S.customHi = std::clamp(S.customHi, 1.0f, 7250.0f);
    }
    flowNext(10 * gUi);
    if (!S.scanning) {
        if (runningButton(a, tr("Scan"), true)) { makePlan(a); S.step = 0; S.stepAt = 0; S.scanning = !S.plan.empty(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Step the radio over the chosen bands; every sync raster point (GSCN) inside each step is searched for SS/PBCH blocks. Receive only.");
    } else {
        if (ImGui::SmallButton("Stop scan")) S.scanning = false;
        ImGui::SameLine(); ImGui::TextDisabled("%zu / %zu", S.step + 1, S.plan.size());
    }
    flowNext(10 * gUi);
    if (ImGui::SmallButton("Clear")) a.engine.nr().clearCells();
    flowNext(10 * gUi);
    ImGui::TextDisabled("Hide bands"); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); ImGui::InputTextWithHint("##nbhide", "e.g. 41 79", S.hide, sizeof S.hide);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("NR band numbers whose cells are left out of the list and the export (bands overlap, so a frequency can be named for the wrong band).");
    flowNext(10 * gUi);
    ImGui::TextDisabled("Min RSRP"); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); ImGui::SliderFloat("##nbmin", &S.minRsrp, -120, -20, S.minRsrp <= -120 ? "off" : "%.0f dBFS");
    flowNext(10 * gUi);
    ImGui::Checkbox("MIB read##nbmib", &S.mibOnly);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lists only cells whose PBCH decoded (not those known from the synchronisation signals alone).");
    flowNext(10 * gUi);
    {
        static std::string note;
        resexp::Table t; t.cols = {"time", "frequency_hz", "mode", "band", "gscn", "pci", "ssb_khz", "beams", "sfn", "sib1_scheduled", "rsrp_db", "rsrq_db", "sinr_db"};
        const std::string now = resexp::stamp();
        for (const auto& c : a.rx.nr.cells) {
            if (!dect2::nrCellPasses(c, filterNow())) continue;
            t.add({now, resexp::num(c.freqHz, 0), "5G NR", bandText(c), c.gscn > 0 ? std::to_string(c.gscn) : "", std::to_string(c.pci), std::to_string(c.scsKhz),
                   c.hasMib ? std::to_string(c.beams.size()) : "", c.hasMib ? std::to_string(c.sfn) : "", c.hasMib ? (c.sib1Scheduled ? "yes" : "no") : "",
                   resexp::num(c.rsrpDb, 1), resexp::num(c.rsrqDb, 1), resexp::num(c.sinrDb, 1)});
        }
        resexp::saveButton(t, "nr", note, "svnr");
        if (!note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", note.c_str()); }
    }
    flowEnd();
}

void cellTable(App& a) {
    const dect2::NrTelemetry& t = a.rx.nr;
    const bool wide = ImGui::GetContentRegionAvail().x > 980 * gUi;
    const int nc = wide ? 12 : 7;
    const ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV;
    if (!ImGui::BeginTable("##nrcells", nc, fl, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    if (wide) for (const char* h : {"Band", "GSCN", "MHz", "PCI", "SSB", "Beams", "SFN", "SIB1", "RSRP", "RSRQ", "SINR", "Seen"}) ImGui::TableSetupColumn(h);
    else for (const char* h : {"Band", "GSCN", "PCI", "SSB", "Beams", "RSRP", "SINR"}) ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < t.cells.size(); i++) {
        const auto& c = t.cells[i];
        if (!dect2::nrCellPasses(c, filterNow())) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        char l[64]; snprintf(l, sizeof l, "%s##nc%zu", bandText(c).c_str(), i);
        if (ImGui::Selectable(l, S.sel == (int)i, ImGuiSelectableFlags_SpanAllColumns)) S.sel = (int)i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("PCI %d (N_ID1 %d, N_ID2 %d), %.2f MHz, %d kHz SSB, RSRQ %.1f dB, seen %s ago", c.pci, c.nid1, c.nid2, c.freqHz / 1e6, c.scsKhz, c.rsrqDb, ago(c.lastWall).c_str());
        ImGui::TableNextColumn(); if (c.gscn > 0) ImGui::Text("%d", c.gscn); else ImGui::TextDisabled("-");
        if (wide) { ImGui::TableNextColumn(); ImGui::Text("%.2f", c.freqHz / 1e6); }
        ImGui::TableNextColumn(); ImGui::Text("%d", c.pci);
        ImGui::TableNextColumn(); ImGui::Text("%d kHz", c.scsKhz);
        ImGui::TableNextColumn(); if (c.hasMib) ImGui::Text("%zu / %d", c.beams.size(), c.lmax); else ImGui::TextDisabled("sync only");
        if (wide) { ImGui::TableNextColumn(); if (c.hasMib) ImGui::Text("%d", c.sfn); else ImGui::TextDisabled("-"); }
        if (wide) { ImGui::TableNextColumn(); if (!c.hasMib) ImGui::TextDisabled("-"); else ImGui::TextUnformatted(c.sib1Scheduled ? "yes" : "none"); }
        ImGui::TableNextColumn(); ImGui::Text("%.0f", c.rsrpDb);
        if (wide) { ImGui::TableNextColumn(); ImGui::Text("%.1f", c.rsrqDb); }
        ImGui::TableNextColumn(); ImGui::Text("%.1f", c.sinrDb);
        if (wide) { ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(c.lastWall).c_str()); }
    }
    ImGui::EndTable();
}

void detail(App& a) {
    ImGui::PushTextWrapPos(0);
    struct Pop { ~Pop() { ImGui::PopTextWrapPos(); } } pop_;
    const dect2::NrTelemetry& t = a.rx.nr;
    if (S.sel < 0 || S.sel >= (int)t.cells.size()) { ImGui::TextDisabled("select a cell"); return; }
    const auto& c = t.cells[(size_t)S.sel];
    ImGui::Text("PCI %d (N_ID1 %d, N_ID2 %d), %s %.2f MHz, GSCN %d, %d kHz SSB", c.pci, c.nid1, c.nid2, bandText(c).c_str(), c.freqHz / 1e6, c.gscn, c.scsKhz);
    ImGui::Text("SS-RSRP %.1f dBFS, SS-RSRQ %.1f dB, SS-SINR %.1f dB, offset %+.0f Hz", c.rsrpDb, c.rsrqDb, c.sinrDb, c.cfoHz);
    if (!c.hasMib) { ImGui::TextDisabled("MIB not decoded yet (PSS / SSS only)"); return; }
    ImGui::Text("MIB: SFN %d, half frame %d, common subcarrier spacing %d kHz, k_SSB %d, DMRS type A position %d, %s, intra-frequency reselection %s", c.sfn, c.hrf ? 1 : 0, c.scsCommonKhz,
                c.kSsb, c.dmrsTypeAPos, c.barred ? "barred" : "not barred", c.intraFreqResel ? "allowed" : "not allowed");
    if (c.sib1Scheduled) {
        if (c.cs0Rbs) ImGui::Text("SIB1: CORESET#0 index %d (%d RBs, %d symbol%s, %d RB offset), search space 0 index %d (SIB1 is not decoded)", c.coreset0, c.cs0Rbs, c.cs0Syms, c.cs0Syms == 1 ? "" : "s",
                                  c.cs0OffRb, c.ss0);
        else ImGui::Text("SIB1: CORESET#0 index %d, search space 0 index %d (SIB1 is not decoded)", c.coreset0, c.ss0);
    } else ImGui::Text("No SIB1 on this SSB (k_SSB %d > 23): an SSB for measurements or for a non-standalone (EN-DC) carrier", c.kSsb);
    ImGui::Text("Strongest SSB %d of Lmax %d, %llu PBCH decoded", c.ssbIdx, c.lmax, (unsigned long long)c.mibs);
}

void beamsView(App& a) {
    const dect2::NrTelemetry& t = a.rx.nr;
    if (!ImGui::BeginChild("##nrbeams")) { ImGui::EndChild(); return; }
    bool any = false;
    for (const auto& c : t.cells) {
        if (c.beams.empty()) continue;
        any = true;
        char h[128]; snprintf(h, sizeof h, "PCI %d, %s %.2f MHz: %zu beam%s##b%d_%d", c.pci, bandText(c).c_str(), c.freqHz / 1e6, c.beams.size(), c.beams.size() == 1 ? "" : "s", c.pci, c.gscn);
        if (!ImGui::TreeNodeEx(h, ImGuiTreeNodeFlags_DefaultOpen)) continue;
        float top = -300;
        for (const auto& b : c.beams) top = std::max(top, b.rsrpDb);
        for (const auto& b : c.beams) {
            ImGui::Text("SSB %d", b.ssbIdx); ImGui::SameLine(6 * ImGui::GetFontSize());
            const float frac = std::max(0.02f, std::min(1.0f, (b.rsrpDb - (top - 30)) / 30));
            char o[32]; snprintf(o, sizeof o, "%.1f dBFS", b.rsrpDb);
            ImGui::ProgressBar(frac, ImVec2(std::min(20 * ImGui::GetFontSize(), ImGui::GetContentRegionAvail().x), 0), o);
        }
        ImGui::TreePop();
    }
    if (!any) ImGui::TextDisabled("no beam identified yet (the SSB index comes with the MIB)");
    ImGui::EndChild();
}

void mapView(App& a) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Tower positions are looked up by their cell identity (OpenCelliD, as in Cell towers), which an NR cell sends only in SIB1; SIB1 is not decoded yet, so only the receiver is shown.");
    ImGui::PopTextWrapPos();
    const ImVec2 sz = ImGui::GetContentRegionAvail();
    if (sz.y < 40) return;
    const double rLat = plat::prefs().getD("adsbLat", 0), rLon = plat::prefs().getD("adsbLon", 0);
    const bool haveRx = rLat != 0 || rLon != 0;
    if (!S.fitted && haveRx) { S.map.lat = rLat; S.map.lon = rLon; S.map.zoom = 12; S.fitted = true; }
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    adsbmap::draw(S.map, sz);
    adsbmap::fullCorner(p0, sz);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), true);
    if (haveRx) {
        const ImVec2 q = adsbmap::project(rLat, rLon);
        dl->AddRectFilled(ImVec2(q.x - 4 * gUi, q.y - 4 * gUi), ImVec2(q.x + 4 * gUi, q.y + 4 * gUi), IM_COL32(255, 255, 255, 230));
        dl->AddText(ImVec2(q.x + 7 * gUi, q.y - 7 * gUi), IM_COL32(255, 255, 255, 230), "receiver");
    }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + sz.y - ImGui::GetTextLineHeight() - 6 * gUi));
    adsbmap::legend(sz.x, "%zu NR cell%s heard, none placed (no SIB1)", a.rx.nr.cells.size(), a.rx.nr.cells.size() == 1 ? "" : "s");
}

void logView(App& a) {
    for (const auto& e : a.rx.nr.events) ImGui::TextUnformatted(e.text.c_str());
    if (a.rx.nr.events.empty()) ImGui::TextDisabled("nothing yet");
}

void tick(App& a) {
    if (!S.scanning) return;
    if (!a.engine.running()) { S.scanning = false; return; }
    const double now = ImGui::GetTime();
    // a step ends once the dwell is over and the receiver has finished two blocks at this centre (a block cut off by the retune is thrown
    // away), or after 30 s
    const dect2::NrTelemetry& t = a.rx.nr;
    const bool done = t.blocks >= S.blocksAt + 2;
    if (S.stepAt == 0 || (now - S.stepAt >= S.dwell && done) || now - S.stepAt >= 30) {
        if (S.stepAt != 0) S.step++;
        if (S.step >= S.plan.size()) { S.scanning = false; return; }
        tuneFreq(a, S.plan[S.step]);
        S.stepAt = now;
        S.blocksAt = t.blocks;
    }
}

void tab(App& a) {
    scanBar(a);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Receive only: the public SS/PBCH blocks every 5G NR cell broadcasts to all phones (cell id, beams, MIB). Nothing addressed to a phone is decoded, nothing is decrypted.");
    ImGui::PopTextWrapPos();
    subNav("nrv", S.view, {"Cells", "Beams", "Map", "Log"});
    if (S.view == 2) { mapView(a); return; }
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to find the NR cells around you"); return; }
    if (S.view == 0) {
        const float h = ImGui::GetContentRegionAvail().y;
        if (ImGui::BeginChild("##nrct", ImVec2(0, h * 0.7f))) cellTable(a);
        ImGui::EndChild();
        detail(a);
    } else if (S.view == 1) beamsView(a);
    else logView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    for (const auto& c : a.rx.nr.cells) ImGui::Text("%-4s PCI %4d %.0f", bandText(c).c_str(), c.pci, c.rsrpDb);
    if (a.rx.nr.cells.empty()) ImGui::TextDisabled("none yet");
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::NrTelemetry& t = a.rx.nr;
    ImGui::Text("%s", dect2::nrSummary(t).c_str());
    ImGui::Text("%llu blocks searched, %llu PBCH decoded, %llu failed", (unsigned long long)t.blocks, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    ImGui::Text("input %.2f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    if (t.lastGscnLo > 0) ImGui::Text("last block: GSCN %d - %d, %d candidate%s", t.lastGscnLo, t.lastGscnHi, t.lastCandidates, t.lastCandidates == 1 ? "" : "s");
    else ImGui::Text("last block: %d candidate%s (the dial frequency)", t.lastCandidates, t.lastCandidates == 1 ? "" : "s");
    if (t.inputRate > 0 && t.inputRate < 7.68e6 * 0.97) ImGui::TextDisabled("below 7.68 Msps: only 15 kHz SSBs (n1, n3, n7, n8, n20, n28, n38) are searched");
}

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::nrSummary(a.rx.nr).c_str() : (a.engine.running() ? "starting" : "stopped"));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "5G NR cells";
    if (live(a)) l2 = dect2::nrSummary(a.rx.nr);
}

} // namespace

extern const ModeUi kNrUi;
const ModeUi kNrUi = {
    .sideTitle = "NR CELLS",
    .tabName = "NR cells",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
