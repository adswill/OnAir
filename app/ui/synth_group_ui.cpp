// The "Group radios" window (synthetic wideband, dect2/source/synth_group.h, docs/SYNTH-WIDEBAND.md): pick 2-5 of the radios found, in
// frequency order, set the centre and total span; the window shows how the span is cut (each radio's centre, the overlap, the span reached,
// the stitched rate) and USB warnings, and saves the group by the radios' serials. While a group runs it shows the alignment of every seam
// and radio (overlap mismatch, drift, lock, dropouts, delivered samples) with a Recalibrate button and a switch per radio (hot remove/add).
#include "../app.h"
#include "dect2/source/diversity_source.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

bool groupable(const DeviceInfo& d) { return d.isRadio() && d.kind != DeviceInfo::Group && d.kind != DeviceInfo::Hamlib; }

void loadIntoEditor(App& a, int gi) {
    a.groupEdit = gi;
    a.groupPick.clear();
    if (gi < 0 || gi >= (int)a.synthGroups.size() || a.synthGroups[(size_t)gi].diversity) {   // a diversity pair has no editor: make it again
        a.groupEdit = -1;
        a.groupName[0] = 0;
        a.groupCenterMhz = a.freqMhz; a.groupSpanMhz = 8.0; a.groupRateMsps = 0;
        return;
    }
    const SynthGroupDef& g = a.synthGroups[(size_t)gi];
    snprintf(a.groupName, sizeof a.groupName, "%s", g.name.c_str());
    a.groupCenterMhz = g.centerHz / 1e6; a.groupSpanMhz = g.spanHz / 1e6; a.groupRateMsps = g.childRateHz / 1e6;
    for (const auto& m : g.members)
        for (int i = 0; i < (int)a.devices.size(); i++)
            if (groupable(a.devices[(size_t)i]) && memberMatches(m, a.devices[(size_t)i])) { a.groupPick.push_back(i); break; }
}

// the live status of a running group, with Recalibrate and the per-radio switches
void runningStatus(App& a, const std::string& name) {
    SynthGroupStatus st;
    if (!synthGroupStatus(name, st)) return;
    ImGui::SeparatorText(tr("Running"));
    ImGui::Text(tr("%.3f Msps stitched, span %.3f MHz, overlap %.0f kHz, each radio at %.3f Msps"), st.outRateHz / 1e6, st.spanHz / 1e6, st.overlapHz / 1e3, st.childRateHz / 1e6);
    if (ImGui::BeginTable("##grpst", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(tr("On"));
        ImGui::TableSetupColumn(tr("Radio"));
        ImGui::TableSetupColumn(tr("Tuned MHz"));
        ImGui::TableSetupColumn(tr("Locked"));
        ImGui::TableSetupColumn(tr("Offset Hz"));
        ImGui::TableSetupColumn(tr("Drift ppm"));
        ImGui::TableSetupColumn(tr("Delivered"));
        ImGui::TableSetupColumn(tr("Dropouts"));
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)st.members.size(); i++) {
            const SynthMemberStatus& m = st.members[(size_t)i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool on = m.enabled;
            ImGui::PushID(i);
            if (ImGui::Checkbox("##on", &on)) synthGroupSetMember(name, i, on);
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(m.name.c_str());
            if (!m.error.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.error.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.4f", m.centerHz / 1e6);
            ImGui::TableNextColumn();
            if (!m.alive) ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1), "%s", m.enabled ? tr("down") : tr("off"));
            else ImGui::TextColored(m.locked ? ImVec4(0.4f, 0.85f, 0.45f, 1) : ImVec4(0.95f, 0.78f, 0.25f, 1), "%s", m.locked ? tr("yes") : tr("aligning"));
            ImGui::TableNextColumn(); ImGui::Text("%.0f", m.offsetHz);
            ImGui::TableNextColumn(); ImGui::Text("%.2f", m.driftPpm);
            ImGui::TableNextColumn();
            if (m.deliveredPct > 0 && m.deliveredPct < 99.0) ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.25f, 1), "%.1f %%", m.deliveredPct);
            else ImGui::Text("%.1f %%", m.deliveredPct);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("Samples received against the radio's rate. Below 100 %% the USB link (or the hub) loses samples. Thrown away by OnAir: %llu"), (unsigned long long)m.queueDropped);
            ImGui::TableNextColumn(); ImGui::Text("%lld", m.dropouts);
        }
        ImGui::EndTable();
    }
    if (ImGui::BeginTable("##grpseam", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(tr("Seam"));
        ImGui::TableSetupColumn(tr("Mismatch dB"));
        ImGui::TableSetupColumn(tr("Gain dB"));
        ImGui::TableSetupColumn(tr("Coherence"));
        ImGui::TableSetupColumn(tr("Phase track"));
        ImGui::TableHeadersRow();
        for (int p = 0; p < (int)st.seams.size(); p++) {
            const synthwide::PairStatus& s = st.seams[(size_t)p];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d-%d", p + 1, p + 2);
            ImGui::TableNextColumn();
            if (!s.valid) ImGui::TextDisabled("-");
            else ImGui::TextColored(s.mismatchDb < -20 ? ImVec4(0.4f, 0.85f, 0.45f, 1) : s.mismatchDb < -10 ? ImVec4(0.95f, 0.78f, 0.25f, 1) : ImVec4(1.f, 0.45f, 0.4f, 1), "%.1f", s.mismatchDb);
            ImGui::TableNextColumn(); if (s.valid) ImGui::Text("%.2f", s.gainDb); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (s.valid) ImGui::Text("%.2f", s.confidence); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (s.tracking) ImGui::Text(tr("%.1f deg/hop"), s.trackRmsDeg); else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("%s", tr("Mismatch: what is left of the two radios' difference in their overlap after the correction (-30 dB good, above -10 dB poor). "
                                 "It needs a signal both radios hear in the overlap: with only noise there it stays poor, and that is fine."));
    if (ImGui::Button(tr("Recalibrate"))) synthGroupRecalibrate(name);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Measure frequency, delay, drift, gain and phase of every radio again from nothing (after moving a radio or a cable, or when a seam stays poor)."));
    if (!st.usbWarning.empty()) { ImGui::PushTextWrapPos(0.0f); ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.25f, 1), "%s", st.usbWarning.c_str()); ImGui::PopTextWrapPos(); }
}

// the live state of a running diversity pair: what it is doing, how much it gains, the mode, a re-measure button
void diversityRunning(App& a, const std::string& name) {
    DiversitySourceStatus st;
    if (!diversityStatus(name, st)) return;
    ImGui::SeparatorText(tr("Diversity pair running"));
    const ImVec4 good(0.4f, 0.85f, 0.45f, 1), warn(0.95f, 0.78f, 0.25f, 1);
    ImGui::TextColored(st.combining ? good : warn, "%s", st.state.c_str());
    for (const auto& m : st.members) {
        ImGui::BulletText("%s: %s, %.1f %% of the samples", m.name.c_str(), m.running ? tr("delivering") : tr("not delivering"), m.deliveredPct);
        if (!m.error.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.error.c_str());
    }
    if (st.combining) {
        ImGui::Text(tr("Signal-to-noise in the signal's band: radio 1 %.1f dB, radio 2 %.1f dB, combined about %.1f dB"), st.dsp.snrADb, st.dsp.snrBDb, st.dsp.outSnrDb);
        ImGui::TextDisabled(tr("Frequency offset between the radios %.0f Hz, delay %d samples, level of radio 2 against radio 1 %.1f dB"), st.dsp.cfoHz, st.dsp.lagSamples, st.dsp.gainBDb);
    }
    int mode = st.mode;
    ImGui::SetNextItemWidth(220 * gUi);
    const char* modes[] = {tr("Selection (the better radio)"), tr("Maximal-ratio (adds both)")};
    if (ImGui::Combo(tr("Combining"), &mode, modes, 2)) {
        diversitySetMode(name, mode);
        for (auto& g : a.synthGroups) if (g.name == name && g.diversity) { g.diversityMode = mode; savePrefs(a); }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Maximal-ratio adds the two signals in phase so the noise partly cancels: up to 3 dB better with two equal radios. Selection simply uses the better radio and is the safe choice when the two do not hear the same signal."));
    ImGui::SameLine();
    if (ImGui::Button(tr("Measure again"))) diversityRealign(name);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Find the delay and frequency offset between the two radios again (after moving an antenna or a cable)."));
    if (!st.usbWarning.empty()) { ImGui::PushTextWrapPos(0.0f); ImGui::TextColored(warn, "%s", st.usbWarning.c_str()); ImGui::PopTextWrapPos(); }
}

} // namespace

void synthGroupWindow(App& a) {
    static bool wasOpen = false;
    if (!a.groupOpen) { wasOpen = false; return; }
    if (!wasOpen) loadIntoEditor(a, a.groupEdit);
    wasOpen = true;
    ImGui::SetNextWindowSize(ImVec2(560 * gUi, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin(trw("Group radios"), &a.groupOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    ImGui::PushTextWrapPos(540 * gUi);
    ImGui::TextDisabled("%s", tr("Several narrow radios (for example 5 RTL-SDRs) tuned side by side with some overlap become one wide radio in the list. "
                                 "Fine for the spectrum, waterfall, FM/AM, narrow voice and scanning; wide digital TV across the seams is experimental. See docs/SYNTH-WIDEBAND.md."));
    ImGui::PopTextWrapPos();

    // diversity pair: two radios on one frequency, combined for weak narrowband signals (one click)
    ImGui::SeparatorText(tr("Diversity pair (weak signals)"));
    ImGui::PushTextWrapPos(540 * gUi);
    ImGui::TextDisabled("%s", tr("Two radios tuned to the same frequency (two antennas, or the same antenna through a splitter) are combined into one better signal for narrowband modes "
                                 "such as FM, AM, voice and paging. Off until you make a pair. Needs a signal both radios hear; see docs/DIVERSITY.md."));
    ImGui::PopTextWrapPos();
    {
        std::vector<int> cand;
        for (int i = 0; i < (int)a.devices.size(); i++) if (groupable(a.devices[(size_t)i])) cand.push_back(i);
        // two radios of the same kind first (two RTL-SDRs), else the first two
        int ia = -1, ib = -1;
        for (size_t x = 0; x < cand.size() && ia < 0; x++)
            for (size_t y = x + 1; y < cand.size(); y++)
                if (a.devices[(size_t)cand[x]].board == a.devices[(size_t)cand[y]].board) { ia = cand[x]; ib = cand[y]; break; }
        if (ia < 0 && cand.size() >= 2) { ia = cand[0]; ib = cand[1]; }
        ImGui::BeginDisabled(ia < 0);
        if (ImGui::Button(tr("Make a diversity pair from two radios"))) {
            SynthGroupDef g;
            g.diversity = true; g.diversityMode = 1;
            g.members = {memberOf(a.devices[(size_t)ia]), memberOf(a.devices[(size_t)ib])};
            std::string base = "Pair";
            for (int n = 1;; n++) {
                g.name = base + " " + std::to_string(n);
                bool clash = false;
                for (const auto& o : a.synthGroups) if (o.name == g.name) clash = true;
                if (!clash) break;
            }
            a.synthGroups.push_back(g);
            savePrefs(a);
            a.devicesDirty = true;   // "Diversity: <name>" shows up in the radio list
        }
        ImGui::EndDisabled();
        if (ia >= 0) { ImGui::SameLine(); ImGui::TextDisabled("%s + %s", a.devices[(size_t)ia].name.c_str(), a.devices[(size_t)ib].name.c_str()); }
        else { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("needs two radios")); }
    }

    // the saved groups
    if (!a.synthGroups.empty()) {
        ImGui::SeparatorText(tr("Saved groups"));
        for (int gi = 0; gi < (int)a.synthGroups.size(); gi++) {
            ImGui::PushID(gi);
            const SynthGroupDef& g = a.synthGroups[(size_t)gi];
            if (g.diversity) ImGui::Text("%s  (%s)", g.name.c_str(), tr("diversity pair"));
            else ImGui::Text("%s  (%zu radios, %.3f MHz span)", g.name.c_str(), g.members.size(), g.spanHz / 1e6);
            ImGui::SameLine();
            if (!g.diversity) { if (ImGui::SmallButton(tr("Edit"))) loadIntoEditor(a, gi); ImGui::SameLine(); }
            const bool live = [&] { SynthGroupStatus s; DiversitySourceStatus d; return g.diversity ? diversityStatus(g.name, d) : synthGroupStatus(g.name, s); }();
            ImGui::BeginDisabled(live);
            if (ImGui::SmallButton(tr("Delete"))) {
                a.synthGroups.erase(a.synthGroups.begin() + gi);
                if (a.groupEdit == gi) loadIntoEditor(a, -1); else if (a.groupEdit > gi) a.groupEdit--;
                savePrefs(a); a.devicesDirty = true;
                ImGui::PopID(); ImGui::EndDisabled();
                break;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        if (ImGui::SmallButton(tr("New group"))) loadIntoEditor(a, -1);
    }

    // the editor
    ImGui::SeparatorText(a.groupEdit >= 0 ? tr("Edit group") : tr("New group"));
    ImGui::SetNextItemWidth(220 * gUi);
    ImGui::InputTextWithHint(tr("Name"), tr("e.g. 5x RTL-SDR"), a.groupName, sizeof a.groupName);
    ImGui::TextUnformatted(tr("Radios, lowest slice first (2 to 5):"));
    // picked radios in order, with up/down/remove
    for (int k = 0; k < (int)a.groupPick.size(); k++) {
        const int di = a.groupPick[(size_t)k];
        if (di < 0 || di >= (int)a.devices.size()) { a.groupPick.erase(a.groupPick.begin() + k); break; }
        ImGui::PushID(1000 + k);
        ImGui::Text("%d. %s", k + 1, a.devices[(size_t)di].name.c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(k == 0);
        if (ImGui::SmallButton("^")) std::swap(a.groupPick[(size_t)k], a.groupPick[(size_t)k - 1]);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(k + 1 >= (int)a.groupPick.size());
        if (ImGui::SmallButton("v")) std::swap(a.groupPick[(size_t)k], a.groupPick[(size_t)k + 1]);
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool rm = ImGui::SmallButton("x");
        ImGui::PopID();
        if (rm) { a.groupPick.erase(a.groupPick.begin() + k); break; }
    }
    // the radios found that are not picked yet
    {
        std::vector<int> free;
        for (int i = 0; i < (int)a.devices.size(); i++)
            if (groupable(a.devices[(size_t)i]) && std::find(a.groupPick.begin(), a.groupPick.end(), i) == a.groupPick.end()) free.push_back(i);
        ImGui::BeginDisabled(free.empty() || (int)a.groupPick.size() >= synthwide::kSynthMaxSlices);
        ImGui::SetNextItemWidth(320 * gUi);
        if (ImGui::BeginCombo("##grpadd", free.empty() ? tr("(no other radio found)") : tr("Add a radio…"))) {
            for (int i : free) if (ImGui::Selectable(a.devices[(size_t)i].name.c_str())) a.groupPick.push_back(i);
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
    }
    ImGui::SetNextItemWidth(140 * gUi);
    ImGui::InputDouble(tr("Centre MHz"), &a.groupCenterMhz, 0.1, 1.0, "%.4f");
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Use the receiver's"))) a.groupCenterMhz = a.freqMhz;
    ImGui::SetNextItemWidth(140 * gUi);
    ImGui::InputDouble(tr("Total span MHz"), &a.groupSpanMhz, 0.5, 1.0, "%.3f");
    a.groupSpanMhz = std::max(0.1, std::min(200.0, a.groupSpanMhz));
    ImGui::SetNextItemWidth(140 * gUi);
    ImGui::InputDouble(tr("Rate per radio Msps"), &a.groupRateMsps, 0.0, 0.0, "%.3f");
    a.groupRateMsps = std::max(0.0, std::min(100.0, a.groupRateMsps));
    ImGui::SameLine(); ImGui::TextDisabled("%s", tr("0 = automatic (2.4 for RTL-SDRs)"));

    // the plan
    std::vector<DeviceInfo> mem;
    for (int di : a.groupPick) mem.push_back(a.devices[(size_t)di]);
    const double fs = groupChildRate(mem, a.groupRateMsps * 1e6);
    const SlicePlan plan = planSlices((int)mem.size(), fs, a.groupSpanMhz * 1e6);
    if (mem.size() >= 2) {
        if (plan.ok) {
            ImGui::Text(tr("Span reached %.3f MHz (at most %.3f), overlap %.0f kHz, stitched rate %.3f Msps, each radio at %.3f Msps"),
                        plan.spanHz / 1e6, plan.maxSpanHz / 1e6, plan.overlapHz / 1e3, plan.outRateHz / 1e6, fs / 1e6);
            if (!plan.why.empty()) ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.25f, 1), "%s", plan.why.c_str());
            for (size_t i = 0; i < mem.size(); i++)
                ImGui::BulletText("%s: %.4f MHz", mem[i].name.c_str(), a.groupCenterMhz + plan.sliceOffsetHz[i] / 1e6);
        } else {
            ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1), "%s", plan.why.c_str());
        }
        const std::string w = usbLoadWarning(mem, fs);
        if (!w.empty()) { ImGui::PushTextWrapPos(540 * gUi); ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.25f, 1), "%s", w.c_str()); ImGui::PopTextWrapPos(); }
    } else {
        ImGui::TextDisabled("%s", tr("Pick at least 2 radios."));
    }

    // save
    const std::string nm = a.groupName;
    bool clash = false;
    for (int gi = 0; gi < (int)a.synthGroups.size(); gi++) if (gi != a.groupEdit && a.synthGroups[(size_t)gi].name == nm) clash = true;
    const bool editingLive = [&] { SynthGroupStatus s; return a.groupEdit >= 0 && a.groupEdit < (int)a.synthGroups.size() && synthGroupStatus(a.synthGroups[(size_t)a.groupEdit].name, s); }();
    ImGui::BeginDisabled(nm.empty() || clash || !plan.ok || mem.size() < 2 || editingLive);
    if (ImGui::Button(a.groupEdit >= 0 ? tr("Save changes") : tr("Save group"))) {
        SynthGroupDef g;
        g.name = nm; g.centerHz = a.groupCenterMhz * 1e6; g.spanHz = a.groupSpanMhz * 1e6; g.childRateHz = a.groupRateMsps * 1e6;
        for (const auto& d : mem) g.members.push_back(memberOf(d));
        if (a.groupEdit >= 0 && a.groupEdit < (int)a.synthGroups.size()) a.synthGroups[(size_t)a.groupEdit] = g;
        else { a.synthGroups.push_back(g); a.groupEdit = (int)a.synthGroups.size() - 1; }
        savePrefs(a);
        a.devicesDirty = true;   // the group shows up in the radio list
    }
    ImGui::EndDisabled();
    if (clash) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1), "%s", tr("a group with this name exists")); }
    if (editingLive) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("stop the receiver to change a running group")); }
    ImGui::SameLine();
    ImGui::BeginDisabled(mem.empty() || !plan.ok);
    if (ImGui::Button(tr("Tune the receiver to the centre"))) a.freqMhz = a.groupCenterMhz;
    ImGui::EndDisabled();

    // the running group (the one chosen in the radio list)
    if (a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[(size_t)a.devIdx].kind == DeviceInfo::Group) {
        runningStatus(a, a.devices[(size_t)a.devIdx].nativeArgs);
        diversityRunning(a, a.devices[(size_t)a.devIdx].nativeArgs);
    }

    ImGui::End();
    if (!a.groupOpen) wasOpen = false;
}
