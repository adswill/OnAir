// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NB-IoT cells screens: the cell table (operator, band, EARFCN, PCI, operating mode, eNB / sector, TAC, level, last seen), the details of the
// selected cell and a band scan over the 100 kHz LTE raster. Only what cells broadcast to every device is shown: NPSS / NSSS, MIB-NB, SIB1-NB.
// Receive only: nothing addressed to a device is decoded.
#include "app.h"
#include "state/results_export.h"
#include "dect2/cell/cell_rx.h"
#include "dect2/nbiot/nbiot_rx.h"
#include "dect2/nbiot/nbiot_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

constexpr int kMaxBands = 18;

struct State {
    int view = 0;                 // 0 cells, 1 log
    int sel = -1;                 // selected cell (index in cells)
    bool bands[kMaxBands] = {true, true, true, false, false};   // B20, B8, B3
    bool custom = false;          // a custom range instead of (with) the bands
    float customLo = 790, customHi = 830;   // MHz
    bool scanning = false;
    std::vector<double> plan;     // centres, MHz
    size_t step = 0;
    double stepAt = 0;
    float dwell = 3.0f;
    uint64_t blocksAt = 0;        // receiver blocks done when this step was tuned
};
State S;

bool live(const App& a) { return a.rx.standard == 70; }   // the engine reports its standard code minus one

std::string ago(int64_t t) {
    if (t <= 0) return "-";
    const long d = (long)(std::time(nullptr) - t);
    char b[32];
    if (d < 60) snprintf(b, sizeof b, "%ld s", std::max(0L, d)); else if (d < 3600) snprintf(b, sizeof b, "%ld min", d / 60); else snprintf(b, sizeof b, "%ld h", d / 3600);
    return b;
}

std::string opText(const dect2::NbCell& c) {
    if (!c.hasSib1 || c.plmns.empty()) return "-";
    return c.op.empty() ? c.plmns[0] : c.plmns[0] + " " + c.op;
}

void makePlan(const App& a) {
    S.plan.clear();
    const double span = std::max(1.0, a.tune.sampleRate / 1e6 * 0.7);   // the part of the sample band searched whole
    auto range = [&](double lo, double hi) { for (double c = lo + span / 2; c - span / 2 < hi; c += span) S.plan.push_back(std::round(c * 10) / 10); };
    const auto& list = dect2::nbiotBands();
    for (int i = 0; i < kMaxBands && i < (int)list.size(); i++) {
        if (!S.bands[i]) continue;
        if (const dect2::LteBand* b = dect2::lteBandInfo(list[(size_t)i])) range(b->dlLowMhz, b->dlHighMhz);
    }
    if (S.custom && S.customHi > S.customLo) range(S.customLo, S.customHi);
}

void scanBar(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Bands");
    const auto& list = dect2::nbiotBands();
    for (int i = 0; i < kMaxBands && i < (int)list.size(); i++) {
        flowNext(6 * gUi);
        char l[24]; snprintf(l, sizeof l, "B%d##nb%d", list[(size_t)i], i);
        ImGui::Checkbox(l, &S.bands[i]);
        if (ImGui::IsItemHovered()) if (const dect2::LteBand* b = dect2::lteBandInfo(list[(size_t)i])) ImGui::SetTooltip("Band %d: %.0f - %.0f MHz (%s)", b->band, b->dlLowMhz, b->dlHighMhz, b->region);
    }
    flowNext(10 * gUi);
    ImGui::Checkbox("Custom##nbc", &S.custom);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Also scan a range of your own, start and end in MHz, on the 100 kHz raster");
    if (S.custom) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); ImGui::InputFloat("##nblo", &S.customLo, 0, 0, "%.1f");
        ImGui::SameLine(); ImGui::TextUnformatted("-"); ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); ImGui::InputFloat("MHz##nbhi", &S.customHi, 0, 0, "%.1f");
        S.customLo = std::clamp(S.customLo, 1.0f, 7250.0f); S.customHi = std::clamp(S.customHi, 1.0f, 7250.0f);
    }
    flowNext(10 * gUi);
    if (!S.scanning) {
        if (runningButton(a, tr("Scan"), true)) { makePlan(a); S.step = 0; S.stepAt = 0; S.scanning = !S.plan.empty(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Step the radio over the chosen bands; every 100 kHz raster point in each step is searched for an NB-IoT carrier. Receive only.");
    } else {
        if (ImGui::SmallButton("Stop scan")) S.scanning = false;
        ImGui::SameLine(); ImGui::TextDisabled("%zu / %zu", S.step + 1, S.plan.size());
    }
    flowNext(10 * gUi);
    if (ImGui::SmallButton("Clear") && live(a)) a.engine.nbiot().clearCells();
    flowNext(10 * gUi);
    {
        static std::string note;
        resexp::Table t; t.cols = {"time", "frequency_hz", "mode", "operator", "band", "earfcn", "pci", "operating_mode", "enb", "sector", "tac", "level_db", "snr_db"};
        const std::string now = resexp::stamp();
        for (const auto& c : a.rx.nbiot.cells)
            t.add({now, resexp::num(c.freqHz, 0), "NB-IoT", opText(c), c.band ? std::to_string(c.band) : "", c.earfcn >= 0 ? std::to_string(c.earfcn) : "", std::to_string(c.pci),
                   c.hasMib ? dect2::nbiotModeName(c.mode) : "", c.hasSib1 ? std::to_string(c.enb) : "", c.hasSib1 ? std::to_string(c.sector) : "", c.hasSib1 ? std::to_string(c.tac) : "",
                   resexp::num(c.levelDb, 1), resexp::num(c.snrDb, 1)});
        resexp::saveButton(t, "nbiot", note, "svnbiot");
        if (!note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", note.c_str()); }
    }
    flowEnd();
}

void cellTable(App& a) {
    const dect2::NbiotTelemetry& t = a.rx.nbiot;
    const bool wide = ImGui::GetContentRegionAvail().x > 900 * gUi;
    const int nc = wide ? 11 : 7;
    const ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV;
    if (!ImGui::BeginTable("##nbcells", nc, fl, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    if (wide) for (const char* h : {"Operator", "Band", "EARFCN", "MHz", "PCI", "Mode", "eNB / sector", "TAC", "Level", "SNR", "Seen"}) ImGui::TableSetupColumn(h);
    else for (const char* h : {"Operator", "Band", "EARFCN", "PCI", "Mode", "eNB / sector", "Level"}) ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < t.cells.size(); i++) {
        const auto& c = t.cells[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        char l[96]; snprintf(l, sizeof l, "%s##nc%zu", wide ? opText(c).c_str() : (c.hasSib1 && !c.plmns.empty() ? c.plmns[0].c_str() : "-"), i);
        if (ImGui::Selectable(l, S.sel == (int)i, ImGuiSelectableFlags_SpanAllColumns)) S.sel = (int)i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s, %.1f MHz, TAC %u, SNR %.1f dB, seen %s ago", opText(c).c_str(), c.freqHz / 1e6, c.tac, c.snrDb, ago(c.lastWall).c_str());
        ImGui::TableNextColumn(); if (c.band) ImGui::Text("B%d", c.band); else ImGui::TextDisabled("?");
        ImGui::TableNextColumn(); if (c.earfcn >= 0) ImGui::Text("%d", c.earfcn); else ImGui::TextDisabled("-");
        if (wide) { ImGui::TableNextColumn(); ImGui::Text("%.1f", c.freqHz / 1e6); }
        ImGui::TableNextColumn(); ImGui::Text("%d", c.pci);
        ImGui::TableNextColumn(); if (c.hasMib) ImGui::TextUnformatted(dect2::nbiotModeName(c.mode)); else ImGui::TextDisabled("sync only");
        ImGui::TableNextColumn(); if (c.hasSib1) ImGui::Text("%u/%d", c.enb, c.sector); else ImGui::TextDisabled("-");
        if (wide) { ImGui::TableNextColumn(); if (c.hasSib1) ImGui::Text("%u", c.tac); else ImGui::TextDisabled("-"); }
        ImGui::TableNextColumn(); ImGui::Text("%.0f", c.levelDb);
        if (wide) { ImGui::TableNextColumn(); ImGui::Text("%.1f", c.snrDb); }
        if (wide) { ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(c.lastWall).c_str()); }
    }
    ImGui::EndTable();
    if (t.cells.empty()) ImGui::TextDisabled("no NB-IoT carrier found yet: it takes a second or more per block; tune to a band with NB-IoT or press Scan");
}

void detail(App& a) {
    const dect2::NbiotTelemetry& t = a.rx.nbiot;
    if (S.sel < 0 || S.sel >= (int)t.cells.size()) { ImGui::TextDisabled("select a cell for its details"); return; }
    const dect2::NbCell& c = t.cells[(size_t)S.sel];
    auto row = [&](const char* k, const char* fmt, ...) {
        ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
        char b[512];
        va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
        ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
    };
    row("cell", "PCI %d at %.1f MHz%s, %+.1f kHz from the raster", c.pci, c.freqHz / 1e6, c.earfcn >= 0 ? (" (EARFCN " + std::to_string(c.earfcn) + ")").c_str() : "", c.offsetKhz);
    if (c.hasMib) row("operating mode", "%s, %d NRS port%s, SFN %d, SIB1 schedule %d, value tag %d%s", dect2::nbiotModeName(c.mode), c.ports, c.ports == 1 ? "" : "s", c.sfn, c.sched, c.valueTag, c.ab ? ", access barring on" : "");
    else row("operating mode", "not read yet (the MIB-NB takes about 640 ms of a clear signal)");
    if (c.hasSib1) {
        std::string pl;
        for (const auto& p : c.plmns) pl += (pl.empty() ? "" : ", ") + p;
        row("operator", "%s %s", pl.c_str(), c.op.c_str());
        row("area / cell", "TAC %u, cell identity %u (eNB %u, sector %d)", c.tac, c.cellId, c.enb, c.sector);
        row("access", "%s%s, band %d, q-RxLevMin %d dBm%s", c.barred ? "barred" : "not barred", c.reservedOp ? ", reserved for operator use" : "", c.band, c.qRxLevMinDbm,
            c.pMaxDbm > -99 ? (", p-Max " + std::to_string(c.pMaxDbm) + " dBm").c_str() : "");
    } else row("operator", "not read yet (SIB1-NB repeats over 2.56 s)");
    row("signal", "level %.1f dBFS, SNR %.1f dB, offset %+.1f kHz, clock %+.1f ppm, detection %.1f", c.levelDb, c.snrDb, c.cfoHz / 1e3, c.clockPpm, c.npssScore);
}

void logView(App& a) {
    for (const auto& e : a.rx.nbiot.events) ImGui::TextUnformatted(e.text.c_str());
    if (a.rx.nbiot.events.empty()) ImGui::TextDisabled("nothing yet");
}

void tick(App& a) {
    if (!S.scanning) return;
    if (!a.engine.running() || !live(a)) { if (!a.engine.running()) S.scanning = false; return; }
    const double now = ImGui::GetTime();
    // a step ends once the dwell is over and the receiver has finished two blocks at this centre (a block cut off by the retune is thrown away),
    // or after 40 s
    const dect2::NbiotTelemetry& t = a.rx.nbiot;
    const bool done = t.blocks >= S.blocksAt + 2;
    if (S.stepAt == 0 || (now - S.stepAt >= S.dwell && done) || now - S.stepAt >= 40) {
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
    ImGui::TextDisabled("Receive only: the public information every NB-IoT cell broadcasts to all devices (cell id, operating mode, operator, band). Nothing addressed to a device "
                        "is decoded: no paging, no data, no device identities. Receiving broadcasts is legal in most places, but laws differ: check the rules where you live.");
    ImGui::PopTextWrapPos();
    subNav("nbv", S.view, {"Cells", "Log"});
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to find the NB-IoT cells around you"); return; }
    if (S.view == 0) {
        const float h = ImGui::GetContentRegionAvail().y;
        if (ImGui::BeginChild("##nbct", ImVec2(0, h * 0.62f))) cellTable(a);
        ImGui::EndChild();
        detail(a);
    } else logView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    for (const auto& c : a.rx.nbiot.cells) ImGui::Text("%-14s PCI %3d %.0f", opText(c).c_str(), c.pci, c.levelDb);
    if (a.rx.nbiot.cells.empty()) ImGui::TextDisabled("none yet");
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::NbiotTelemetry& t = a.rx.nbiot;
    ImGui::Text("%s", dect2::nbiotSummary(t).c_str());
    ImGui::Text("%llu blocks searched, %llu SIB1-NB messages, %llu failed", (unsigned long long)t.blocks, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    ImGui::Text("input %.2f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
}

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::nbiotSummary(a.rx.nbiot).c_str() : (a.engine.running() ? "starting" : "stopped"));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "NB-IoT cells";
    if (live(a)) l2 = dect2::nbiotSummary(a.rx.nbiot);
}

} // namespace

extern const ModeUi kNbiotUi;
const ModeUi kNbiotUi = {
    .sideTitle = "NB-IOT",
    .tabName = "NB-IoT",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
