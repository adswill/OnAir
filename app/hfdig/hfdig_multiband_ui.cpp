// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF digital, "Monitor bands": pick (band, mode) pairs of FT8 / FT4 / FT2 / WSPR from 160 m to 6 m and decode every pair the radio's capture
// holds at the same time (core: hfdig_multiband.h). Shows what the capture covers, what it does not (and the span that would be needed),
// a "cover the most selected" button, saved presets, and the state of the CPU guard. The results go to the one combined decode list of
// the FT8 view (hfdig_ftx_ui.cpp), which asks hfdigMultiBandActive() / hfdigMbShown() for its extra columns and the band filter.
// Receive only.
#include "app.h"
#include "dect2/hfdig/hfdig_multiband.h"
#include "dect2/hfdig/hfdig_rx.h"
#include "dect2/hfdig/hfdig_tel.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct MbUi {
    bool loaded = false;
    bool on = false;
    uint64_t mask = 0;
    uint64_t appliedMask = ~0ull;
    bool appliedOn = false;
    int bandFilter = 0;          // 0 all bands, else band + 1
};
MbUi M;

void load() {
    if (M.loaded) return;
    M.loaded = true;
    auto& p = plat::prefs();
    M.on = p.getB("mbOn", false);
    M.mask = (uint64_t)(uint32_t)p.getI("mbMaskLo", 0) | ((uint64_t)(uint32_t)p.getI("mbMaskHi", 0) << 32);
}
void save() {
    auto& p = plat::prefs();
    p.setB("mbOn", M.on);
    p.setI("mbMaskLo", (long)(uint32_t)(M.mask & 0xffffffffu)); p.setI("mbMaskHi", (long)(uint32_t)(M.mask >> 32));
}
std::string presetKey(int i, bool hi) { return std::string("mbPre") + std::to_string(i) + (hi ? "Hi" : "Lo"); }
bool presetHas(int i) { return plat::prefs().getI(presetKey(i, false).c_str(), 0) != 0 || plat::prefs().getI(presetKey(i, true).c_str(), 0) != 0; }

bool selected(int b, int m) { return (M.mask >> (b * dect2::kMbModes + m)) & 1; }
void flip(int b, int m) { M.mask ^= 1ull << (b * dect2::kMbModes + m); }

} // namespace

bool hfdigMultiBandActive() { load(); return M.on && M.mask != 0; }
// The combined list's band filter
bool hfdigMbShown(const dect2::FtxDecode& d) {
    if (!hfdigMultiBandActive() || M.bandFilter == 0) return true;
    return dect2::mbBandOf(d.dialHz) == M.bandFilter - 1;
}
int hfdigMbBandOf(const dect2::FtxDecode& d) { return dect2::mbBandOf(d.dialHz); }

void hfdigMultiBandPanel(App& a, const dect2::HfdigTelemetry& t) {
    using namespace dect2;
    load();
    if (!ImGui::CollapsingHeader(M.on ? "Monitor bands (on)###mbhdr" : "Monitor bands###mbhdr", M.on ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        // keep what was chosen working while the panel is folded
    } else {
        bool changed = false;
        if (ImGui::Checkbox("Decode all selected at once", &M.on)) changed = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Every selected band and mode that lies inside what this radio captures is decoded at the same time (each on its own dial\n"
                              "frequency, from one slot clock). An RTL-SDR (2.4 MHz) hears one band; a wideband radio hears many.");
        ImGui::SameLine(0, 14);
        if (ImGui::SmallButton("Clear")) { M.mask = 0; changed = true; }
        ImGui::SameLine(0, 6);
        if (ImGui::SmallButton("All FT8")) { for (int b = 0; b < kMbBands; b++) M.mask |= 1ull << (b * kMbModes); changed = true; }
        ImGui::SameLine(0, 6);
        if (ImGui::SmallButton("All on 20 m")) { for (int m = 0; m < kMbModes; m++) if (mbDialHz(5, m) > 0) M.mask |= 1ull << (5 * kMbModes + m); changed = true; }

        const double rate = t.inputRate > 0 ? t.inputRate : 0;
        const double centre = t.multiBandOn && t.multiBand.centreHz > 0 ? t.multiBand.centreHz : a.freqMhz * 1e6;
        const std::vector<MbSel> sels = mbFromMask(M.mask);
        const std::vector<MbCover> cov = mbCoverage(centre, rate, sels);
        auto coverOf = [&](int b, int m) -> const MbCover* { for (const MbCover& c : cov) if (c.sel.band == b && c.sel.mode == m) return &c; return nullptr; };

        if (ImGui::BeginTable("##mbgrid", kMbBands + 1, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInner)) {
            ImGui::TableSetupColumn("");
            for (int b = 0; b < kMbBands; b++) ImGui::TableSetupColumn(mbBand(b).name);
            ImGui::TableHeadersRow();
            static const char* const kModeName[kMbModes] = {"FT8", "FT4", "FT2", "WSPR"};
            for (int m = 0; m < kMbModes; m++) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(kModeName[m]);
                for (int b = 0; b < kMbBands; b++) {
                    ImGui::TableNextColumn();
                    const double dial = mbDialHz(b, m);
                    if (dial <= 0) { ImGui::TextDisabled(" -"); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s has no standard dial frequency on %s", kModeName[m], mbBand(b).name); continue; }
                    ImGui::PushID(b * 8 + m);
                    bool v = selected(b, m);
                    if (ImGui::Checkbox("##c", &v)) { flip(b, m); changed = true; }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s on %s, dial %.4f MHz (upper sideband)", kModeName[m], mbBand(b).name, dial / 1e6);
                    const MbCover* c = v ? coverOf(b, m) : nullptr;
                    if (c && rate > 0) {
                        ImGui::SameLine(0, 2);
                        if (c->covered) ImGui::TextColored(pal::okGreen(), "o");
                        else {
                            ImGui::TextColored(pal::warnAmber(), "!");
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("Not covered by this radio: needs a retune or a wider SDR.\nThe capture would have to span %.1f MHz around %.3f MHz\n(now %.2f MHz around %.3f MHz).",
                                                  c->neededSpanHz / 1e6, centre / 1e6, rate / 1e6, centre / 1e6);
                        }
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        // what is covered, and the way to cover more
        int nCov = 0, nNo = 0;
        for (const MbCover& c : cov) { if (c.covered) nCov++; else nNo++; }
        ImGui::AlignTextToFramePadding();
        if (sels.empty()) ImGui::TextDisabled("Tick the bands and modes to monitor.");
        else if (rate <= 0) ImGui::TextDisabled("Start the radio to see what it covers.");
        else {
            ImGui::TextDisabled("Capture %.3f MHz, %.2f Msps: %d of %zu selected covered", centre / 1e6, rate / 1e6, nCov, sels.size());
            if (nNo > 0) {
                int best = 0;
                const double bc = mbBestCentre(rate, sels, &best);
                ImGui::SameLine(0, 10);
                char lb[96];
                snprintf(lb, sizeof lb, "Cover the most (%d): centre %.3f MHz###mbbest", best, bc / 1e6);
                if (best > nCov && ImGui::SmallButton(lb)) tuneFreq(a, bc / 1e6);
                const double need = mbNeededRate(sels);
                if (need > 0) { ImGui::SameLine(0, 10); ImGui::TextDisabled("all of them at once need a capture of %.1f Msps", need / 1e6); }
            }
        }
        // the CPU guard
        if (t.multiBandOn) {
            const MbStatus& s = t.multiBand;
            ImGui::TextDisabled("%d bands, %d dial frequencies decoding, load %.0f %%", s.bands, s.channels, s.load * 100);
            if (s.tooSlow) {
                std::string dropped;
                for (const MbSel& d : s.droppedSel) { if (!dropped.empty()) dropped += ", "; dropped += std::string(mbBand(d.band).name) + " " + ftxModeName(d.mode); }
                ImGui::TextColored(pal::warnAmber(), "Too slow for this computer%s%s: the quietest bands were left out. Select fewer bands or a lower sample rate.",
                                   dropped.empty() ? "" : ": dropped ", dropped.c_str());
            }
        }
        // presets
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Presets");
        for (int i = 0; i < 4; i++) {
            ImGui::SameLine(0, 8);
            ImGui::PushID(100 + i);
            char lb[16];
            snprintf(lb, sizeof lb, "%d", i + 1);
            if (ImGui::SmallButton(lb) && presetHas(i)) {
                M.mask = (uint64_t)(uint32_t)plat::prefs().getI(presetKey(i, false).c_str(), 0) | ((uint64_t)(uint32_t)plat::prefs().getI(presetKey(i, true).c_str(), 0) << 32);
                changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(presetHas(i) ? "Load preset %d (the selection saved there)" : "Preset %d is empty: select, then Save", i + 1);
            ImGui::SameLine(0, 2);
            if (ImGui::SmallButton("save")) {
                plat::prefs().setI(presetKey(i, false).c_str(), (long)(uint32_t)(M.mask & 0xffffffffu));
                plat::prefs().setI(presetKey(i, true).c_str(), (long)(uint32_t)(M.mask >> 32));
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save the current selection as preset %d", i + 1);
            ImGui::PopID();
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("List"); ImGui::SameLine(0, 6 * gUi);
        ImGui::SetNextItemWidth(90 * gUi);
        if (ImGui::BeginCombo("##mbfilter", M.bandFilter == 0 ? "all bands" : mbBand(M.bandFilter - 1).name)) {
            if (ImGui::Selectable("all bands", M.bandFilter == 0)) M.bandFilter = 0;
            for (int b = 0; b < kMbBands; b++) if (ImGui::Selectable(mbBand(b).name, M.bandFilter == b + 1)) M.bandFilter = b + 1;
            ImGui::EndCombo();
        }
        if (changed) save();
    }
    // hand the selection to the receiver when it changed
    const bool want = M.on && M.mask != 0;
    if (want != M.appliedOn || (want && M.mask != M.appliedMask)) {
        a.engine.hfdig().setMultiBand(want, mbFromMask(M.mask));
        M.appliedOn = want; M.appliedMask = M.mask;
    }
}
