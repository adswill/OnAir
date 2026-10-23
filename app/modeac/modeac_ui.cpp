// Mode A / C screens: the aircraft map of the ADS-B screen (the Mode S aircraft of the same capture) and the table of transponder codes heard:
// squawk (Mode A), altitude (Mode C, 100 ft steps), the ident pulse, how often and how strong, and the Mode S aircraft each code was matched to.
// The map, the aircraft readout and the antenna position are the ADS-B screen's: this file borrows them for the ADS-B front end that runs inside the mode.
#include "../adsb/adsb_internal.h"
#include "dect2/modeac/modeac_live.h"
#include "dect2/modeac/modeac_proto.h"
#include "dect2/modeac/modeac_tel.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <deque>

using namespace adsbui;
using dect2::ModeAcCode;
using dect2::ModeAcTelemetry;

namespace {

struct MState {
    int view = 0;                       // 0 map, 1 codes
    uint16_t sel = 0; bool haveSel = false;
    int confirm = 2, pushedConfirm = -1;
    double pushedLat = 1e9, pushedLon = 1e9;
    bool wasRunning = false;
    std::deque<float> rate, codes;
    uint64_t lastSeq = 0;
    std::string note;
};
MState M;

bool mlive(const App& a) { return a.rx.standard == 143; }

// The ADS-B screens read a.rx.adsb and expect the ADS-B mode to be running: give them the front end's aircraft for the length of the call
struct AdsbView {
    App& a; int saved;
    explicit AdsbView(App& app) : a(app), saved(app.rx.standard) { std::swap(a.rx.adsb, a.rx.modeac.adsb); a.rx.standard = 12; }
    ~AdsbView() { std::swap(a.rx.adsb, a.rx.modeac.adsb); a.rx.standard = saved; }
};

const char* kindName(const ModeAcCode& c) { return c.kind == 1 ? "A" : c.kind == 2 ? "C" : "A/C"; }

std::string icaoOf(const ModeAcCode& c) { return c.icao ? icaoText(c.icao) + (c.callsign.empty() ? "" : "  " + c.callsign) : std::string("-"); }

std::string altText(const ModeAcCode& c) { return c.altValid ? uAlt(c.altFt) : std::string("-"); }

bool emergency(const ModeAcCode& c) { return dect2::modeac::isEmergencySquawk(c.squawk) && c.kind != 2; }

void selectCode(App& a, const ModeAcCode& c) {
    M.sel = c.bits; M.haveSel = true;
    S.sel = c.icao ? c.icao : 0xFFFFFFFFu;
    if (c.icao) for (const auto& ac : a.rx.modeac.adsb.aircraft) if (ac.icao == c.icao && ac.hasPos) { S.map.lat = ac.lat; S.map.lon = ac.lon; S.mapHome = false; }
}

void codesTable(App& a, bool compact) {
    const ModeAcTelemetry& t = a.rx.modeac;
    const bool on = mlive(a);
    if (!compact) {
        resexp::Table x;
        x.cols = {"time", "frequency_hz", "mode", "squawk", "altitude_ft", "type", "spi", "replies", "level_dbfs", "age_s", "icao", "callsign"};
        const std::string now = resexp::stamp();
        if (on) for (const auto& c : t.codes)
            x.add({now, "1090000000", "Mode A/C", dect2::modeac::squawkText(c.squawk), c.altValid ? std::to_string(c.altFt) : "", kindName(c), c.spi ? "1" : "", std::to_string(c.count), resexp::num(c.levelDbfs, 0), resexp::num(c.ageSec, 0), c.icao ? icaoText(c.icao) : "", c.callsign});
        resexp::saveButton(x, "modeac", M.note, "svmodeac");
        unitsControls();
        if (!M.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", M.note.c_str()); }
    }
    if (!ImGui::BeginTable(compact ? "##mac" : "##macf", compact ? 4 : 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Squawk", ImGuiTableColumnFlags_WidthFixed, 56 * gUi);
    const std::string hAlt = std::string("Alt ") + au::altUnit(S.units);
    ImGui::TableSetupColumn(hAlt.c_str(), ImGuiTableColumnFlags_WidthFixed, 74 * gUi);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 38 * gUi);
    if (!compact) { ImGui::TableSetupColumn("Ident", ImGuiTableColumnFlags_WidthFixed, 44 * gUi); ImGui::TableSetupColumn("Replies"); ImGui::TableSetupColumn("dBFS"); ImGui::TableSetupColumn("Age"); }
    ImGui::TableSetupColumn("Aircraft", ImGuiTableColumnFlags_WidthFixed, 130 * gUi);
    ImGui::TableHeadersRow();
    if (on) for (const auto& c : t.codes) {
        ImGui::TableNextRow();
        ImGui::PushID((int)c.bits);
        ImGui::TableNextColumn();
        const bool sel = M.haveSel && M.sel == c.bits;
        ImGui::PushFont(a.mono, 0);
        if (emergency(c)) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1));
        if (ImGui::Selectable(dect2::modeac::squawkText(c.squawk).c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) selectCode(a, c);
        if (emergency(c)) ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::TableNextColumn();
        if (c.kind == 1 || !c.altValid) ImGui::TextDisabled("-"); else if (c.kind == 0) ImGui::TextDisabled("%s", altText(c).c_str()); else ImGui::TextUnformatted(altText(c).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(kindName(c));
        if (!compact) {
            ImGui::TableNextColumn(); if (c.spi) ImGui::TextColored(kWarn, "SPI"); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%u", c.count);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", c.levelDbfs);
            ImGui::TableNextColumn(); ImGui::Text("%.0f s", c.ageSec);
        }
        ImGui::TableNextColumn();
        if (c.icao) ImGui::TextColored(kGood, "%s", icaoOf(c).c_str()); else ImGui::TextDisabled("-");
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// what the codes say about the aircraft picked on the map, then the ADS-B readout
void detailPane(App& a) {
    const ModeAcTelemetry& t = a.rx.modeac;
    if (mlive(a) && S.sel != 0xFFFFFFFFu) {
        bool any = false;
        for (const auto& c : t.codes) if (c.icao == S.sel) {
            any = true;
            ImGui::TextDisabled("Mode %s", kindName(c)); ImGui::SameLine(0, 6 * gUi);
            ImGui::PushFont(a.mono, 0);
            if (c.kind == 2) ImGui::Text("%s  %u replies", altText(c).c_str(), c.count);
            else ImGui::Text("%s  %u replies%s", dect2::modeac::squawkText(c.squawk).c_str(), c.count, c.spi ? "  SPI" : "");
            ImGui::PopFont();
        }
        if (!any) ImGui::TextDisabled("no Mode A/C reply matched to it yet");
        ImGui::Separator();
    }
    AdsbView v(a);
    detail(a);
}

void tab(App& a) {
    subNav("macv", M.view, {tr("Map"), tr("Codes")});
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    if (M.view == 1) { ImGui::BeginChild("##mac_t", ImVec2(W, H)); codesTable(a, false); ImGui::EndChild(); return; }
    const float dw = std::min(250.f * gUi, W * 0.26f);
    ImGui::BeginChild("##mac_l", ImVec2(W - dw - 8 * gUi, H));
    { AdsbView v(a); mapView(a, ImGui::GetContentRegionAvail()); }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##mac_r", ImVec2(0, H));
    detailPane(a);
    ImGui::EndChild();
}

void list(App& a) {
    if (!mlive(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see the codes")); ImGui::PopTextWrapPos(); return; }
    if (resultsClearButton("modeac")) a.engine.modeac().clearResults();
    ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%u codes, %u matched to a Mode S aircraft", a.rx.modeac.codeCount, a.rx.modeac.matched); ImGui::PopTextWrapPos();
    codesTable(a, true);
}

void receiver(App& a) {
    const ModeAcTelemetry& t = a.rx.modeac;
    if (!mlive(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("state", "%s", t.state == 2 ? "receiving replies" : t.state == 1 ? "Mode S only, no replies" : "searching");
    kv("replies", "%.1f / s, %llu accepted", (double)t.repliesPerSec, (unsigned long long)t.blocksOk);
    kv("candidates", "%llu pairs, %llu failed the shape test", (unsigned long long)t.candidates, (unsigned long long)t.blocksBad);
    kv("on Mode S", "%llu left out (a Mode S message was there)", (unsigned long long)t.masked);
    kv("waiting", "%llu codes seen once", (unsigned long long)t.pending);
    kv("signal", "%.1f dBFS, noise %.1f dBFS, SNR %.1f dB", (double)t.levelDbfs, (double)t.noiseDbfs, (double)t.snrDb);
    kv("codes", "%u (%u matched)", t.codeCount, t.matched);
    kv("Mode S", "%llu messages, %u aircraft (%u with position)", (unsigned long long)t.modeSGood, t.adsb.aircraftCount, t.adsb.withPosition);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Mode A is the squawk, Mode C the altitude in 100 ft steps. A reply does not say which of the two it is: a code shows as A/C until it can only be a squawk or is matched to a Mode S aircraft by its squawk or its altitude."));
    ImGui::PopTextWrapPos();
}

void panels(App& a) {
    const ModeAcTelemetry& t = a.rx.modeac;
    const bool on = mlive(a);
    (void)t; (void)on;
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi, colW = std::max(120.f, (W - 2 * gap) / 2.f), plotH = std::max(60.f, H - ImGui::GetTextLineHeightWithSpacing() - 6);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap * 0.5f);
    ImGui::BeginGroup();
    captionFit(colW, "Replies per second");
    std::vector<float> v(M.rate.begin(), M.rate.end());
    if (v.empty()) v.push_back(0);
    ImGui::PlotLines("##macrate", v.data(), (int)v.size(), 0, nullptr, 0.f, FLT_MAX, ImVec2(colW, plotH));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "Codes in the table");
    std::vector<float> c(M.codes.begin(), M.codes.end());
    if (c.empty()) c.push_back(0);
    ImGui::PlotLines("##maccodes", c.data(), (int)c.size(), 0, nullptr, 0.f, FLT_MAX, ImVec2(colW, plotH));
    ImGui::EndGroup();
}

void status(App& a) {
    const bool run = a.engine.running(), on = mlive(a);
    const ModeAcTelemetry& t = a.rx.modeac;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Replies", !on ? 0 : t.state == 2 ? 1 : t.state == 1 ? 2 : 0); flowNext(12 * gUi);
    lamp("Mode S", !on ? 0 : t.adsb.state == 2 ? 1 : t.adsb.state == 1 ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[64];
    if (!on) { ro("State", run ? "starting" : "stopped"); return; }
    ro("State", t.state == 2 ? "Receiving" : t.state == 1 ? "Mode S only" : "Searching");
    snprintf(b, sizeof b, "%.1f /s", (double)t.repliesPerSec); ro("Replies", b);
    snprintf(b, sizeof b, "%u (%u)", t.codeCount, t.matched); ro("Codes", b);
    snprintf(b, sizeof b, "%u", t.adsb.aircraftCount); ro("Aircraft", b);
    snprintf(b, sizeof b, "%.1f dB", (double)t.snrDb); ro("SNR", b);
    snprintf(b, sizeof b, "%.1f dBFS", (double)t.noiseDbfs); ro("Noise", b);
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Mode A/C";
    if (!mlive(a)) return;
    char b[96];
    snprintf(b, sizeof b, "%s  %u codes  %.1f replies/s", a.rx.modeac.state == 2 ? "receiving" : "searching", a.rx.modeac.codeCount, (double)a.rx.modeac.repliesPerSec);
    l2 = b;
}

void tuner(App& a, bool& retune) {
    double la, lo;
    receiverLocation(la, lo);    // loads the antenna position the ADS-B screen keeps
    ImGui::TextDisabled("Band");
    ImGui::SetNextItemWidth(std::min(220 * gUi, ImGui::GetContentRegionAvail().x));
    const bool std1090 = std::fabs(a.freqMhz - 1090.0) < 0.001;
    if (ImGui::BeginCombo("##macband", std1090 ? "1090 MHz  Mode A/C / Mode S" : "custom frequency")) {
        if (ImGui::Selectable("1090 MHz  Mode A/C / Mode S", std1090)) { a.freqMhz = 1090.0; retune = true; }
        ImGui::EndCombo();
    }
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Antenna position (for the map)"); ImGui::PopTextWrapPos(); }
    positionFields(a);
}

void decoder(App& a, bool&) {
    static const char* names[] = {"1 reply", "2 replies", "3 replies"};
    ImGui::TextDisabled("SHOW AFTER"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(100 * gUi, ImGui::GetContentRegionAvail().x));
    const int cur = std::max(1, std::min(3, M.confirm)) - 1;
    if (ImGui::BeginCombo("##macconf", names[cur])) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(names[i], cur == i)) M.confirm = i + 1;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A code goes into the table after this many replies with the same pattern. One reply lets in more false codes from garbled replies; three is the cleanest.");
    (void)a;
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("simulated airspace"); ImGui::PopTextWrapPos(); }
    flowNext(); ImGui::SetNextItemWidth(90 * gUi);
    int n = sc.modeOpt[0] == 0 ? 12 : sc.modeOpt[0];
    if (ImGui::SliderInt("##macn", &n, 1, 100, "%d aircraft")) { sc.modeOpt[0] = n; changed = true; }
    flowNext(); ImGui::SetNextItemWidth(90 * gUi);
    float mul = sc.modeVal[0] > 0 ? (float)sc.modeVal[0] : 1.f;
    if (ImGui::SliderFloat("##macm", &mul, 0.2f, 10.f, "x%.1f replies", ImGuiSliderFlags_Logarithmic)) { sc.modeVal[0] = mul; changed = true; }
    flowNext();
    bool ms = sc.modeOpt[1] == 0;
    if (ImGui::Checkbox("Mode S traffic", &ms)) { sc.modeOpt[1] = ms ? 0 : 1; changed = true; }
    flowNext();
    bool emg = sc.modeOpt[3] == 1;
    if (ImGui::Checkbox("squawk 7700", &emg)) { sc.modeOpt[3] = emg ? 1 : 0; changed = true; }
    flowNext();
    bool spi = sc.modeOpt[3] == 2;
    if (ImGui::Checkbox("ident pulse", &spi)) { sc.modeOpt[3] = spi ? 2 : 0; changed = true; }
}

void tick(App& a) {
    double la, lo;
    receiverLocation(la, lo);
    a.tune.synth.modeVal[2] = S.refLat; a.tune.synth.modeVal[3] = S.refLon;   // the simulated traffic flies around the antenna position
    const bool run = a.engine.running();
    if (run) {
        if (!M.wasRunning) { M.pushedLat = M.pushedLon = 1e9; M.pushedConfirm = -1; }
        if (receiverLocationSet() && (M.pushedLat != S.refLat || M.pushedLon != S.refLon)) {
            a.engine.modeac().setReference(S.refLat, S.refLon);
            M.pushedLat = S.refLat; M.pushedLon = S.refLon;
        }
        if (M.pushedConfirm != M.confirm) { a.engine.modeac().setConfirm(M.confirm); M.pushedConfirm = M.confirm; }
    }
    M.wasRunning = run;
    if (mlive(a) && a.rx.seq != M.lastSeq) {
        M.lastSeq = a.rx.seq;
        M.rate.push_back(a.rx.modeac.repliesPerSec); if (M.rate.size() > 240) M.rate.pop_front();
        M.codes.push_back((float)a.rx.modeac.codeCount); if (M.codes.size() > 240) M.codes.pop_front();
    }
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const ModeAcTelemetry& t = a.rx.modeac;
    out.push_back({"REPLIES / s", "%.1f", t.repliesPerSec, 0, 60, t.repliesPerSec > 0.5 ? 1 : 2});
    out.push_back({"CODES", "%.0f", (double)t.codeCount, 0, 60, 0});
    out.push_back({"SNR  dB", "%.1f", t.snrDb, 0, 40, t.snrDb >= 15 ? 1 : 2});
    out.push_back({"NOISE  dBFS", "%.1f", t.noiseDbfs, -80, -20, t.noiseDbfs > -35 ? 3 : 0});
}

} // namespace

// draw_ui.cpp: the map popped out over the whole window in Mode A/C (#235): the ADS-B map and its aircraft panel, on Mode A/C's aircraft
extern const ModeUi kModeAcUi;
bool modeacFullMap(App& a, ImVec2 disp) {
    if (modeUi(a.family) != &kModeAcUi) return false;
    AdsbView v(a);
    return adsbFullMap(a, disp);
}

extern const ModeUi kModeAcUi;
const ModeUi kModeAcUi = {
    .sideTitle = "CODES",
    .tabName = "Replies",
    .tabIcon = Ic::Compass,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .panels = panels,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .decoder = decoder,
    .synth = synth,
    .tick = tick,
    .meters = meters,
};
