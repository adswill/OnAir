// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Manual notches (core dsp/notch_bank.h) in the app: the list is kept as text in a.notchText (settings key "notchList", for all devices), handed to
// the engine when it changes, edited from the spectrum's right-click menu ("Add notch here") and from the "Notches" list in the auto gain popup
// (next to "Clean the input"): enable, frequency (typed in Hz / kHz / MHz), width, kind, delete. The markers on the spectrum are drawn by plots.cpp.
#include "../app.h"
#include "../ui/theme.h"
#include "../ui/touch_logic.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace dect2;

namespace {
std::vector<Notch>& cache() { static std::vector<Notch> v; return v; }
std::string& cacheText() { static std::string t = "\x01"; return t; }

void refresh(const App& a) {
    if (cacheText() != a.notchText) { cacheText() = a.notchText; cache() = NotchBank::fromText(a.notchText); }
}
void commit(App& a, const std::vector<Notch>& v) {
    a.notchText = v.empty() ? std::string() : NotchBank::toText(v);
    refresh(a);
    a.engine.setNotches(v);
    savePrefs(a);
}
// "404.007", "404007k", "-7 kHz", "+2000": a signed number with an optional unit; a bare number is in defaultHz units
bool parseSigned(const char* s, double defaultUnitHz, bool allowNeg, double& hz) {
    while (*s == ' ') s++;
    bool neg = false;
    if (*s == '-' || *s == '+') { neg = *s == '-'; s++; }
    if (neg && !allowNeg) return false;
    double v = 0;
    touch::Unit u = touch::Unit::Hz;
    for (const touch::Unit c : {touch::Unit::Hz, touch::Unit::kHz, touch::Unit::MHz}) if (touch::unitHz(c) == defaultUnitHz) u = c;
    if (!touch::parseFrequency(s, u, v)) return false;
    hz = neg ? -v : v;
    return true;
}
void fmtHz(double hz, bool rf, char* b, size_t n) {
    if (rf) snprintf(b, n, "%.6f MHz", hz / 1e6);
    else if (std::fabs(hz) >= 1e4) snprintf(b, n, "%.3f kHz", hz / 1e3);
    else snprintf(b, n, "%.0f Hz", hz);
}
}

std::vector<Notch> notchList(const App& a) { refresh(a); return cache(); }

void notchSync(App& a) {
    static const Engine* who = nullptr; static std::string applied = "\x01";
    if (who != &a.engine || applied != a.notchText) {
        who = &a.engine; applied = a.notchText;
        a.engine.setNotches(NotchBank::fromText(a.notchText));
    }
}

bool notchAdd(App& a, double rfHz) {
    std::vector<Notch> v = notchList(a);
    if ((int)v.size() >= NotchBank::kMax) return false;
    Notch n; n.rfHz = rfHz; n.widthHz = NotchBank::kDefaultWidthHz;
    v.push_back(n);
    commit(a, v);
    return true;
}

// where on the spectrum's axis a notch is, in MHz (a following notch sits that far from the radio's centre)
double notchAxisMhz(const App& a, const Notch& n) { return n.follow ? radioCenterMhz(a) + n.offsetHz / 1e6 : n.rfHz / 1e6; }

void notchPanel(App& a) {
    std::vector<Notch> v = notchList(a);
    bool changed = false;
    ImGui::TextDisabled("%s", tr("Notches"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Narrow band-stops in front of the spectrum, the waterfall and the decoders, for a birdie you point at: right-click a peak on the spectrum and choose Add notch here. 'Stays at this RF frequency' keeps cancelling the same carrier when you retune; 'Follows the tuning' keeps the same offset from the centre (an image or LO artefact of the radio). The spectrum shows the signal after the notch."));
    int del = -1;
    static char buf[NotchBank::kMax][32]; static int editing = -1;
    for (size_t i = 0; i < v.size(); i++) {
        Notch& n = v[i];
        ImGui::PushID((int)i);
        if (ImGui::Checkbox("##nen", &n.enabled)) changed = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Notch on"));
        ImGui::SameLine();
        const double shown = n.follow ? n.offsetHz : n.rfHz;
        if (editing != (int)i) fmtHz(shown, !n.follow, buf[i], sizeof buf[i]);
        ImGui::SetNextItemWidth(130 * gUi);
        ImGui::InputText("##nf", buf[i], sizeof buf[i]);
        if (ImGui::IsItemActive()) editing = (int)i;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            double hz = 0;
            if (parseSigned(buf[i], n.follow ? 1e3 : 1e6, n.follow, hz)) { (n.follow ? n.offsetHz : n.rfHz) = hz; changed = true; }
        }
        if (ImGui::IsItemDeactivated() && editing == (int)i) editing = -1;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", n.follow ? tr("Offset from the centre of the capture. Type Hz, kHz or MHz (a bare number is kHz); a minus sign is below the centre.")
                                                                   : tr("Radio frequency of the carrier. Type Hz, kHz or MHz (a bare number is MHz)."));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90 * gUi);
        float w = (float)n.widthHz;
        if (ImGui::DragFloat("##nw", &w, 5.f, (float)NotchBank::kMinWidthHz, (float)NotchBank::kMaxWidthHz, "%.0f Hz", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp)) { n.widthHz = NotchBank::clampWidth(w); changed = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Width of the cut, 25 Hz to 5 kHz. A narrow notch removes a carrier; a wider one forgives a carrier that drifts."));
        ImGui::SameLine();
        int kind = n.follow ? 1 : 0;
        ImGui::SetNextItemWidth(170 * gUi);
        const char* kinds[] = {tr("Stays at this RF frequency"), tr("Follows the tuning")};
        if (ImGui::Combo("##nk", &kind, kinds, 2)) {
            const double c = radioCenterMhz(a) * 1e6;   // keep the same place on the spectrum when the kind changes
            if (kind == 1 && !n.follow) n.offsetHz = n.rfHz - c; else if (kind == 0 && n.follow) n.rfHz = c + n.offsetHz;
            n.follow = kind == 1; changed = true; editing = -1;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Delete"))) del = (int)i;
        ImGui::PopID();
    }
    if (del >= 0) { v.erase(v.begin() + del); changed = true; editing = -1; }
    if (v.empty()) ImGui::TextDisabled("%s", tr("None. Right-click a peak on the spectrum: Add notch here."));
    else if (ImGui::SmallButton(tr("Delete all notches"))) { v.clear(); changed = true; editing = -1; }
    if ((int)v.size() < NotchBank::kMax) {
        if (!v.empty()) ImGui::SameLine();
        if (ImGui::SmallButton(tr("Add a notch at the tuned frequency"))) { Notch n; n.rfHz = a.freqMhz * 1e6; v.push_back(n); changed = true; }
    }
    if (changed) commit(a, v);
}
