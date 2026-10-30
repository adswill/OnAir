// D-STAR screens: the current call (callsigns, repeaters, flags, message, position, voice), the call log, the slow data messages, the
// positions on a map (the ADS-B map underneath, as for APRS and M17), the calling-channel presets, and a "?" about the mbelib library the
// voice needs (docs/CODECS.md).
#include "app.h"
#include "ui/kit/mode_frame.h"
#include "ui/kit/session_list.h"
#include "voice/voice_monitor_ui.h"
#include "dect2/codec/codec_registry.h"
#include "dect2/codec/codec_registry.h"
#include "../codec/speech_codec_ui.h"
#include "ui/ui_common.h"
#include "../adsb/adsb_map.h"
#include "dect2/dstar/dstar_ambe.h"
#include "dect2/dstar/dstar_rx.h"
#include "dect2/dstar/dstar_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;                        // 0 now, 1 calls, 2 messages, 3 map
    bool wasRunning = false;
    float pushedVol = -1; bool pushedMute = false;
    adsbmap::View map;
    bool mapFollow = true;
    bool loaded = false;
};
State S;

bool live(const App& a) { return a.rx.standard == 34; }   // the engine reports its standard code minus one

void load() {
    if (S.loaded) return;
    S.loaded = true;
    S.map.zoom = (int)plat::prefs().getI("dstarZoom", 9);
}

std::string myOf(const dect2::DstarCall& c) {
    if (c.my.empty()) return "?";
    return c.sfx.empty() ? c.my : c.my + "/" + c.sfx;
}

void presets(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Calling"); flowNext(6 * gUi);
    static const struct { const char* name; double mhz; } kP[] = {{"2 m  145.375", 145.375}, {"70 cm  433.450", 433.450}};
    for (const auto& p : kP) {
        if (ImGui::Button(p.name)) tuneFreq(a, p.mhz);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Digital voice calling channel of the IARU Region 1 (Europe) band plan; repeaters have their own frequencies");
        flowNext(6 * gUi);
    }
    ImGui::AlignTextToFramePadding();
    std::string lib;
    if (!dect2::codec::pluginLoaded("ambe")) dect2::dstarAmbeAvailable(&lib);
    appcodec::voiceCodecLine("ambe", "AMBE", lib);
    ImGui::SameLine(0, 4 * gUi);
    codecHelpButton(CodecHelp::DstarAmbe);
    flowEnd();
}

void nowView(App& a) {
    presets(a);
    ImGui::Spacing();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to hear D-STAR"); return; }
    const dect2::DstarTelemetry& t = a.rx.dstar;
    const dect2::DstarCall& n = t.now;
    if (n.wallTime == 0) { ImGui::TextDisabled("nothing heard yet"); return; }
    ImGui::PushFont(a.mono, 0);
    ImGui::TextColored(n.active ? pal::okGreen() : pal::grey(0.6f), "%s  %s  ->  %s", n.active ? "ON AIR" : "last", myOf(n).c_str(), n.ur.empty() ? "?" : n.ur.c_str());
    ImGui::PopFont();
    if (!n.headerOk) kvRow(a, "header", "%s", n.lateEntry ? "joined late: waiting for the header in the slow data" : "damaged: waiting for the header in the slow data");
    else {
        kvRow(a, "RPT1 / RPT2", "%s / %s", n.rpt1.c_str(), n.rpt2.c_str());
        kvRow(a, "flags", "%s%s", n.flags.c_str(), n.headerFromSlow ? (n.lateEntry ? " (header from the slow data, joined late)" : " (header from the slow data)") : "");
        if (n.flag[0] & 0x08) { ImGui::TextDisabled("note"); kvColumn(130 * gUi); ImGui::TextColored(pal::warnAmber(), "emergency flag set"); }
    }
    kvRow(a, "duration", "%.1f s, %u voice frames", n.durSec, n.voiceFrames);
    kvRow(a, "started", "%s%s", clockText(n.wallTime).c_str(), dect2::timefmt::sfx());
    kvRow(a, "voice", "%s", n.voice ? "playing (AMBE through mbelib)" : t.ambe ? "-" : "no mbelib: frames counted only");
    if (!n.text.empty()) kvRow(a, "message", "%s%s", n.text.c_str(), n.textBlocks == 15 ? "" : " (part)");
    if (n.hasPos) kvRow(a, "position", "%.5f, %.5f", n.lat, n.lon);
    if (!n.gps.empty()) kvRow(a, "GPS text", "%s", n.gps.c_str());
    if (n.squelch >= 0) kvRow(a, "code squelch", "%d", n.squelch);
    kvRow(a, "bit errors", "%.2f %% (header and data syncs)", n.berPct);
}

void callsView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the calls"); return; }
    const auto& log = a.rx.dstar.log;
    if (ImGui::Button("Copy")) {
        std::string txt;
        for (const auto& e : log) {
            char b[400];
            snprintf(b, sizeof b, "%s\t%s\t%s\t%s\t%s\t%s\t%.1f s\t%.2f %%\n", clockText(e.wallTime).c_str(), myOf(e).c_str(), e.ur.c_str(), e.rpt1.c_str(), e.rpt2.c_str(), e.text.c_str(), e.durSec, e.berPct);
            txt += b;
        }
        ImGui::SetClipboardText(txt.c_str());
    }
    if (!ImGui::BeginTable("##dstarlog", 8 + (wideActive(a) ? 1 : 0), ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    if (wideActive(a)) ImGui::TableSetupColumn("Carrier"); ImGui::TableSetupColumn(dect2::timefmt::headTime()); ImGui::TableSetupColumn("MY"); ImGui::TableSetupColumn("UR"); ImGui::TableSetupColumn("RPT1");
    ImGui::TableSetupColumn("RPT2"); ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Length"); ImGui::TableSetupColumn("BER");
    ImGui::TableHeadersRow();
    auto row = [&](const dect2::DstarCall& e, bool on) {
        ImGui::TableNextRow();
        if (wideActive(a)) { ImGui::TableNextColumn(); wideCarrierCell(a, e.carrierId, e.carrierHz); }
        const ImVec4 c = on ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::TableNextColumn(); ImGui::TextColored(c, "%s", on ? "on air" : clockText(e.wallTime).c_str());
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextColored(c, "%s", myOf(e).c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(e.ur.empty() ? "?" : e.ur.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(e.rpt1.empty() ? "?" : e.rpt1.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(e.rpt2.empty() ? "?" : e.rpt2.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn();
        if (e.flag[0] & 0x08) { ImGui::TextColored(pal::warnAmber(), "EMERGENCY"); ImGui::SameLine(); }
        ImGui::TextUnformatted(e.text.c_str());
        if (e.hasPos) { ImGui::SameLine(); ImGui::TextDisabled("[pos]"); }
        ImGui::TableNextColumn(); ImGui::Text("%.1f s", e.durSec);
        ImGui::TableNextColumn(); ImGui::Text("%.1f %%", e.berPct);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s%s%s", e.endSeen ? "ended with the end pattern" : "signal lost", e.lateEntry ? ", joined late" : "", e.headerFromSlow ? ", header from the slow data" : "");
    };
    if (a.rx.dstar.now.active) row(a.rx.dstar.now, true);
    for (const auto& e : log) row(e, false);
    ImGui::EndTable();
    if (log.empty() && !a.rx.dstar.now.active) ImGui::TextDisabled("nothing heard yet");
}

void messageView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see messages"); return; }
    const auto& m = a.rx.dstar.messages;
    if (!ImGui::BeginTable("##dstarmsg", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(dect2::timefmt::headTime()); ImGui::TableSetupColumn("MY"); ImGui::TableSetupColumn("UR"); ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& e : m) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clockText(e.wallTime).c_str());
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(e.my.empty() ? "?" : e.my.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(e.ur.empty() ? "?" : e.ur.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(e.text.c_str()); ImGui::PopTextWrapPos();
    }
    ImGui::EndTable();
    if (m.empty()) ImGui::TextDisabled("no messages yet");
}

void mapView(App& a) {
    const ImVec2 size(ImGui::GetContentRegionAvail().x, std::max(120.f, ImGui::GetContentRegionAvail().y));
    const bool on = live(a);
    const auto& pos = a.rx.dstar.positions;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (S.mapFollow && on && !pos.empty()) {
        double la0 = 90, la1 = -90, lo0 = 180, lo1 = -180;
        for (const auto& v : pos) { la0 = std::min(la0, v.lat); la1 = std::max(la1, v.lat); lo0 = std::min(lo0, v.lon); lo1 = std::max(lo1, v.lon); }
        S.map.lat = (la0 + la1) / 2; S.map.lon = (lo0 + lo1) / 2;
        const double span = std::max(0.005, std::max(lo1 - lo0, (la1 - la0) * 1.4));
        S.map.zoom = std::max(2, std::min(14, (int)std::floor(std::log2(0.7 * size.x * 360.0 / (256.0 * span)))));
    }
    const double lat0 = S.map.lat, lon0 = S.map.lon;
    adsbmap::draw(S.map, size);
    adsbmap::fullCorner(p0, size);   // the shared full-screen button (#195)
    if (S.map.lat != lat0 || S.map.lon != lon0) S.mapFollow = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    if (on) {
        std::vector<std::string> drawn;   // the newest position of each station is labelled, older ones are dots (its track)
        for (const auto& v : pos) {
            const ImVec2 q = adsbmap::project(v.lat, v.lon);
            const bool first = std::find(drawn.begin(), drawn.end(), v.call) == drawn.end();
            if (!first) { dl->AddCircleFilled(q, 2.5f * gUi, IM_COL32(130, 170, 240, 160)); continue; }
            drawn.push_back(v.call);
            const float r = 5.f * gUi;
            dl->AddCircleFilled(q, r, v.dprs ? IM_COL32(110, 150, 240, 255) : IM_COL32(90, 200, 220, 255));
            dl->AddCircle(q, r, IM_COL32(0, 0, 0, 160), 16, 1.f);
            const std::string lb = v.call.empty() ? std::string("?") : v.call;
            const ImVec2 ts = ImGui::CalcTextSize(lb.c_str());
            const ImVec2 lp(q.x + 10 * gUi, q.y - ts.y * 0.5f - 2 * gUi);
            dl->AddRectFilled(lp, ImVec2(lp.x + ts.x + 8 * gUi, lp.y + ts.y + 4 * gUi), IM_COL32(10, 12, 14, 210), 2.f);
            dl->AddText(ImVec2(lp.x + 4 * gUi, lp.y + 2 * gUi), IM_COL32(235, 238, 240, 255), lb.c_str());
        }
    }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    mapCornerButtons(S.map, S.mapFollow, "never the position itself");
    plat::prefs().setI("dstarZoom", S.map.zoom);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
    if (!on) adsbmap::legend(size.x, "start the receiver");
    else if (pos.empty()) adsbmap::legend(size.x, "no positions yet (GPS / D-PRS in the slow data)");
    else adsbmap::legend(size.x, "%zu positions", pos.size());
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
    ImGui::Dummy(ImVec2(0, 0));   // an item after SetCursorScreenPos: ImGui otherwise reports "extend window/parent boundaries"
}

// ---------------------------------------------------------------- hooks

void tick(App& a) {
    if (live(a)) appcodec::voiceCodecTick("ambe");   // the user's codec plug-in (docs/CODECS.md), loaded whichever tab is shown
    load();
    if (!a.engine.running()) { S.wasRunning = false; return; }
    dect2::DstarReceiver& r = a.engine.dstar();
    if (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute) {   // the sound controls are shared with the other modes
        r.setVolume(a.volume); r.setMuted(a.muted);
        S.pushedVol = a.volume; S.pushedMute = a.muted;
    }
    S.wasRunning = true;
}

void tab(App& a) {
    load();
    {   // the voice monitor's strip, the same in every voice mode
        const dect2::DstarTelemetry& t = a.rx.dstar;
        voiceui::Link l;
        l.live = live(a); l.locked = l.live && t.state >= 1; l.codecName = "AMBE"; l.codecId = "ambe";
        l.codecReady = t.ambe || dect2::codec::pluginLoaded("ambe");
        l.errorRate = l.locked ? t.berPct / 100.f : -1.f;
        voiceui::strip(a, "dstar", "D-STAR", l);
    }
    kit::modeHeader({33, "D-STAR", live(a) ? 1 : a.engine.running() ? 2 : 0, -1000.f, a.freqMhz * 1e6});
    kit::modeNav("dstarv", S.view, {{tr("Live"), 0}, {tr("Calls"), 4}, {tr("Map"), 3}, {tr("Call list"), 1, true}, {tr("Messages"), 2, true}});
    if (S.view == 4) { kit::sessionItems(33, 1, "dstar"); return; }
    if (S.view == 0) nowView(a); else if (S.view == 1) callsView(a); else if (S.view == 2) messageView(a); else mapView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    if (resultsClearButton("dstar")) a.engine.dstar().clearResults();
    const auto& log = a.rx.dstar.log;
    if (!ImGui::BeginTable("##dstarl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Station"); ImGui::TableSetupColumn("Heard", ImGuiTableColumnFlags_WidthStretch);
    const auto& n = a.rx.dstar.now;
    if (n.active) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextColored(pal::okGreen(), "%s", myOf(n).c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextColored(pal::okGreen(), "on air");
    }
    std::vector<std::string> seen;
    for (const auto& e : log) {
        if (std::find(seen.begin(), seen.end(), e.my) != seen.end()) continue;
        seen.push_back(e.my);
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(myOf(e).c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s  %s", clockText(e.wallTime).c_str(), e.rpt1 == "DIRECT" || e.rpt1.empty() ? "" : e.rpt1.c_str());
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    wideCarriersTable(a, 0);
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::DstarTelemetry& t = a.rx.dstar;
    kvRow(a, "state", "%s", dect2::dstarSummary(t).c_str());
    kvRow(a, "signal", "bit SNR %.1f dB, bit sync level %.0f Hz, offset %+.0f Hz%s", t.snrDb, t.deviationHz, t.cfoHz, t.inverted ? ", spectrum mirrored" : "");
    kvRow(a, "bit errors", "%.2f %% (header before its FEC, data syncs)", t.berPct);
    kvRow(a, "headers", "%llu good, %llu failed CRC, %llu from the slow data, %llu joined late", (unsigned long long)t.headersOk, (unsigned long long)t.headersBad,
          (unsigned long long)t.slowHeaders, (unsigned long long)t.lateEntries);
    kvRow(a, "voice frames", "%llu received, %llu data syncs missed, %llu end patterns", (unsigned long long)t.voiceFrames, (unsigned long long)t.blocksBad, (unsigned long long)t.endPatterns);
    kvRow(a, "GPS", "%llu lines, %llu damaged", (unsigned long long)t.gpsLines, (unsigned long long)t.gpsBad);
    kvRow(a, "voice", "%llu AMBE frames decoded%s", (unsigned long long)t.voiceDecoded, t.ambe ? "" : " (mbelib not found: click ? for the file names and folders)");
    if (t.ambe) kvRow(a, "mbelib", "%s", t.ambePath.c_str());
    kvRow(a, "input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kvRow(a, "signal time", "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool on = live(a);
    StatusPanel panel;
    statusIqLamp(a);
    lamp("Sync", !on ? 0 : a.rx.dstar.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("On air", !on ? 0 : a.rx.dstar.state == 2 ? 1 : 0); flowNext(10 * gUi);
    statusState(a, on, [&] { return dect2::dstarSummary(a.rx.dstar); });
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "D-STAR";
    if (live(a)) l2 = dect2::dstarSummary(a.rx.dstar);
}

} // namespace

extern const ModeUi kDstarUi;
const ModeUi kDstarUi = {
    .sideTitle = "STATIONS",
    .tabName = "D-STAR",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
