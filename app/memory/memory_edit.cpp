// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Memory panel's dialogs: edit a memory (also where "Store current" lands, with the name prefilled) and the question when the same
// frequency and mode are stored already.
#include "memory_ui.h"
#include <algorithm>
#include <cstring>
#include <sstream>

namespace memui {

std::string freqReadout(uint64_t hz) {
    std::string d = std::to_string((unsigned long long)hz), o;
    for (size_t i = 0; i < d.size(); i++) { if (i && (d.size() - i) % 3 == 0) o += '.'; o += d[i]; }
    return o;
}
std::string memoryTitle(const dect2::Memory& m) {
    std::string t = m.name.empty() ? std::string(tr("(no name)")) : m.name;
    t += "  " + freqReadout(m.freqHz);
    const int mi = memoryModeIndex(m.mode);
    if (mi >= 0) t += std::string("  ") + kModes[mi].name;
    return t;
}

namespace {
struct E {
    bool request = false, askDup = false, isNew = false, useCol = false;
    uint64_t id = 0, dupId = 0, stored = 0;
    dect2::Memory m;
    char name[96] = "", group[64] = "", tags[128] = "", notes[512] = "", radio[96] = "", opts[768] = "";
    double mhz = 0, rateKhz = 0, bwKhz = 0;
    float col[3] = {1, 0.6f, 0.2f};
    char modeFilter[32] = "";
    std::string err;
} e;

void put(char* dst, size_t n, const std::string& s) { snprintf(dst, n, "%s", s.c_str()); }

std::string optionsText(const std::map<std::string, std::string>& o) {
    std::string t;
    for (const auto& kv : o) t += kv.first + "=" + kv.second + "\n";
    return t;
}
std::map<std::string, std::string> parseOptions(const char* text) {
    std::map<std::string, std::string> o;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!k.empty() && k.back() == ' ') k.pop_back();
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        o[k] = v;
    }
    return o;
}

// the form back into a Memory
dect2::Memory collect() {
    dect2::Memory m = e.m;
    m.name = e.name; m.group = e.group; m.tags = e.tags; m.notes = e.notes; m.radio = e.radio;
    m.freqHz = (uint64_t)std::max(0.0, e.mhz * 1e6 + 0.5);
    m.sampleRateHz = std::max(0.0, e.rateKhz * 1e3); m.bandwidthHz = std::max(0.0, e.bwKhz * 1e3);
    m.options = parseOptions(e.opts);
    m.colour = e.useCol ? ((uint32_t)(e.col[0] * 255 + 0.5f) << 16 | (uint32_t)(e.col[1] * 255 + 0.5f) << 8 | (uint32_t)(e.col[2] * 255 + 0.5f)) : 0;
    if (e.useCol && m.colour == 0) m.colour = 0x000001;   // black is a colour too: 0 means none
    return m;
}

void commit(const dect2::Memory& m, bool update) {
    dect2::MemoryBank& b = memoryBank();
    if (e.isNew) {
        const dect2::MemoryAddResult r = b.add(m, update ? dect2::DupPolicy::Update : dect2::DupPolicy::Keep);
        if (r.what == dect2::MemoryAdd::Full) { e.err = tr("The memory bank is full."); return; }
        e.stored = r.id;
    } else {
        b.update(e.id, m);
        e.stored = e.id;
    }
    memoryChanged();
}
}  // namespace

void openEdit(const dect2::Memory& m, bool isNew) {
    e.m = m; e.isNew = isNew; e.id = m.id; e.err.clear(); e.askDup = false; e.modeFilter[0] = 0;
    put(e.name, sizeof e.name, m.name); put(e.group, sizeof e.group, m.group); put(e.tags, sizeof e.tags, m.tags);
    put(e.notes, sizeof e.notes, m.notes); put(e.radio, sizeof e.radio, m.radio); put(e.opts, sizeof e.opts, optionsText(m.options));
    e.mhz = (double)m.freqHz / 1e6; e.rateKhz = m.sampleRateHz / 1e3; e.bwKhz = m.bandwidthHz / 1e3;
    e.useCol = m.colour != 0;
    if (e.useCol) { e.col[0] = ((m.colour >> 16) & 255) / 255.f; e.col[1] = ((m.colour >> 8) & 255) / 255.f; e.col[2] = (m.colour & 255) / 255.f; }
    e.request = true;
}

uint64_t editDialogs() {
    e.stored = 0;
    if (e.request) { ImGui::OpenPopup(trw("Edit memory")); e.request = false; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(trw("Edit memory"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const float w = 330 * gUi;
        auto label = [&](const char* t) { ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", t); ImGui::SameLine(140 * gUi); ImGui::SetNextItemWidth(w); };
        label(tr("Name")); ImGui::InputText("##mname", e.name, sizeof e.name);
        label(tr("Frequency (MHz)")); ImGui::InputDouble("##mfreq", &e.mhz, 0, 0, "%.6f");
        label(tr("Mode"));
        const int cur = memoryModeIndex(e.m.mode);
        if (ImGui::BeginCombo("##mmode", e.m.mode.empty() ? tr("(frequency only)") : cur >= 0 ? kModes[cur].name : e.m.mode.c_str())) {
            if (ImGui::Selectable(tr("(frequency only)"), e.m.mode.empty())) { e.m.mode.clear(); e.m.subId.clear(); e.m.family = -1; e.m.stdMode = 0; }
            const char* q = modeSearchBox();
            for (int i = 0; i < kNumModes; i++) {
                if (!modeMatches(kModes[i], q)) continue;
                if (ImGui::Selectable(kModes[i].name, i == cur)) {
                    e.m.mode = memoryModeKey(kModes[i]); e.m.family = kModes[i].family; e.m.subId = kModes[i].subId ? kModes[i].subId : "";
                    e.m.stdMode = kModes[i].family >= 6 ? kModes[i].family + 2 : 0;
                }
            }
            ImGui::EndCombo();
        }
        label(tr("Sample rate (kHz)")); ImGui::InputDouble("##mrate", &e.rateKhz, 0, 0, "%.3f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("0 keeps the mode's own rate (Auto)"));
        label(tr("Bandwidth (kHz)")); ImGui::InputDouble("##mbw", &e.bwKhz, 0, 0, "%.3f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Informative: the channel width at the time. The mode's own width options are in the options below."));
        label(tr("Radio")); ImGui::InputText("##mradio", e.radio, sizeof e.radio);
        ImGui::Checkbox(tr("Keep the gains"), &e.m.hasGain);
        if (e.m.hasGain) {
            ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); ImGui::InputInt("LNA##mlna", &e.m.lna, 0, 0);
            ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); ImGui::InputInt("VGA##mvga", &e.m.vga, 0, 0);
            ImGui::SameLine(); ImGui::Checkbox("Amp##mamp", &e.m.amp);
            ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); ImGui::InputDouble("dB##mgain", &e.m.gainDb, 0, 0, "%.1f");
        }
        label(tr("Options")); ImGui::InputTextMultiline("##mopts", e.opts, sizeof e.opts, ImVec2(w, 62 * gUi));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The mode's settings, one key=value per line (demodulator, de-emphasis, widths ...)."));
        label(tr("Group")); ImGui::InputText("##mgroup", e.group, sizeof e.group);
        {
            const std::vector<std::string> g = memoryBank().groups();
            if (!g.empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("v##mgl")) ImGui::OpenPopup("##mgpop");
                if (ImGui::BeginPopup("##mgpop")) {
                    for (const std::string& s : g) if (ImGui::Selectable(s.c_str())) put(e.group, sizeof e.group, s);
                    ImGui::EndPopup();
                }
            }
        }
        label(tr("Tags")); ImGui::InputText("##mtags", e.tags, sizeof e.tags);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Words separated by commas; the search finds them."));
        label(tr("Notes")); ImGui::InputTextMultiline("##mnotes", e.notes, sizeof e.notes, ImVec2(w, 46 * gUi));
        ImGui::Checkbox(tr("Colour"), &e.useCol);
        if (e.useCol) { ImGui::SameLine(); ImGui::ColorEdit3("##mcol", e.col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel); }
        ImGui::SameLine(0, 16 * gUi); ImGui::Checkbox(tr("Favourite"), &e.m.favourite);
        ImGui::SameLine(0, 16 * gUi); ImGui::Checkbox(tr("Include in the memory scan"), &e.m.scan);
        if (e.m.scan) { ImGui::SameLine(); ImGui::SetNextItemWidth(60 * gUi); ImGui::InputDouble(tr("dwell s"), &e.m.dwellSec, 0, 0, "%.1f"); }
        if (!e.err.empty()) ImGui::TextColored(pal::badRed(), "%s", e.err.c_str());
        ImGui::Spacing();
        if (ImGui::Button(tr("Save"), fitButton(tr("Save"), 90 * gUi))) {
            const dect2::Memory m = collect();
            if (m.freqHz == 0) e.err = tr("Enter a frequency.");
            else if (e.isNew && memoryBank().findDuplicate(m.freqHz, m.mode)) { e.dupId = memoryBank().findDuplicate(m.freqHz, m.mode)->id; e.askDup = true; }
            else commit(m, false);
        }
        ImGui::SameLine(0, 8 * gUi);
        if (ImGui::Button(tr("Cancel"), fitButton(tr("Cancel"), 90 * gUi)) || (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsPopupOpen("##mdup"))) ImGui::CloseCurrentPopup();
        if (e.askDup) { ImGui::OpenPopup("##mdup"); e.askDup = false; }
        if (ImGui::BeginPopupModal("##mdup", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
            const dect2::Memory* d = memoryBank().find(e.dupId);
            ImGui::PushTextWrapPos(360 * gUi);
            ImGui::Text(tr("\"%s\" is stored on this frequency and mode already."), d ? d->name.c_str() : "");
            ImGui::PopTextWrapPos();
            if (ImGui::Button(tr("Update it"))) { commit(collect(), true); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button(tr("Add another"))) { commit(collect(), false); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button(tr("Back"))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (e.stored) ImGui::CloseCurrentPopup();   // stored from the form or from the duplicate question
        ImGui::EndPopup();
    }
    return e.stored;
}

}  // namespace memui
