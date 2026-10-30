// dPMR screens: the current call (own / called id, call type, colour code, voice), the call log (voice, status, short messages, packet
// data), the decoded headers and end frames, the PMR446 channel presets with "scan all 16", and a "?" about the mbelib library the
// voice needs (docs/CODECS.md).
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "ui/kit/mode_frame.h"
#include "ui/kit/session_list.h"
#include "voice/voice_monitor_ui.h"
#include "dect2/codec/codec_registry.h"
#include "dect2/codec/codec_registry.h"
#include "../codec/speech_codec_ui.h"
#include "dect2/dpmr/dpmr_proto.h"
#include "dect2/dpmr/dpmr_rx.h"
#include "dect2/dpmr/dpmr_tel.h"
#include "dect2/nxdn/nxdn_ambe.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;                        // 0 now, 1 calls, 2 messages
    bool wasRunning = false, loaded = false, scan = false, pushedScan = false;
    float pushedVol = -1; bool pushedMute = false;
};
State S;

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.scan = plat::prefs().getB("dpmrScan", false);
}

bool live(const App& a) { return a.rx.standard == 46; }   // the engine reports its standard code minus one

std::string clock(int64_t t) {
    if (t <= 0) return "-";
    return dect2::timefmt::clock(t);
}

const char* kindOf(const dect2::DpmrCall& c) { return c.kind == 1 ? "status" : c.kind == 2 ? "message" : c.kind == 3 ? "packet" : "voice"; }
std::string ccText(int cc) { return cc < 0 ? std::string("?") : std::to_string(cc); }

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

void voiceLine() {
    ImGui::AlignTextToFramePadding();
    std::string lib;
    if (!dect2::codec::pluginLoaded("ambe2")) dect2::nxdnAmbeAvailable(&lib);
    appcodec::voiceCodecLine("ambe2", "AMBE+2", lib);
    ImGui::SameLine(0, 4 * gUi);
    codecHelpButton(CodecHelp::DpmrAmbe);
}

void nowView(App& a) {
    voiceLine();
    ImGui::SameLine(0, 8 * gUi);
    voiceKeysLink(a);
    ImGui::Spacing();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to hear dPMR"); return; }
    const dect2::DpmrTelemetry& t = a.rx.dpmr;
    const dect2::DpmrCall& n = t.now;
    if (n.wallTime == 0) { ImGui::TextDisabled("%s", t.scanning ? "scanning the PMR446 channels, no call heard yet" : "no call heard yet"); return; }
    ImGui::PushFont(a.mono, 0);
    ImGui::TextColored(n.active ? pal::okGreen() : pal::grey(0.6f), "%s  %s  ->  %s", n.active ? "ON AIR" : "last", dect2::dpmrId(n.src).c_str(), dect2::dpmrId(n.dst).c_str());
    ImGui::PopFont();
    kvRow(a, "call", "%s, %s, colour code %s%s", dect2::dpmr::callTypeName(n.callType), dect2::dpmr::commModeName(n.commMode), ccText(n.cc).c_str(), n.lateEntry ? ", joined late" : "");
    if (n.channel) kvRow(a, "channel", "PMR446 %d (%.5f MHz)", n.channel, dect2::dpmr::pmr446Mhz(n.channel));
    if (n.emergency) { ImGui::TextDisabled("note"); kvColumn(130 * gUi); ImGui::TextColored(pal::warnAmber(), "emergency call"); }
    kvRow(a, "duration", "%.1f s, %u voice frames", n.durSec, n.voiceFrames);
    kvRow(a, "started", "%s%s", clock(n.wallTime).c_str(), dect2::timefmt::sfx());
    kvRow(a, "voice", "%s", n.voice ? "playing (AMBE+2 through mbelib)" : t.ambe ? "-" : "no mbelib: frames counted only");
    kvRow(a, "bit errors", "%.2f %% (corrected by the Hamming code)", n.berPct);
}

void callsView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the calls"); return; }
    const auto& log = a.rx.dpmr.log;
    if (ImGui::Button("Copy")) {
        std::string txt;
        for (const auto& e : log) {
            char b[400];
            snprintf(b, sizeof b, "%s\t%s\t%s\t%s\t%s\tCC %s\t%s\t%.1f s\n", clock(e.wallTime).c_str(), kindOf(e), dect2::dpmrId(e.src).c_str(), dect2::dpmrId(e.dst).c_str(),
                     dect2::dpmr::callTypeName(e.callType), ccText(e.cc).c_str(), e.text.c_str(), e.durSec);
            txt += b;
        }
        ImGui::SetClipboardText(txt.c_str());
    }
    if (!ImGui::BeginTable("##dpmrlog", 8 + (wideActive(a) ? 1 : 0), ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    if (wideActive(a)) ImGui::TableSetupColumn("Carrier"); ImGui::TableSetupColumn(dect2::timefmt::headTime()); ImGui::TableSetupColumn("Kind"); ImGui::TableSetupColumn("From"); ImGui::TableSetupColumn("To");
    ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("CC"); ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Duration");
    ImGui::TableHeadersRow();
    auto row = [&](const dect2::DpmrCall& e, bool on) {
        ImGui::TableNextRow();
        if (wideActive(a)) { ImGui::TableNextColumn(); wideCarrierCell(a, e.carrierId, e.carrierHz); }
        const ImVec4 c = on ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::TableNextColumn(); ImGui::TextColored(c, "%s", on ? "on air" : clock(e.wallTime).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(kindOf(e));
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextColored(c, "%s", dect2::dpmrId(e.src).c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(dect2::dpmrId(e.dst).c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::dpmr::callTypeName(e.callType));
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ccText(e.cc).c_str());
        ImGui::TableNextColumn();
        if (e.emergency) { ImGui::TextColored(pal::warnAmber(), "emergency"); ImGui::SameLine(); }
        ImGui::TextUnformatted(e.kind == 0 ? (e.voice ? "voice played" : "") : e.text.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.1f s", e.durSec);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s%s%s, %.1f %% bit errors", e.endSeen ? "ended with the end frame" : "signal lost", e.lateEntry ? ", joined late" : "",
                              e.channel ? (", PMR446 channel " + std::to_string(e.channel)).c_str() : "", e.berPct);
    };
    if (a.rx.dpmr.now.active) row(a.rx.dpmr.now, true);
    for (const auto& e : log) row(e, false);
    ImGui::EndTable();
    if (log.empty() && !a.rx.dpmr.now.active) ImGui::TextDisabled("nothing heard yet");
}

void messageView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see messages"); return; }
    const auto& ev = a.rx.dpmr.events;
    ImGui::PushFont(a.mono, 0);
    for (const auto& s : ev) ImGui::TextUnformatted(s.c_str());
    ImGui::PopFont();
    if (ev.empty()) ImGui::TextDisabled("no headers yet");
}

// ---------------------------------------------------------------- hooks

void tick(App& a) {
    if (live(a)) appcodec::voiceCodecTick("ambe2");   // the user's codec plug-in (docs/CODECS.md), loaded whichever tab is shown
    loadState();
    if (!a.engine.running()) { S.wasRunning = false; return; }
    dect2::DpmrReceiver& r = a.engine.dpmr();
    if (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute) {   // the sound controls are shared with the other modes
        r.setVolume(a.volume); r.setMuted(a.muted);
        S.pushedVol = a.volume; S.pushedMute = a.muted;
    }
    if (!S.wasRunning || S.scan != S.pushedScan) { r.setScan(S.scan); S.pushedScan = S.scan; }
    S.wasRunning = true;
}

void tuner(App& a, bool& retune) {
    loadState();
    ImGui::TextDisabled("PMR446");
    int cur = 0;
    for (int c = 1; c <= 16; c++) if (std::fabs(a.freqMhz - dect2::dpmr::pmr446Mhz(c)) < 0.0005) cur = c;
    char label[32];
    snprintf(label, sizeof label, cur ? "channel %d" : "other", cur);
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(120 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##dpmrch", label)) {
        for (int c = 1; c <= 16; c++) {
            char b[40];
            snprintf(b, sizeof b, "%2d  %.5f MHz", c, dect2::dpmr::pmr446Mhz(c));
            if (ImGui::Selectable(b, c == cur)) { tuneFreq(a, dect2::dpmr::pmr446Mhz(c)); retune = true; }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("PMR446 digital channels (6.25 kHz): 446.103125 to 446.196875 MHz. Licensed dPMR channels: type the frequency.");
    ImGui::SameLine(0, 8 * gUi);
    if (ImGui::Checkbox("scan all 16", &S.scan)) { plat::prefs().setB("dpmrScan", S.scan); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Step through the 16 PMR446 channels (half a second each) and stay where a call is heard.\nTune to a PMR446 channel first: all 16 fit in the captured band.");
}

void tab(App& a) {
    {   // the voice monitor's strip, the same in every voice mode
        const dect2::DpmrTelemetry& t = a.rx.dpmr;
        voiceui::Link l;
        l.live = live(a); l.locked = l.live && t.state >= 1; l.codecName = "AMBE+2"; l.codecId = "ambe2";
        l.codecReady = t.ambe || dect2::codec::pluginLoaded("ambe2");
        l.errorRate = l.locked ? t.berPct / 100.f : -1.f;
        voiceui::strip(a, "dpmr", "dPMR", l);
    }
    kit::modeHeader({45, "dPMR", live(a) ? 1 : a.engine.running() ? 2 : 0, -1000.f, a.freqMhz * 1e6});
    kit::modeNav("dpmrv", S.view, {{tr("Live"), 0}, {tr("Calls"), 3}, {tr("Call list"), 1, true}, {tr("Messages"), 2, true}});
    if (S.view == 3) { kit::sessionItems(45, 1, "dpmr"); return; }
    if (S.view == 0) nowView(a); else if (S.view == 1) callsView(a); else messageView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    if (resultsClearButton("dpmr")) a.engine.dpmr().clearResults();
    const auto& log = a.rx.dpmr.log;
    if (!ImGui::BeginTable("##dpmrl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Radio"); ImGui::TableSetupColumn("Heard", ImGuiTableColumnFlags_WidthStretch);
    const auto& n = a.rx.dpmr.now;
    if (n.active) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextColored(pal::okGreen(), "%s", dect2::dpmrId(n.src).c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextColored(pal::okGreen(), "on air -> %s", dect2::dpmrId(n.dst).c_str());
    }
    std::vector<int64_t> seen;
    for (const auto& e : log) {
        if (std::find(seen.begin(), seen.end(), e.src) != seen.end()) continue;
        seen.push_back(e.src);
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(dect2::dpmrId(e.src).c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s  -> %s", clock(e.wallTime).c_str(), dect2::dpmrId(e.dst).c_str());
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    wideCarriersTable(a, 0);
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::DpmrTelemetry& t = a.rx.dpmr;
    kvRow(a, "state", "%s", dect2::dpmrSummary(t).c_str());
    kvRow(a, "signal", "symbol SNR %.1f dB, +3 deviation %.0f Hz, offset %+.0f Hz%s", t.snrDb, t.deviationHz, t.cfoHz, t.inverted ? ", inverted" : "");
    if (t.channel) kvRow(a, "channel", "PMR446 %d%s", t.channel, t.scanning ? " (scanning)" : "");
    kvRow(a, "colour code", "%s", ccText(t.cc).c_str());
    kvRow(a, "frame syncs", "FS1 %llu, FS2 %llu, FS3 %llu, FS4 %llu", (unsigned long long)t.fs1, (unsigned long long)t.fs2, (unsigned long long)t.fs3, (unsigned long long)t.fs4);
    kvRow(a, "bit errors", "%.2f %% (corrected by the Hamming code)", t.berPct);
    kvRow(a, "header", "%llu good, %llu failed", (unsigned long long)t.hdrOk, (unsigned long long)t.hdrBad);
    kvRow(a, "CCH", "%llu good, %llu failed CRC, slow data 0x%05X", (unsigned long long)t.cchOk, (unsigned long long)t.cchBad, t.slowData);
    kvRow(a, "data blocks", "%llu good, %llu failed CRC", (unsigned long long)t.dataOk, (unsigned long long)t.dataBad);
    kvRow(a, "end frames", "%llu good, %llu failed", (unsigned long long)t.endOk, (unsigned long long)t.endBad);
    kvRow(a, "voice", "%llu frames, %llu decoded%s, %llu calls joined late", (unsigned long long)t.voiceFrames, (unsigned long long)t.voiceDecoded, t.ambe ? "" : " (mbelib not found: click ? for the file names and folders)",
          (unsigned long long)t.lateEntries);
    if (t.ambe) kvRow(a, "mbelib", "%s", t.ambePath.c_str());
    kvRow(a, "input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kvRow(a, "signal time", "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Sync", !on ? 0 : a.rx.dpmr.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("On air", !on ? 0 : a.rx.dpmr.state == 2 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::dpmrSummary(a.rx.dpmr).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "dPMR";
    if (live(a)) l2 = dect2::dpmrSummary(a.rx.dpmr);
}

} // namespace

extern const ModeUi kDpmrUi;
const ModeUi kDpmrUi = {
    .sideTitle = "RADIOS",
    .tabName = "dPMR",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .tick = tick,
};
