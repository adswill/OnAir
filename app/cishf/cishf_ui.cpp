// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CIS HF modes screens (families 620 - 626, core/src/cishf, docs/CIS-HF-3.9.md): what was found, the measured parameters, the decoded text
// (characters, frames or phase steps) and the events. Receive only; nothing is decrypted. Advanced: forced speed and shift for the FSK modes.
#include "app.h"
#include "dect2/cishf/cishf_rx.h"
#include "dect2/cishf/cishf_tel.h"
#include <string>

namespace {

int kindOf(const App& a) { return a.rx.standard - 621; }
bool live(const App& a, int kind) { return a.rx.standard == 621 + kind; }

const char* note(int kind) {
    switch (kind) {
    case 0: return N_("CIS-36-50 and CIS-50-50 (BEE, T-600): 2-FSK, 36 / 50 Bd idle and 50 / 100 Bd traffic, found automatically. The start word, the session key "
                      "and the 3:4 characters are shown as hex: the traffic is encrypted and nothing is decrypted.");
    case 1: return N_("CIS-14 (AMOR): 2-FSK at 96 Bd. The 14-bit frame is found from its period and its fields are shown raw (state bits, two 5-bit words, "
                      "two parity bits); the parity rule is not published, so it is not checked.");
    case 2: return N_("CIS-12 (MS5, AT-3004D, FIRE): 12 carriers 700 - 2900 Hz at 120 Bd with a 3300 Hz pilot. Physical layer only: the pilot, the PSK order "
                      "and the phase steps of every carrier. No frame format is published.");
    case 3: return N_("CIS-36 (MFSK-36): identification only, from the tone count, the 40 Hz spacing and the 10 / 20 / 40 Bd tone rate.");
    case 4: return N_("CIS OFDM HDR modem (CIS-45, CIS-60, CIS-93): identification only, from the OFDM symbol, the carriers and the PSK order.");
    case 5: return N_("CIS-128: identification only: about 128 OFDM carriers at about 21 Bd in 3 kHz (the 6 kHz form is not covered).");
    default: return N_("CIS-3000: identification only: a single 8-PSK carrier at 3000 Bd.");
    }
}

void tab(App& a) {
    const int kind = kindOf(a);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr(note(kind >= 0 && kind < dect2::kCisHfKinds ? kind : 0)));
    ImGui::TextDisabled("%s", tr("Tune the dial frequency, upper sideband. Checked on simulated signals only."));
    ImGui::PopTextWrapPos();
    if (kind < 0 || kind >= dect2::kCisHfKinds) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver")); return; }
    const dect2::CisHfTelemetry& t = a.rx.cishf;
    ImGui::TextUnformatted(dect2::cisHfSummary(t).c_str());
    if (!t.params.empty() && ImGui::BeginTable("##cisp", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        for (const auto& p : t.params) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", p.first.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.second.c_str());
        }
        ImGui::EndTable();
    }
    if (kind <= 2) {
        ImGui::Text("%s: %llu  %s: %llu", tr("read"), (unsigned long long)t.blocksOk, tr("failed a check"), (unsigned long long)t.blocksBad);
        ImGui::BeginChild("##cist", ImVec2(0, 180), ImGuiChildFlags_Borders);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(t.text.empty() ? "-" : t.text.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
    if (ImGui::Button(tr("Clear"))) a.engine.cisHf(kind).clearResults();
    ImGui::SeparatorText(tr("Events"));
    for (const auto& e : t.events) ImGui::Text("%7.1f s  %s", e.timeSec, e.text.c_str());
    if (kind <= 1 && ImGui::CollapsingHeader(trw("Advanced"))) {
        static float baud[2] = {0, 0}, shift[2] = {0, 0};
        bool ch = false;
        ImGui::SetNextItemWidth(140 * gUi);
        ch |= ImGui::InputFloat(tr("Speed (Bd, 0 = automatic)"), &baud[kind], 0, 0, "%.1f");
        ImGui::SetNextItemWidth(140 * gUi);
        ch |= ImGui::InputFloat(tr("Shift (Hz, 0 = automatic)"), &shift[kind], 0, 0, "%.0f");
        if (ch) a.engine.cisHf(kind).setForced(baud[kind], shift[kind]);
    }
}
void receiver(App& a) { const int k = kindOf(a); ImGui::TextUnformatted(k >= 0 && k < dect2::kCisHfKinds && live(a, k) ? dect2::cisHfSummary(a.rx.cishf).c_str() : tr("stopped")); }
void status(App& a) { receiver(a); }
void summary(const App& a, std::string& l1, std::string& l2) {
    const int k = kindOf(a);
    l1 = dect2::cisHfName(k >= 0 && k < dect2::kCisHfKinds ? k : 0);
    if (k >= 0 && k < dect2::kCisHfKinds) l2 = dect2::cisHfSummary(a.rx.cishf);
}

} // namespace

#define CISHF_UI(var, name) \
    extern const ModeUi var; \
    const ModeUi var = {.sideTitle = "CIS HF", .tabName = name, .tabIcon = Ic::Signal, .tab = tab, .receiver = receiver, .status = status, .summary = summary};
CISHF_UI(kCisBeeUi, "BEE")
CISHF_UI(kCis14Ui, "CIS-14")
CISHF_UI(kCis12Ui, "CIS-12")
CISHF_UI(kCis36Ui, "CIS-36")
CISHF_UI(kCisHdrUi, "CIS HDR")
CISHF_UI(kCis128Ui, "CIS-128")
CISHF_UI(kCis3000Ui, "CIS-3000")
