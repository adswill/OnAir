// ARDOP screens: the band presets (the digital part of each band, upper sideband), Listen, then three views: the frames heard (UTC, frame
// type, session, calls, locator, level, details: ACK quality, the leader length of a connect acknowledge, the bytes of a data frame), the
// stations named in connect requests, IDs and pings, and the text of the data frames that passed their CRC. Monitor only.
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "winlink/winlink_ui.h"
#include "gfx/gfx.h"
#include "i18n/i18n.h"
#include "dect2/ardop/ardop_rx.h"
#include "dect2/ardop/ardop_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    bool wasRunning = false, loaded = false;
    bool listen = false; int pushedListen = -1; float pushedVol = -1; bool pushedMute = false;   // "Listen": off until switched on
    int view = 0;                        // 0 frames, 1 stations, 2 text
    int demod = dect2::HfDemodAuto;
};
State S;

bool live(const App& a) { return a.rx.standard == 141; }   // the engine reports its standard code minus one

void load() {
    if (S.loaded) return;
    S.loaded = true;
    S.listen = plat::prefs().getB("ardopListen", false);
    S.demod = hfDemodLoad("ardopDemod");
}

const ImVec4 kCallCol(1.0f, 0.82f, 0.36f, 1);

void utcText(double t, char* out, size_t n) {
    snprintf(out, n, "%s", dect2::timefmt::clock((int64_t)std::floor(t)).c_str());
}

ImVec4 frameColour(const std::string& kind) {
    if (kind == "data") return ImVec4(0.55f, 0.85f, 0.95f, 1);
    if (kind == "ACK") return ImVec4(0.40f, 0.95f, 0.55f, 1);
    if (kind == "NAK" || kind == "rejected (busy)" || kind == "rejected (bandwidth)") return ImVec4(0.95f, 0.55f, 0.45f, 1);
    if (kind == "ID" || kind == "connect request" || kind == "connect acknowledge" || kind == "ping" || kind == "ping acknowledge") return kCallCol;
    return ImVec4(0.80f, 0.84f, 0.88f, 1);
}

void presets(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Band"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("ARDOP has no calling frequency: every gateway publishes its own dial frequency. These buttons only jump to the digital part of each band."));
    flowNext(6 * gUi);
    int n = 0;
    const dect2::ArdopPreset* p = dect2::ardopPresets(n);
    for (int i = 0; i < n; i++) {
        const bool here = std::fabs(a.freqMhz - p[i].mhz) < 0.002;
        if (here) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
        char b[40]; snprintf(b, sizeof b, "%s##ardopp%d", p[i].name, i);
        if (ImGui::Button(b)) tuneFreq(a, p[i].mhz);
        if (here) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.4f MHz", p[i].mhz);
        flowNext(4 * gUi);
    }
    ImGui::TextDisabled("|"); flowNext(8 * gUi);
    listenControls(a, S.listen, "ardopListen", "ardop", tr("Play the sideband (dial + 200 Hz to 3.8 kHz) on the sound card; decoding is the same either way"));
    flowNext(8 * gUi);
    char cl[40]; snprintf(cl, sizeof cl, "%s##ardop", tr("Clear"));
    if (ImGui::Button(cl)) a.engine.ardop().clearResults();
    flowEnd();
}

std::string detail(const dect2::ArdopFrameLog& f) {
    char b[160];
    std::string s;
    if (f.bodyRead && !f.bodyOk) return tr("CRC or Reed-Solomon failed");
    if (f.quality >= 0 && f.snrDb == -1000 && f.name.compare(0, 4, "Data") == 0) { snprintf(b, sizeof b, tr("quality %d/100"), f.quality); return b; }
    if (f.name == "PingAck") { snprintf(b, sizeof b, tr("S/N %d dB, quality %d/100"), f.snrDb, f.quality); return b; }
    if (f.leaderMs >= 0) { snprintf(b, sizeof b, tr("leader %d ms"), f.leaderMs); return b; }
    if (f.kind == "data") {
        if (!f.bodyRead) return tr("header only (PSK, QAM or 600 baud data)");
        snprintf(b, sizeof b, "%d %s", (int)(f.hex.empty() ? 0 : (f.hex.size() + 1) / 3), tr("bytes"));
        s = b;
        if (f.rsFixed) { snprintf(b, sizeof b, ", %d %s", f.rsFixed, tr("bytes corrected")); s += b; }
        s += "  ";
        s += f.text;
        return s;
    }
    return std::string();
}

void framesView(App& a) {
    const auto& fr = a.rx.ardop.frames;
    if (!ImGui::BeginTable("##ardf", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(tr("Frame")); ImGui::TableSetupColumn(tr("Session")); ImGui::TableSetupColumn(tr("From"));
    ImGui::TableSetupColumn(tr("To")); ImGui::TableSetupColumn("dB"); ImGui::TableSetupColumn(tr("Detail"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    char b[48];
    for (const auto& f : fr) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); utcText((double)f.utc, b, sizeof b); ImGui::TextDisabled("%s", b);
        ImGui::TableNextColumn(); ImGui::TextColored(frameColour(f.kind), "%s", f.name.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("0x%02X, %s%s%s, offset %+.1f Hz", f.code, f.kind.c_str(), f.mod.empty() ? "" : ", ", f.mod.c_str(), f.offsetHz);
        ImGui::TableNextColumn();
        if (f.sid < 0 || f.sid == 255) ImGui::TextDisabled("-"); else ImGui::Text("%02X", f.sid);
        ImGui::TableNextColumn(); if (f.from.empty()) ImGui::TextDisabled("-"); else ImGui::TextColored(kCallCol, "%s", f.from.c_str());
        ImGui::TableNextColumn();
        if (!f.to.empty()) ImGui::TextUnformatted(f.to.c_str());
        else if (!f.locator.empty()) ImGui::TextDisabled("%s", f.locator.c_str());
        else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::Text("%+.0f", f.toneSnrDb);
        ImGui::TableNextColumn();
        ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(detail(f).c_str()); ImGui::PopTextWrapPos();
    }
    ImGui::PopFont();
    ImGui::EndTable();
    if (fr.empty()) ImGui::TextDisabled("%s", tr("no frames yet"));
}

void stationsView(App& a) {
    const auto& st = a.rx.ardop.stations;
    if (!ImGui::BeginTable("##ards", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Station")); ImGui::TableSetupColumn(tr("Last heard")); ImGui::TableSetupColumn(tr("Locator")); ImGui::TableSetupColumn(tr("Count"));
    ImGui::TableSetupColumn(tr("Called")); ImGui::TableSetupColumn(tr("Last"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    char b[32];
    for (const auto& s : st) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextColored(kCallCol, "%s", s.call.c_str());
        ImGui::TableNextColumn(); utcText((double)s.lastSeen, b, sizeof b); ImGui::TextDisabled("%s", b);
        ImGui::TableNextColumn(); if (s.locator.empty()) ImGui::TextDisabled("-"); else ImGui::TextUnformatted(s.locator.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)s.count);
        ImGui::TableNextColumn(); if (s.heardTo.empty()) ImGui::TextDisabled("-"); else ImGui::TextUnformatted(s.heardTo.c_str());
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", s.last.c_str());
    }
    ImGui::PopFont();
    ImGui::EndTable();
    if (st.empty()) ImGui::TextDisabled("%s", tr("no stations yet"));
}

void textView(App& a) {
    const auto& t = a.rx.ardop;
    if (ImGui::BeginChild("##ardt", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        ImGui::PushFont(a.mono, 0);
        if (t.text.empty()) ImGui::TextDisabled("%s", tr("no data yet"));
        else { ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(t.text.c_str()); ImGui::PopTextWrapPos(); }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
        ImGui::PopFont();
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------- hooks

void tick(App& a) {
    load();
    winlinkTick(a);   // Winlink email (app/winlink): its session runs while the screen is open
    if (!a.engine.running()) { S.wasRunning = false; return; }
    dect2::ArdopReceiver& r = a.engine.ardop();
    if (!S.wasRunning || (int)S.listen != S.pushedListen) { r.setListen(S.listen); S.pushedListen = S.listen; }
    r.setDemod(dect2::hfDemodResolve(S.demod, a.freqMhz));   // Auto follows the dial
    if (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute) {   // the sound controls are shared with the other modes
        r.setVolume(a.volume); r.setMuted(a.muted);
        S.pushedVol = a.volume; S.pushedMute = a.muted;
    }
    S.wasRunning = true;
}

void tuner(App& a, bool&) {
    load();
    hfDemodControl(a, S.demod, "ardopDemod", "ardop");
}

void tab(App& a) {
    load();
    presets(a);
    ImGui::Spacing();
    subNav("ardv", S.view, {"Frames", "Stations", "Text", "Winlink"});
    if (S.view == 3) { winlinkPanel(a, 1); return; }
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    if (S.view == 0) framesView(a); else if (S.view == 1) stationsView(a); else textView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    if (!ImGui::BeginTable("##ardl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn(tr("Station")); ImGui::TableSetupColumn(tr("Heard"), ImGuiTableColumnFlags_WidthStretch);
    char b[32];
    for (const auto& s : a.rx.ardop.stations) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextColored(kCallCol, "%s", s.call.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); utcText((double)s.lastSeen, b, sizeof b); ImGui::TextDisabled("%s, %llux", b, (unsigned long long)s.count);
    }
    ImGui::EndTable();
    if (a.rx.ardop.stations.empty()) ImGui::TextDisabled("%s", tr("no stations yet"));
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::ArdopTelemetry& t = a.rx.ardop;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv(tr("state"), "%s", dect2::ardopSummary(t).c_str());
    kv(tr("frames"), "%llu, %llu %s", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, tr("unreadable"));
    kv(tr("data frames"), "%llu OK, %llu %s", (unsigned long long)t.dataOk, (unsigned long long)t.dataBad, tr("failed"));
    kv(tr("other modulations"), "%llu", (unsigned long long)t.otherModulations);
    kv(tr("Reed-Solomon corrections"), "%llu", (unsigned long long)t.rsCorrections);
    kv(tr("tone offset"), "%+.1f Hz", t.cfoHz);
    kv(tr("input"), "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kv(tr("signal time"), "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Signal"), !on ? 0 : a.rx.ardop.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp(tr("Frames"), !on ? 0 : a.rx.ardop.dataValid ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::ardopSummary(a.rx.ardop).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "ARDOP";
    if (live(a)) l2 = dect2::ardopSummary(a.rx.ardop);
}

} // namespace

extern const ModeUi kArdopUi;
const ModeUi kArdopUi = {
    .sideTitle = "STATIONS",
    .tabName = "ARDOP",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .tick = tick,
};
