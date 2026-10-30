// M17 screens: the current transmission (callsigns, CAN, stream type, encryption, duration, META text and position), the activity log,
// the text messages, the positions on a map (the ADS-B map underneath, as for APRS), the European calling-frequency presets, and a "?"
// about the Codec 2 library the voice needs.
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "ui/kit/mode_frame.h"
#include "ui/kit/session_list.h"
#include "voice/voice_monitor_ui.h"
#include "../adsb/adsb_map.h"
#include "dect2/m17/m17_codec2.h"
#include "dect2/m17/m17_rx.h"
#include "dect2/m17/m17_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;                        // 0 now, 1 activity, 2 messages, 3 map
    int can = -1;                        // play only this CAN (-1 all)
    bool wasRunning = false;
    float pushedVol = -1; bool pushedMute = false; int pushedCan = -2;
    adsbmap::View map;
    bool mapFollow = true;
    bool loaded = false;
};
State S;

bool live(const App& a) { return a.rx.standard == 32; }   // the engine reports its standard code minus one

void load() {
    if (S.loaded) return;
    S.loaded = true;
    S.can = (int)plat::prefs().getI("m17Can", -1);
    S.map.zoom = (int)plat::prefs().getI("m17Zoom", 9);
}

std::string clock(int64_t t) {
    if (t <= 0) return "-";
    return dect2::timefmt::clock(t);
}

// the "?" next to the voice state: what the voice needs
void codecHelp(const App& a) {
    ImGui::SameLine(0, 4 * gUi);
    if (ImGui::SmallButton("?##m17codec")) ImGui::OpenPopup("##m17codechelp");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("What the M17 voice needs");
    if (ImGui::BeginPopup("##m17codechelp")) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
        ImGui::TextUnformatted("M17 voice is Codec 2 (3200 or 1600 bit/s). OnAir ships Codec 2 as the library libcodec2 next to the program, and decodes the voice through it: nothing to install.");
        ImGui::Spacing();
        ImGui::TextDisabled("%s", dect2::m17Codec2InstallHint());
        ImGui::Spacing();
        std::string path;
        if (dect2::m17Codec2Available(&path)) ImGui::TextColored(pal::okGreen(), "found: %s", path.c_str());
        else ImGui::TextColored(pal::warnAmber(), "not found (this copy of OnAir came without it): callsigns, text and positions are shown, without sound");
        (void)a;
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
}

void presets(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Calling"); flowNext(6 * gUi);
    static const struct { const char* name; double mhz; } kP[] = {{"2 m  144.875", 144.875}, {"70 cm  433.475", 433.475}};
    for (const auto& p : kP) {
        if (ImGui::Button(p.name)) tuneFreq(a, p.mhz);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("M17 frequency suggested for IARU Region 1 (Europe) in the IARU VHF+ newsletter 92 (June 2023)");
        flowNext(6 * gUi);
    }
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("CAN"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(90 * gUi);
    const char* items[] = {"all", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15"};
    int idx = S.can + 1;
    if (ImGui::Combo("##m17can", &idx, items, 17)) { S.can = idx - 1; plat::prefs().setI("m17Can", S.can); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Channel access number: play only stations sending this CAN (all are still shown)");
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    const bool c2 = dect2::m17Codec2Available();
    ImGui::TextColored(c2 ? pal::okGreen() : pal::warnAmber(), "%s", c2 ? "voice: libcodec2" : "voice: needs libcodec2");
    codecHelp(a);
    flowEnd();
}

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

void nowView(App& a) {
    presets(a);
    ImGui::Spacing();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to hear M17"); return; }
    const dect2::M17Telemetry& t = a.rx.m17;
    const dect2::M17Transmission& n = t.now;
    if (n.wallTime == 0) { ImGui::TextDisabled("nothing heard yet"); return; }
    ImGui::PushFont(a.mono, 0);
    ImGui::TextColored(n.active ? pal::okGreen() : pal::grey(0.6f), "%s  %s  ->  %s", n.active ? "ON AIR" : "last", n.src.empty() ? "?" : n.src.c_str(), n.dst.empty() ? "?" : n.dst.c_str());
    ImGui::PopFont();
    kvRow(a, "type", "%s%s", n.type.empty() ? "?" : n.type.c_str(), n.lsfFromLich ? " (LSF rebuilt from the LICH)" : "");
    kvRow(a, "CAN", n.can >= 0 ? "%d" : "?", n.can);
    if (!n.enc.empty()) { ImGui::TextDisabled("encryption"); kvColumn(130 * gUi); ImGui::TextColored(pal::warnAmber(), "encrypted (%s): not decoded", n.enc.c_str()); }
    kvRow(a, "duration", "%.1f s, %u frames%s", n.durSec, n.frames, n.badFrames ? (" (" + std::to_string(n.badFrames) + " lost)").c_str() : "");
    kvRow(a, "started", "%s%s", clock(n.wallTime).c_str(), dect2::timefmt::sfx());
    if (!n.packet && n.enc.empty()) kvRow(a, "voice", "%s", n.voice ? "playing (Codec 2)" : t.codec2 ? "-" : "no libcodec2: frames only");
    if (!n.text.empty()) kvRow(a, n.packet ? "message" : "text", "%s", n.text.c_str());
    if (!n.data.empty()) kvRow(a, "data", "%s", n.data.c_str());
    if (n.hasGnss) kvRow(a, "position", "%.5f, %.5f", n.lat, n.lon);
    if (!n.ext1.empty()) kvRow(a, "via", "%s%s%s", n.ext1.c_str(), n.ext2.empty() ? "" : "  reflector ", n.ext2.c_str());
    kvRow(a, "bit errors", "%.2f %% corrected", n.berPct);
}

void activityView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the activity"); return; }
    const auto& log = a.rx.m17.log;
    if (ImGui::Button("Copy")) {
        std::string txt;
        for (const auto& e : log) {
            char b[400];
            snprintf(b, sizeof b, "%s\t%s\t%s\t%s\tCAN %d\t%.1f s\t%s%s\t%s\n", clock(e.wallTime).c_str(), e.src.c_str(), e.dst.c_str(), e.type.c_str(), e.can, e.durSec,
                     e.enc.empty() ? "" : "encrypted ", e.enc.c_str(), e.text.c_str());
            txt += b;
        }
        ImGui::SetClipboardText(txt.c_str());
    }
    if (!ImGui::BeginTable("##m17log", 7 + (wideActive(a) ? 1 : 0), ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    if (wideActive(a)) ImGui::TableSetupColumn("Carrier"); ImGui::TableSetupColumn(dect2::timefmt::headTime()); ImGui::TableSetupColumn("From"); ImGui::TableSetupColumn("To"); ImGui::TableSetupColumn("Type");
    ImGui::TableSetupColumn("CAN"); ImGui::TableSetupColumn("Length"); ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& e : log) {
        ImGui::TableNextRow();
        if (wideActive(a)) { ImGui::TableNextColumn(); wideCarrierCell(a, e.carrierId, e.carrierHz); }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(e.wallTime).c_str());
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(e.src.empty() ? "?" : e.src.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(e.dst.empty() ? "?" : e.dst.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(e.type.empty() ? "?" : e.type.c_str());
        ImGui::TableNextColumn(); if (e.can >= 0) ImGui::Text("%d", e.can); else ImGui::TextDisabled("?");
        ImGui::TableNextColumn(); ImGui::Text("%.1f s", e.durSec);
        ImGui::TableNextColumn();
        if (!e.enc.empty()) { ImGui::TextColored(pal::warnAmber(), "encrypted %s", e.enc.c_str()); ImGui::SameLine(); }
        ImGui::TextUnformatted(e.text.c_str());
    }
    ImGui::EndTable();
    if (log.empty()) ImGui::TextDisabled("nothing heard yet");
}

void messageView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see messages"); return; }
    const auto& m = a.rx.m17.messages;
    if (!ImGui::BeginTable("##m17msg", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(dect2::timefmt::headTime()); ImGui::TableSetupColumn("From"); ImGui::TableSetupColumn("To"); ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& e : m) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(e.wallTime).c_str());
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(e.src.empty() ? "?" : e.src.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(e.dst.empty() ? "?" : e.dst.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::PushTextWrapPos(0); ImGui::Text("%s%s", e.sms ? "" : "[META] ", e.text.c_str()); ImGui::PopTextWrapPos();
    }
    ImGui::EndTable();
    if (m.empty()) ImGui::TextDisabled("no messages yet");
}

void mapView(App& a) {
    const ImVec2 size(ImGui::GetContentRegionAvail().x, std::max(120.f, ImGui::GetContentRegionAvail().y));
    const bool on = live(a);
    const auto& pos = a.rx.m17.positions;
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
    if (S.map.lat != lat0 || S.map.lon != lon0) S.mapFollow = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    if (on) {
        std::vector<std::string> drawn;   // the newest position of each station is labelled, older ones are dots (its track)
        for (const auto& v : pos) {
            const ImVec2 q = adsbmap::project(v.lat, v.lon);
            const bool first = std::find(drawn.begin(), drawn.end(), v.src) == drawn.end();
            if (!first) { dl->AddCircleFilled(q, 2.5f * gUi, IM_COL32(120, 220, 160, 160)); continue; }
            drawn.push_back(v.src);
            const float r = 5.f * gUi;
            dl->AddCircleFilled(q, r, IM_COL32(90, 210, 140, 255));
            dl->AddCircle(q, r, IM_COL32(0, 0, 0, 160), 16, 1.f);
            const std::string lb = v.src.empty() ? std::string("?") : v.src;
            const ImVec2 ts = ImGui::CalcTextSize(lb.c_str());
            const ImVec2 lp(q.x + 10 * gUi, q.y - ts.y * 0.5f - 2 * gUi);
            dl->AddRectFilled(lp, ImVec2(lp.x + ts.x + 8 * gUi, lp.y + ts.y + 4 * gUi), IM_COL32(10, 12, 14, 210), 2.f);
            dl->AddText(ImVec2(lp.x + 4 * gUi, lp.y + 2 * gUi), IM_COL32(235, 238, 240, 255), lb.c_str());
        }
    }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) { S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1); S.mapFollow = false; } if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom in"));
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) { S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1); S.mapFollow = false; } if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom out"));
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("fit")) S.mapFollow = true;
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the position itself");
    plat::prefs().setI("m17Zoom", S.map.zoom);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
    if (!on) adsbmap::legend(size.x, "start the receiver");
    else if (pos.empty()) adsbmap::legend(size.x, "no positions yet (GNSS META)");
    else adsbmap::legend(size.x, "%zu positions", pos.size());
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
    ImGui::Dummy(ImVec2(0, 0));   // an item after SetCursorScreenPos: ImGui otherwise reports "extend window/parent boundaries"
}

// ---------------------------------------------------------------- hooks

void tick(App& a) {
    load();
    if (!a.engine.running()) { S.wasRunning = false; return; }
    dect2::M17Receiver& r = a.engine.m17();
    if (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute) {   // the sound controls are shared with the other modes
        r.setVolume(a.volume); r.setMuted(a.muted);
        S.pushedVol = a.volume; S.pushedMute = a.muted;
    }
    if (!S.wasRunning || S.can != S.pushedCan) { r.setCanFilter(S.can); S.pushedCan = S.can; }
    S.wasRunning = true;
}

void tab(App& a) {
    load();
    {   // the voice monitor's strip, the same in every voice mode
        const dect2::M17Telemetry& t = a.rx.m17;
        voiceui::Link l;
        l.live = live(a); l.locked = l.live && t.state >= 1; l.codecName = "Codec 2";
        l.codecReady = t.codec2;
        l.errorRate = l.locked ? t.berPct / 100.f : -1.f;
        voiceui::strip(a, "m17", "M17", l);
    }
    kit::modeHeader({31, "M17", live(a) ? 1 : a.engine.running() ? 2 : 0, -1000.f, a.freqMhz * 1e6});
    kit::modeNav("m17v", S.view, {{tr("Live"), 0}, {tr("Calls"), 4}, {tr("Map"), 3}, {tr("Activity"), 1, true}, {tr("Messages"), 2, true}});
    if (S.view == 4) { kit::sessionItems(31, 1, "m17"); return; }
    if (S.view == 0) nowView(a); else if (S.view == 1) activityView(a); else if (S.view == 2) messageView(a); else mapView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    if (resultsClearButton("m17")) a.engine.m17().clearResults();
    const auto& log = a.rx.m17.log;
    if (!ImGui::BeginTable("##m17l", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Station"); ImGui::TableSetupColumn("Heard", ImGuiTableColumnFlags_WidthStretch);
    const auto& n = a.rx.m17.now;
    if (n.active) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextColored(pal::okGreen(), "%s", n.src.empty() ? "?" : n.src.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextColored(pal::okGreen(), "on air");
    }
    std::vector<std::string> seen;
    for (const auto& e : log) {
        if (std::find(seen.begin(), seen.end(), e.src) != seen.end()) continue;
        seen.push_back(e.src);
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(e.src.empty() ? "?" : e.src.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", clock(e.wallTime).c_str());
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    wideCarriersTable(a, 0);
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::M17Telemetry& t = a.rx.m17;
    kvRow(a, "state", "%s", dect2::m17Summary(t).c_str());
    kvRow(a, "signal", "symbol SNR %.1f dB, deviation %.0f Hz (2400 nominal), offset %+.0f Hz%s", t.snrDb, t.deviationHz, t.cfoHz, t.inverted ? ", spectrum mirrored" : "");
    kvRow(a, "bit errors", "%.2f %% (corrected by the decoders)", t.berPct);
    kvRow(a, "frames", "%llu decoded, %llu lost", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kvRow(a, "LSF", "%llu, %llu rebuilt from the LICH", (unsigned long long)t.lsfOk, (unsigned long long)t.lichLsf);
    kvRow(a, "stream / packet", "%llu / %llu frames, %llu packets (%llu failed CRC), %llu BERT", (unsigned long long)t.streamFrames, (unsigned long long)t.packetFrames,
          (unsigned long long)t.packetsOk, (unsigned long long)t.packetsBad, (unsigned long long)t.bertFrames);
    kvRow(a, "voice", "%llu Codec 2 frames%s", (unsigned long long)t.voiceFrames, t.codec2 ? "" : " (libcodec2 not found)");
    if (t.codec2) kvRow(a, "codec2", "%s", t.codec2Path.c_str());
    kvRow(a, "input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kvRow(a, "signal time", "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Sync", !on ? 0 : a.rx.m17.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("On air", !on ? 0 : a.rx.m17.state == 2 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::m17Summary(a.rx.m17).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "M17";
    if (live(a)) l2 = dect2::m17Summary(a.rx.m17);
}

} // namespace

extern const ModeUi kM17Ui;
const ModeUi kM17Ui = {
    .sideTitle = "STATIONS",
    .tabName = "M17",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
