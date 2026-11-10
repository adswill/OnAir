// CubeSat telemetry screens: the frame log (time, satellite, framing, callsigns or CSP addresses, size, Reed-Solomon corrections) with the
// selected frame's fields and a hex / ASCII dump, the editable satellite table (name, NORAD number, frequency, framing, speed, callsign)
// the frames are named after, and the KISS forwarding option. The pass planner (passes_ui.cpp) starts this mode at AOS for its CubeSat rows
// and keeps the Doppler corrected; it tells the receiver which satellite it follows.
#include "app.h"
#include "dect2/cubesat/cubesat_proto.h"
#include "dect2/cubesat/cubesat_rx.h"
#include "dect2/cubesat/cubesat_tel.h"
#include "dect2/net/feeds.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

void cubesatSmartView(App& a);   // cubesat_smart_ui.cpp

namespace {

using namespace dect2::cubesat;

struct State {
    bool loaded = false, pushed = false;
    int view = 0;                        // 0 frames, 1 satellites
    std::vector<SatDef> sats;
    bool forwardKiss = true;
    int ccsdsLen = 255;
    int sel = -1;                        // the selected frame: index into the newest-first list, counted from the oldest so it stays put
    uint64_t selKey = 0;
    char filter[64] = "";
};
State S;

void saveState() {
    plat::prefs().setS("cubesatSats", satsToText(S.sats));
    plat::prefs().setB("cubesatKiss", S.forwardKiss);
    plat::prefs().setI("cubesatCcsdsLen", S.ccsdsLen);
    S.pushed = false;
}

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    const std::string s = plat::prefs().getS("cubesatSats", "");
    S.sats = s.empty() ? defaultSats() : satsFromText(s);
    S.forwardKiss = plat::prefs().getB("cubesatKiss", true);
    S.ccsdsLen = (int)plat::prefs().getI("cubesatCcsdsLen", 255);
}

bool live(const App& a) { return a.rx.standard == 62; }   // the engine reports its standard code minus one

std::string clock(int64_t t, double sigSec) {
    char b[24];
    if (t <= 0) { snprintf(b, sizeof b, "%.1f s", sigSec); return b; }
    const time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    snprintf(b, sizeof b, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

std::string addrText(const dect2::CubeFrame& f) {
    if (!f.from.empty()) return f.from + " > " + f.to + (f.path.empty() ? "" : "," + f.path);
    if (f.hasCsp) { char b[64]; snprintf(b, sizeof b, "CSP %d:%d > %d:%d", f.csp.src, f.csp.sport, f.csp.dst, f.csp.dport); return b; }
    return "-";
}

bool passes(const dect2::CubeFrame& f) {
    if (!S.filter[0]) return true;
    const std::string q = lower(S.filter);
    for (const std::string& s : {f.sat, f.from, f.to, std::string(framingShort(f.framing)), asciiOf(f.payload)})
        if (lower(s).find(q) != std::string::npos) return true;
    return false;
}

uint64_t keyOf(const dect2::CubeFrame& f) { return (uint64_t)(f.timeSec * 1000.0) * 31u + f.data.size(); }

void frameDetail(const App& a, const dect2::CubeFrame& f) {
    kvRow(a, "satellite", "%s%s", f.sat.empty() ? "not in the table" : f.sat.c_str(), f.norad ? (" (#" + std::to_string(f.norad) + ")").c_str() : "");
    kvRow(a, "framing", "%s, %d baud%s", framingName(f.framing), f.baud, f.rsErrors > 0 ? (", " + std::to_string(f.rsErrors) + " byte errors put right").c_str() : f.rsErrors == 0 ? ", no byte errors" : "");
    if (!f.from.empty()) kvRow(a, "AX.25", "%s > %s%s%s, %s, PID %02X", f.from.c_str(), f.to.c_str(), f.path.empty() ? "" : " via ", f.path.c_str(), f.type.c_str(), f.pid < 0 ? 0 : f.pid);
    if (f.hasCsp) kvRow(a, "CSP", "priority %d, source %d port %d, destination %d port %d, flags %02X", f.csp.prio, f.csp.src, f.csp.sport, f.csp.dst, f.csp.dport, f.csp.flags);
    kvRow(a, "received", "%s, SNR %.1f dB%s", clock(f.wallTime, f.timeSec).c_str(), f.snrDb, f.forwarded ? ", sent to KISS" : "");
    ImGui::Spacing();
    ImGui::TextDisabled("ASCII");
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(asciiOf(f.payload).c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
    ImGui::Spacing();
    ImGui::TextDisabled("hex (%zu bytes)", f.data.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(hexDump(f.data).c_str());
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(hexDump(f.data).c_str()); ImGui::PopFont();
}

void framesView(App& a) {
    const bool on = live(a);
    const std::vector<dect2::CubeFrame> none;
    const std::vector<dect2::CubeFrame>& fr = on ? a.rx.cube.frames : none;
    ImGui::SetNextItemWidth(std::min(220 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##cbf", "filter: satellite, callsign, text", S.filter, sizeof S.filter);
    flowNext();
    if (ImGui::SmallButton("Clear")) { a.engine.cubesat().clearFrames(); S.selKey = 0; }
    const float h = ImGui::GetContentRegionAvail().y;
    const float listH = S.selKey ? h * 0.45f : h;
    if (ImGui::BeginTable("##cbt", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, listH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Satellite"); ImGui::TableSetupColumn("Framing");
        ImGui::TableSetupColumn("From / to"); ImGui::TableSetupColumn("Bytes"); ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& f : fr) {
            if (!passes(f)) continue;
            const uint64_t k = keyOf(f);
            ImGui::TableNextRow();
            ImGui::PushID((int)k);
            ImGui::TableNextColumn();
            if (ImGui::Selectable(clock(f.wallTime, f.timeSec).c_str(), S.selKey == k, ImGuiSelectableFlags_SpanAllColumns)) S.selKey = S.selKey == k ? 0 : k;
            ImGui::TableNextColumn(); ImGui::TextUnformatted(f.sat.empty() ? "-" : f.sat.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%s %d", framingShort(f.framing), f.baud);
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(addrText(f).c_str()); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::Text("%zu%s", f.data.size(), f.rsErrors > 0 ? " RS" : "");
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(asciiOf(f.payload).substr(0, 120).c_str()); ImGui::PopFont();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (fr.empty()) ImGui::TextDisabled("%s", on ? "no frames yet" : a.engine.running() ? "starting" : "stopped");
    if (!S.selKey) return;
    for (const auto& f : fr)
        if (keyOf(f) == S.selKey) {
            ImGui::Separator();
            if (ImGui::BeginChild("##cbd", ImVec2(0, 0))) frameDetail(a, f);
            ImGui::EndChild();
            return;
        }
}

void satsView(App& a) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Frames are named after this table: by the AX.25 callsign, else the satellite the pass planner follows, else the frequency tuned. "
                        "Every framing is decoded whatever the table says; by frequency a frame is named only after a satellite listed with its framing. Frequencies change: edit them.");
    ImGui::PopTextWrapPos();
    bool changed = false;
    int del = -1;
    if (ImGui::BeginTable("##cbs", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("NORAD"); ImGui::TableSetupColumn("MHz");
        ImGui::TableSetupColumn("Framing"); ImGui::TableSetupColumn("Baud"); ImGui::TableSetupColumn("Callsign"); ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)S.sats.size(); i++) {
            SatDef& s = S.sats[(size_t)i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            char nb[64]; snprintf(nb, sizeof nb, "%s", s.name.c_str());
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); if (ImGui::InputText("##n", nb, sizeof nb)) { s.name = nb; changed = true; }
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(70 * gUi); if (ImGui::InputInt("##no", &s.norad, 0, 0)) changed = true;
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(80 * gUi); if (ImGui::InputDouble("##mhz", &s.mhz, 0, 0, "%.3f")) changed = true;
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(150 * gUi);
            if (ImGui::BeginCombo("##fr", framingName(s.framing))) {
                for (int k = 0; k < kNumFramings; k++) if (ImGui::Selectable(framingName(k), k == s.framing)) { s.framing = k; changed = true; }
                ImGui::EndCombo();
            }
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(60 * gUi); if (ImGui::InputInt("##bd", &s.baud, 0, 0)) changed = true;
            char cb[16]; snprintf(cb, sizeof cb, "%s", s.call.c_str());
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(80 * gUi); if (ImGui::InputText("##cs", cb, sizeof cb, ImGuiInputTextFlags_CharsUppercase)) { s.call = cb; changed = true; }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Tune")) tuneFreq(a, s.mhz);
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) del = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (del >= 0) { S.sats.erase(S.sats.begin() + del); changed = true; }
    if (ImGui::Button("Add a satellite")) { SatDef d; d.name = "new satellite"; d.mhz = 437.0; d.framing = Ax25G3ruh; d.baud = 9600; S.sats.push_back(d); changed = true; }
    ImGui::SameLine();
    if (ImGui::Button("Restore the defaults")) { S.sats = defaultSats(); changed = true; }
    if (changed) saveState();
}

// ---------------------------------------------------------------- hooks

void tick(App& a) {
    loadState();
    if (S.pushed) return;
    S.pushed = true;
    a.engine.cubesat().setSatellites(S.sats);
    a.engine.cubesat().setForwardKiss(S.forwardKiss);
    a.engine.cubesat().setCcsdsLength(S.ccsdsLen);
}

void tab(App& a) {
    loadState();
    subNav("cbv", S.view, {"Smart", "Frames", "Satellites"});
    if (S.view == 0) cubesatSmartView(a); else if (S.view == 1) framesView(a); else satsView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::CubeTelemetry& t = a.rx.cube;
    struct Heard { std::string name; int n = 0; double last = -1e9; };
    std::vector<Heard> h;
    for (const auto& f : t.frames) {
        const std::string n = !f.sat.empty() ? f.sat : !f.from.empty() ? f.from : std::string(framingShort(f.framing));
        auto it = std::find_if(h.begin(), h.end(), [&](const Heard& x) { return x.name == n; });
        if (it == h.end()) { h.push_back({n, 0, f.timeSec}); it = h.end() - 1; }
        it->n++;
    }
    if (!ImGui::BeginTable("##cbl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Satellite", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Frames");
    for (const auto& x : h) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, t.timeSec - x.last < 10 ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text));
        ImGui::TextUnformatted(x.name.c_str());
        ImGui::PopStyleColor();
        ImGui::TableNextColumn(); ImGui::Text("%d", x.n);
    }
    ImGui::EndTable();
    if (h.empty()) ImGui::TextDisabled("%s", t.tracked.empty() ? "nothing heard yet" : ("waiting for " + t.tracked).c_str());
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::CubeTelemetry& t = a.rx.cube;
    kvRow(a, "state", "%s", dect2::cubesatSummary(t).c_str());
    if (!t.tracked.empty()) kvRow(a, "pass planner", "following %s (#%d), Doppler corrected about once a second", t.tracked.c_str(), t.trackedNorad);
    if (!t.trackedSetup.empty()) kvRow(a, "set up for it", "%s", t.trackedSetup.c_str());
    kvRow(a, "frames", "%llu good, %llu failed their checks, %llu put right by Reed-Solomon", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.rsFixed);
    kvRow(a, "by framing", "AFSK %llu, G3RUH %llu, AX100 Mode 5 %llu, Mode 6 %llu, CCSDS %llu", (unsigned long long)t.perFraming[0], (unsigned long long)t.perFraming[1],
          (unsigned long long)t.perFraming[2], (unsigned long long)t.perFraming[3], (unsigned long long)t.perFraming[4]);
    kvRow(a, "KISS", "%llu frames sent%s", (unsigned long long)t.kissSent, dect2::Feeds::get().wantKiss() ? "" : " (the KISS feed is off: View > Data feeds)");
    kvRow(a, "signal", "%s, SNR %.1f dB, offset %+.2f kHz", t.carrier ? "carrier" : "no carrier", t.snrDb, t.cfoHz / 1e3);
    kvRow(a, "input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kvRow(a, "signal time", "%.1f s", t.timeSec);
}

void decoder(App& a, bool&) {
    loadState();
    if (ImGui::Checkbox("Frames to KISS", &S.forwardKiss)) { saveState(); tick(a); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every decoded frame goes to the KISS TCP server of the data feeds (View > Data feeds), for SatNOGS-style tools:\n"
                                                  "AX.25 frames without their FCS, AX100 frames as the CSP packet, CCSDS frames as the data field.");
    flowNext();
    ImGui::TextDisabled("CCSDS block");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(70 * gUi);
    if (ImGui::InputInt("##cbcl", &S.ccsdsLen, 0, 0)) { S.ccsdsLen = std::max(33, std::min(255, S.ccsdsLen)); saveState(); tick(a); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The length of a CCSDS Reed-Solomon code block in bytes, 32 of them parity (255: the full code; shorter: a shortened code)");
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    ImGui::TextDisabled("simulated frames");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(190 * gUi, ImGui::GetContentRegionAvail().x));
    const int cur = sc.modeOpt[0] >= 1 && sc.modeOpt[0] <= kNumFramings ? sc.modeOpt[0] : 0;
    if (ImGui::BeginCombo("##cbsy", cur ? framingName(cur - 1) : "every framing")) {
        if (ImGui::Selectable("every framing", cur == 0)) { sc.modeOpt[0] = 0; changed = true; }
        for (int k = 0; k < kNumFramings; k++) if (ImGui::Selectable(framingName(k), cur == k + 1)) { sc.modeOpt[0] = k + 1; changed = true; }
        ImGui::EndCombo();
    }
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Carrier", !on ? 0 : a.rx.cube.carrier ? 1 : 0); flowNext(10 * gUi);
    lamp("Frames", !on ? 0 : a.rx.cube.state >= 2 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::cubesatSummary(a.rx.cube).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "CubeSat telemetry";
    if (live(a)) l2 = dect2::cubesatSummary(a.rx.cube);
}

} // namespace

extern const ModeUi kCubesatUi;
const ModeUi kCubesatUi = {
    .sideTitle = "SATELLITES",
    .tabName = "Frames",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .decoder = decoder,
    .synth = synth,
    .tick = tick,
};
