// LoRa satellite screens: the frame table (time, satellite, SF, bandwidth, carrier offset = Doppler, Doppler rate, SNR, level, payload as hex and text),
// the Doppler curve of the pass drawn from the frames, the preset list (public frequencies and settings with how sure we are: all of them unverified),
// and the settings. Receive only. The pass planner (Satellites tab) names the satellite and starts this mode; it does not retune for the Doppler shift here:
// the decoder follows the shift and the Doppler rate itself.
#include "app.h"
#include "dect2/lorasat/lorasat_presets.h"
#include "dect2/lorasat/lorasat_rx.h"
#include "dect2/lorasat/lorasat_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    bool loaded = false;
    dect2::LoraSatSettings cfg;
    int view = 0;
    char filter[64] = "";
};
State S;

void saveState() {
    auto& p = plat::prefs();
    p.setB("lsFollow", S.cfg.followDial); p.setB("lsAutoSf", S.cfg.autoSf); p.setI("lsSf", S.cfg.sf); p.setB("lsAutoBw", S.cfg.autoBw); p.setI("lsBw", S.cfg.bwKhz);
    p.setI("lsSync", S.cfg.sync); p.setB("lsTrack", S.cfg.track); p.setI("lsDopp", (int)S.cfg.dopplerMaxHz);
    p.flush();
}
void loadState(App& a) {
    if (S.loaded) return;
    S.loaded = true;
    auto& p = plat::prefs();
    S.cfg.followDial = p.getB("lsFollow", true); S.cfg.autoSf = p.getB("lsAutoSf", true); S.cfg.sf = (int)p.getI("lsSf", 11); S.cfg.autoBw = p.getB("lsAutoBw", false);
    S.cfg.bwKhz = (int)p.getI("lsBw", 125); S.cfg.sync = (int)p.getI("lsSync", -1); S.cfg.track = p.getB("lsTrack", true); S.cfg.dopplerMaxHz = p.getI("lsDopp", 0);
    a.engine.lorasat().setSettings(S.cfg);
}
void apply(App& a) { a.engine.lorasat().setSettings(S.cfg); saveState(); }
bool live(const App& a) { return a.rx.standard == 202; }

std::string clock(int64_t t) {
    if (t <= 0) return "-";
    const time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char b[16];
    snprintf(b, sizeof b, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}
void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

void note() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Receive only. A satellite pass moves the signal by up to +-10 kHz at 436 MHz and +-20 kHz at 868 / 915 MHz, and the shift changes by up to 150 - 300 Hz per second: "
                        "the channel filter is widened for it and the decoder follows the carrier through each frame, so the pass planner does not retune the radio for this mode. "
                        "The preset list is unverified (see docs/LORA_SATELLITES.md): satellites change frequency and retire, and a frequency of your own can always be typed in. "
                        "Needs a radio with 1 Msps or more.");
    ImGui::PopTextWrapPos();
}

void framesView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see LoRa satellite frames"); return; }
    const dect2::LoraSatTelemetry& t = a.rx.lorasat;
    ImGui::AlignTextToFramePadding();
    if (t.presetRunning >= 0) ImGui::TextDisabled("preset: %s", dect2::lorasat::presets()[(size_t)t.presetRunning].name); else ImGui::TextDisabled("manual settings");
    flowNext(10 * gUi);
    if (ImGui::Button("Clear")) a.engine.lorasat().clearResults();
    flowBreak();
    if (!ImGui::BeginTable("##lslog", 11, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    for (const char* h : {"Time UTC", "Satellite", "SF", "BW", "Doppler Hz", "Rate Hz/s", "SNR dB", "Level", "CRC", "Payload hex", "Text"}) ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    for (const auto& r : t.log) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(r.wallTime).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(r.sat.empty() ? "-" : r.sat.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%d", r.sf);
        ImGui::TableNextColumn(); ImGui::Text("%.1f", r.bwHz / 1e3);
        ImGui::TableNextColumn(); ImGui::Text("%+.0f", r.cfoStartHz);
        ImGui::TableNextColumn(); ImGui::Text("%+.0f", r.rateHzS);
        ImGui::TableNextColumn(); ImGui::Text("%.1f", r.snrDb);
        ImGui::TableNextColumn(); ImGui::Text("%.0f", r.levelDb);
        ImGui::TableNextColumn(); if (!r.hasCrc) ImGui::TextDisabled("none"); else if (r.crcOk) ImGui::TextColored(pal::okGreen(), "ok"); else ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "bad");
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(r.hex.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(r.ascii.c_str()); ImGui::PopFont();
    }
    ImGui::EndTable();
    if (t.log.empty()) ImGui::TextDisabled("no frames yet");
}

void dopplerView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver first"); return; }
    const dect2::LoraSatTelemetry& t = a.rx.lorasat;
    ImGui::TextDisabled("Carrier offset of each good frame against time: the Doppler curve of the pass. The filter allows +-%.1f kHz.", t.dopplerMaxHz / 1e3);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = std::max(200.f, ImGui::GetContentRegionAvail().x), h = std::max(120.f * gUi, std::min(ImGui::GetContentRegionAvail().y - 4, 260.f * gUi));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(20, 24, 30, 255));
    const float y0 = p0.y + h * 0.5f;
    dl->AddLine(ImVec2(p0.x, y0), ImVec2(p0.x + w, y0), IM_COL32(90, 90, 90, 255));
    const double lim = std::max(1000.0, t.dopplerMaxHz);
    if (t.doppler.size() >= 1) {
        const float t0 = t.doppler.front().sec, t1 = std::max(t0 + 1.f, t.doppler.back().sec);
        for (const auto& d : t.doppler) {
            const float x = p0.x + 6 + (w - 12) * (d.sec - t0) / (t1 - t0), y = y0 - (float)(d.hz / lim) * (h * 0.5f - 6);
            dl->AddCircleFilled(ImVec2(x, y), 3.f * gUi, IM_COL32(120, 210, 150, 255));
            const float dx = (w - 12) * 0.5f / std::max(1.f, (t1 - t0)) * 4.f, dy = (float)(d.rate / lim) * (h * 0.5f - 6) * 4.f;   // the slope: 4 s of the drift
            dl->AddLine(ImVec2(x - dx, y + dy), ImVec2(x + dx, y - dy), IM_COL32(230, 190, 90, 200), 1.f);
        }
    }
    char b[48];
    snprintf(b, sizeof b, "+%.0f kHz", lim / 1e3); dl->AddText(ImVec2(p0.x + 3, p0.y + 1), IM_COL32(150, 150, 150, 255), b);
    snprintf(b, sizeof b, "-%.0f kHz", lim / 1e3); dl->AddText(ImVec2(p0.x + 3, p0.y + h - ImGui::GetTextLineHeight() - 1), IM_COL32(150, 150, 150, 255), b);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::TextDisabled("Dots: carrier at the start of each frame. Short lines: the drift through the frame (Hz per second), drawn for four seconds.");
}

void presetsView(App& a) {
    loadState(a);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Public frequencies and settings of satellites that use LoRa. None of them was checked against a real signal: 'plausible' is from the operator's public descriptions, "
                        "'uncertain' is from memory of community lists. Click a row to tune the frequency; with 'Follow the dial' on, the receiver then uses the preset's settings.");
    ImGui::PopTextWrapPos();
    if (!ImGui::BeginTable("##lspre", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    for (const char* h : {"MHz", "SF", "BW kHz", "Sure?", "Direction"}) ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    const auto& v = dect2::lorasat::presets();
    for (size_t i = 0; i < v.size(); i++) {
        const auto& p = v[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::Selectable(p.name, false, ImGuiSelectableFlags_SpanAllColumns)) { tuneFreq(a, p.mhz); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.note);
        ImGui::TableNextColumn(); ImGui::Text("%.3f", p.mhz);
        ImGui::TableNextColumn(); if (p.sf) ImGui::Text("%d", p.sf); else ImGui::TextDisabled("search");
        ImGui::TableNextColumn(); ImGui::Text("%.1f", dect2::lorasat::bandwidthHz(p.bwKhz) / 1e3);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::lorasat::confidenceName(p.conf));
        ImGui::TableNextColumn(); ImGui::TextUnformatted(p.uplinkOnly ? "ground to satellite" : "satellite to ground");
    }
    ImGui::EndTable();
}

void settings(App& a, bool& retune) {
    (void)retune;
    loadState(a);
    bool ch = false;
    ch |= ImGui::Checkbox("Follow the dial", &S.cfg.followDial);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("When the frequency is within 15 kHz of a preset, run that preset's spreading factor, bandwidth and sync word.");
    flowNext(10 * gUi);
    ch |= ImGui::Checkbox("SF auto", &S.cfg.autoSf);
    if (!S.cfg.autoSf) {
        ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(std::min(70 * gUi, ImGui::GetContentRegionAvail().x));
        char l[8]; snprintf(l, sizeof l, "SF%d", S.cfg.sf);
        if (ImGui::BeginCombo("##lssf", l)) {
            for (int s = 7; s <= 12; s++) { snprintf(l, sizeof l, "SF%d", s); if (ImGui::Selectable(l, s == S.cfg.sf)) { S.cfg.sf = s; ch = true; } }
            ImGui::EndCombo();
        }
    }
    flowNext(10 * gUi);
    ch |= ImGui::Checkbox("BW auto", &S.cfg.autoBw);
    if (!S.cfg.autoBw) {
        ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(std::min(90 * gUi, ImGui::GetContentRegionAvail().x));
        static const int kBw[] = {41, 62, 125, 250, 500};
        char l[16]; snprintf(l, sizeof l, "%s", S.cfg.bwKhz == 41 ? "41.7 kHz" : S.cfg.bwKhz == 62 ? "62.5 kHz" : (std::to_string(S.cfg.bwKhz) + " kHz").c_str());
        if (ImGui::BeginCombo("##lsbw", l)) {
            for (int b : kBw) { snprintf(l, sizeof l, "%s", b == 41 ? "41.7 kHz" : b == 62 ? "62.5 kHz" : (std::to_string(b) + " kHz").c_str()); if (ImGui::Selectable(l, b == S.cfg.bwKhz)) { S.cfg.bwKhz = b; ch = true; } }
            ImGui::EndCombo();
        }
    }
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Sync"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(130 * gUi, ImGui::GetContentRegionAvail().x));
    const char* sn = S.cfg.sync < 0 ? "0x12 and 0x34" : S.cfg.sync == 0x12 ? "0x12" : "0x34";
    if (ImGui::BeginCombo("##lssync", sn)) {
        if (ImGui::Selectable("0x12 and 0x34", S.cfg.sync < 0)) { S.cfg.sync = -1; ch = true; }
        if (ImGui::Selectable("0x12", S.cfg.sync == 0x12)) { S.cfg.sync = 0x12; ch = true; }
        if (ImGui::Selectable("0x34", S.cfg.sync == 0x34)) { S.cfg.sync = 0x34; ch = true; }
        ImGui::EndCombo();
    }
    flowNext(10 * gUi);
    ch |= ImGui::Checkbox("Follow the carrier through a frame", &S.cfg.track);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Needed for long frames (SF10 - SF12) while the Doppler shift changes. Off: one measurement per frame, as for a ground link.");
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Doppler +-Hz"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(80 * gUi, ImGui::GetContentRegionAvail().x));
    int dm = (int)S.cfg.dopplerMaxHz;
    if (ImGui::InputInt("##lsd", &dm, 0, 0)) { S.cfg.dopplerMaxHz = std::max(0, std::min(60000, dm)); ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The widest shift the channel filter passes. 0: worked out from the frequency (LEO at 600 km, with some margin).");
    flowBreak();
    if (ch) apply(a);
}

void tuner(App& a, bool& retune) {
    loadState(a);
    ImGui::TextDisabled("Preset");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(230 * gUi, ImGui::GetContentRegionAvail().x));
    const int cur = dect2::lorasat::nearest(a.freqMhz, 1.0);
    const auto& v = dect2::lorasat::presets();
    if (ImGui::BeginCombo("##lspr", cur >= 0 ? v[(size_t)cur].name : "frequency")) {
        for (size_t i = 0; i < v.size(); i++) if (ImGui::Selectable(v[i].name, (int)i == cur)) { tuneFreq(a, v[i].mhz); retune = true; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Public LoRa satellite frequencies (unverified, see the Presets tab). Any frequency of 137 MHz - 1 GHz can be typed in.");
}

void tab(App& a) {
    loadState(a);
    subNav("lsv", S.view, {"Frames", "Doppler", "Presets", "Settings"});
    if (S.view == 0) { note(); framesView(a); }
    else if (S.view == 1) dopplerView(a);
    else if (S.view == 2) presetsView(a);
    else { bool rt = false; note(); ImGui::Spacing(); ImGui::PushID("lstab"); settings(a, rt); ImGui::PopID(); }
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::LoraSatTelemetry& t = a.rx.lorasat;
    if (!ImGui::BeginTable("##lsl", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("SF"); ImGui::TableSetupColumn("Doppler");
    for (size_t i = 0; i < t.log.size() && i < 60; i++) {
        const auto& r = t.log[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(r.ascii.empty() ? "-" : r.ascii.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%d", r.sf);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%+.1f kHz", r.cfoStartHz / 1e3);
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::LoraSatTelemetry& t = a.rx.lorasat;
    kvRow(a, "state", "%s", dect2::loraSatSummary(t).c_str());
    kvRow(a, "satellite", "%s", t.tracked.empty() ? "- (none from the pass planner)" : t.tracked.c_str());
    kvRow(a, "settings", "%s", t.presetRunning >= 0 ? dect2::lorasat::presets()[(size_t)t.presetRunning].name : "manual");
    kvRow(a, "frames", "%llu good, %llu CRC errors, %llu preambles", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.preambles);
    kvRow(a, "last frame", "SNR %.1f dB, Doppler %+.0f Hz", t.snrDb, t.cfoHz);
    kvRow(a, "channel filter", "%.0f kHz (Doppler +-%.1f kHz)", t.filterHz / 1e3, t.dopplerMaxHz / 1e3);
    kvRow(a, "input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    ImGui::Spacing();
    note();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Frames", !on ? 0 : a.rx.lorasat.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::loraSatSummary(a.rx.lorasat).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "LoRa satellites";
    if (live(a)) l2 = dect2::loraSatSummary(a.rx.lorasat);
}

void synth(App& a, bool& changed) {
    ImGui::TextDisabled("Test signal");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(240 * gUi, ImGui::GetContentRegionAvail().x));
    int& o = a.tune.synth.modeOpt[0];
    if (ImGui::BeginCombo("##lssyn", o == 1 ? "10 minute pass at 868.1 MHz, SF10" : "10 minute pass at 436.8 MHz, SF11")) {
        if (ImGui::Selectable("10 minute pass at 436.8 MHz, SF11", o == 0)) { o = 0; changed = true; }
        if (ImGui::Selectable("10 minute pass at 868.1 MHz, SF10", o == 1)) { o = 1; changed = true; }
        ImGui::EndCombo();
    }
}

} // namespace

extern const ModeUi kLoraSatUi;
const ModeUi kLoraSatUi = {
    .sideTitle = "FRAMES",
    .tabName = "LoRa sat",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .decoder = settings,
    .synth = synth,
};
