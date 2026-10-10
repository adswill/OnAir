// HF digital, the FT8 / FT4 / FT2 / WSPR view of the main tab (called by hfdig_ui.cpp): which modes decode, the slot clock and its
// error, and the decodes: a list per slot (UTC, dB, DT, Hz, mode, message; CQ calls highlighted), the stations on the map (the ADS-B
// map, by their locators) and the WSPR spots.
#include "app.h"
#include "adsb_map.h"
#include "dect2/hfdig_ftx.h"
#include "dect2/hfdig_rx.h"
#include "dect2/hfdig_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;        // 0 decodes, 1 map, 2 WSPR spots
    int filter = 0;      // 0 all, 1 + mode
    bool cqOnly = false;
    adsbmap::View map;
    bool mapFollow = true;
    bool loaded = false;
};
State S;

void utcText(double t, char* out, size_t n, bool seconds = true) {
    const time_t tt = (time_t)std::floor(t);
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    strftime(out, n, seconds ? "%H:%M:%S" : "%H:%M", &g);
}

ImVec4 modeColour(int m) {
    switch (m) {
    case 0: return ImVec4(0.55f, 0.78f, 0.95f, 1);
    case 1: return ImVec4(0.62f, 0.85f, 0.58f, 1);
    case 2: return ImVec4(0.85f, 0.70f, 0.95f, 1);
    default: return ImVec4(0.95f, 0.75f, 0.45f, 1);
    }
}

bool shown(const dect2::FtxDecode& d) {
    if (S.filter > 0 && d.mode != S.filter - 1) return false;
    if (S.cqOnly && !d.cq) return false;
    return true;
}

void decodeTable(App& a, const dect2::HfdigFtxTelemetry& f, bool wsprOnly) {
    const ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV;
    const int cols = wsprOnly ? 8 : 6;
    if (!ImGui::BeginTable(wsprOnly ? "##ftx_w" : "##ftx_d", cols, fl, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC");
    ImGui::TableSetupColumn("dB");
    ImGui::TableSetupColumn("DT");
    ImGui::TableSetupColumn("Hz");
    if (wsprOnly) {
        ImGui::TableSetupColumn("Call");
        ImGui::TableSetupColumn("Grid");
        ImGui::TableSetupColumn("dBm");
        ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
    } else {
        ImGui::TableSetupColumn("Mode");
        ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
    }
    ImGui::TableHeadersRow();
    double lastSlot = -1;
    int lastMode = -1;
    for (size_t i = f.decodes.size(); i-- > 0;) {   // newest slot first
        const dect2::FtxDecode& d = f.decodes[i];
        if (wsprOnly ? d.mode != 3 : !shown(d)) continue;
        const bool newSlot = d.slotUtc != lastSlot || d.mode != lastMode;
        lastSlot = d.slotUtc; lastMode = d.mode;
        ImGui::TableNextRow();
        if (newSlot) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(40, 46, 50, 255));
        char b[32];
        ImGui::PushFont(a.mono, 0);
        ImGui::TableNextColumn();
        utcText(d.slotUtc, b, sizeof b, d.mode != 3);
        if (newSlot) ImGui::TextUnformatted(b); else ImGui::TextDisabled("%s", b);
        ImGui::TableNextColumn(); ImGui::Text("%+3.0f", d.snrDb);
        ImGui::TableNextColumn(); ImGui::Text("%+4.1f", d.dt);
        ImGui::TableNextColumn(); ImGui::Text(d.mode == 3 ? "%7.1f" : "%5.0f", d.hz);
        if (wsprOnly) {
            ImGui::TableNextColumn(); ImGui::TextUnformatted(d.call.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(d.grid.c_str());
            ImGui::TableNextColumn(); if (d.dbm >= 0) ImGui::Text("%d", d.dbm);
        } else {
            ImGui::TableNextColumn(); ImGui::TextColored(modeColour(d.mode), "%s", dect2::ftxModeName(d.mode));
        }
        ImGui::TableNextColumn();
        if (d.cq && d.mode != 3) ImGui::TextColored(pal::okGreen(), "%s", d.msg.c_str());
        else ImGui::TextUnformatted(d.msg.c_str());
        if (d.mirrored) { ImGui::SameLine(); ImGui::TextDisabled("(upside down)"); }
        ImGui::PopFont();
    }
    ImGui::EndTable();
}

void mapView(App& a, const dect2::HfdigFtxTelemetry& f, ImVec2 size) {
    // the newest decode of each call with a locator
    struct Pt { std::string call, grid; double lat, lon; int mode; bool cq; float snr; };
    std::vector<Pt> pts;
    for (size_t i = f.decodes.size(); i-- > 0;) {
        const auto& d = f.decodes[i];
        if (d.call.empty() || d.grid.empty() || !shown(d)) continue;
        bool have = false;
        for (const auto& p : pts) have |= p.call == d.call;
        if (have) continue;
        double la, lo;
        if (!dect2::ftxGridToLatLon(d.grid, la, lo)) continue;
        pts.push_back({d.call, d.grid, la, lo, d.mode, d.cq, d.snrDb});
    }
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (S.mapFollow && !pts.empty()) {
        double la0 = 90, la1 = -90, lo0 = 180, lo1 = -180;
        for (const auto& p : pts) { la0 = std::min(la0, p.lat); la1 = std::max(la1, p.lat); lo0 = std::min(lo0, p.lon); lo1 = std::max(lo1, p.lon); }
        S.map.lat = (la0 + la1) / 2; S.map.lon = (lo0 + lo1) / 2;
        const double span = std::max(2.0, std::max(lo1 - lo0, (la1 - la0) * 1.4));
        S.map.zoom = std::max(adsbmap::kMinZoom, std::min(8, (int)std::floor(std::log2(0.7 * size.x * 360.0 / (256.0 * span)))));
    }
    const double lat0 = S.map.lat, lon0 = S.map.lon;
    adsbmap::draw(S.map, size);
    if (S.map.lat != lat0 || S.map.lon != lon0) S.mapFollow = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    for (const auto& p : pts) {
        const ImVec2 q = adsbmap::project(p.lat, p.lon);
        if (q.x < p0.x - 60 || q.y < p0.y - 30 || q.x > p0.x + size.x + 60 || q.y > p0.y + size.y + 30) continue;
        const ImVec4 c = modeColour(p.mode);
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(c);
        dl->AddCircleFilled(q, 4.5f * gUi, col);
        dl->AddCircle(q, 4.5f * gUi, p.cq ? IM_COL32(120, 230, 140, 255) : IM_COL32(0, 0, 0, 150), 16, p.cq ? 1.8f : 1.f);
        const ImVec2 ts = ImGui::CalcTextSize(p.call.c_str());
        const ImVec2 lp(q.x + 8 * gUi, q.y - ts.y * 0.5f - 2 * gUi);
        dl->AddRectFilled(lp, ImVec2(lp.x + ts.x + 8 * gUi, lp.y + ts.y + 4 * gUi), IM_COL32(10, 12, 14, 210), 2.f);
        dl->AddText(ImVec2(lp.x + 4 * gUi, lp.y + 2 * gUi), IM_COL32(235, 238, 240, 255), p.call.c_str());
    }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) { S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1); S.mapFollow = false; }
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) { S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1); S.mapFollow = false; }
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("fit")) S.mapFollow = true;
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the locators themselves");
    plat::prefs().setI("ftxZoom", S.map.zoom);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
    if (pts.empty()) adsbmap::legend(size.x, "no locators heard yet");
    else adsbmap::legend(size.x, "%zu stations by their locators (the middle of the square)", pts.size());
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
}

} // namespace

void hfdigFtxPresetFt8(App& a) {
    a.engine.hfdig().ftx().setEnabled(0, true);   // 0 FT8
    S.filter = 1;                                  // 1 + FT8
    S.view = 0;
}

void hfdigFtxTab(App& a, const dect2::HfdigTelemetry& t) {
    using namespace dect2;
    if (!S.loaded) { S.loaded = true; S.map.zoom = (int)plat::prefs().getI("ftxZoom", 3); }
    const HfdigFtxTelemetry& f = t.ftx;
    HfdigFtx& dec = a.engine.hfdig().ftx();
    // the modes and the filter
    for (int m = 0; m < kFtxModes; m++) {
        bool on = dec.enabled(m);
        ImGui::PushStyleColor(ImGuiCol_CheckMark, modeColour(m));
        if (ImGui::Checkbox(ftxModeName(m), &on)) dec.setEnabled(m, on);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decode %s (%g s slots)", ftxModeName(m), ftxPeriod(m));
        flowNext(10 * gUi);
    }
    bool mir = dec.mirrored();
    if (ImGui::Checkbox("Upside down too", &mir)) dec.setMirrored(mir);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Also look for signals sent on the lower sideband (their tones run backwards).");
    flowNext(14 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Show"); ImGui::SameLine(0, 5 * gUi);
    const char* fl[] = {"all", "FT8", "FT4", "FT2", "WSPR"};
    ImGui::SetNextItemWidth(78 * gUi);
    if (ImGui::BeginCombo("##ftxfilter", fl[S.filter])) {
        for (int i = 0; i < 5; i++) if (ImGui::Selectable(fl[i], i == S.filter)) S.filter = i;
        ImGui::EndCombo();
    }
    flowNext(10 * gUi);
    ImGui::Checkbox("CQ only", &S.cqOnly);
    flowNext(10 * gUi);
    if (ImGui::Button("Clear")) dec.clearDecodes();
    flowEnd();
    // the clock
    {
        char now[16];
        utcText(f.nowUtc, now, sizeof now);
        const char* src = f.timeMode == 1 ? "recording start" : f.timeMode == 2 ? "searching the slots" : f.timeMode == 3 ? "slots found from the signals" : "system clock";
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("UTC"); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(f.nowUtc > 0 ? now : "--:--:--"); ImGui::PopFont();
        ImGui::SameLine(0, 6 * gUi); ImGui::TextDisabled("(%s)", src);
        flowNext(14 * gUi);
        if (f.clockN > 0) {
            char b[96];
            snprintf(b, sizeof b, "DT %+.2f s, spread %.2f s, %d decodes", f.clockErr, f.dtSpread, f.clockN);
            if (f.clockWarn) ImGui::TextColored(pal::warnAmber(), "clock off by %+.1f s: set the computer clock (decodes still work to 2.5 s)", f.clockErr);
            else ImGui::TextDisabled("%s", b);
        } else ImGui::TextDisabled("no decodes yet");
        flowEnd();
        std::string last;
        for (int m = 0; m < kFtxModes; m++) {
            if (!f.slots[m]) continue;
            char tm[16], b[80];
            utcText(f.lastSlotUtc[m], tm, sizeof tm);
            snprintf(b, sizeof b, "%s%s %s: %d (%.0f ms)", last.empty() ? "" : "   ", ftxModeName(m), tm, f.lastSlotCount[m], f.decodeMs[m]);
            last += b;
        }
        if (!last.empty()) { ImGui::TextDisabled("last slots"); ImGui::SameLine(0, 6 * gUi); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(last.c_str()); ImGui::PopFont(); }
    }
    subNav("ftxv", S.view, {"Decodes", "Map", "WSPR spots"});
    if (S.view == 0) decodeTable(a, f, false);
    else if (S.view == 1) {
        const ImVec2 av = ImGui::GetContentRegionAvail();
        if (av.x > 60 && av.y > 60) mapView(a, f, av);
    } else decodeTable(a, f, true);
}
