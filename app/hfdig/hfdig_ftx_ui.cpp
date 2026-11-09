// HF digital, the FT8 / FT4 / FT2 / WSPR view of the main tab (called by hfdig_ui.cpp): which modes decode, the slot clock and its
// error, and the decodes: a list per slot (UTC, dB, DT, Hz, mode, message; CQ calls highlighted), the stations on the map (the ADS-B
// map, by their locators) and the WSPR spots.
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "../adsb/adsb_map.h"
#include "ui/kit/kit.h"
#include "dect2/hfdig/hfdig_ftx.h"
#include "dect2/hfdig/hfdig_rx.h"
#include "dect2/hfdig/hfdig_tel.h"
#include "dect2/map/contact_graph.h"
#include "../map/contact_lines.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

// hfdig_multiband_ui.cpp: the "Monitor bands" panel and the band filter of the combined list
void hfdigMultiBandPanel(App& a, const dect2::HfdigTelemetry& t);
bool hfdigMultiBandActive();
bool hfdigMbShown(const dect2::FtxDecode& d);
int hfdigMbBandOf(const dect2::FtxDecode& d);

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
    snprintf(out, n, "%s", dect2::timefmt::format((int64_t)std::floor(t), dect2::timefmt::zone(), seconds ? dect2::timefmt::Style::Clock : dect2::timefmt::Style::ClockShort, false).c_str());
}

ImVec4 modeColour(int m) {
    switch (m) {
    case 0: return ImVec4(0.55f, 0.78f, 0.95f, 1);
    case 1: return ImVec4(0.62f, 0.85f, 0.58f, 1);
    case 2: return ImVec4(0.85f, 0.70f, 0.95f, 1);
    case 4: return ImVec4(0.95f, 0.60f, 0.55f, 1);   // JT65
    case 5: return ImVec4(0.55f, 0.90f, 0.85f, 1);   // JT9
    case 10: return ImVec4(0.90f, 0.82f, 0.58f, 1);   // JT4
    case 6: return ImVec4(0.98f, 0.85f, 0.50f, 1);   // Q65
    case 7: case 9: return ImVec4(0.70f, 0.75f, 0.98f, 1);   // FST4, FST4W
    case 8: return ImVec4(0.90f, 0.65f, 0.80f, 1);   // MSK144
    default: return ImVec4(0.95f, 0.75f, 0.45f, 1);
    }
}

bool shown(const dect2::FtxDecode& d) {
    if (S.filter > 0 && d.mode != S.filter - 1 && !(S.filter == 8 && d.mode == 9)) return false;   // FST4 also shows its FST4W decodes
    if (S.cqOnly && !d.cq) return false;
    if (!hfdigMbShown(d)) return false;
    return true;
}

void decodeTable(App& a, const dect2::HfdigFtxTelemetry& f, bool wsprOnly) {
    const ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV;
    const bool mb = hfdigMultiBandActive();   // the monitor is on: the dial frequency of every decode is shown
    const int cols = (wsprOnly ? 8 : 6) + (mb ? 2 : 0);
    if (!ImGui::BeginTable(wsprOnly ? "##ftx_w" : "##ftx_d", cols, fl, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC");
    ImGui::TableSetupColumn("dB");
    ImGui::TableSetupColumn("DT");
    ImGui::TableSetupColumn("Hz");
    if (mb) { ImGui::TableSetupColumn("Band"); ImGui::TableSetupColumn("Dial MHz"); }
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
        if (mb) {
            const int bd = hfdigMbBandOf(d);
            ImGui::TableNextColumn(); if (bd >= 0) ImGui::TextUnformatted(dect2::mbBand(bd).name);
            ImGui::TableNextColumn(); if (d.dialHz > 0) ImGui::Text("%.4f", d.dialHz / 1e6);
        }
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
        if (d.early) { ImGui::SameLine(); ImGui::TextDisabled("(early)"); if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decoded before the slot ended; the full decode replaces it."); }
        if (d.how == 1) { ImGui::SameLine(); ImGui::TextDisabled("(combined)"); if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decoded by adding this signal's soft bits from earlier slots (the same message repeated)."); }
        else if (d.how == 2) { ImGui::SameLine(); ImGui::TextDisabled("(a-priori)"); if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decoded with the calls of a conversation heard on the band taken as known."); }
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
    const auto& co = contactlines::options();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    size_t nLines = 0;
    if (co.lines) {
        // the contacts of the messages shown, up to the newest one
        dect2::ContactGraph g(co.keepMin * 60.0);
        double now = 0;
        for (const auto& d : f.decodes) if (shown(d)) now = std::max(now, d.slotUtc);
        for (const auto& d : f.decodes) if (shown(d) && d.slotUtc >= now - co.keepMin * 60.0) g.add(d.slotUtc, d.msg, d.mode, d.dialHz, d.grid);
        g.expire(now);
        ImVec2 click;
        if (adsbmap::clickedAt(click)) contactlines::pickAt(g, click);
        nLines = contactlines::draw(dl, g, now, S.map.lon);
    }
    for (const auto& p : pts) {
        const ImVec2 q = adsbmap::project(p.lat, p.lon);
        if (q.x < p0.x - 60 || q.y < p0.y - 30 || q.x > p0.x + size.x + 60 || q.y > p0.y + size.y + 30) continue;
        const ImVec4 c = modeColour(p.mode);
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(c);
        dl->AddCircleFilled(q, 4.5f * gUi, col);
        contactlines::markSelected(dl, p.call, q);
        dl->AddCircle(q, 4.5f * gUi, p.cq ? IM_COL32(120, 230, 140, 255) : IM_COL32(0, 0, 0, 150), 16, p.cq ? 1.8f : 1.f);
        const ImVec2 ts = ImGui::CalcTextSize(p.call.c_str());
        const ImVec2 lp(q.x + 8 * gUi, q.y - ts.y * 0.5f - 2 * gUi);
        dl->AddRectFilled(lp, ImVec2(lp.x + ts.x + 8 * gUi, lp.y + ts.y + 4 * gUi), IM_COL32(10, 12, 14, 210), 2.f);
        dl->AddText(ImVec2(lp.x + 4 * gUi, lp.y + 2 * gUi), IM_COL32(235, 238, 240, 255), p.call.c_str());
    }
    if (receiverLocationSet()) {   // home
        double rl, ro; receiverLocation(rl, ro);
        const ImVec2 q = adsbmap::project(rl, ro);
        dl->AddRectFilled(ImVec2(q.x - 5 * gUi, q.y - 5 * gUi), ImVec2(q.x + 5 * gUi, q.y + 5 * gUi), IM_COL32(240, 200, 60, 255));
        dl->AddRect(ImVec2(q.x - 5 * gUi, q.y - 5 * gUi), ImVec2(q.x + 5 * gUi, q.y + 5 * gUi), IM_COL32(0, 0, 0, 200));
        dl->AddText(ImVec2(q.x + 8 * gUi, q.y - 7 * gUi), IM_COL32(240, 200, 60, 255), tr("home"));
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
    contactlines::controls();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the locators themselves");
    plat::prefs().setI("ftxZoom", S.map.zoom);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
    if (pts.empty()) adsbmap::legend(size.x, "no locators heard yet");
    else if (co.lines) adsbmap::legend(size.x, tr("%zu stations by their locators, %zu contact lines"), pts.size(), nLines);
    else adsbmap::legend(size.x, "%zu stations by their locators (the middle of the square)", pts.size());
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
    ImGui::Dummy(ImVec2(0, 0));   // an item after SetCursorScreenPos: ImGui otherwise reports "extend window/parent boundaries"
}

} // namespace

// the usual dial frequencies (upper sideband) of each mode, as WSJT-X lists them; 60 m WSPR is the Region 1 one
struct FtxBand { const char* name; double mhz; };
const FtxBand kFt8Bands[] = {{"160m", 1.840}, {"80m", 3.573}, {"60m", 5.357}, {"40m", 7.074}, {"30m", 10.136}, {"20m", 14.074}, {"17m", 18.100},
                             {"15m", 21.074}, {"12m", 24.915}, {"11m", 27.265}, {"10m", 28.074}, {"6m", 50.313}, {"4m", 70.154}, {"2m", 144.174}};
const FtxBand kFt4Bands[] = {{"80m", 3.575}, {"40m", 7.0475}, {"30m", 10.140}, {"20m", 14.080}, {"17m", 18.104}, {"15m", 21.140},
                             {"12m", 24.919}, {"10m", 28.180}, {"6m", 50.318}, {"2m", 144.170}};
const FtxBand kFt2Bands[] = {{"20m", 14.084}};
// JT65 / JT9 / Q65 / FST4 / MSK144: the dial frequencies WSJT-X lists (JT65 and JT9 on the same bands, 2 kHz apart; MSK144 and Q65 on the VHF meteor-scatter / EME frequencies)
const FtxBand kJt65Bands[] = {{"160m", 1.838}, {"80m", 3.570}, {"40m", 7.076}, {"30m", 10.138}, {"20m", 14.076}, {"17m", 18.102}, {"15m", 21.076},
                              {"12m", 24.917}, {"10m", 28.076}, {"6m", 50.310}, {"2m", 144.120}};
const FtxBand kJt9Bands[] = {{"160m", 1.839}, {"80m", 3.572}, {"40m", 7.078}, {"30m", 10.140}, {"20m", 14.078}, {"17m", 18.104}, {"15m", 21.078},
                             {"12m", 24.919}, {"10m", 28.078}};
const FtxBand kQ65Bands[] = {{"20m", 14.100}, {"10m", 28.100}, {"6m", 50.275}, {"2m", 144.116}, {"70cm", 432.065}};
const FtxBand kFst4Bands[] = {{"160m", 1.8366}, {"80m", 3.5686}, {"40m", 7.0386}, {"30m", 10.1387}, {"20m", 14.0956}, {"17m", 18.1046}, {"15m", 21.0946},
                              {"10m", 28.1246}};
const FtxBand kJt4Bands[] = {{"2m", 144.120}, {"70cm", 432.065}, {"23cm", 1296.100}};   // EME / weak-signal calling areas
const FtxBand kMsk144Bands[] = {{"10m", 28.145}, {"6m", 50.260}, {"4m", 70.230}, {"2m", 144.150}};
const FtxBand kWsprBands[] = {{"160m", 1.8366}, {"80m", 3.5686}, {"60m", 5.3647}, {"40m", 7.0386}, {"30m", 10.1387}, {"20m", 14.0956},
                              {"17m", 18.1046}, {"15m", 21.0946}, {"12m", 24.9246}, {"10m", 28.1246}, {"6m", 50.293}, {"2m", 144.489}};

void bandButtons(App& a, int mode) {
    const FtxBand* b = kFt8Bands; int n = IM_ARRAYSIZE(kFt8Bands);
    if (mode == 1) { b = kFt4Bands; n = IM_ARRAYSIZE(kFt4Bands); }
    else if (mode == 2) { b = kFt2Bands; n = IM_ARRAYSIZE(kFt2Bands); }
    else if (mode == 3) { b = kWsprBands; n = IM_ARRAYSIZE(kWsprBands); }
    else if (mode == 4) { b = kJt65Bands; n = IM_ARRAYSIZE(kJt65Bands); }
    else if (mode == 5) { b = kJt9Bands; n = IM_ARRAYSIZE(kJt9Bands); }
    else if (mode == 6) { b = kQ65Bands; n = IM_ARRAYSIZE(kQ65Bands); }
    else if (mode == 7 || mode == 9) { b = kFst4Bands; n = IM_ARRAYSIZE(kFst4Bands); }
    else if (mode == 10) { b = kJt4Bands; n = IM_ARRAYSIZE(kJt4Bands); }
    else if (mode == 8) { b = kMsk144Bands; n = IM_ARRAYSIZE(kMsk144Bands); }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Band"); flowNext(6 * gUi);
    for (int i = 0; i < n; i++) {
        const bool here = std::fabs(a.freqMhz - b[i].mhz) < 0.002;
        if (here) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
        ImGui::PushID(i);
        if (ImGui::SmallButton(b[i].name)) tuneFreq(a, b[i].mhz);
        ImGui::PopID();
        if (here) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s on %s: dial %.4f MHz (upper sideband)", dect2::ftxModeName(mode), b[i].name, b[i].mhz);
        flowNext(4 * gUi);
    }
    flowEnd();
}

// The sub-mode and the T/R period of the modes that have a choice (JT65 A B C, Q65 A .. E and 15 .. 300 s, FST4 / FST4W 15 .. 1800 s, MSK144 15 / 30 s),
// remembered in the settings and handed to the decoder every frame.
struct ModeOpt { int sub; double per; };
ModeOpt& modeOpt(int m) {
    static ModeOpt o[dect2::kFtxModes] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 60}, {0, 60}, {0, 60}, {0, 60}, {0, 15}, {0, 120}, {0, 60}};
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        for (int k = 4; k < dect2::kFtxModes; k++) {
            char key[24];
            snprintf(key, sizeof key, "ftxSub%d", k); o[k].sub = (int)plat::prefs().getI(key, o[k].sub);
            snprintf(key, sizeof key, "ftxPer%d", k); o[k].per = (double)plat::prefs().getI(key, (long)o[k].per);
        }
    }
    return o[m];
}

void modeOptions(App& a, int only) {
    using namespace dect2;
    HfdigFtx& dec = a.engine.hfdig().ftx();
    auto save = [&](int m) {
        char key[24];
        snprintf(key, sizeof key, "ftxSub%d", m); plat::prefs().setI(key, modeOpt(m).sub);
        snprintf(key, sizeof key, "ftxPer%d", m); plat::prefs().setI(key, (long)modeOpt(m).per);
    };
    auto periodCombo = [&](int m, const double* list, int n, const char* id) {
        ModeOpt& o = modeOpt(m);
        char cur[16];
        snprintf(cur, sizeof cur, "%g s", o.per);
        ImGui::SetNextItemWidth(78 * gUi);
        if (ImGui::BeginCombo(id, cur)) {
            for (int i = 0; i < n; i++) {
                char b[16];
                snprintf(b, sizeof b, "%g s", list[i]);
                if (ImGui::Selectable(b, std::fabs(list[i] - o.per) < 0.5)) { o.per = list[i]; save(m); }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: the length of the transmit / receive periods", ftxModeName(m));
    };
    auto subCombo = [&](int m, const char* const* names, int n, const char* id) {
        ModeOpt& o = modeOpt(m);
        ImGui::SetNextItemWidth(52 * gUi);
        if (ImGui::BeginCombo(id, names[std::max(0, std::min(n - 1, o.sub))])) {
            for (int i = 0; i < n; i++) if (ImGui::Selectable(names[i], i == o.sub)) { o.sub = i; save(m); }
            ImGui::EndCombo();
        }
    };
    static const char* const abc[] = {"A", "B", "C"};
    static const char* const ae[] = {"A", "B", "C", "D", "E"};
    static const double q65p[] = {15, 30, 60, 120, 300}, fstp[] = {15, 30, 60, 120, 300, 900, 1800}, mskp[] = {15, 30};
    auto label = [&](const char* t) { ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", t); ImGui::SameLine(0, 4 * gUi); };
    if (only < 0 || only == 4) { label("JT65"); subCombo(4, abc, 3, "##jt65sub"); if (ImGui::IsItemHovered()) ImGui::SetTooltip("JT65 sub-mode: A, B and C have tone spacings of 2.7, 5.4 and 10.8 Hz"); flowNext(10 * gUi); }
    if (only < 0 || only == 10) { static const char* const ag[] = {"A", "B", "C", "D", "E", "F", "G"}; label("JT4"); subCombo(10, ag, 7, "##jt4sub"); if (ImGui::IsItemHovered()) ImGui::SetTooltip("JT4 sub-mode: A to G have tone spacings of 4.4 Hz doubling up to 280 Hz"); flowNext(10 * gUi); }
    if (only < 0 || only == 6) { label("Q65"); subCombo(6, ae, 5, "##q65sub"); flowNext(4 * gUi); periodCombo(6, q65p, 5, "##q65per"); flowNext(10 * gUi); }
    if (only < 0 || only == 7) { label("FST4"); periodCombo(7, fstp, 7, "##fst4per"); flowNext(10 * gUi); }
    if (only < 0 || only == 7) { label("FST4W"); periodCombo(9, fstp, 7, "##fst4wper"); flowNext(10 * gUi); }
    if (only < 0 || only == 8) { label("MSK144"); periodCombo(8, mskp, 2, "##mskper"); flowNext(10 * gUi); }
    for (int m = 4; m < kFtxModes; m++) dec.setModeOption(m, modeOpt(m).sub, modeOpt(m).per);
}

void hfdigFtxTab(App& a, const dect2::HfdigTelemetry& t) {
    using namespace dect2;
    if (!S.loaded) { S.loaded = true; S.map.zoom = (int)plat::prefs().getI("ftxZoom", 3); }
    const HfdigFtxTelemetry& f = t.ftx;
    HfdigFtx& dec = a.engine.hfdig().ftx();
    // the modes and the filter; an entry of one mode (FT8, FT4, FT2, WSPR) decodes and shows only that one
    const SubMode* sm = curSub(a);
    const int only = hfdigMultiBandActive() ? -1 : (sm ? sm->sel : -1);   // the monitor lists every mode of every band in one list
    if (only >= 0) S.filter = only + 1;
    for (int m = 0; m < kFtxModes && only < 0; m++) {
        bool on = dec.enabled(m);
        ImGui::PushStyleColor(ImGuiCol_CheckMark, modeColour(m));
        if (ImGui::Checkbox(ftxModeName(m), &on)) dec.setEnabled(m, on);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decode %s (%g s slots)", ftxModeName(m), ftxPeriod(m));
        flowNext(10 * gUi);
    }
    bool mir = dec.mirrored();
    if (ImGui::Checkbox("Upside down too", &mir)) dec.setMirrored(mir);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Also look for signals sent on the lower sideband (their tones run backwards). FT8, FT4, FT2 and WSPR only: the other modes expect upper sideband.");
    flowNext(14 * gUi);
    bool early = dec.earlyDecode();
    if (ImGui::Checkbox("Early decode", &early)) dec.setEarlyDecode(early);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("FT8 and FT4: a first decode a few seconds before the slot ends, marked (early) and replaced by the full decode.");
    flowNext(14 * gUi);
    if (only < 0) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Show"); ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(78 * gUi);
        if (ImGui::BeginCombo("##ftxfilter", S.filter == 0 ? "all" : ftxModeName(S.filter - 1))) {
            if (ImGui::Selectable("all", S.filter == 0)) S.filter = 0;
            for (int i = 0; i < kFtxModes; i++) if (ImGui::Selectable(ftxModeName(i), i + 1 == S.filter)) S.filter = i + 1;
            ImGui::EndCombo();
        }
        flowNext(10 * gUi);
    }
    ImGui::Checkbox("CQ only", &S.cqOnly);
    flowNext(10 * gUi);
    if (ImGui::Button("Clear")) a.engine.hfdig().clearResults(8);   // the decodes the receiver keeps (also in the results file)
    flowEnd();
    modeOptions(a, only);
    flowEnd();
    hfdigMultiBandPanel(a, t);
    if (kit::collapsingSection(tr("My location"), false)) {   // the one receiver position of the whole program (ADS-B, passes, maps), also set here (#453)
        ImGui::AlignTextToFramePadding();
        receiverPositionFields(a);
        if (!receiverLocationSet()) { ImGui::SameLine(); ImGui::TextColored(pal::warnAmber(), "%s", tr("not set")); }
        ImGui::TextDisabled("%s", tr("The same position as in every other mode (ADS-B antenna position, Passes, Map). It is marked on the map below."));
    }
    bandButtons(a, only >= 0 ? only : 0);
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
            if (!f.slots[m] || (only >= 0 && m != only && !(only == 7 && m == 9))) continue;
            char tm[16], b[80];
            utcText(f.lastSlotUtc[m], tm, sizeof tm);
            snprintf(b, sizeof b, "%s%s %s: %d (%.0f ms)", last.empty() ? "" : "   ", ftxModeName(m), tm, f.lastSlotCount[m], f.decodeMs[m]);
            last += b;
        }
        if (!last.empty()) { ImGui::TextDisabled("last slots"); ImGui::SameLine(0, 6 * gUi); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(last.c_str()); ImGui::PopFont(); }
    }
    if (only >= 0) {   // FT8 / FT4 / FT2: decodes and map; WSPR: spots and map
        if (S.view > 1) S.view = 0;
        subNav("ftxv1", S.view, {only == 3 ? "Spots" : "Decodes", "Map"});
        if (S.view == 0) decodeTable(a, f, only == 3);
        else { const ImVec2 av = ImGui::GetContentRegionAvail(); if (av.x > 60 && av.y > 60) mapView(a, f, av); }
        return;
    }
    subNav("ftxv", S.view, {"Decodes", "Map", "WSPR spots"});
    if (S.view == 0) decodeTable(a, f, false);
    else if (S.view == 1) {
        const ImVec2 av = ImGui::GetContentRegionAvail();
        if (av.x > 60 && av.y > 60) mapView(a, f, av);
    } else decodeTable(a, f, true);
}
