// JS8Call screens: the dial-frequency presets, the submodes looked for (normal, fast, turbo), the waterfall of the audio passband with
// the newest frames marked, then four views: every frame decoded (UTC, submode, SNR, DT, Hz, text), the messages put together from
// their frames (directed messages, free text, relays), the stations heard (call, grid, SNR, last heard), and their grids on a map.
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "../map/contact_lines.h"
#include "gfx/gfx.h"
#include "../adsb/adsb_map.h"
#include "dect2/js8/js8_proto.h"
#include "dect2/js8/js8_rx.h"
#include "dect2/js8/js8_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    bool en[3] = {true, true, true};
    bool pushed[3] = {false, false, false};
    bool wasRunning = false, loaded = false;
    bool listen = false; int pushedListen = -1; float pushedVol = -1; bool pushedMute = false;   // "Listen": off until switched on
    int view = 0;                        // 0 frames, 1 messages, 2 stations, 3 map
    static constexpr int kRows = 140;
    int bins = 0;
    std::vector<uint32_t> px;
    uint64_t specSeq = 0;
    float lo = -120, hi = -60;
    gfx::Image* wf = nullptr;
    int wfW = 0;
    bool dirty = false;
    adsbmap::View map;
    bool mapFollow = true;
};
State S;

bool live(const App& a) { return a.rx.standard == 38; }   // the engine reports its standard code minus one

void load() {
    if (S.loaded) return;
    S.loaded = true;
    for (int m = 0; m < 3; m++) { char k[24]; snprintf(k, sizeof k, "js8Sub%d", m); S.en[m] = plat::prefs().getB(k, true); }
    S.map.zoom = (int)plat::prefs().getI("js8Zoom", 4);
    S.listen = plat::prefs().getB("js8Listen", false);
}

void utcText(double t, char* out, size_t n) {
    snprintf(out, n, "%s", dect2::timefmt::clock((int64_t)std::floor(t)).c_str());
}

const char* subLetter(int m) { return m == 0 ? "N" : m == 1 ? "F" : "T"; }

void presets(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Band"); flowNext(6 * gUi);
    int n = 0;
    const dect2::Js8Preset* p = dect2::js8Presets(n);
    for (int i = 0; i < n; i++) {
        const bool here = std::fabs(a.freqMhz - p[i].mhz) < 0.002;
        if (here) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
        char b[40]; snprintf(b, sizeof b, "%s##js8p%d", p[i].name, i);
        if (ImGui::Button(b)) tuneFreq(a, p[i].mhz);
        if (here) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("JS8Call dial frequency %.3f MHz (upper sideband)", p[i].mhz);
        flowNext(4 * gUi);
    }
    ImGui::TextDisabled("|"); flowNext(8 * gUi);
    static const char* names[3] = {"Normal##js8n", "Fast##js8f", "Turbo##js8t"};
    static const char* tips[3] = {"15 s slots, 6.25 baud, 50 Hz wide", "10 s slots, 10 baud, 80 Hz wide", "6 s slots, 20 baud, 160 Hz wide"};
    for (int m = 0; m < 3; m++) {
        if (ImGui::Checkbox(names[m], &S.en[m])) { char k[24]; snprintf(k, sizeof k, "js8Sub%d", m); plat::prefs().setB(k, S.en[m]); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tips[m]);
        flowNext(6 * gUi);
    }
    listenControls(a, S.listen, "js8Listen", "js8", "Play the sideband (dial + 200 Hz to 3.8 kHz) on the sound card; decoding is the same either way");
    flowNext(8 * gUi);
    if (ImGui::Button("Clear##js8")) a.engine.js8().clearDecodes();
    flowEnd();
}

void follow(const dect2::Js8Telemetry& t) {
    if (t.spectrum.empty() || t.specSeq == S.specSeq) return;
    S.specSeq = t.specSeq;
    const int W = (int)t.spectrum.size();
    if (W != S.bins) { S.bins = W; S.px.assign((size_t)W * State::kRows, 0xFF000000u); }
    std::vector<float> srt(t.spectrum);
    std::nth_element(srt.begin(), srt.begin() + W / 2, srt.end());
    const float med = srt[(size_t)W / 2];
    S.lo += (med - 3 - S.lo) * 0.2f;
    S.hi += (med + 25 - S.hi) * 0.2f;
    std::copy_backward(S.px.begin(), S.px.end() - W, S.px.end());
    for (int k = 0; k < W; k++) S.px[(size_t)k] = Waterfall::jet((t.spectrum[(size_t)k] - S.lo) / std::max(1.f, S.hi - S.lo));
    S.dirty = true;
}

void waterfall(App& a, float h) {
    const dect2::Js8Telemetry& t = a.rx.js8;
    const bool on = live(a);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float l = p0.x, r = p0.x + w, top = p0.y, bot = p0.y + h - ImGui::GetTextLineHeight() - 4 * gUi;
    dl->AddRectFilled(p0, ImVec2(r, p0.y + h), IM_COL32(8, 9, 10, 255));
    const double lo = t.hiHz > t.loHz ? t.loHz : 200, hi = t.hiHz > t.loHz ? t.hiHz : 3200;
    auto X = [&](double hz) { return (float)(l + (hz - lo) / (hi - lo) * (r - l)); };
    if (gGfx && S.bins > 0 && (!S.wf || S.wfW != S.bins)) { delete S.wf; S.wf = gGfx->createImage(S.bins, State::kRows, 0xFF000000u); S.wfW = S.bins; }
    if (S.wf && S.dirty) { S.wf->update(0, 0, S.bins, State::kRows, S.px.data()); S.dirty = false; }
    if (S.wf && on) dl->AddImage(S.wf->texture(), ImVec2(l, top), ImVec2(r, bot));
    const ImU32 txt = IM_COL32(120, 124, 128, 255);
    char b[64];
    for (int k = 500; k <= 3000; k += 500) {
        const float x = X(k);
        dl->AddLine(ImVec2(x, bot), ImVec2(x, bot + 4 * gUi), txt);
        snprintf(b, sizeof b, "%d", k);
        dl->AddText(ImVec2(x - ImGui::CalcTextSize(b).x / 2, bot + 3 * gUi), txt, b);
    }
    if (on) {   // the frames of the last minute: a label at their frequency
        int row = 0;
        for (auto it = t.decodes.rbegin(); it != t.decodes.rend() && row < 12; ++it) {
            if (t.nowUtc - it->slotUtc > 60) break;
            const double bw = 8 * dect2::js8::spec(it->submode).baud();
            const float x0 = X(it->hz), x1 = X(it->hz + bw);
            dl->AddRectFilled(ImVec2(x0, top), ImVec2(std::max(x0 + 2, x1), top + 4 * gUi), IM_COL32(255, 210, 90, 200));
            const std::string lb = it->from.empty() ? it->text.substr(0, 10) : it->from;
            dl->AddText(ImVec2(x0, top + 5 * gUi + (row % 3) * ImGui::GetTextLineHeight()), IM_COL32(255, 225, 150, 230), lb.c_str());
            row++;
        }
    } else {
        dl->AddText(ImVec2(l + 10, top + 10), txt, a.engine.running() ? "starting" : "start the receiver to listen");
    }
    ImGui::InvisibleButton("##js8wf", ImVec2(w, h));
    if (on && ImGui::IsItemHovered()) ImGui::SetTooltip("%.0f Hz above the dial", lo + (ImGui::GetIO().MousePos.x - l) / std::max(1.f, r - l) * (hi - lo));
}

void framesView(App& a) {
    const auto& d = a.rx.js8.decodes;
    {   // the public map's ham contacts (Server > Share ham contacts; ignored unless chosen): directed frames only, callsigns and locators, never the text
        static double fed = 0;
        double newest = fed;
        for (const auto& x : d) {
            if (x.slotUtc <= fed) continue;
            newest = std::max(newest, x.slotUtc);
            if (!x.from.empty() && !x.grid.empty()) mapHamRemember(x.from, x.grid);
            if (!x.from.empty() && !x.to.empty() && x.to[0] != '@' && x.to != "CQ")
                mapHamHeard("JS8", (a.freqMhz * 1e6) + x.hz, x.from, x.grid, x.to, x.slotUtc);
        }
        fed = newest;
    }
    if (!ImGui::BeginTable("##js8d", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(""); ImGui::TableSetupColumn("dB"); ImGui::TableSetupColumn("DT"); ImGui::TableSetupColumn("Hz");
    ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    char b[32];
    for (const auto& x : d) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); utcText(x.slotUtc, b, sizeof b); ImGui::TextDisabled("%s", b);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", subLetter(x.submode));
        ImGui::TableNextColumn(); ImGui::Text("%+.0f", x.snrDb);
        ImGui::TableNextColumn(); ImGui::Text("%+.1f", x.dt);
        ImGui::TableNextColumn(); ImGui::Text("%.0f", x.hz);
        ImGui::TableNextColumn();
        const bool hb = x.type == dect2::js8::kHeartbeat;
        if (hb) ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.f, 1), "%s", x.text.c_str()); else ImGui::TextUnformatted(x.text.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("frame %s, flags %s%s%s", x.chars.c_str(), x.flags & 1 ? "first " : "", x.flags & 2 ? "last " : "", x.flags & 4 ? "data" : "");
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::PopFont();
    ImGui::EndTable();
    if (d.empty()) ImGui::TextDisabled("no frames yet");
}

void messagesView(App& a) {
    const auto& m = a.rx.js8.messages;
    if (!ImGui::BeginTable("##js8m", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn("dB"); ImGui::TableSetupColumn("Hz"); ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    char b[32];
    for (const auto& x : m) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); utcText(x.utc, b, sizeof b); ImGui::TextDisabled("%s", b);
        ImGui::TableNextColumn(); ImGui::Text("%+.0f", x.snrDb);
        ImGui::TableNextColumn(); ImGui::Text("%.0f", x.hz);
        ImGui::TableNextColumn();
        ImGui::PushTextWrapPos(0);
        const ImVec4 col = x.relay ? ImVec4(0.95f, 0.75f, 0.45f, 1) : x.complete ? ImVec4(0.92f, 0.94f, 0.96f, 1) : ImVec4(0.6f, 0.62f, 0.65f, 1);
        ImGui::TextColored(col, "%s%s", x.text.c_str(), x.complete ? "" : " ...");
        if (x.relay && !x.relayTo.empty()) ImGui::TextDisabled("relay: %s -> %s -> %s", x.from.c_str(), x.to.c_str(), x.relayTo.c_str());
        ImGui::PopTextWrapPos();
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::PopFont();
    ImGui::EndTable();
    if (m.empty()) ImGui::TextDisabled("no messages yet");
}

void stationsView(App& a) {
    const auto& st = a.rx.js8.stations;
    if (!ImGui::BeginTable("##js8s", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Call"); ImGui::TableSetupColumn("Grid"); ImGui::TableSetupColumn("dB"); ImGui::TableSetupColumn("Hz"); ImGui::TableSetupColumn("UTC");
    ImGui::TableSetupColumn("Last", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    char b[32];
    for (const auto& s : st) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.36f, 1), "%s", s.call.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(s.grid.empty() ? "-" : s.grid.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%+.0f", s.snrDb);
        ImGui::TableNextColumn(); ImGui::Text("%.0f", s.hz);
        ImGui::TableNextColumn(); utcText(s.lastUtc, b, sizeof b); ImGui::TextDisabled("%s", b);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", s.lastText.c_str());
    }
    ImGui::PopFont();
    ImGui::EndTable();
    if (st.empty()) ImGui::TextDisabled("no stations yet");
}

void mapView(App& a) {
    const ImVec2 size(ImGui::GetContentRegionAvail().x, std::max(120.f, ImGui::GetContentRegionAvail().y));
    const bool on = live(a);
    const auto& st = a.rx.js8.stations;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    int npos = 0;
    if (S.mapFollow && on) {
        double la0 = 90, la1 = -90, lo0 = 180, lo1 = -180;
        for (const auto& v : st) if (v.hasPos) { npos++; la0 = std::min(la0, v.lat); la1 = std::max(la1, v.lat); lo0 = std::min(lo0, v.lon); lo1 = std::max(lo1, v.lon); }
        if (npos) {
            S.map.lat = (la0 + la1) / 2; S.map.lon = (lo0 + lo1) / 2;
            const double span = std::max(2.0, std::max(lo1 - lo0, (la1 - la0) * 1.4));
            S.map.zoom = std::max(2, std::min(10, (int)std::floor(std::log2(0.7 * size.x * 360.0 / (256.0 * span)))));
        }
    }
    const double lat0 = S.map.lat, lon0 = S.map.lon; const int zoom0 = S.map.zoom;
    adsbmap::draw(S.map, size);
    if (S.map.lat != lat0 || S.map.lon != lon0 || S.map.zoom != zoom0) S.mapFollow = false;   // panned or zoomed (the wheel at the centre moves nothing)
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    npos = 0;
    size_t nLines = 0;
    if (on && contactlines::options().lines) {   // who talks to whom: the directed messages between two stations
        const auto& co = contactlines::options();
        dect2::ContactGraph g(co.keepMin * 60.0);
        double now = 0;
        for (const auto& d : a.rx.js8.decodes) now = std::max(now, d.slotUtc);
        for (const auto& d : a.rx.js8.decodes) if (d.slotUtc >= now - co.keepMin * 60.0) g.add(d.slotUtc, d.text, 0, 0, d.grid);
        g.expire(now);
        ImVec2 click;
        if (adsbmap::clickedAt(click)) contactlines::pickAt(g, click);
        nLines = contactlines::draw(dl, g, now, S.map.lon);
    }
    if (on)
        for (const auto& v : st) {
            if (!v.hasPos) continue;
            npos++;
            const ImVec2 q = adsbmap::project(v.lat, v.lon);
            const float r = 5.f * gUi;
            dl->AddCircleFilled(q, r, v.cq ? IM_COL32(240, 180, 80, 255) : IM_COL32(90, 200, 220, 255));
            dl->AddCircle(q, r, IM_COL32(0, 0, 0, 160), 16, 1.f);
            contactlines::markSelected(dl, v.call, q);
            const ImVec2 ts = ImGui::CalcTextSize(v.call.c_str());
            const ImVec2 lp(q.x + 10 * gUi, q.y - ts.y * 0.5f - 2 * gUi);
            dl->AddRectFilled(lp, ImVec2(lp.x + ts.x + 8 * gUi, lp.y + ts.y + 4 * gUi), IM_COL32(10, 12, 14, 210), 2.f);
            dl->AddText(ImVec2(lp.x + 4 * gUi, lp.y + 2 * gUi), IM_COL32(235, 238, 240, 255), v.call.c_str());
        }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+##js8z", ImVec2(26 * gUi, 0))) { S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1); S.mapFollow = false; } if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom in"));
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-##js8z", ImVec2(26 * gUi, 0))) { S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1); S.mapFollow = false; } if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom out"));
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("fit##js8")) S.mapFollow = true;
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    contactlines::controls();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the position itself");
    plat::prefs().setI("js8Zoom", S.map.zoom);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
    if (!on) adsbmap::legend(size.x, "start the receiver");
    else if (npos == 0) adsbmap::legend(size.x, "no grids yet (heartbeats and CQs send them)");
    else if (contactlines::options().lines) adsbmap::legend(size.x, tr("%d stations at the centre of their grid squares, %zu contact lines"), npos, nLines);
    else adsbmap::legend(size.x, "%d stations at the centre of their grid squares", npos);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
    ImGui::Dummy(ImVec2(0, 0));   // an item after SetCursorScreenPos: ImGui otherwise reports "extend window/parent boundaries"
}

// ---------------------------------------------------------------- hooks

void tick(App& a) {
    load();
    if (!a.engine.running()) { S.wasRunning = false; return; }
    if (live(a)) follow(a.rx.js8);
    dect2::Js8Receiver& r = a.engine.js8();
    for (int m = 0; m < 3; m++) if (!S.wasRunning || S.en[m] != S.pushed[m]) { r.setEnabled(m, S.en[m]); S.pushed[m] = S.en[m]; }
    if (!S.wasRunning || (int)S.listen != S.pushedListen) { r.setListen(S.listen); S.pushedListen = S.listen; }
    if (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute) {   // the sound controls are shared with the other modes
        r.setVolume(a.volume); r.setMuted(a.muted);
        S.pushedVol = a.volume; S.pushedMute = a.muted;
    }
    S.wasRunning = true;
}

void tab(App& a) {
    load();
    presets(a);
    ImGui::Spacing();
    waterfall(a, std::max(80.f, ImGui::GetContentRegionAvail().y * 0.26f));
    ImGui::Spacing();
    subNav("js8v", S.view, {"Frames", "Messages", "Stations", "Map"});
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    if (S.view == 0) framesView(a); else if (S.view == 1) messagesView(a); else if (S.view == 2) stationsView(a); else mapView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    if (!ImGui::BeginTable("##js8l", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Call"); ImGui::TableSetupColumn("Grid", ImGuiTableColumnFlags_WidthStretch);
    for (const auto& s : a.rx.js8.stations) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(s.call.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s %+.0f dB", s.grid.c_str(), s.snrDb);
    }
    ImGui::EndTable();
    if (a.rx.js8.stations.empty()) ImGui::TextDisabled("no stations yet");
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::Js8Telemetry& t = a.rx.js8;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("state", "%s", dect2::js8Summary(t).c_str());
    kv("clock", "%s, DT median %+.2f s", t.timeMode ? "the recording's start time" : "system clock (UTC)", t.clockErr);
    if (t.clockTrim != 0.f) kv("correction", "computer clock %+.1f s (automatic)", t.clockTrim);
    for (int m = 0; m < 3; m++)
        kv(dect2::js8::spec(m).name, "%s, %llu slots, %d frames in the last, %.0f ms to decode", t.enabled[m] ? "on" : "off", (unsigned long long)t.slots[m], t.lastSlotCount[m], t.decodeMs[m]);
    kv("frames", "%llu decoded, %llu candidates that did not", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kv("input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Slots", !on ? 0 : a.rx.js8.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("Frames", !on ? 0 : a.rx.js8.state == 2 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::js8Summary(a.rx.js8).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "JS8Call";
    if (live(a)) l2 = dect2::js8Summary(a.rx.js8);
}

} // namespace

extern const ModeUi kJs8Ui;
const ModeUi kJs8Ui = {
    .sideTitle = "STATIONS",
    .tabName = "JS8Call",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
