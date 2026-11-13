// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tools > Digital video generator, tab "Digital Television Mode Selector" (videogen_modes_ui.h). Left: the link (bitrate, bandwidth,
// C/N), and under it the most optimal mode. Right: a block per system (DVB-T, DVB-T2, DVB-T2 1.7 MHz, DVB-S, DVB-S2, DVB-S2X), its parameters on the left - each left open ("Any") or set -
// and on the right the modes that still work on the link with those parameters, the best first. The inputs and the parameters are
// kept in <data dir>/videogen_modes.txt.
#include "videogen_modes_ui.h"
#include "../app.h"
#include "../platform/platform.h"
#include "dect2/common/u8file.h"
#include "dect2/tv/dvb_modes.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace dect2::vgen;

namespace {

const char* const kLinkHelp = N_("What the link offers. The bitrate is what has to be carried: the generator's TS mux rate unless you untick it. The bandwidth is the room you have on the spectrum (a channel, a band-plan segment). The C/N is the ratio of the carrier to the noise you see on the spectrum analyser, in the same bandwidth as the signal.");

const char* const kModesHelp = N_("Per system, set the parameters you want or leave them on Any. The list beside them shows the modes that carry the bitrate within the bandwidth at the C/N, the best first: the smallest bandwidth, then the most C/N margin. Satellite modes have a free symbol rate: their bandwidth is what the bitrate needs, their capacity what the whole bandwidth would carry. The thresholds are the ideal AWGN values of the standards (no implementation margin, phase noise or multipath): keep a few dB in hand. The DVB-T2 bitrate is the ideal cell capacity (P1/P2 symbols and signalling not deducted), so the real one is a few percent lower.");

const char* const kBestHelp = N_("The mode with the smallest bandwidth that carries the bitrate at this C/N, over all systems and with the parameters set in their blocks; between modes of the same bandwidth, the one with the most C/N margin. Set parameters in a block (or the bandwidth) to steer it, e.g. to a system your receiver has.");

struct Sel {
    bool init = false;
    bool fromGenerator = true;   // the bitrate follows the generator's mux rate
    double mbps = 1.3, bwMHz = 8, cnDb = 20;
    std::array<ModeFilter, kNumDvbSystems> filter;
    // the available modes per system, worked out again when the link or a filter changes
    bool cached = false;
    LinkInput cachedIn;
    std::array<ModeFilter, kNumDvbSystems> cachedFilter;
    std::array<std::vector<ModeChoice>, kNumDvbSystems> avail;
    std::array<size_t, kNumDvbSystems> matching{};   // the modes with the parameters set, working or not
    std::array<std::string, kNumDvbSystems> why;   // when none is available: what rules them out
    std::string saved;
    double lastSave = 0;
    Sel() { filter.fill(openFilter()); }
};

Sel& S() {
    static Sel s;
    return s;
}

std::string cfgPath() { return plat::dataDir() + "/videogen_modes.txt"; }

std::string saveText(const Sel& s) {
    std::array<char, 160> b{};
    snprintf(b.data(), b.size(), "gen=%d\nmbps=%.6f\nbw=%.6f\ncn=%.3f\n", s.fromGenerator ? 1 : 0, s.mbps, s.bwMHz, s.cnDb);
    std::string o = b.data();
    for (int i = 0; i < kNumDvbSystems; i++) {   // "f.DVB-T2=-1,-1,4,..." per system: the index of each parameter's value, -1 = Any
        o += std::string("f.") + dvbSystemName((DvbSystem)i) + "=";
        for (size_t k = 0; k < modeParams((DvbSystem)i).size(); k++) o += (k ? "," : "") + std::to_string(s.filter[(size_t)i][k]);
        o += "\n";
    }
    return o;
}

void loadFilter(DvbSystem sys, const std::string& v, ModeFilter& f) {
    const std::vector<ModeParam>& params = modeParams(sys);
    size_t pos = 0;
    for (size_t k = 0; k < params.size() && pos <= v.size(); k++) {
        const int i = atoi(v.c_str() + pos);
        f[k] = i >= 0 && i < (int)params[k].values.size() ? i : -1;
        const size_t comma = v.find(',', pos);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
}

void loadText(const std::string& text, Sel& s) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "gen") s.fromGenerator = atoi(v.c_str()) != 0;
        else if (k == "mbps") s.mbps = std::clamp(atof(v.c_str()), 0.001, 1000.0);
        else if (k == "bw") s.bwMHz = std::clamp(atof(v.c_str()), 0.01, 1000.0);
        else if (k == "cn") s.cnDb = std::clamp(atof(v.c_str()), -20.0, 60.0);
        else
            for (int i = 0; i < kNumDvbSystems; i++)
                if (k == std::string("f.") + dvbSystemName((DvbSystem)i)) loadFilter((DvbSystem)i, v, s.filter[(size_t)i]);
    }
}

void loadOnce(Sel& s) {
    if (s.init) return;
    s.init = true;
    std::string t;
    if (FILE* f = dect2::u8fopen(cfgPath(), "rb")) {
        std::array<char, 1024> b{};
        size_t n;
        while ((n = fread(b.data(), 1, b.size(), f)) > 0) t.append(b.data(), n);
        fclose(f);
    }
    loadText(t, s);
    s.saved = saveText(s);
}

void saveSoon(Sel& s) {
    const double now = ImGui::GetTime();
    if (now - s.lastSave < 1.0) return;
    s.lastSave = now;
    const std::string t = saveText(s);
    if (t == s.saved) return;
    if (FILE* f = dect2::u8fopen(cfgPath(), "wb")) { fwrite(t.data(), 1, t.size(), f); fclose(f); }
    s.saved = t;
}

// A dimmed "(?)" after the last item, its text wrapped in a tooltip (as in the generator's other tabs)
void helpMark(const char* text) {
    ImGui::SameLine(0, 4 * gUi);
    ImGui::TextDisabled("(?)");
    if (!ImGui::IsItemHovered()) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380 * gUi);
    ImGui::TextUnformatted(tr(text));
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

bool numberRow(const char* label, const char* id, double& v, const char* fmt, double lo, double hi, float col, float w) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr(label));
    ImGui::SameLine(col);
    ImGui::SetNextItemWidth(w);
    if (!ImGui::InputDouble(id, &v, 0, 0, fmt)) return false;
    v = std::clamp(v, lo, hi);
    return true;
}

void linkInputs(Sel& s, int generatorKbps) {
    ImGui::SeparatorText(tr("Link"));
    helpMark(kLinkHelp);
    const float col = 110 * gUi, w = std::max(90 * gUi, ImGui::GetContentRegionAvail().x - col);
    if (s.fromGenerator) s.mbps = generatorKbps / 1000.0;
    ImGui::BeginDisabled(s.fromGenerator);
    numberRow(N_("Bitrate"), "##dmsrate", s.mbps, "%.3f Mbit/s", 0.001, 1000, col, w);
    ImGui::EndDisabled();
    ImGui::Checkbox(tr("The generator's TS mux rate"), &s.fromGenerator);
    numberRow(N_("Bandwidth"), "##dmsbw", s.bwMHz, "%.3f MHz", 0.01, 1000, col, w);
    numberRow(N_("C/N"), "##dmscn", s.cnDb, "%.1f dB", -20, 60, col, w);
}

// Why no mode of a system works: no mode has the parameters, none carries the bitrate in the bandwidth, or the C/N is too low for those
// that would
std::string whyNone(DvbSystem sys, const ModeFilter& f, const LinkInput& in) {
    bool any = false;
    double bestCn = 1e9;
    for (const DvbMode& m : dvbModes(sys)) {
        if (!matches(m, f)) continue;
        any = true;
        LinkInput noCn = in;
        noCn.cnDb = 1e9;
        if (fitMode(m, noCn).ok) bestCn = std::min(bestCn, m.cnDb);
    }
    if (!any) return tr("No mode has these parameters.");
    if (bestCn < 1e9) {
        std::array<char, 64> b{};
        snprintf(b.data(), b.size(), "%.1f dB", bestCn);
        return std::string(tr("The C/N is too low: these parameters need at least")) + " " + b.data() + ".";
    }
    return tr("No mode with these parameters carries the bitrate within the bandwidth.");
}

void update(Sel& s) {
    const LinkInput in{s.mbps * 1000, s.bwMHz, s.cnDb};
    if (s.cached && in.kbps == s.cachedIn.kbps && in.bwMHz == s.cachedIn.bwMHz && in.cnDb == s.cachedIn.cnDb && s.filter == s.cachedFilter) return;
    s.cached = true;
    s.cachedIn = in;
    s.cachedFilter = s.filter;
    for (int i = 0; i < kNumDvbSystems; i++) {
        const auto sys = (DvbSystem)i;
        s.avail[(size_t)i] = availableModes(sys, s.filter[(size_t)i], in);
        const std::vector<DvbMode>& all = dvbModes(sys);
        s.matching[(size_t)i] = (size_t)std::count_if(all.begin(), all.end(), [&](const DvbMode& m) { return matches(m, s.filter[(size_t)i]); });
        s.why[(size_t)i] = s.avail[(size_t)i].empty() ? whyNone(sys, s.filter[(size_t)i], in) : std::string();
    }
}

// the parameters of a system: a combo each, "Any" first
void paramFilters(Sel& s, DvbSystem sys) {
    ModeFilter& f = s.filter[(size_t)sys];
    const std::vector<ModeParam>& params = modeParams(sys);
    const float col = 100 * gUi, w = std::max(80 * gUi, ImGui::GetContentRegionAvail().x - col);
    for (size_t k = 0; k < params.size(); k++) {
        ImGui::PushID((int)k);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(tr(params[k].name));
        ImGui::SameLine(col);
        ImGui::SetNextItemWidth(w);
        const char* shown = f[k] < 0 ? tr("Any") : tr(params[k].values[(size_t)f[k]].c_str());
        if (ImGui::BeginCombo("##param", shown)) {
            if (ImGui::Selectable(tr("Any"), f[k] < 0)) f[k] = -1;
            for (size_t v = 0; v < params[k].values.size(); v++)
                if (ImGui::Selectable(tr(params[k].values[v].c_str()), f[k] == (int)v)) f[k] = (int)v;
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }
    const bool set = std::any_of(f.begin(), f.end(), [](int v) { return v >= 0; });
    ImGui::BeginDisabled(!set);
    if (ImGui::SmallButton(tr("All on Any"))) f = openFilter();
    ImGui::EndDisabled();
}

// the parameters of a mode in words, for the tooltip of its name
void modeTooltip(const DvbMode& m) {
    const std::vector<ModeParam>& params = modeParams(m.system);
    ImGui::BeginTooltip();
    for (size_t k = 0; k < params.size(); k++) ImGui::Text("%s: %s", tr(params[k].name), tr(params[k].values[(size_t)m.value[k]].c_str()));
    if (!isSatellite(m.system)) ImGui::Text("%s: %.3f Mbit/s", tr("Bitrate"), m.rateMbps);
    else ImGui::Text("%s: %.4f bit/symbol", tr("Spectral efficiency"), m.rateMbps);
    ImGui::EndTooltip();
}

void modeList(const Sel& s, DvbSystem sys, float h) {
    const std::vector<ModeChoice>& avail = s.avail[(size_t)sys];
    const bool sat = isSatellite(sys);
    ImGui::Text("%s: %zu / %zu", tr("Available modes"), avail.size(), s.matching[(size_t)sys]);
    if (avail.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.2f, 1), "%s", s.why[(size_t)sys].c_str());
        ImGui::Dummy(ImVec2(0, std::max(0.0f, h - 2 * ImGui::GetTextLineHeightWithSpacing())));
        return;
    }
    const int cols = sat ? 6 : 5;
    const ImGuiTableFlags fl = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##modes", cols, fl, ImVec2(0, h - ImGui::GetTextLineHeightWithSpacing()))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Mode"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(tr("MHz"));
    if (sat) ImGui::TableSetupColumn(tr("Msym/s"));
    ImGui::TableSetupColumn(sat ? tr("Max Mbit/s") : tr("Mbit/s"));
    ImGui::TableSetupColumn(tr("C/N dB"));
    ImGui::TableSetupColumn(tr("Margin dB"));
    ImGui::TableHeadersRow();
    ImGuiListClipper clip;
    clip.Begin((int)avail.size());
    while (clip.Step())
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
            const ModeChoice& c = avail[(size_t)r];
            ImGui::TableNextRow();
            if (r == 0) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_HeaderHovered));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(modeName(*c.mode).c_str());
            if (ImGui::IsItemHovered()) modeTooltip(*c.mode);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", c.fit.bwMHz);
            if (sat) { ImGui::TableNextColumn(); ImGui::Text("%.3f", c.fit.symbolRate); }
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", c.fit.capacityMbps);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", c.mode->cnDb);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", c.fit.marginDb);
        }
    ImGui::EndTable();
}

// The best mode of all systems: the front of each system's list, compared the same way as within a list
void bestMode(const Sel& s) {
    ImGui::Spacing();
    ImGui::SeparatorText(tr("Most optimal mode"));
    helpMark(kBestHelp);
    const ModeChoice* best = nullptr;
    for (const std::vector<ModeChoice>& a : s.avail)
        if (!a.empty() && (!best || betterChoice(a.front(), *best))) best = &a.front();
    if (!best) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.65f, 0.2f, 1));
        ImGui::TextWrapped("%s", tr("No mode carries this bitrate within this bandwidth at this C/N. Allow more bandwidth, a lower bitrate or a better C/N, or leave more parameters on Any."));
        ImGui::PopStyleColor();
        return;
    }
    const DvbMode& m = *best->mode;
    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_PlotHistogram), "%s", modeName(m).c_str());
    if (ImGui::IsItemHovered()) modeTooltip(m);
    if (!ImGui::BeginTable("##dmsbest", 2, ImGuiTableFlags_SizingStretchProp)) return;
    auto row = [](const char* k, const std::string& v) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", k);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(v.c_str());
    };
    auto num = [](double v, const char* unit) {
        std::array<char, 48> b{};
        snprintf(b.data(), b.size(), "%.3f", v);
        return std::string(b.data()) + " " + unit;
    };
    row(tr("System"), dvbSystemName(m.system));
    const std::vector<ModeParam>& params = modeParams(m.system);
    for (size_t k = 0; k < params.size(); k++) row(tr(params[k].name), tr(params[k].values[(size_t)m.value[k]].c_str()));
    row(tr("Bandwidth"), num(best->fit.bwMHz, "MHz"));
    if (isSatellite(m.system)) row(tr("Symbol rate"), num(best->fit.symbolRate, "Msym/s"));
    row(isSatellite(m.system) ? tr("Capacity in the bandwidth") : tr("Capacity"), num(best->fit.capacityMbps, "Mbit/s"));
    row(tr("C/N needed"), num(m.cnDb, "dB"));
    row(tr("C/N margin"), num(best->fit.marginDb, "dB"));
    ImGui::EndTable();
}

// one system: the parameters beside the modes they leave
void systemBlock(Sel& s, DvbSystem sys) {
    ImGui::PushID((int)sys);
    ImGui::SeparatorText(dvbSystemName(sys));
    const int rows = std::max((int)modeParams(sys).size() + 1, 6);
    const float h = rows * ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginTable("##block", 2, ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("##params", ImGuiTableColumnFlags_WidthFixed, 240 * gUi);
        ImGui::TableSetupColumn("##list", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        paramFilters(s, sys);
        ImGui::TableNextColumn();
        modeList(s, sys, h);
        ImGui::EndTable();
    }
    ImGui::PopID();
}

} // namespace

void dvbModeSelectorTab(int generatorKbps) {
    Sel& s = S();
    loadOnce(s);
    const float left = std::min(330 * gUi, ImGui::GetContentRegionAvail().x * 0.35f);
    ImGui::BeginChild("##dmsinput", ImVec2(left, 0));
    linkInputs(s, generatorKbps);
    update(s);
    bestMode(s);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##dmsmodes", ImVec2(0, 0));
    ImGui::TextDisabled("%s", tr("Modes per system"));
    helpMark(kModesHelp);
    for (int i = 0; i < kNumDvbSystems; i++) systemBlock(s, (DvbSystem)i);
    ImGui::EndChild();
    saveSoon(s);
}
