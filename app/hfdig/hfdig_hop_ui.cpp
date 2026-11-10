// Band hopping for FT8 / FT4 / FT2 / WSPR on one narrow radio: the plan (bands per UTC hour, coordinated or in turn) and the retune at each
// 2-minute slot. Core: dect2/hfdig/hfdig_hop.h; the dial frequencies are the multiband ones (WSJT-X defaults).
#include "app.h"
#include "dect2/hfdig/hfdig_hop.h"
#include "dect2/hfdig/hfdig_multiband.h"
#include "i18n/i18n.h"
#include <cmath>
#include <cstdio>
#include <ctime>

void hfdigHopTick(App& a);
void hfdigHopPanel(App& a, int mode);

namespace {
struct Hop {
    bool loaded = false, on = false;
    dect2::HopPlan plan;
    int mode = 3;            // 0 FT8, 1 FT4, 2 FT2, 3 WSPR: whose dial frequencies
    int lastBand = -2;
    int64_t lastSlot = 0;
    std::string note;
};
Hop H;
void load() {
    if (H.loaded) return;
    H.loaded = true;
    plat::Prefs& p = plat::prefs();
    H.on = p.getB("hfdigHopOn", false);
    H.plan = p.has("hfdigHopPlan") ? dect2::hopFromText(p.getS("hfdigHopPlan", "")) : dect2::HopPlan::allHours(0x3FF);
    H.mode = (int)p.getI("hfdigHopMode", 3);
}
void save() { plat::prefs().setS("hfdigHopPlan", dect2::hopToText(H.plan)); plat::prefs().setB("hfdigHopOn", H.on); plat::prefs().setI("hfdigHopMode", H.mode); }
}  // namespace

void hfdigHopTick(App& a) {
    load();
    if (!H.on || !a.engine.running() || a.rx.standard != 26) return;
    const int64_t now = (int64_t)time(nullptr);
    const int64_t slot = now / H.plan.slotSec;
    if (slot == H.lastSlot) return;
    H.lastSlot = slot;
    const int b = dect2::hopBand(H.plan, now);
    if (b < 0) { H.note = tr("no band on this hour"); return; }
    const double hz = dect2::mbDialHz(b, H.mode);
    if (hz <= 0) { H.note = std::string(dect2::mbBand(b).name) + tr(": no dial frequency for this mode"); return; }
    if (std::fabs(a.freqMhz * 1e6 - hz) > 1) {
        a.freqMhz = hz / 1e6;
        a.tune.centerHz = a.freqMhz * 1e6;
        a.engine.retuneReset(a.tune);
        a.peak.clear();
    }
    H.lastBand = b;
    H.note = std::string(tr("now ")) + dect2::mbBand(b).name;
}

void hfdigHopPanel(App& a, int mode) {
    load();
    (void)a;
    if (mode >= 0 && mode <= 3 && mode != H.mode) { H.mode = mode; save(); }
    if (!ImGui::CollapsingHeader(tr("Band hopping##hfhop"))) return;
    if (ImGui::Checkbox(tr("Hop bands every 2 minutes"), &H.on)) { save(); H.lastSlot = 0; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("For one narrow radio: at the start of each 2-minute slot the dial moves to the next band switched on for this UTC hour. The radio follows when it is under OnAir's control."));
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Coordinated (the same band as other stations)"), &H.plan.coordinated)) save();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The 20-minute cycle 160, 80, 60, 40, 30, 20, 17, 15, 12, 10 m, one band per 2-minute slot from the hour, as WSJT-X's coordinated hopping. A band you switched off gives its slot to the next one."));
    if (!H.note.empty() && H.on) { ImGui::SameLine(); ImGui::TextDisabled("%s, %s %s", H.note.c_str(), tr("mode"), H.mode == 3 ? "WSPR" : H.mode == 0 ? "FT8" : H.mode == 1 ? "FT4" : "FT2"); }
    // the plan: one row per UTC hour, one column per band
    const int nb = dect2::kMbBands;
    if (!ImGui::BeginTable("##hophrs", nb + 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, 220 * gUi))) return;
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("UTC");
    for (int b = 0; b < nb; b++) ImGui::TableSetupColumn(dect2::mbBand(b).name);
    ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();
    const int cur = (int)((time(nullptr) % 86400) / 3600);
    bool ch = false;
    for (int h = 0; h < 24; h++) {
        ImGui::TableNextRow();
        if (h == cur) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(50, 70, 50, 255));
        ImGui::TableNextColumn(); ImGui::Text("%02d", h);
        for (int b = 0; b < nb; b++) {
            ImGui::TableNextColumn();
            bool on = H.plan.hours[(size_t)h] >> b & 1;
            ImGui::PushID(h * 32 + b);
            if (ImGui::Checkbox("##b", &on)) { H.plan.hours[(size_t)h] = (uint16_t)(on ? H.plan.hours[(size_t)h] | (1 << b) : H.plan.hours[(size_t)h] & ~(1 << b)); ch = true; }
            ImGui::PopID();
        }
        ImGui::TableNextColumn();
        ImGui::PushID(h);
        if (ImGui::SmallButton(tr("all"))) { H.plan.hours[(size_t)h] = 0x3FF; ch = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("copy down")) && h < 23) { for (int k = h + 1; k < 24; k++) H.plan.hours[(size_t)k] = H.plan.hours[(size_t)h]; ch = true; }
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (ch) { save(); H.lastSlot = 0; }
}
