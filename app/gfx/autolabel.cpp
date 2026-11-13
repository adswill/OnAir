// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Auto-labelled spectrum, the App side: gathers the names (band plan, frequency database, the FM / DAB / DVB scans and what is heard on the
// channel now, the user's memories) into a store that is rebuilt only when a source changed, lays them out for the view (autolabel_logic.h),
// draws them on the spectrum and the waterfall and handles the click (tune, and open the label's mode) and the source toggles. Each plot
// keeps its layout until the view, the size or the store changes, so a frame with nothing new costs a comparison and the drawing of a few boxes.
#include "../app.h"
#include "../memory/memory_options.h"
#include "../ui/ui_common.h"
#include "plot_util.h"
#include "dect2/channels/freq_db.h"
#include "dect2/channels/memory_bank.h"
#include <map>

namespace {

using autolabel::Label;

autolabel::Store gStore;
uint64_t gRev = 0, gStoreRev = 0;
double gRevT = -1;
std::map<std::pair<int, long>, Label> gHeard;   // what was heard on a channel in this session: (source, kHz) -> name
size_t gPerSrc[autolabel::NumSrc] = {};
bool gHovered = false;
std::string gNote;
double gNoteT = -1e9;
float gBtnW = 0;

struct Slot {
    autolabel::View v;
    uint64_t rev = ~0ull;
    float fs = 0;
    autolabel::Result r;
    bool valid = false;
} gSlot[2];

const char* srcName(int s) {
    switch (s) {
        case autolabel::Band: return tr("Band plan");
        case autolabel::FreqDb: return tr("Frequency database");
        case autolabel::Fm: return tr("FM stations (RDS names)");
        case autolabel::Dab: return tr("DAB ensembles");
        case autolabel::Dvb: return tr("DVB networks and services");
        default: return tr("Memories");
    }
}
ImU32 srcColour(int s, int alpha) {
    static const ImU32 c[autolabel::NumSrc] = {IM_COL32(120, 150, 190, 255), IM_COL32(205, 170, 105, 255), IM_COL32(110, 205, 140, 255),
                                              IM_COL32(190, 140, 220, 255), IM_COL32(235, 140, 95, 255), IM_COL32(240, 215, 100, 255)};
    ImU32 o = pal::remap(c[s >= 0 && s < autolabel::NumSrc ? s : 0]);
    return (o & 0x00FFFFFFu) | ((ImU32)alpha << 24);
}

void bandwidthLine(ImDrawList* dl, int src, float x0, float x1, float cx, float y, float thickness, bool fadeLeft, bool fadeRight, int alpha) {
    if (x1 <= x0) return;
    const float visible0 = std::max(x0, dl->GetClipRectMin().x), visible1 = std::min(x1, dl->GetClipRectMax().x);
    const float centre = cx > visible0 && cx < visible1 ? cx : (visible0 + visible1) * 0.5f;   // anchor fades to the pointer when visible, otherwise the visible band's middle
    const float maxFade = 48 * gUi;   // wide bands must not turn the whole line into a gradient
    const float solid0 = x0 + (fadeLeft ? std::min(maxFade, (centre - x0) * 0.9f) : 0), solid1 = x1 - (fadeRight ? std::min(maxFade, (x1 - centre) * 0.9f) : 0);
    const ImU32 c = srcColour(src, alpha), clear = srcColour(src, 0);
    dl->AddRectFilled(ImVec2(solid0, y - thickness * 0.5f), ImVec2(solid1, y + thickness * 0.5f), c);
    if (fadeLeft) dl->AddRectFilledMultiColor(ImVec2(x0, y - thickness * 0.5f), ImVec2(solid0, y + thickness * 0.5f), clear, c, c, clear);
    else dl->AddLine(ImVec2(x0, y), ImVec2(x0, y + 3 * gUi), c, thickness);
    if (fadeRight) dl->AddRectFilledMultiColor(ImVec2(solid1, y - thickness * 0.5f), ImVec2(x1, y + thickness * 0.5f), c, clear, clear, c);
    else dl->AddLine(ImVec2(x1, y), ImVec2(x1, y + 3 * gUi), c, thickness);
}

std::string clip(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    size_t k = n;
    while (k > 0 && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80) k--;   // not inside a UTF-8 character
    return s.substr(0, k) + "...";
}
std::string shown(const Label& l, bool detailed) { return detailed && !l.detail.empty() ? l.text + "  " + clip(l.detail, 56) : l.text; }

std::string modeKeyOf(const App& a) {
    for (int i = 0; i < kNumModes; i++) if (modeSelected(a, kModes[i])) return memoryModeKey(kModes[i]);
    return std::string();
}

std::string stripTag(std::string s) { const size_t br = s.rfind(" ["); if (br != std::string::npos) s.resize(br); return s; }

// The names heard on the channel the receiver is on now (RDS name, DAB ensemble, DVB network): kept for the session, so the next sweep of the band
// shows them without a scan. Called twice a second.
void learn(const App& a) {
    if (!a.engine.running()) return;
    const long khz = std::lround(a.freqMhz * 1000.0);
    auto put = [&](int src, const std::string& text, const std::string& detail, const std::string& mode) {
        if (text.empty()) return;
        Label l; l.mhz = (double)khz / 1000.0; l.text = text; l.detail = detail; l.mode = mode; l.src = src; l.prio = 1.f;   // heard just now: over what a scan found
        gHeard[{src, khz}] = l;
    };
    if (a.fmMode) { put(autolabel::Fm, autolabel::trimmed(a.rx.fm.psName), a.rx.fm.radioText, "fm"); return; }
    if (a.rx.standard == 3) { if (a.rx.dab.state == 2 && a.rx.dab.ensemble) put(autolabel::Dab, autolabel::trimmed(a.rx.dab.ensembleLabel), "", "dab"); return; }
    if (!a.ts.services.empty()) {
        std::string names;
        for (const auto& sv : a.ts.services) if (!sv.name.empty()) names += (names.empty() ? "" : ", ") + sv.name;
        const std::string key = modeKeyOf(a);
        if (key == "dvb") put(autolabel::Dvb, !a.ts.networkName.empty() ? a.ts.networkName : a.ts.services.front().name, names, key);
    }
}

uint64_t revision(const App& a) {
    using autolabel::mix;
    uint64_t h = 1469598103934665603ull;
    h = mix(h, a.bandPlan); h = mix(h, a.uiLang); h = mix(h, modeKeyOf(a));
    for (const auto& r : a.fmScan.results) { h = mix(h, (uint64_t)std::llround(r.mhz * 1000)); h = mix(h, r.name); }
    for (const auto& r : a.dabScan.results) { h = mix(h, (uint64_t)std::llround(r.mhz * 1000)); h = mix(h, r.label); h = mix(h, r.name); }
    for (const auto& s : a.fmSaved) { h = mix(h, (uint64_t)std::llround(s.mhz * 1000)); h = mix(h, s.name); }
    for (const auto& r : a.scanner.results()) { h = mix(h, (uint64_t)std::llround(r.freqMHz * 1000)); h = mix(h, r.networkName); h = mix(h, (uint64_t)r.services.size()); }
    for (const auto& m : memoryBank().all()) { h = mix(h, m.freqHz); h = mix(h, m.name); h = mix(h, (uint64_t)std::llround(m.bandwidthHz)); }
    for (const auto& kv : gHeard) { h = mix(h, (uint64_t)kv.first.second); h = mix(h, kv.second.text); }
    h = mix(h, (uint64_t)dect2::builtinFrequencies().size());
    return h;
}

void rebuild(App& a) {
    using namespace autolabel;
    gStore.clear();
    std::vector<Label> v;
    bandPlanLabelSource(a, v);
    freqdbLabelSource(v);
    // The frequency database lists every TV standard's channels (DVB-T/T2, DTMB, ATSC, ISDB-T share 470-862 MHz): with a TV mode open
    // only that standard's channel names are shown, so a DTMB receiver is not labelled DVB-T (#178)
    {
        auto tvKey = [](const std::string& m) { return m == "dvb" || m == "dtmb" || m == "atsc" || m == "atsc3" || m == "isdbt"; };
        const std::string cur = modeKeyOf(a);
        if (tvKey(cur))
            v.erase(std::remove_if(v.begin(), v.end(), [&](const Label& l) { return l.src == FreqDb && tvKey(l.mode) && l.mode != cur; }), v.end());
    }
    for (Label& l : v) gStore.add(std::move(l));
    for (const auto& r : a.fmScan.results) {
        if (!r.found || autolabel::trimmed(r.name).empty()) continue;
        Label l; l.mhz = r.mhz; l.text = autolabel::trimmed(r.name); l.detail = !r.rt.empty() ? r.rt : r.pty; l.mode = "fm"; l.src = Fm;
        gStore.add(l);
    }
    for (const auto& s : a.fmSaved) {
        if (s.name.empty()) continue;
        Label l; l.mhz = s.mhz; l.text = s.name; l.mode = "fm"; l.src = Fm; l.prio = -0.5f;   // a name the user typed: under one the station sent
        gStore.add(l);
    }
    for (const auto& r : a.dabScan.results) {
        if (!r.found) continue;
        const std::string t = !r.label.empty() ? r.label : r.name;
        if (autolabel::trimmed(t).empty()) continue;
        Label l; l.mhz = r.mhz; l.text = autolabel::trimmed(t); l.detail = r.stations; l.mode = "dab"; l.src = Dab;
        gStore.add(l);
    }
    for (const auto& r : a.scanner.results()) {
        if (!r.t2) continue;
        std::string t = r.networkName, names;
        for (const auto& x : r.services) names += (names.empty() ? "" : ", ") + stripTag(x);
        if (t.empty() && !r.services.empty()) t = stripTag(r.services.front());
        if (t.empty()) continue;
        Label l; l.mhz = r.freqMHz; l.text = t; l.detail = names; l.mode = "dvb"; l.src = Dvb;
        gStore.add(l);
    }
    for (const auto& kv : gHeard) gStore.add(kv.second);
    for (const auto& m : memoryBank().all()) {
        if (m.name.empty() || m.freqHz == 0) continue;
        Label l; l.mhz = (double)m.freqHz / 1e6; l.text = m.name; l.detail = m.group; l.mode = m.mode; l.src = Memories; l.bwMhz = m.bandwidthHz > 0 ? m.bandwidthHz / 1e6 : 0;
        gStore.add(l);
    }
    gStore.finish();
    for (size_t& n : gPerSrc) n = 0;
    for (size_t i = 0; i < gStore.size(); i++) gPerSrc[gStore.at(i).src]++;
}

// twice a second: learn the names of the channel, and rebuild the store when a source changed
void refresh(App& a) {
    const double now = glfwGetTime();
    if (gRevT >= 0 && now - gRevT < 0.5) return;
    gRevT = now;
    learn(a);
    const uint64_t r = revision(a);
    if (r != gRev || gStoreRev == 0) { gRev = r; rebuild(a); gStoreRev++; }
}

void activate(App& a, const Label& l) {
    const bool sameMode = l.mode.empty() || l.mode == modeKeyOf(a);
    if (sameMode || memoryModeIndex(l.mode) < 0) { spectrumTune(a, l.mhz); return; }   // the mode is already right (or this build lacks it): the usual click
    dect2::Memory m;
    m.name = l.text;
    m.freqHz = (uint64_t)std::llround(l.mhz * 1e6);
    m.mode = l.mode;
    std::string note;
    if (!memoryRecallApp(a, m, true, note)) { gNote = note.empty() ? tr("Could not open this mode.") : note; gNoteT = ImGui::GetTime(); }
}

void popupBody(App& a) {
    bool save = false;
    if (ImGui::Checkbox(tr("Names on the spectrum"), &a.specLabels)) save = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Station and channel names on the spectrum and the waterfall. A click on a name tunes to it and opens its mode."));
    ImGui::Separator();
    ImGui::BeginDisabled(!a.specLabels);
    for (int s = 0; s < autolabel::NumSrc; s++) {
        bool on = (a.specLabelMask >> s) & 1;
        ImGui::PushID(s);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 3 * gUi), ImVec2(p.x + 3 * gUi, p.y + h - 3 * gUi), srcColour(s, 255));
        ImGui::Dummy(ImVec2(6 * gUi, h));
        ImGui::SameLine(0, 0);
        char lb[96]; std::snprintf(lb, sizeof lb, "%s (%zu)###src", srcName(s), gPerSrc[s]);
        if (ImGui::Checkbox(lb, &on)) { a.specLabelMask = on ? (a.specLabelMask | (1 << s)) : (a.specLabelMask & ~(1 << s)); save = true; }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    ImGui::PushTextWrapPos(280 * gUi);
    ImGui::TextDisabled("%s", tr("Names that do not fit are left out. Zoom in to see more of them."));
    ImGui::TextDisabled("%s", tr("Bandwidth lines use the free space beside each name and fade at neighbouring labels. Hover a name for its full bandwidth."));
    ImGui::PopTextWrapPos();
    if (save) savePrefs(a);
}

} // namespace

bool autoLabelsHovered() { return gHovered; }

float autoLabelsButton(App& a, ImVec2 plotMin, ImVec2 plotSize, float rightUsed) {
    const std::string lbl = std::string(tr("Labels")) + "###lblbtn";
    gBtnW = specCornerPopup(a, lbl.c_str(), "##lblpop", plotMin, plotSize, rightUsed, 40 * gUi, 0, a.specLabels,
                            tr("Names of the stations and channels on the spectrum: choose which sources are shown"), popupBody);
    return gBtnW;
}

void autoLabelsDraw(App& a, int plotId, ImVec2 plotMin, ImVec2 plotSize, double loMhz, double hiMhz) {
    if (plotId == 0) gHovered = false;
    if (!a.specLabels || a.specLabelMask == 0 || plotSize.x < 80 * gUi || plotSize.y < 60 * gUi || !(hiMhz > loMhz)) return;
    if (plotId == 0) refresh(a);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize() * 0.86f, padX = 4 * gUi, rowH = fs + 4 * gUi, rowGap = 7 * gUi, triH = 5 * gUi, barH = std::max(1.f, gUi);   // rowGap holds the pointer triangle and its thin bandwidth line
    // below the axis title and the corner buttons (one text line), and left of the Waterfall button on the waterfall
    const float top = ImGui::GetTextLineHeight() + 8 * gUi, rightKeep = plotId ? 96 * gUi : 0;
    const int rows = std::max(1, std::min(3, (int)((plotSize.y * 0.3f - top) / (rowH + rowGap))));
    Slot& sl = gSlot[plotId ? 1 : 0];
    autolabel::View v;
    v.loMhz = loMhz; v.hiMhz = hiMhz; v.px0 = plotMin.x + 2 * gUi; v.px1 = plotMin.x + plotSize.x - 2 * gUi - rightKeep;
    v.ax0 = plotMin.x; v.ax1 = plotMin.x + plotSize.x;   // the frequency axis spans the whole plot rectangle, as in the other pane
    v.maxRows = rows; v.gap = 4 * gUi; v.padX = padX; v.mask = (unsigned)a.specLabelMask;
    const bool same = sl.valid && sl.rev == gStoreRev && sl.fs == fs && sl.v.loMhz == v.loMhz && sl.v.hiMhz == v.hiMhz && sl.v.px0 == v.px0 && sl.v.px1 == v.px1 && sl.v.ax0 == v.ax0 && sl.v.ax1 == v.ax1 &&
                      sl.v.maxRows == v.maxRows && sl.v.mask == v.mask;
    if (!same) {
        sl.r = autolabel::layout(gStore, v, [&](const Label& l, bool detailed) { return font->CalcTextSizeA(fs, FLT_MAX, 0, shown(l, detailed).c_str()).x + 2 * padX + 3 * gUi; });
        autolabel::layoutBandwidth(gStore, v, sl.r);
        sl.v = v; sl.rev = gStoreRev; sl.fs = fs; sl.valid = true;
    }
    const float y0 = plotMin.y + top;
    dl->PushClipRect(ImVec2(plotMin.x, plotMin.y), ImVec2(plotMin.x + plotSize.x, plotMin.y + plotSize.y), true);
    const bool plotHover = ImGui::IsMouseHoveringRect(plotMin, ImVec2(plotMin.x + plotSize.x, plotMin.y + plotSize.y)) && !ImGui::IsAnyItemActive();
    int hot = -1;
    if (plotHover) hot = autolabel::hit(sl.r, ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y, y0, rowH, rowGap);
    if (hot >= 0) gHovered = true;
    // the faint line from a name down through the plot, so the name can be matched to its signal
    for (size_t i = 0; i < sl.r.placed.size(); i++) {
        const autolabel::Placed& p = sl.r.placed[i];
        const Label& l = gStore.at(p.idx);
        if (!p.centreIn) continue;
        const float yTop = y0 + p.row * (rowH + rowGap) + rowH + triH;
        dl->AddLine(ImVec2(p.cx, yTop), ImVec2(p.cx, plotMin.y + plotSize.y), srcColour(l.src, (int)i == hot ? 150 : plotId ? 34 : 46), 1.f);
    }
    for (size_t i = 0; i < sl.r.placed.size(); i++) {
        const autolabel::Placed& p = sl.r.placed[i];
        const Label& l = gStore.at(p.idx);
        const float y = y0 + p.row * (rowH + rowGap);
        const bool h = (int)i == hot;
        if (!h) bandwidthLine(dl, l.src, p.bar0, p.bar1, p.cx, y + rowH + 1, barH, p.fadeLeft, p.fadeRight, 210);
        // the pointer: a triangle under the name with its tip at the frequency; a name kept for its band only has a stub arrow at the edge
        if (p.centreIn) dl->AddTriangleFilled(ImVec2(p.cx - triH * 0.8f, y + rowH + 1), ImVec2(p.cx + triH * 0.8f, y + rowH + 1), ImVec2(p.cx, y + rowH + triH + 1), srcColour(l.src, 255));
        else {
            const float ex = p.side < 0 ? v.px0 - 2 * gUi : v.px1 + 2 * gUi, d = p.side < 0 ? 1.f : -1.f, ym = y + rowH + triH * 0.5f;
            dl->AddTriangleFilled(ImVec2(ex, ym), ImVec2(ex + d * triH, ym - triH * 0.7f), ImVec2(ex + d * triH, ym + triH * 0.7f), srcColour(l.src, 200));
        }
        const int dim = l.src == autolabel::Band ? 175 : 235;
        dl->AddRectFilled(ImVec2(p.x0, y), ImVec2(p.x1, y + rowH), h ? IM_COL32(40, 46, 54, 245) : IM_COL32(14, 16, 20, 190), 3 * gUi);
        if (h) dl->AddRect(ImVec2(p.x0, y), ImVec2(p.x1, y + rowH), srcColour(l.src, 220), 3 * gUi);
        dl->AddText(font, fs, ImVec2(p.x0 + padX + 3 * gUi, y + (rowH - fs) * 0.5f), IM_COL32(236, 238, 240, dim), shown(l, sl.r.detailed).c_str());
    }
    if (hot >= 0) {   // the hovered band is drawn last, at full width over the other labels; only the plot clips it
        const autolabel::Placed& p = sl.r.placed[(size_t)hot];
        const Label& l = gStore.at(p.idx);
        if (l.bwMhz > 0 && !l.allocation)
            bandwidthLine(dl, l.src, autolabel::xOf(v, l.mhz - l.bwMhz * 0.5), autolabel::xOf(v, l.mhz + l.bwMhz * 0.5),
                          p.cx, y0 + p.row * (rowH + rowGap) + rowH + 1, barH, false, false, 255);
    }
    dl->PopClipRect();
    if (hot >= 0) {
        const Label& l = gStore.at(sl.r.placed[(size_t)hot].idx);
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(l.text.c_str());
        ImGui::TextDisabled("%s  -  %s", plt::formatFreqUnit(l.mhz).c_str(), srcName(l.src));
        if (l.bwMhz > 0 && !l.allocation) ImGui::TextDisabled(tr("Bandwidth: %s"), plt::formatFreqUnit(l.bwMhz).c_str());
        if (!l.detail.empty()) { ImGui::PushTextWrapPos(320 * gUi); ImGui::TextUnformatted(clip(l.detail, 240).c_str()); ImGui::PopTextWrapPos(); }
        ImGui::TextDisabled("%s", l.mode.empty() ? tr("Click to tune here") : tr("Click to tune here and open its mode"));
        ImGui::EndTooltip();
        const ImVec2 d = ImGui::GetMouseDragDelta(0, 0);
        if (ImGui::IsMouseReleased(0) && std::fabs(d.x) < 4 && std::fabs(d.y) < 4) activate(a, l);
    }
    if (gNoteT > ImGui::GetTime() - 4 && plotId == 0) {
        const ImVec2 ts = ImGui::CalcTextSize(gNote.c_str());
        const ImVec2 q(plotMin.x + 8 * gUi, plotMin.y + plotSize.y - ts.y - 8 * gUi);
        dl->AddRectFilled(ImVec2(q.x - 5 * gUi, q.y - 2 * gUi), ImVec2(q.x + ts.x + 5 * gUi, q.y + ts.y + 2 * gUi), IM_COL32(20, 22, 26, 220), 3 * gUi);
        dl->AddText(q, IM_COL32(240, 170, 120, 255), gNote.c_str());
    }
}
