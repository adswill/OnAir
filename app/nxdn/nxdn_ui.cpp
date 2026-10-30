// NXDN screens: the current call (unit id, talkgroup, call type, encryption, voice), the call log (voice, status and short data), the
// decoded control messages, the site information, and a "?" about the mbelib library the voice needs (docs/CODECS.md).
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "voice/voice_monitor_ui.h"
#include "dect2/codec/codec_registry.h"
#include "../codec/speech_codec_ui.h"
#include "dect2/nxdn/nxdn_ambe.h"
#include "dect2/nxdn/nxdn_proto.h"
#include "dect2/nxdn/nxdn_rx.h"
#include "dect2/nxdn/nxdn_tel.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;                        // 0 now, 1 calls, 2 messages, 3 site
    bool wasRunning = false;
    float pushedVol = -1; bool pushedMute = false;
};
State S;

bool live(const App& a) { return a.rx.standard == 39; }   // the engine reports its standard code minus one

std::string clock(int64_t t) {
    if (t <= 0) return "-";
    return dect2::timefmt::clock(t);
}

std::string dstOf(const dect2::NxdnCall& c) { return (c.callType == 1 ? "TG " : "") + dect2::nxdnId(c.dst); }
const char* kindOf(const dect2::NxdnCall& c) { return c.kind == 1 ? "status" : c.kind == 2 ? "data" : "voice"; }

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
    codecHelpButton(CodecHelp::NxdnAmbe);
}

void nowView(App& a) {
    voiceLine();
    ImGui::Spacing();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to hear NXDN"); return; }
    const dect2::NxdnTelemetry& t = a.rx.nxdn;
    const dect2::NxdnCall& n = t.now;
    if (n.wallTime == 0) { ImGui::TextDisabled("no call heard yet"); return; }
    ImGui::PushFont(a.mono, 0);
    ImGui::TextColored(n.active ? pal::okGreen() : pal::grey(0.6f), "%s  %s  ->  %s", n.active ? "ON AIR" : "last", dect2::nxdnId(n.src).c_str(), dstOf(n).c_str());
    ImGui::PopFont();
    kvRow(a, "call", "%s, NXDN%d, RAN %d%s", dect2::nxdn::callTypeName(n.callType), n.baud == 4800 ? 96 : 48, n.ran, n.lateEntry ? ", joined late" : "");
    if (n.cipher) {
        ImGui::TextDisabled("note"); kvColumn(130 * gUi);
        if (n.decrypt.empty()) ImGui::TextColored(pal::warnAmber(), "encrypted (cipher %d, key %d): no sound", n.cipher, n.keyId);
        else { ImGui::Text("cipher %d, key %d:", n.cipher, n.keyId); ImGui::SameLine(); voiceKeysStatus(a, n.decrypt); }   // the key status and the way to the Keys window
    }
    kvRow(a, "duration", "%.1f s, %u voice frames", n.durSec, n.voiceFrames);
    kvRow(a, "started", "%s%s", clock(n.wallTime).c_str(), dect2::timefmt::sfx());
    kvRow(a, "voice", "%s", n.voice ? "playing (AMBE+2 through mbelib)" : t.ambe ? "-" : "no mbelib: frames counted only");
    kvRow(a, "bit errors", "%.2f %% (control channels)", n.berPct);
}

void callsView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the calls"); return; }
    const auto& log = a.rx.nxdn.log;
    if (ImGui::Button("Copy")) {
        std::string txt;
        for (const auto& e : log) {
            char b[400];
            snprintf(b, sizeof b, "%s\t%s\t%s\t%s\t%s\tNXDN%d\t%s\t%.1f s\n", clock(e.wallTime).c_str(), kindOf(e), dect2::nxdnId(e.src).c_str(), dstOf(e).c_str(),
                     dect2::nxdn::callTypeName(e.callType), e.baud == 4800 ? 96 : 48, e.text.c_str(), e.durSec);
            txt += b;
        }
        ImGui::SetClipboardText(txt.c_str());
    }
    if (!ImGui::BeginTable("##nxdnlog", 8 + (wideActive(a) ? 1 : 0), ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    if (wideActive(a)) ImGui::TableSetupColumn("Carrier"); ImGui::TableSetupColumn(dect2::timefmt::headTime()); ImGui::TableSetupColumn("Kind"); ImGui::TableSetupColumn("From"); ImGui::TableSetupColumn("To");
    ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("RAN"); ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Length");
    ImGui::TableHeadersRow();
    auto row = [&](const dect2::NxdnCall& e, bool on) {
        ImGui::TableNextRow();
        if (wideActive(a)) { ImGui::TableNextColumn(); wideCarrierCell(a, e.carrierId, e.carrierHz); }
        const ImVec4 c = on ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::TableNextColumn(); ImGui::TextColored(c, "%s", on ? "on air" : clock(e.wallTime).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(kindOf(e));
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextColored(c, "%s", dect2::nxdnId(e.src).c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(dstOf(e).c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::nxdn::callTypeName(e.callType));
        ImGui::TableNextColumn(); ImGui::Text("%d", e.ran);
        ImGui::TableNextColumn();
        if (e.cipher) { ImGui::TextColored(pal::warnAmber(), "encrypted"); ImGui::SameLine(); }
        ImGui::TextUnformatted(e.kind == 0 ? (e.voice ? "voice played" : "") : e.text.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.1f s", e.durSec);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("NXDN%d, %s%s, %.1f %% bit errors", e.baud == 4800 ? 96 : 48, e.endSeen ? "ended with TX_REL" : "signal lost", e.lateEntry ? ", joined late" : "", e.berPct);
    };
    if (a.rx.nxdn.now.active) row(a.rx.nxdn.now, true);
    for (const auto& e : log) row(e, false);
    ImGui::EndTable();
    if (log.empty() && !a.rx.nxdn.now.active) ImGui::TextDisabled("nothing heard yet");
}

void messageView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see messages"); return; }
    const auto& ev = a.rx.nxdn.events;
    ImGui::PushFont(a.mono, 0);
    for (const auto& s : ev) ImGui::TextUnformatted(s.c_str());
    ImGui::PopFont();
    if (ev.empty()) ImGui::TextDisabled("no messages yet");
}

void siteView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::NxdnSite& s = a.rx.nxdn.siteInfo;
    if (!s.valid) { ImGui::TextDisabled("no site information (a trunked control channel sends it; conventional repeaters do not)"); return; }
    kvRow(a, "location ID", "0x%06X", s.location);
    kvRow(a, "system / site", "%d / %d", s.system, s.site);
    kvRow(a, "RAN", "%d", s.ran);
    kvRow(a, "service", "0x%04X", s.service);
    kvRow(a, "channel", "0x%04X", s.channel);
    kvRow(a, "heard", "%s%s", clock(s.wallTime).c_str(), dect2::timefmt::sfx());
}

// ---------------------------------------------------------------- hooks

void tick(App& a) {
    if (live(a)) appcodec::voiceCodecTick("ambe2");   // the user's codec plug-in (docs/CODECS.md), loaded whichever tab is shown
    if (!a.engine.running()) { S.wasRunning = false; return; }
    dect2::NxdnReceiver& r = a.engine.nxdn();
    if (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute) {   // the sound controls are shared with the other modes
        r.setVolume(a.volume); r.setMuted(a.muted);
        S.pushedVol = a.volume; S.pushedMute = a.muted;
    }
    S.wasRunning = true;
}

void tab(App& a) {
    {   // the voice monitor's strip, the same in every voice mode
        const dect2::NxdnTelemetry& t = a.rx.nxdn;
        voiceui::Link l;
        l.live = live(a); l.locked = l.live && t.state >= 1; l.codecName = "AMBE+2"; l.codecId = "ambe2";
        l.codecReady = t.ambe || dect2::nxdnAmbeAvailable();
        l.errorRate = l.locked ? t.berPct / 100.f : -1.f;
        voiceui::strip(a, "nxdn", "NXDN", l);
    }
    subNav("nxdnv", S.view, {"Now", "Calls", "Messages", "Site"});
    if (S.view == 0) nowView(a); else if (S.view == 1) callsView(a); else if (S.view == 2) messageView(a); else siteView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    if (resultsClearButton("nxdn")) a.engine.nxdn().clearResults();
    const auto& log = a.rx.nxdn.log;
    if (!ImGui::BeginTable("##nxdnl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Unit"); ImGui::TableSetupColumn("Heard", ImGuiTableColumnFlags_WidthStretch);
    const auto& n = a.rx.nxdn.now;
    if (n.active) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextColored(pal::okGreen(), "%s", dect2::nxdnId(n.src).c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextColored(pal::okGreen(), "on air -> %s", dstOf(n).c_str());
    }
    std::vector<int> seen;
    for (const auto& e : log) {
        if (std::find(seen.begin(), seen.end(), e.src) != seen.end()) continue;
        seen.push_back(e.src);
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(dect2::nxdnId(e.src).c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s  -> %s", clock(e.wallTime).c_str(), dstOf(e).c_str());
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    wideCarriersTable(a, 0);
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::NxdnTelemetry& t = a.rx.nxdn;
    kvRow(a, "state", "%s", dect2::nxdnSummary(t).c_str());
    kvRow(a, "signal", "NXDN%d, symbol SNR %.1f dB, +3 deviation %.0f Hz, offset %+.0f Hz%s", t.baud == 4800 ? 96 : 48, t.snrDb, t.deviationHz, t.cfoHz, t.inverted ? ", inverted" : "");
    kvRow(a, "bit errors", "%.2f %% (control channels before their FEC)", t.berPct);
    kvRow(a, "frames", "%llu, %llu with a damaged LICH", (unsigned long long)t.frames, (unsigned long long)t.lichBad);
    kvRow(a, "SACCH", "%llu good, %llu failed CRC", (unsigned long long)t.sacchOk, (unsigned long long)t.sacchBad);
    kvRow(a, "FACCH1", "%llu good, %llu failed CRC", (unsigned long long)t.facchOk, (unsigned long long)t.facchBad);
    kvRow(a, "CAC", "%llu good, %llu failed CRC", (unsigned long long)t.cacOk, (unsigned long long)t.cacBad);
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
    lamp("Sync", !on ? 0 : a.rx.nxdn.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("On air", !on ? 0 : a.rx.nxdn.state == 2 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::nxdnSummary(a.rx.nxdn).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "NXDN";
    if (live(a)) l2 = dect2::nxdnSummary(a.rx.nxdn);
}

} // namespace

extern const ModeUi kNxdnUi;
const ModeUi kNxdnUi = {
    .sideTitle = "UNITS",
    .tabName = "NXDN",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
