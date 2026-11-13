// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The merged Scanner view (View > Scanner): the memory table is also the scan list. Each memory is a row (frequency, mode, name, tone, group, last hit,
// hit count, lockout, priority, star); the control bar scans all / a group / a range / the starred rows. A scan runs on the scanner's machine
// (scanner_ui.cpp, scan_list.h); what the scan covers and what a hit writes back is scan_plan.h (unit tested). The scan fields (lockout, priority,
// hits, last hit, tone) are kept in the memory's options map, so memories.json needs no new columns. The classic Scanner, Memory panel and the
// tone decoder mode stay reachable from the buttons at the top.
#include <string>
#include "../app.h"
#include "../memory/memory_ui.h"
#include "../ui/ui_common.h"
#include "fast_scan_ui.h"
#include "scan_plan.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <set>

namespace {

using dect2::Memory;
using scanplan::Row;

struct V {
    int scope = 0;                       // scanplan::Scope
    char group[48] = "";
    double lo = 118, hi = 137, stepKhz = 25;
    bool stepped = false;                // a range scan steps through the range at stepKhz instead of using the table rows in it
    bool skipLocked = true, priorityCheck = true;
    float squelch = 8;
    double dwell = 2, hold = 0, resume = 3, prioEvery = 5;
    std::vector<uint64_t> idOf;          // list entry -> memory id (0: a stepped frequency)
    uint64_t hitSeq = 0, liveRow = 0;
    std::string msg, liveTone;
    uint64_t toneRow = 0;                // the row the live tone belongs to
    bool loaded = false;
} v;

Row rowOf(const Memory& m) {
    Row r;
    r.mhz = (double)m.freqHz / 1e6; r.mode = m.mode; r.name = m.name; r.group = m.group; r.dwellSec = m.dwellSec; r.star = m.favourite;
    scanplan::readRow(m.options, r);
    return r;
}

void commit(uint64_t id, const Row& r) {   // a row's scan fields back to its memory
    dect2::MemoryBank& b = memoryBank();
    const Memory* p = b.find(id);
    if (!p) return;
    Memory m = *p;
    scanplan::writeRow(m.options, r);
    m.favourite = r.star;
    b.update(id, m);
}

void load() {
    if (v.loaded) return;
    v.loaded = true;
    plat::Prefs& p = plat::prefs();
    v.scope = (int)std::max(0L, std::min(4L, p.getI("svScope", 0)));
    v.squelch = (float)p.getD("svSquelch", 8); v.dwell = p.getD("svDwell", 2); v.hold = p.getD("svHold", 0);
    v.resume = p.getD("svResume", 3); v.prioEvery = p.getD("svPrio", 5); v.stepKhz = p.getD("svStep", 25);
    v.lo = p.getD("svLo", 118); v.hi = p.getD("svHi", 137);
    v.skipLocked = p.getB("svSkip", true); v.priorityCheck = p.getB("svPrioOn", true); v.stepped = p.getB("svStepped", false);
}
void savePrefsV() {
    plat::Prefs& p = plat::prefs();
    p.setI("svScope", v.scope); p.setD("svSquelch", v.squelch); p.setD("svDwell", v.dwell); p.setD("svHold", v.hold);
    p.setD("svResume", v.resume); p.setD("svPrio", v.prioEvery); p.setD("svStep", v.stepKhz);
    p.setD("svLo", v.lo); p.setD("svHi", v.hi); p.setB("svSkip", v.skipLocked); p.setB("svPrioOn", v.priorityCheck); p.setB("svStepped", v.stepped);
}

scanplan::Params params() {
    scanplan::Params p;
    p.scope = (scanplan::Scope)v.scope; p.group = v.group; p.loMhz = v.lo; p.hiMhz = v.hi;
    p.skipLocked = v.skipLocked; p.priorityCheck = v.priorityCheck; p.defaultDwell = v.dwell;
    return p;
}

std::vector<Row> tableRows(std::vector<uint64_t>& ids) {
    std::vector<Row> rows;
    ids.clear();
    for (const Memory& m : memoryBank().all()) { rows.push_back(rowOf(m)); ids.push_back(m.id); }
    return rows;
}

void startScan(App& a) {
    std::vector<uint64_t> ids;
    std::vector<Row> rows = tableRows(ids);
    std::vector<int> rowIdx;
    const scanplan::Params p = params();
    std::vector<ScanSeed> seeds;
    v.idOf.clear();
    if (p.scope == scanplan::Scope::Range && v.stepped) {   // a stepped sweep of the range in the receiver's current mode
        std::string mode;
        for (int i = 0; i < kNumModes; i++) if (modeSelected(a, kModes[i])) { mode = scanModeKey(kModes[i]); break; }
        for (double f : scanplan::rangeSteps(v.lo, v.hi, v.stepKhz)) { ScanSeed s; s.mhz = f; s.mode = mode.empty() ? "fm" : mode; s.dwellSec = v.dwell; seeds.push_back(s); v.idOf.push_back(0); }
    } else {
        for (const scanlist::Entry& e : scanplan::toEntries(rows, p, rowIdx)) {
            ScanSeed s; s.mhz = e.mhz; s.mode = e.mode; s.name = e.name; s.dwellSec = e.dwellSec; s.priority = e.priority;
            seeds.push_back(s);
        }
        for (int i : rowIdx) v.idOf.push_back(ids[i]);
    }
    if (seeds.empty()) { v.msg = tr("Nothing to scan: no rows in this scope (or all locked out)."); return; }
    v.msg.clear();
    scannerTuning(v.squelch, v.resume, v.hold, v.priorityCheck ? v.prioEvery : 0);
    v.hitSeq = scannerLive().hitSeq;
    scannerLoadList(a, seeds, false);
}

// what a hit writes back: the counters, and the tone when one is read
void onHit(const ScanLive& l) {
    if (l.entry < 0 || l.entry >= (int)v.idOf.size() || !v.idOf[l.entry]) return;
    const uint64_t id = v.idOf[l.entry];
    const Memory* m = memoryBank().find(id);
    if (!m) return;
    Row r = rowOf(*m);
    scanplan::recordHit(r, (long long)time(nullptr));
    commit(id, r);
    memoryChanged();
}

// The CTCSS / DCS reading while a channel is open. It exists when the tone decoder runs on this audio (the Tone decoders mode); other modes give none.
void pollTone(App& a, const ScanLive& l) {
    v.liveTone.clear(); v.toneRow = 0;
    if (!l.running || l.state < 2 || l.entry < 0 || l.entry >= (int)v.idOf.size() || !v.idOf[l.entry]) return;
    const dect2::TonesTelemetry& t = a.rx.tones;
    v.liveTone = scanplan::toneText(t.ctcssIdx >= 0 ? t.ctcssHz : 0, t.dcsCode, t.dcsInv);
    if (!v.liveTone.empty()) v.toneRow = v.idOf[l.entry];
}

void saveTone(uint64_t id, const std::string& tone) {
    const Memory* m = memoryBank().find(id);
    if (!m) return;
    Row r = rowOf(*m);
    r.tone = tone;
    commit(id, r);
    memoryChanged();
}

void addCurrent(App& a) {
    Memory m = memoryCapture(a);
    m.name = memorySuggestName(a);
    dect2::MemoryAddResult r = memoryBank().add(m);
    if (r.what == dect2::MemoryAdd::Added) { memoryChanged(); v.msg = tr("Added the current frequency."); }
    else v.msg = tr(r.what == dect2::MemoryAdd::Duplicate ? "This frequency and mode is already in the table." : "The table is full.");
}

void openTonesMode(App& a) {
    for (int i = 0; i < kNumModes; i++)
        if (std::strstr(kModes[i].name, "Tone decoders")) { selectMode(a, kModes[i].family, kModes[i].subId); return; }
}

void scopeCombo(bool& ch) {
    ImGui::SetNextItemWidth(110 * gUi);
    static const char* const kScope[5] = {N_("Scan all"), N_("Scan group"), N_("Scan range"), N_("Scan starred"), N_("Fast scan")};
    if (ImGui::BeginCombo("##scope", tr(kScope[v.scope]))) {
        for (int i = 0; i < 5; i++) if (ImGui::Selectable(tr(kScope[i]), i == v.scope)) { v.scope = i; ch = true; }
        ImGui::EndCombo();
    }
}

void controlBar(App& a, const ScanLive& l) {
    if (v.scope == (int)scanplan::Scope::Fast) {   // the FFT fast scan: its own controls; the scope box stays so one can go back
        bool c2 = false;
        ImGui::BeginDisabled(fastscanui::running());
        scopeCombo(c2);
        ImGui::EndDisabled();
        if (c2) savePrefsV();
        if (!fastscanui::running() && l.running) scannerControl(a, 0);
        fastscanui::controls(a);
        return;
    }
    if (fastscanui::running()) fastscanui::stop();
    const bool run = l.running;
    if (ImGui::Button(tr(run ? "Stop" : "Start scan"))) { if (run) scannerControl(a, 0); else startScan(a); }
    ImGui::SameLine();
    ImGui::BeginDisabled(!run);
    if (ImGui::Button(tr("Skip"))) scannerControl(a, 1);
    ImGui::SameLine();
    if (ImGui::Button(tr("Lock out"))) {
        if (l.entry >= 0 && l.entry < (int)v.idOf.size() && v.idOf[l.entry]) if (const Memory* m = memoryBank().find(v.idOf[l.entry])) { Row r = rowOf(*m); r.lockout = true; commit(v.idOf[l.entry], r); memoryChanged(); }
        scannerControl(a, 2);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (run) {
        static const char* const kSt[4] = {N_("idle"), N_("listening"), N_("ACTIVE"), N_("hold")};
        const ImVec4 col = l.state == 2 ? pal::okGreen() : l.state == 3 ? pal::warnAmber() : pal::grey();
        ImGui::TextColored(col, tr("%s  %.4f MHz  %s%s  %.1f dB"), tr(kSt[std::min(3, l.state)]), l.mhz, l.name.c_str(), l.priority ? tr("  (priority)") : "", l.levelDb);
    } else if (!v.msg.empty()) ImGui::TextDisabled("%s", v.msg.c_str());

    bool ch = false;
    ImGui::BeginDisabled(run);
    ImGui::SetNextItemWidth(110 * gUi);
    scopeCombo(ch);
    if (v.scope == 1) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(150 * gUi);
        if (ImGui::BeginCombo("##grp", v.group[0] ? v.group : tr("(pick a group)"))) {
            std::set<std::string> gs;
            for (const Memory& m : memoryBank().all()) if (!m.group.empty()) gs.insert(m.group);
            for (const std::string& g : gs) if (ImGui::Selectable(g.c_str(), g == v.group)) { snprintf(v.group, sizeof v.group, "%s", g.c_str()); ch = true; }
            ImGui::EndCombo();
        }
    }
    if (v.scope == 2) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); ch |= ImGui::InputDouble("##lo", &v.lo, 0, 0, "%.4f");
        ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); ch |= ImGui::InputDouble("##hi", &v.hi, 0, 0, "%.4f");
        ImGui::SameLine(); ch |= ImGui::Checkbox(tr("step"), &v.stepped);
        if (v.stepped) { ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); if (ImGui::InputDouble("kHz##step", &v.stepKhz, 0, 0, "%.3f")) { v.stepKhz = std::max(0.1, v.stepKhz); ch = true; } }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Step through the range in the receiver's current mode, instead of scanning the table rows inside it."));
    }
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(120 * gUi); ch |= ImGui::SliderFloat("##sq", &v.squelch, 2, 30, tr("squelch %.0f dB"));
    ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); if (ImGui::InputDouble("dwell s", &v.dwell, 0, 0, "%.1f")) { v.dwell = std::max(0.5, v.dwell); ch = true; }
    ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); if (ImGui::InputDouble("hold s", &v.hold, 0, 0, "%.0f")) { v.hold = std::max(0.0, v.hold); ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The longest stay on one active channel. 0: stay while it is active."));
    ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); if (ImGui::InputDouble("resume s", &v.resume, 0, 0, "%.1f")) { v.resume = std::max(0.0, v.resume); ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("After the activity ends, wait this long before moving on."));
    ImGui::BeginDisabled(run);
    ImGui::Checkbox(tr("skip locked-out"), &v.skipLocked);
    ImGui::EndDisabled();
    ImGui::SameLine(); ch |= ImGui::Checkbox(tr("priority check"), &v.priorityCheck);
    if (v.priorityCheck) { ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); if (ImGui::InputDouble("every s", &v.prioEvery, 0, 0, "%.0f")) { v.prioEvery = std::max(1.0, v.prioEvery); ch = true; } }
    if (ch) { savePrefsV(); scannerTuning(v.squelch, v.resume, v.hold, v.priorityCheck ? v.prioEvery : 0); }
}

void table(App& a, const ScanLive& l) {
    std::vector<uint64_t> ids;
    std::vector<Row> rows = tableRows(ids);
    const scanplan::Params sp = params();
    const std::vector<int> inScan = scanplan::pick(rows, sp);
    const std::set<int> covered(inScan.begin(), inScan.end());
    const uint64_t liveId = (l.running && l.entry >= 0 && l.entry < (int)v.idOf.size()) ? v.idOf[l.entry] : 0;
    uint64_t del = 0, edit = 0, tune = 0;
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##svtable", 11, tf, ImVec2(0, std::max(160.f * gUi, ImGui::GetContentRegionAvail().y)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("MHz"), 0, 90 * gUi);
        ImGui::TableSetupColumn(tr("Mode"), 0, 90 * gUi);
        ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Tone"), 0, 150 * gUi);
        ImGui::TableSetupColumn(tr("Group"), 0, 90 * gUi);
        ImGui::TableSetupColumn(tr("Last hit"), 0, 110 * gUi);
        ImGui::TableSetupColumn(tr("Hits"), 0, 40 * gUi);
        ImGui::TableSetupColumn(tr("Lock"), 0, 36 * gUi);
        ImGui::TableSetupColumn(tr("Prio"), 0, 36 * gUi);
        ImGui::TableSetupColumn(tr("Star"), 0, 36 * gUi);
        ImGui::TableSetupColumn(tr(""), 0, 130 * gUi);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)rows.size(); i++) {
            Row& r = rows[i];
            const uint64_t id = ids[i];
            bool changed = false;
            ImGui::PushID((int)id);
            ImGui::TableNextRow();
            if (id == liveId) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(pal::accent(0.25f)));
            const bool dim = !covered.count(i);
            if (dim) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TableNextColumn(); ImGui::Text("%.4f", r.mhz);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.mode.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableNextColumn();
            if (dim) ImGui::PopStyleColor();
            if (id == v.toneRow && !v.liveTone.empty() && v.liveTone != r.tone) {   // the live reading, with the one-click save
                ImGui::TextColored(pal::okGreen(), "%s", v.liveTone.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("Save tone"))) saveTone(id, v.liveTone);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Keep this CTCSS / DCS reading with the memory"));
            } else ImGui::TextUnformatted(r.tone.empty() ? "-" : r.tone.c_str());
            if (dim) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.group.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.lastHit > 0 ? fmtLocal(r.lastHit, "%m-%d %H:%M").c_str() : "-");
            ImGui::TableNextColumn(); ImGui::Text("%d", r.hits);
            if (dim) ImGui::PopStyleColor();
            ImGui::TableNextColumn(); changed |= ImGui::Checkbox("##l", &r.lockout);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Locked out: skipped by the scan (a running scan sees it at the next start)"));
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##p", &r.priority)) { scanplan::setPriority(rows, i, r.priority); changed = true; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The priority channel is checked every few seconds, also while another channel holds"));
            ImGui::TableNextColumn(); changed |= ImGui::Checkbox("##s", &r.star);
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(l.running);
            if (ImGui::SmallButton(tr("Tune"))) tune = id;
            ImGui::EndDisabled();
            ImGui::SameLine(); if (ImGui::SmallButton(tr("Edit"))) edit = id;
            ImGui::SameLine(); if (ImGui::SmallButton("x")) del = id;
            ImGui::PopID();
            if (changed) {
                for (int k = 0; k < (int)rows.size(); k++) commit(ids[k], rows[k]);   // setPriority may have cleared others
                memoryChanged();
            }
        }
        ImGui::EndTable();
    }
    if (tune) {
        std::string note;
        if (const Memory* m = memoryBank().find(tune)) { const Memory c = *m; v.msg = memoryRecallApp(a, c, true, note) ? memui::memoryTitle(c) : (note.empty() ? tr("Could not apply this memory.") : note); }
    }
    if (edit) if (const Memory* m = memoryBank().find(edit)) memui::openEdit(*m, false);
    if (del) { memoryBank().remove(del); memoryChanged(); }
}

void panel(App& a) {
    firstWindowSize(920 * gUi, 620 * gUi);
    if (!ImGui::Begin(trw("Scanner (highly experimental)"), &a.scanViewOpen)) { ImGui::End(); return; }
    const ScanLive l = scannerLive();
    ImGui::TextColored(pal::warnAmber(), "%s", tr("HIGHLY EXPERIMENTAL"));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", tr("Your memories are the scan list. Open the old views:"));
    ImGui::SameLine(); if (ImGui::SmallButton(tr("Classic scanner"))) a.scannerOpen = true;
    ImGui::SameLine(); if (ImGui::SmallButton(tr("Memory panel"))) a.memoryOpen = true;
    ImGui::SameLine(); if (ImGui::SmallButton(tr("Tone decoders"))) openTonesMode(a);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Switches the receiver to the Tone decoders mode (CTCSS, DCS, DTMF ...). Tones are read live there."));
    controlBar(a, l);
    if (ImGui::Button(tr("Add current frequency"))) addCurrent(a);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Stores the frequency, mode and settings the receiver is on now as a new row"));
    ImGui::Separator();
    if (v.scope == (int)scanplan::Scope::Fast) fastscanui::body(a); else table(a, l);
    memui::editDialogs();
    ImGui::End();
}

}  // namespace

void scanViewWindow(App& a) {
    load();
    const ScanLive l = scannerLive();
    if (l.running && l.hitSeq != v.hitSeq) { v.hitSeq = l.hitSeq; onHit(l); }
    pollTone(a, l);
    if (a.scanViewOpen) panel(a);
}
