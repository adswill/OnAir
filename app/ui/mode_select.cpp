// Choosing a receiver mode: selectMode(), the highlight and search tests on a kModes row, the search box of the
// mode drop-downs and the segmented mode selector of the top bar. The table itself is in mode_table.cpp.
// Search matches the name, the sub line, the blurb and the extra search words of a kModes row.
#include "../app.h"
#include "../state/presets_app.h"
#include "dect2/engine/rate_choice.h"
#include "dect2/source/antenna_ports.h"
#include "dect2/hdr/hdr_rx.h"
#include "dect2/source/hamlib_link.h"
#include "dect2/source/iq_file.h"
#include <algorithm>
#include <cctype>
#include <memory>
#include <thread>
#include "toolbar_internal.h"

float gSwitchWidth = 420;   // width of the mode selector as drawn (the guided tour points at it)

void selectMode(App& a, int fam, const char* subId) {
    if (txViewFlag()) { txSelectMode(a, fam, subId); return; }
    if (int k = 0; autodetRow(subId, &k)) { autodetSelect(a, k); return; }   // "TV (auto)", "Digital voice (auto)"
    autodetCancel(a);   // any other row: the automatic mode ends
    modeRecordRecent(a, fam, subId);   // the Mode Browser's Recent tab
    const int sub = subIndexOf(subId);
    if (fam == a.family && (sub == a.sub || !sub)) return;
    // Switch in one action: stop the old decoder before updating the mode, then
    // restart using the new mode's own frequency, sample rate and bandwidth.
    const bool wasRunning = a.engine.running();
    if (wasRunning && a.engine.recordingStats().active) { a.engine.log("stop the recording before switching mode"); return; }   // stopping the radio would end it
    if (wasRunning) a.engine.stop();
    setMode(a, fam, sub);
    presetsapp::applyModeDefault(a);   // the mode's default preset, when the user set one
    savePrefs(a);
    if (wasRunning) {
        a.engine.log("switching receiver mode");
        startReceiver(a);
    }
}
bool modeSelected(const App& a, const ModeDef& m) {
    if (txViewFlag()) return txModeHighlighted(m);
    if (int k = 0; autodetRow(m.subId, &k)) return autodetActive(a, k);
    return m.family == a.family && (!m.subId || subIndexOf(m.subId) == a.sub);
}
bool modeMatches(const ModeDef& m, const char* q) {
    if (!q || !*q) return true;
    auto has = [&](const char* s) {
        if (!s) return false;
        std::string h(s), n(q);
        for (auto& c : h) c = (char)tolower((unsigned char)c);
        for (auto& c : n) c = (char)tolower((unsigned char)c);
        return h.find(n) != std::string::npos;
    };
    return has(m.name) || has(m.sub) || has(tr(m.sub)) || has(m.blurb) || has(m.keys);
}
// a search box at the top of a mode drop-down; returns the query
const char* modeSearchBox() {
    static char q[32] = "";
    if (ImGui::IsWindowAppearing()) { q[0] = 0; ImGui::SetKeyboardFocusHere(); }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##modeq", tr("search (FT8, LoRa, APRS...)"), q, sizeof q);
    return q;
}

// Mode selector: the modes in their groups (kGroupNames) as one segmented control each; a drop-down when the window is too narrow for them all
void standardSwitch(App& a) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = ImGui::GetFrameHeight() - 2 * gUi;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ModeDef* cur = &kModes[0];
    for (const auto& m : allModes()) if (modeSelected(a, m)) cur = &m;

    std::vector<float> segW((size_t)kNumModes), segX((size_t)kNumModes);   // one per mode, however many there are
    float capX[kNumGroups] = {}, grpL[kNumGroups] = {}, grpR[kNumGroups] = {}, capW[kNumGroups];
    for (int g = 0; g < kNumGroups; g++) capW[g] = ImGui::CalcTextSize(tr(kGroupNames[g])).x;
    float x = p.x;
    for (int g = 0; g < kNumGroups; g++) {
        if (!modeGroupListed(a, g)) { grpL[g] = grpR[g] = capX[g] = x; capW[g] = 0; continue; }   // Modes shown: a hidden group takes no room
        capX[g] = x; x += capW[g] + 8 * gUi;
        grpL[g] = x;
        for (int i = 0; i < kNumModes; i++) if (kModes[i].group == g && modeListed(a, kModes[i])) { segW[i] = ImGui::CalcTextSize(kModes[i].name).x + 22 * gUi; segX[i] = x; x += segW[i]; }
        grpR[g] = x;
        x += 18 * gUi;
    }
    const float total = x - 18 * gUi - p.x;
    const float room = ImGui::GetContentRegionAvail().x - 150 * gUi;   // keep the right-hand buttons clear
    if (total > room || getenv("DECT2_NARROW")) {   // too narrow: one drop-down with the same groups
        gSwitchWidth = 170 * gUi;
        ImGui::SetNextItemWidth(gSwitchWidth);
        if (ImGui::BeginCombo("##mode", cur->name)) {
            const char* q = modeSearchBox();
            for (int g = 0; g < kNumGroups; g++) {
                if (!modeGroupListed(a, g, q)) continue;
                ImGui::Separator();
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kGroupCols[g]), "%s", tr(kGroupNames[g]));
                for (const auto& m : allModes()) if (m.group == g && modeListed(a, m, q)) {
                    ImGui::PushID(&m);
                    if (ImGui::Selectable(modeHiddenNow(a, m) ? (std::string(m.name) + " (" + tr("hidden") + ")").c_str() : m.name, modeSelected(a, m))) selectMode(a, m.family, m.subId);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s", m.tip, modeCreditsLine(m.name).c_str());
                    ImGui::PopID();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, 14 * gUi);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", cur->blurb);
        return;
    }
    gSwitchWidth = total;
    ImGui::AlignTextToFramePadding();
    for (int g = 0; g < kNumGroups; g++) {
        if (!modeGroupListed(a, g)) continue;
        const ImVec2 ts = ImGui::CalcTextSize(tr(kGroupNames[g]));
        dl->AddText(ImVec2(capX[g], p.y + (h - ts.y) * 0.5f), (kGroupCols[g] & 0x00FFFFFF) | 0xB0000000, tr(kGroupNames[g]));
        dl->AddRectFilled(ImVec2(grpL[g], p.y), ImVec2(grpR[g], p.y + h), IM_COL32(18, 22, 28, 255), 3.f);
        dl->AddRect(ImVec2(grpL[g], p.y), ImVec2(grpR[g], p.y + h), IM_COL32(52, 60, 72, 255), 3.f);
    }
    int selPos = 0;
    for (int i = 0; i < kNumModes; i++) if (modeSelected(a, kModes[i])) selPos = i;
    static float knobX = -1, knobW = 0;
    if (knobX < 0) { knobX = segX[selPos]; knobW = segW[selPos]; }
    knobX += (segX[selPos] - knobX) * 0.35f; knobW += (segW[selPos] - knobW) * 0.35f;
    dl->AddRectFilled(ImVec2(knobX + 2 * gUi, p.y + 2 * gUi), ImVec2(knobX + knobW - 2 * gUi, p.y + h - 2 * gUi), kModes[selPos].col, 3.f * gUi);
    for (int i = 0; i < kNumModes; i++) {
        if (!modeListed(a, kModes[i])) continue;
        ImGui::SetCursorScreenPos(ImVec2(segX[i], p.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", ImVec2(segW[i], h))) selectMode(a, kModes[i].family, kModes[i].subId);
        const bool hov = ImGui::IsItemHovered();
        if (hov) ImGui::SetTooltip("%s%s", kModes[i].tip, modeCreditsLine(kModes[i].name).c_str());
        ImGui::PopID();
        const ImVec2 ts = ImGui::CalcTextSize(kModes[i].name);
        dl->AddText(ImVec2(segX[i] + (segW[i] - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), modeSelected(a, kModes[i]) ? IM_COL32(255, 255, 255, 255) : hov ? IM_COL32(220, 228, 236, 255) : IM_COL32(140, 152, 166, 255), kModes[i].name);
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + total + 14 * gUi, p.y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", cur->blurb);
    // the cursor to the line below: a Dummy after the move, as a bare SetCursorScreenPos at the end of a pane does not extend it and ImGui asserts (a Debug build, #104)
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    ImGui::Dummy(ImVec2(0, 0));
}
