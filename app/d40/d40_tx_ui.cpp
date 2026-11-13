// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Transmit panel notes from core/include/dect2/tx/tx_plan.h: link budget, power ramp steps and SWR back-off advice. Notes only: nothing here blocks or changes a transmission.
#include "../panels/tx_state.h"
#include "../ui/typography.h"
#include "../platform/platform.h"
#include "dect2/tx/tx_control.h"
#include "dect2/tx/tx_plan.h"
#include <cmath>
#include <cstdio>

namespace {
struct P {
    bool init = false;
    double km = 10, txLoss = 1, txGain = 0, rxGain = 0, rxLoss = 1, extra = 6, sens = -110;
    double rampStart = -10, rampStep = 6, swr = 1.0;
} G;
void save() {
    auto& p = plat::basePrefs();
    p.setD("d40tx.km", G.km); p.setD("d40tx.txLoss", G.txLoss); p.setD("d40tx.txGain", G.txGain); p.setD("d40tx.rxGain", G.rxGain);
    p.setD("d40tx.rxLoss", G.rxLoss); p.setD("d40tx.extra", G.extra); p.setD("d40tx.sens", G.sens); p.setD("d40tx.rampStart", G.rampStart); p.setD("d40tx.rampStep", G.rampStep);
}
void in(const char* label, double* v, const char* fmt) {
    ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::InputDouble(label, v, 0, 0, fmt)) save();
}
}  // namespace

namespace txui {
void planReadouts(TxUi& s, const DeviceInfo* radio) {
    if (!radio || s.destKind == 1) return;
    if (!typo::collapsingHeader(trw("Link budget, ramp and SWR"), 0)) return;
    if (!G.init) {
        G.init = true; auto& p = plat::basePrefs();
        G.km = p.getD("d40tx.km", G.km); G.txLoss = p.getD("d40tx.txLoss", G.txLoss); G.txGain = p.getD("d40tx.txGain", G.txGain); G.rxGain = p.getD("d40tx.rxGain", G.rxGain);
        G.rxLoss = p.getD("d40tx.rxLoss", G.rxLoss); G.extra = p.getD("d40tx.extra", G.extra); G.sens = p.getD("d40tx.sens", G.sens);
        G.rampStart = p.getD("d40tx.rampStart", G.rampStart); G.rampStep = p.getD("d40tx.rampStep", G.rampStep);
    }
    const TxCalibration c = calibrationFor(s.radioKey);
    const double pdbm = dect2::txCalPowerDbm(c, s.gainDb, s.ampOn);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Notes only: they never block or change a transmission."));
    ImGui::PopTextWrapPos();
    // link budget
    if (!std::isfinite(pdbm)) ImGui::TextDisabled("%s", tr("The link budget needs this radio's power table (Advanced > Calibration)."));
    else {
        in(tr("Distance km"), &G.km, "%.1f"); ImGui::SameLine(); in(tr("Cable loss TX dB"), &G.txLoss, "%.1f"); ImGui::SameLine(); in(tr("TX antenna dBi"), &G.txGain, "%.1f");
        in(tr("RX antenna dBi"), &G.rxGain, "%.1f"); ImGui::SameLine(); in(tr("RX loss dB"), &G.rxLoss, "%.1f"); ImGui::SameLine(); in(tr("Fade margin dB"), &G.extra, "%.1f");
        in(tr("RX sensitivity dBm"), &G.sens, "%.1f");
        dect2::LinkBudget b; b.txDbm = pdbm; b.txLossDb = G.txLoss; b.txGainDbi = G.txGain; b.rxGainDbi = G.rxGain; b.rxLossDb = G.rxLoss; b.extraLossDb = G.extra; b.rxSensDbm = G.sens;
        const auto r = dect2::linkBudget(b, G.km, s.freqMhz);
        char t[240];
        snprintf(t, sizeof t, tr("EIRP %.1f dBm, path loss %.1f dB, received %.1f dBm, margin %+.1f dB, free-space range at zero margin %.1f km"), r.eirpDbm, r.pathLossDb, r.rxDbm, r.marginDb, r.maxKm);
        ImGui::PushTextWrapPos(0); ImGui::TextColored(r.marginDb < 0 ? pal::warnAmber() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", t); ImGui::PopTextWrapPos();
        ImGui::TextDisabled("%s", tr("Free space plus the margin you enter: terrain and buildings are not modelled."));
        // ramp
        ImGui::Separator();
        in(tr("Ramp from dBm"), &G.rampStart, "%.1f"); ImGui::SameLine(); in(tr("step dB"), &G.rampStep, "%.1f");
        const auto ramp = dect2::powerRamp(G.rampStart, s.targetDbm, G.rampStep);
        std::string rt;
        for (size_t i = 0; i < ramp.size() && i < 12; i++) { char q[40]; snprintf(q, sizeof q, "%s%.0f", i ? " > " : "", ramp[i]); rt += q; }
        ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s: %s dBm (%zu steps)", tr("Steps to the target power"), rt.c_str(), ramp.size()); ImGui::PopTextWrapPos();
        ImGui::TextDisabled("%s", tr("Set the level for each step with \"Set the level for this power\" and watch the SWR."));
    }
    // SWR
    ImGui::Separator();
    double swr = G.swr;
    ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::InputDouble(tr("SWR you measured"), &swr, 0, 0, "%.2f")) G.swr = swr < 1 ? 1 : swr;
    if (G.swr > 1.0) {
        const double back = dect2::swrBackoffDb(G.swr);
        char t[200];
        snprintf(t, sizeof t, tr("SWR %.2f: %.0f %% of the forward power is reflected. %s"), G.swr, dect2::reflectedFraction(G.swr) * 100, back <= 0 ? tr("No change needed.") : "");
        ImGui::TextUnformatted(t);
        if (back > 0) {
            ImGui::SameLine();
            char u[160]; snprintf(u, sizeof u, tr("Advice: back off about %.1f dB%s"), back, std::isfinite(pdbm) ? "" : "");
            ImGui::TextColored(pal::warnAmber(), "%s", u);
            if (std::isfinite(pdbm)) ImGui::TextDisabled("%.1f dBm -> %.1f dBm", s.targetDbm, s.targetDbm - back);
        }
    }
}
}  // namespace txui
