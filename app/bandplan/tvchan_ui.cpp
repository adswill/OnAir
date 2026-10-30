// The Channel selector of every TV mode (DVB-T/T2, ATSC 1.0 / 3.0, ISDB-T, DTMB, DVB-C, Analog TV): previous / next buttons and a list of the numbered
// channels of a plan (core/include/dect2/channels/tvplan.h, docs/TV-CHANNELS.md). The plan is the country's by default (the setup wizard stores the
// country), and can be changed in the list header; picking a channel tunes its centre and sets the plan's channel width where the mode has one.
// The frequency box stays free: the selector only reads it, to show the channel name when the frequency is on a channel of the plan.
#include "../app.h"
#include "../platform/platform.h"
#include "../ui/ui_common.h"
#include "dect2/channels/tvplan.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

namespace tp = dect2::tvplan;

namespace {

// the standards of the mode on screen
unsigned stdMaskOf(const App& a) {
    if (a.family == 7) return tp::Dtmb;
    if (a.family == 8) return tp::Analog;
    if (a.family == 468) return tp::DvbC;
    if (a.isdbtMode) return tp::IsdbT;
    if (a.atsc3Mode) return tp::Atsc3 | tp::Atsc;   // the same channels as ATSC 1.0
    if (a.atscMode) return tp::Atsc | tp::Atsc3;
    return tp::DvbAny;
}
const char* groupOf(unsigned m) {
    if (m & tp::Dtmb) return "dtmb";
    if (m & tp::Analog) return "analog";
    if (m & tp::DvbC) return "dvbc";
    if (m & tp::IsdbT) return "isdbt";
    if (m & tp::Atsc) return "atsc";
    return "dvb";
}

std::map<std::string, std::string>& chosenCache() { static std::map<std::string, std::string> c; return c; }
std::string& chosen(const char* group) {   // the plan the user picked for a group of modes ("" = from the country), loaded once
    auto& cache = chosenCache();
    auto it = cache.find(group);
    if (it == cache.end()) it = cache.emplace(group, plat::prefs().getS((std::string("tvPlan.") + group).c_str(), "")).first;
    return it->second;
}
std::string& countryCache() { static std::string c; return c; }
bool& countryLoaded() { static bool l = false; return l; }
const std::string& country() {
    if (!countryLoaded()) { countryCache() = plat::prefs().getS("locCountry", ""); countryLoaded() = true; }
    return countryCache();
}
const tp::Plan* planFor(unsigned m) {
    const std::string& id = chosen(groupOf(m));
    if (!id.empty()) if (const tp::Plan* p = tp::find(id)) if (p->stds & m) return p;
    return tp::defaultPlan(country(), m);
}

int gLegacy = -1;   // "show reallocated channels", loaded once
bool showLegacy() {
    if (gLegacy < 0) gLegacy = plat::prefs().getB("tvShowLegacy", false) ? 1 : 0;
    return gLegacy == 1;
}
void setShowLegacy(bool on) { gLegacy = on ? 1 : 0; plat::prefs().setB("tvShowLegacy", on); }

// tune the channel: the centre frequency and the plan's width
void pick(App& a, const tp::Channel& c, unsigned m) {
    a.freqMhz = c.centreHz / 1e6;
    const double bw = c.bwHz / 1e6;
    if (m & tp::DvbAny) {
        if (!a.atscMode && !a.isdbtMode && !a.atsc3Mode)
            for (int i = 0; i < (int)(sizeof kBw / sizeof *kBw); i++)
                if (std::fabs(kBw[i].mhz - bw) < 0.01) { setDvbBandwidth(a, i); break; }
    } else if (m & tp::IsdbT) {
        if (bw == 6 || bw == 7 || bw == 8) setIsdbtBandwidth(a, (int)bw);
    } else if (m & tp::Dtmb) {
        if (bw == 6 || bw == 8) setDtmbBandwidth(a, (int)bw);
    }
}

} // namespace

bool tvChannelCombo(App& a) {
    const unsigned m = stdMaskOf(a);
    const tp::Plan* plan = planFor(m);
    if (!plan) return false;
    const double hz = a.freqMhz * 1e6;
    const tp::Channel* cur = plan->at(hz);
    bool changed = false;

    ImGui::PushID("tvch");
    // previous / next channel along the plan
    auto stepBy = [&](int dir) {
        const tp::Channel* n = cur;
        const double from = hz;
        for (int guard = 0; guard < 400; guard++) {
            n = plan->step(n, dir, from);
            if (!n || showLegacy() || !n->legacy) break;
        }
        if (n) { pick(a, *n, m); changed = true; }
    };
    if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) stepBy(-1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Previous channel of the plan"));
    ImGui::SameLine(0, 2 * gUi);

    char lbl[64];
    std::snprintf(lbl, sizeof lbl, "%s", cur ? cur->label.c_str() : tr("Channel"));
    ImGui::SetNextItemWidth(std::min(86 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##list", lbl)) {
        // the plan: a country or region; its name is the header
        char hdr[200];
        std::snprintf(hdr, sizeof hdr, "%s: %s###plan", tr("Region"), plan->name.c_str());
        if (ImGui::TreeNode(hdr)) {
            for (const tp::Plan* p : tp::plansFor(m)) {
                if (ImGui::Selectable(p->name.c_str(), p == plan)) {
                    chosen(groupOf(m)) = p->id;
                    plat::prefs().setS((std::string("tvPlan.") + groupOf(m)).c_str(), p->id);
                }
                if (ImGui::IsItemHovered() && !p->note.empty()) ImGui::SetTooltip("%s", p->note.c_str());
            }
            if (ImGui::Selectable(tr("Automatic (from your location)"), false)) {
                chosen(groupOf(m)).clear();
                plat::prefs().setS((std::string("tvPlan.") + groupOf(m)).c_str(), "");
            }
            ImGui::TreePop();
        }
        static char typed[24] = "";
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::InputTextWithHint("##typed", tr("type a channel"), typed, sizeof typed, ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (const tp::Channel* c = plan->byLabel(typed)) { pick(a, *c, m); changed = true; typed[0] = 0; ImGui::CloseCurrentPopup(); }
        }
        bool sl = showLegacy();
        if (ImGui::Checkbox(tr("Show reallocated channels"), &sl)) setShowLegacy(sl);
        ImGui::Separator();
        for (const tp::Channel& c : plan->channels) {
            if (c.legacy && !showLegacy() && &c != cur) continue;
            char l[96];
            std::snprintf(l, sizeof l, "%-4s %8.3f MHz  %g MHz##%s", c.label.c_str(), c.centreHz / 1e6, c.bwHz / 1e6, c.label.c_str());
            if (c.legacy) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            const bool sel = ImGui::Selectable(l, &c == cur);
            if (c.legacy) ImGui::PopStyleColor();
            if (sel) { pick(a, c, m); changed = true; }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        char t[400];
        if (cur) std::snprintf(t, sizeof t, tr("Channel %s of %s: %.3f MHz, %g MHz wide.%s"), cur->label.c_str(), plan->name.c_str(), cur->centreHz / 1e6, cur->bwHz / 1e6, cur->legacy ? tr(" Reallocated or analog-only today.") : "");
        else std::snprintf(t, sizeof t, tr("Channels of %s. The frequency in the box is not on one of them. Change the region in the list."), plan->name.c_str());
        ImGui::SetTooltip("%s", t);
    }
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::ArrowButton("##next", ImGuiDir_Right)) stepBy(+1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Next channel of the plan"));
    ImGui::SameLine(0, 4 * gUi);
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(420 * gUi);
        ImGui::TextWrapped("%s", tr("TV channel. A channel number belongs to a standard and a country: channel 2 is 57 MHz in the US, 47-54 MHz is E2 in Europe. "
                                    "Pick the region in the list (it starts from your location), then a channel; OnAir tunes its centre and sets its width. "
                                    "The frequency box, the width and the sample rate can still be set to anything. Details: docs/TV-CHANNELS.md."));
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::PopID();
    return changed;
}

// the setup wizard set a new location: the plans follow the new country again
void tvPlanFollowLocation() {
    for (const char* g : {"dvb", "atsc", "isdbt", "dtmb", "dvbc", "analog"}) plat::prefs().setS((std::string("tvPlan.") + g).c_str(), "");
    chosenCache().clear();
    countryLoaded() = false;
}
