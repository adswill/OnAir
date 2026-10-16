// The scanner (View > Scanner, HIGHLY EXPERIMENTAL): steps through a scan list of {frequency, mode, name} or a band (start, stop, step, mode),
// tunes and switches mode the way the mode list does (setMode, then startReceiver), listens for the dwell time, stops on activity (channel
// power over the noise in the mode's bandwidth, or the mode's own telemetry saying it decodes), holds while active plus a hang time, then
// moves on. Skip, lockout, a priority entry checked every N seconds, and a log of the hits. The state machine is app/scan_list.h.
#include "app.h"
#include "results_export.h"
#include "dect2/results_io.h"
#include "dect2/timecompat.h"
#include "scan_list.h"
#include "dect2/u8file.h"
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <type_traits>

namespace {

using scanlist::Entry;
using scanlist::Machine;

struct Hit { time_t wall = 0; double mhz = 0; std::string mode, name, what; float levelDb = 0; double startT = 0, endT = 0; };

struct State {
    bool loaded = false, dirty = false;
    Machine m;
    float thrDb = 8;                 // channel power over noise that counts as activity
    std::vector<Hit> log;
    bool logging = false;            // the last hit is still going on
    uint64_t rxSeq0 = 0, specSeq0 = 0, blocks0 = 0;
    bool haveBlocks0 = false;
    float level = 0;                 // the latest measurement
    bool decoded = false, decodeKnown = false;
    std::string what;
    std::string msg;
    int addMode = -1;                // mode of a new entry (kModes index; -1: the current mode)
};
State S;

// ---------------------------------------------------------------- modes
const char* const kOrigKey[6] = {"dvb", "atsc", "dab", "atsc3", "isdbt", "fm"};
std::string modeKey(const ModeDef& m) {
    if (m.subId) return m.subId;
    if (m.family >= 0 && m.family < 6) return kOrigKey[m.family];
    if (const ModeTuning* mt = modeTuning(m.family + 2)) return mt->id;
    return std::to_string(m.family);
}
int modeIndex(const std::string& key) {
    for (int i = 0; i < kNumModes; i++) if (modeKey(kModes[i]) == key) return i;
    return -1;
}
int currentModeIndex(const App& a) {
    for (int i = 0; i < kNumModes; i++) if (modeSelected(a, kModes[i])) return i;
    return -1;
}
const char* modeName(const std::string& key) { const int i = modeIndex(key); return i >= 0 ? kModes[i].name : key.c_str(); }
bool modeCombo(const char* id, std::string& key, float w) {
    const int cur = modeIndex(key);
    bool ch = false;
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo(id, cur >= 0 ? kModes[cur].name : "(unknown mode)")) {
        const char* q = modeSearchBox();
        for (int i = 0; i < kNumModes; i++) {
            if (!modeMatches(kModes[i], q)) continue;
            if (ImGui::Selectable(kModes[i].name, i == cur)) { key = modeKey(kModes[i]); ch = true; }
        }
        ImGui::EndCombo();
    }
    return ch;
}

// ---------------------------------------------------------------- what the mode's telemetry says
template <class T> auto dvOf(const T& t, int) -> decltype(bool(t.dataValid)) { return t.dataValid; }
template <class T> bool dvOf(const T&, long) { return false; }
template <class T> auto hasDv(const T& t, int) -> decltype(bool(t.dataValid), true) { (void)t; return true; }
template <class T> bool hasDv(const T&, long) { return false; }
template <class T> auto okOf(const T& t, int) -> decltype(uint64_t(t.blocksOk)) { return t.blocksOk; }
template <class T> uint64_t okOf(const T&, long) { return 0; }

struct Decode { bool known = false, valid = false; uint64_t blocks = 0; const char* what = ""; };
template <class T> Decode generic(const T& t) { Decode d; d.known = hasDv(t, 0); d.valid = dvOf(t, 0); d.blocks = okOf(t, 0); d.what = "data"; return d; }

Decode decodeOf(App& a, const RxTelemetry& r) {
    Decode d;
    switch (r.standard) {
    case 0: d.known = true; d.valid = r.dataValid; d.blocks = r.dataFrames; d.what = "T2 frames"; return d;
    case 1: d.known = true; d.valid = r.dvbt.fecSync; d.blocks = r.dvbt.packets; d.what = "DVB-T packets"; return d;
    case 2: d.known = true; d.valid = r.atsc.segSync; d.what = "ATSC sync"; return d;
    case 3: d.known = true; d.valid = r.dab.sync; d.what = "DAB sync"; return d;
    case 4: { Atsc3Telemetry t; d.known = true; d.valid = a.engine.atsc3Telemetry(t) && t.locked; d.what = "ATSC 3.0 frames"; return d; }
    case 5: d.known = true; d.valid = r.isdbt.tmccOk; d.what = "TMCC"; return d;
    case 6: d.known = true; d.valid = r.fm.carrier || r.fm.rdsSync; d.what = r.fm.rdsSync ? "RDS" : "FM carrier"; return d;
#define SC_GEN(n, m) case n: return generic(r.m);
    SC_GEN(7, dvbs) SC_GEN(8, dtmb) SC_GEN(9, atv) SC_GEN(10, dmr) SC_GEN(11, drm) SC_GEN(12, adsb) SC_GEN(13, gnss) SC_GEN(14, sonde)
    SC_GEN(15, ais) SC_GEN(16, marine) SC_GEN(17, acars) SC_GEN(18, inmc) SC_GEN(19, aero) SC_GEN(20, iridium) SC_GEN(21, mesh) SC_GEN(22, hdr)
    SC_GEN(23, cdr) SC_GEN(24, pager) SC_GEN(25, packet) SC_GEN(26, hfdig) SC_GEN(27, airband) SC_GEN(28, vdl2) SC_GEN(29, ism) SC_GEN(31, vor)
    SC_GEN(32, m17) SC_GEN(33, cell) SC_GEN(34, dstar) SC_GEN(35, cw) SC_GEN(36, hfdl) SC_GEN(37, psk) SC_GEN(38, js8) SC_GEN(39, nxdn)
    SC_GEN(40, gsmr) SC_GEN(41, dvbc) SC_GEN(44, uplink) SC_GEN(45, orbcomm) SC_GEN(46, dpmr) SC_GEN(47, tetra) SC_GEN(48, mac)
    SC_GEN(50, ale) SC_GEN(51, p25) SC_GEN(52, ble) SC_GEN(55, wifi)
#undef SC_GEN
    default: return d;   // the measuring modes (spectrum explorer, analyser, radio astronomy, sounders, QO-100): power only
    }
}

// ---------------------------------------------------------------- prefs
void load() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& p = plat::prefs();
    S.m.list = scanlist::fromCsv(p.getS("scanList", ""));
    S.m.hangSec = p.getD("scanHang", 3);
    S.m.prioritySec = p.getD("scanPrio", 5);
    S.thrDb = (float)p.getD("scanThr", 8);
    S.m.useBand = p.getB("scanUseBand", false);
    S.m.band.startMhz = p.getD("scanBandStart", 118);
    S.m.band.stopMhz = p.getD("scanBandStop", 137);
    S.m.band.stepKhz = p.getD("scanBandStep", 25);
    S.m.band.dwellSec = p.getD("scanBandDwell", 2);
    S.m.band.mode = p.getS("scanBandMode", "airband");
}
void save(App& a) {
    plat::Prefs& p = plat::prefs();
    p.setS("scanList", scanlist::toCsv(S.m.list));
    p.setD("scanHang", S.m.hangSec); p.setD("scanPrio", S.m.prioritySec); p.setD("scanThr", S.thrDb);
    p.setB("scanUseBand", S.m.useBand);
    p.setD("scanBandStart", S.m.band.startMhz); p.setD("scanBandStop", S.m.band.stopMhz); p.setD("scanBandStep", S.m.band.stepKhz);
    p.setD("scanBandDwell", S.m.band.dwellSec); p.setS("scanBandMode", S.m.band.mode);
    savePrefs(a);
    S.dirty = false;
}

// ---------------------------------------------------------------- tuning: the path of the mode list (selectMode / setMode) and the Start button
void tuneTo(App& a, const scanlist::Target& t) {
    const int mi = modeIndex(t.mode);
    if (mi < 0) { a.engine.log("scanner: unknown mode " + t.mode); return; }
    const ModeDef& md = kModes[mi];
    if (modeSelected(a, md)) {
        a.freqMhz = t.mhz;
        if (a.engine.running()) { a.tune.centerHz = a.freqMhz * 1e6; a.engine.retune(a.tune); }
        else startReceiver(a);
    } else {
        if (a.engine.running()) a.engine.stop();
        setMode(a, md.family, subIndexOf(md.subId));
        a.freqMhz = t.mhz;
        startReceiver(a);
    }
    S.rxSeq0 = a.rx.seq; S.specSeq0 = a.spec.seq; S.haveBlocks0 = false;
    S.logging = false;
}

void closeHit(double now) { if (S.logging && !S.log.empty()) S.log.back().endT = now; }

void tick(App& a) {
    if (!S.m.running()) return;
    const double now = glfwGetTime();
    const bool specFresh = a.spec.seq != S.specSeq0 && !a.spec.dbfs.empty();
    const bool rxFresh = a.rx.seq != S.rxSeq0;
    const bool ready = a.engine.running() && (specFresh || rxFresh);
    S.level = specFresh ? (float)occupancyDb(a.spec.dbfs, a.tune.sampleRate / 1e6, a.tune.bandwidthMhz) : 0;
    Decode d;
    if (rxFresh) {
        const int want = engineStd(a) - 1;
        if (a.rx.standard == want || (want < 0 && a.rx.standard <= 1)) d = decodeOf(a, a.rx);
    }
    S.decodeKnown = d.known; S.decoded = d.valid;
    if (d.known && !S.haveBlocks0) { S.blocks0 = d.blocks; S.haveBlocks0 = true; }
    {
        char b[96];
        const uint64_t n = d.blocks >= S.blocks0 ? d.blocks - S.blocks0 : 0;
        if (d.valid && n) snprintf(b, sizeof b, "%s (%llu)", d.what, (unsigned long long)n);
        else if (d.valid) snprintf(b, sizeof b, "%s", d.what);
        else snprintf(b, sizeof b, "%s", tr("signal only"));
        S.what = b;
    }
    const bool active = S.level >= S.thrDb || d.valid;
    const Machine::State before = S.m.state();
    bool retune = S.m.tick(now, ready, active);
    if (S.m.hitStarted()) {
        const scanlist::Target& t = S.m.target();
        Hit h; h.wall = time(nullptr); h.mhz = t.mhz; h.mode = t.mode; h.name = t.name; h.levelDb = S.level; h.what = S.what; h.startT = h.endT = now;
        S.log.push_back(h);
        if (S.log.size() > 500) S.log.erase(S.log.begin());
        S.logging = true;
        char b[160]; snprintf(b, sizeof b, "scanner: activity on %.4f MHz (%s), %.1f dB, %s", t.mhz, modeName(t.mode), S.level, S.what.c_str());
        a.engine.log(b);
    }
    if (S.logging && !S.log.empty() && (before == Machine::Hold || S.m.state() == Machine::Hold)) {
        Hit& h = S.log.back();
        h.levelDb = std::max(h.levelDb, S.level);
        if (S.decoded) h.what = S.what;
        h.endT = now;
    }
    if (retune) { closeHit(now); tuneTo(a, S.m.target()); }
    else if (!S.m.running()) { closeHit(now); S.msg = tr("nothing left to scan"); }
}

void startScan(App& a) {
    if (a.scanner.progress().running) { S.msg = "a channel scan is using the radio: stop it first"; return; }
    if (S.m.start(glfwGetTime())) { S.msg.clear(); tuneTo(a, S.m.target()); }
    else S.msg = tr(S.m.useBand ? "the band has no steps (check start, stop and step)" : "the list has nothing to scan (empty, or every entry locked out)");
}

// ---------------------------------------------------------------- window
void listTable(App& a) {
    const bool run = S.m.running();
    int del = -1;
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##scanlist", 7, tf, ImVec2(0, 190 * gUi))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("MHz"), 0, 110 * gUi);
        ImGui::TableSetupColumn(tr("Mode"), 0, 150 * gUi);
        ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Dwell s"), 0, 60 * gUi);
        ImGui::TableSetupColumn(tr("Priority"), 0, 56 * gUi);
        ImGui::TableSetupColumn(tr("Lockout"), 0, 56 * gUi);
        ImGui::TableSetupColumn(tr(""), 0, 24 * gUi);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)S.m.list.size(); i++) {
            Entry& e = S.m.list[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            if (run && S.m.target().entry == i && !S.m.useBand) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(pal::accent(0.25f)));
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1);
            if (ImGui::InputDouble("##f", &e.mhz, 0, 0, "%.5f")) S.dirty = true;
            ImGui::TableNextColumn();
            if (modeCombo("##m", e.mode, -1)) S.dirty = true;
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1);
            char nm[64]; snprintf(nm, sizeof nm, "%s", e.name.c_str());
            if (ImGui::InputText("##n", nm, sizeof nm)) { e.name = nm; S.dirty = true; }
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1);
            if (ImGui::InputDouble("##d", &e.dwellSec, 0, 0, "%.1f")) { e.dwellSec = std::max(0.5, e.dwellSec); S.dirty = true; }
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##p", &e.priority)) { if (e.priority) for (int k = 0; k < (int)S.m.list.size(); k++) if (k != i) S.m.list[k].priority = false; S.dirty = true; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("Checked every %.0f s, also while another channel holds (one priority entry)"), S.m.prioritySec);
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##l", &e.lockout)) S.dirty = true;
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(run);
            if (ImGui::SmallButton("x")) del = i;
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (del >= 0) { S.m.list.erase(S.m.list.begin() + del); S.dirty = true; }
    if (ImGui::Button(tr("Add current"))) {
        Entry e; e.mhz = a.freqMhz; const int mi = currentModeIndex(a); e.mode = mi >= 0 ? modeKey(kModes[mi]) : "fm";
        S.m.list.push_back(e); S.dirty = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Adds the frequency and mode the receiver is on now"));
    ImGui::SameLine();
    if (ImGui::Button(tr("Import CSV..."))) {
        const std::string p = openFileDialog();
        if (!p.empty()) {
            std::ifstream f(dect2::u8file(p), std::ios::binary); std::stringstream ss; ss << f.rdbuf();
            std::vector<Entry> v = scanlist::fromCsv(ss.str());
            int unknown = 0;
            for (const Entry& e : v) if (modeIndex(e.mode) < 0) unknown++;
            S.m.list.insert(S.m.list.end(), v.begin(), v.end()); S.dirty = true;
            char b[120]; snprintf(b, sizeof b, tr("imported %d entries%s"), (int)v.size(), unknown ? tr(" (some with a mode OnAir does not know)") : "");
            S.msg = b;
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Columns: frequency_mhz, mode, name, dwell_s, lockout, priority.\nmode is a mode id: fm, dab, airband, dmr, pocsag, ft8 ..."));
    ImGui::SameLine();
    if (ImGui::Button(tr("Export CSV..."))) {
        const std::string p = saveFileDialog("scanlist.csv");
        if (!p.empty()) { std::ofstream f(dect2::u8file(p), std::ios::binary); f << scanlist::toCsv(S.m.list); S.msg = std::string(tr(f ? "exported" : "could not write")) + " " + p; }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(run || S.m.list.empty());
    if (ImGui::Button(tr("Clear lockouts"))) { for (Entry& e : S.m.list) e.lockout = false; S.dirty = true; }
    ImGui::EndDisabled();
}

void bandFields() {
    scanlist::Band& b = S.m.band;
    ImGui::SetNextItemWidth(110 * gUi); if (ImGui::InputDouble(tr("start MHz"), &b.startMhz, 0, 0, "%.4f")) S.dirty = true;
    ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi); if (ImGui::InputDouble(tr("stop MHz"), &b.stopMhz, 0, 0, "%.4f")) S.dirty = true;
    ImGui::SameLine(); ImGui::SetNextItemWidth(80 * gUi); if (ImGui::InputDouble(tr("step kHz"), &b.stepKhz, 0, 0, "%.3f")) { b.stepKhz = std::max(0.1, b.stepKhz); S.dirty = true; }
    ImGui::SetNextItemWidth(60 * gUi); if (ImGui::InputDouble(tr("dwell s"), &b.dwellSec, 0, 0, "%.1f")) { b.dwellSec = std::max(0.5, b.dwellSec); S.dirty = true; }
    ImGui::SameLine();
    if (modeCombo("mode##band", b.mode, 180 * gUi)) S.dirty = true;
    ImGui::SameLine();
    ImGui::TextDisabled(tr("%d steps, %d locked out"), S.m.bandSteps(), (int)S.m.bandLockouts.size());
    if (!S.m.bandLockouts.empty()) { ImGui::SameLine(); if (ImGui::SmallButton(tr("clear"))) S.m.bandLockouts.clear(); }
}

void logTable() {
    ImGui::TextDisabled("%s", tr("Hits"));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Clear##log"))) { S.log.clear(); S.logging = false; }
    ImGui::SameLine();
    {
        static std::string note;
        resexp::Table x; x.cols = {"time", "frequency_hz", "mode", "name", "what", "level_db", "duration_s"};
        for (const Hit& h : S.log) {
            char tb[32]; std::tm tmv{}; dect2::localTime(h.wall, &tmv); std::strftime(tb, sizeof tb, "%Y-%m-%d %H:%M:%S", &tmv);
            x.add({tb, resexp::num(h.mhz * 1e6, 0), h.mode, h.name, h.what, resexp::num(h.levelDb, 1), h.endT > h.startT ? resexp::num(h.endT - h.startT, 1) : ""});
        }
        resexp::saveButton(x, "scanner", note, "svscanner");
        if (!note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", note.c_str()); }
    }
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##scanlog", 6, tf, ImVec2(0, std::max(120.f * gUi, ImGui::GetContentRegionAvail().y)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Time"), 0, 70 * gUi);
        ImGui::TableSetupColumn(tr("MHz"), 0, 90 * gUi);
        ImGui::TableSetupColumn(tr("Mode"), 0, 120 * gUi);
        ImGui::TableSetupColumn(tr("Level"), 0, 60 * gUi);
        ImGui::TableSetupColumn(tr("For"), 0, 50 * gUi);
        ImGui::TableSetupColumn(tr("Decoded"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int i = (int)S.log.size() - 1; i >= 0; i--) {
            const Hit& h = S.log[i];
            ImGui::TableNextRow();
            char tb[16]; struct tm tmv;
#ifdef _WIN32
            localtime_s(&tmv, &h.wall);
#else
            dect2::localTime(h.wall, &tmv);
#endif
            strftime(tb, sizeof tb, "%H:%M:%S", &tmv);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(tb);
            ImGui::TableNextColumn(); ImGui::Text("%.4f", h.mhz);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(h.name.empty() ? modeName(h.mode) : (std::string(modeName(h.mode)) + " " + h.name).c_str());
            ImGui::TableNextColumn(); ImGui::Text(tr("%.1f dB"), h.levelDb);
            ImGui::TableNextColumn(); ImGui::Text("%.0f s", h.endT - h.startT);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(h.what.c_str());
        }
        ImGui::EndTable();
    }
}

void window(App& a) {
    ImGui::SetNextWindowSize(ImVec2(760 * gUi, 620 * gUi), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(trw("Scanner (highly experimental)"), &a.scannerOpen)) { ImGui::End(); return; }
    ImGui::TextColored(pal::warnAmber(), "%s", tr("HIGHLY EXPERIMENTAL"));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", tr("Steps through the list or a band, stops on activity. A mode change restarts the receiver."));
    const bool run = S.m.running();
    {   // controls
        if (ImGui::Button(tr(run ? "Stop scan" : "Start scan"))) { if (run) { S.m.stop(); closeHit(glfwGetTime()); } else startScan(a); }
        ImGui::SameLine();
        ImGui::BeginDisabled(!run);
        if (ImGui::Button(tr("Skip"))) { closeHit(glfwGetTime()); if (S.m.skip(glfwGetTime())) tuneTo(a, S.m.target()); }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Move on to the next channel now"));
        ImGui::SameLine();
        if (ImGui::Button(tr("Lockout"))) { closeHit(glfwGetTime()); if (S.m.lockout(glfwGetTime())) tuneTo(a, S.m.target()); S.dirty = true; }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Skip this channel from now on"));
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (run) {
            static const char* const kSt[4] = {N_("idle"), N_("listening"), N_("ACTIVE"), N_("hang")};
            const scanlist::Target& t = S.m.target();
            const ImVec4 col = S.m.state() == Machine::Hold ? pal::okGreen() : S.m.state() == Machine::Hang ? pal::warnAmber() : pal::grey();
            ImGui::TextColored(col, tr("%s  %.4f MHz  %s%s%s%s  %.1f dB  %s"), tr(kSt[S.m.state()]), t.mhz, modeName(t.mode), t.name.empty() ? "" : "  ", t.name.c_str(),
                               t.priority ? tr("  (priority)") : "", S.level, S.decodeKnown ? S.what.c_str() : "");
        } else if (!S.msg.empty()) ImGui::TextDisabled("%s", S.msg.c_str());
    }
    {   // settings
        float hang = (float)S.m.hangSec, prio = (float)S.m.prioritySec;
        ImGui::SetNextItemWidth(130 * gUi);
        if (ImGui::SliderFloat("##thr", &S.thrDb, 2, 30, tr("squelch %.0f dB"))) S.dirty = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Channel power over the noise (in the mode's bandwidth) that counts as activity.\nA mode that reports it decodes (sync, frames, data) stops the scan too."));
        ImGui::SameLine(); ImGui::SetNextItemWidth(120 * gUi);
        if (ImGui::SliderFloat("##hang", &hang, 0, 20, tr("hang %.1f s"))) { S.m.hangSec = hang; S.dirty = true; }
        ImGui::SameLine(); ImGui::SetNextItemWidth(140 * gUi);
        if (ImGui::SliderFloat("##prio", &prio, 0, 60, tr(prio > 0 ? "priority every %.0f s" : "priority off"))) { S.m.prioritySec = prio; S.dirty = true; }
        ImGui::SameLine();
        ImGui::BeginDisabled(run);
        if (ImGui::RadioButton(tr("List"), !S.m.useBand)) { S.m.useBand = false; S.dirty = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton(tr("Band"), S.m.useBand)) { S.m.useBand = true; S.dirty = true; }
        ImGui::EndDisabled();
    }
    ImGui::Separator();
    if (S.m.useBand) bandFields(); else listTable(a);
    ImGui::Separator();
    logTable();
    ImGui::End();
}

} // namespace

void scannerWindow(App& a) {
    load();
    tick(a);
    if (a.scannerOpen) window(a);
    if (S.dirty && !ImGui::IsAnyItemActive()) save(a);
}

void scannerButton(App& a) {
    if (ImGui::SmallButton(tr("Scanner (highly experimental)..."))) a.scannerOpen = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("HIGHLY EXPERIMENTAL: step through a list of frequencies and modes, or a band, and stop on activity"));
}

// the hit log for the results file (results_ui.cpp): kept until its Clear
std::string scannerResultsSave() { return dect2::results::pack(S.log); }
bool scannerResultsLoad(const std::string& blob) {
    std::vector<Hit> log;
    if (!dect2::results::unpack(blob, log)) return false;
    S.log = std::move(log);
    S.logging = false;
    return true;
}
