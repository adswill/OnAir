// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Use GPU where available": the global tick, the per-mode overrides, the engine side of both, and the diagnostics lines (app/ui/gpu_logic.h).
#include "../app.h"
#include "gpu_logic.h"
#include "dect2/dsp/gpu_ldpc.h"
#include "dect2/dsp/gpu_viterbi.h"
#include "dect2/dsp/gpu_fft.h"
#include <string>

using namespace dect2;

static bool domainAllowed(const App& a, int d) {
    const char* mode = d == gpupol::DvbS ? "DVB-S/S2" : d == gpupol::Dtmb ? "DTMB" : d == gpupol::Atsc3 ? "ATSC 3.0" : d == gpupol::IsdbT ? "ISDB-T" : d == gpupol::Dsp ? "Spectrum" : "DVB";
    return gulogic_allowed(a, mode);
}

bool gulogic_allowed(const App& a, const char* mode) { return gpulogic::modeAllowed(a.gpuGlobal, a.gpuModes, mode); }

void applyGpuPolicy(App& a) {
    for (int d = 0; d < gpupol::kDomains; d++) a.engine.setGpuAllowed(d, domainAllowed(a, d));
}

void gpuModeTick(App& a, const char* mode) {
    const char* why = gpulogic::noGpuReason(mode);
    bool on = gulogic_allowed(a, mode);
    if (why) ImGui::BeginDisabled();
    if (ImGui::Checkbox((std::string(tr("Use GPU")) + "##gpu_" + mode).c_str(), &on)) {
        const int o = gpulogic::overrideOf(a.gpuModes, mode);
        // a value equal to the global tick is no override: drop it
        a.gpuModes = gpulogic::setOverride(a.gpuModes, mode, on == a.gpuGlobal ? -1 : (on ? 1 : 0));
        (void)o;
        applyGpuPolicy(a); savePrefs(a);
    }
    if (why) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", why ? tr(why) : tr("Off: this mode's error correction runs on the CPU only. The decoded result is the same."));
}

// Settings > Decoding: one row per mode with a GPU stage (a tick), then the modes without one (disabled, with the reason)
void gpuSettingsRow(App& a) {
    int n; const gpulogic::Row* rows = gpulogic::gpuRows(n);
    if (GpuLdpc::instance().available() || GpuViterbi::instance().available()) ImGui::TextDisabled("%s", GpuLdpc::instance().deviceName());
    else { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s %s", tr("No usable GPU found: everything runs on the CPU. Why:"), GpuLdpc::instance().unavailableReason()); ImGui::PopTextWrapPos(); }   // the reason is a log line, not translated
    for (int i = 0; i < n; i++) {
        gpuModeTick(a, rows[i].mode);
        ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("%s", rows[i].mode);
        for (int k = 0; k < 2; k++) {
            const int d = rows[i].domains[k];
            if (d < 0) continue;
            const bool gpuOk = d == gpupol::Dtmb ? GpuLdpc::instance().qcAvailable() : d == gpupol::Dsp ? GpuFft::instance().available() : (d == gpupol::T || d == gpupol::IsdbT) ? GpuViterbi::instance().available() : GpuLdpc::instance().available();
            int onGpu = -1;
            const int st = a.rx.standard;
            if (d == gpupol::T2 && a.engine.running() && st == 0) onGpu = a.rx.plpOnGpu;
            else if (d == gpupol::T && a.engine.running() && st == 1) onGpu = a.rx.dvbt.viterbiOnGpu;
            else if (d == gpupol::DvbS && a.engine.running() && st == 7) onGpu = a.rx.dvbs.onGpu;
            else if (d == gpupol::Dtmb && a.engine.running() && st == 8) onGpu = a.rx.dtmb.onGpu;
            else if ((d == gpupol::Atsc3 || d == gpupol::IsdbT || d == gpupol::Dsp) && a.engine.running()) onGpu = gpupol::live().onGpu[d].load();
            ImGui::TextDisabled("    %s", gpupol::diagLine(d, a.engine.gpuAllowed(d), gpuOk, a.computeMode, onGpu).c_str());
        }
    }
    if (ImGui::TreeNode(trw("Modes with no GPU stage##gpunone"))) {
        bool none = true;
        for (const ModeDef& m : allModes()) {
            if (!gpulogic::noGpuReason(m.name)) continue;
            none = false;
            bool off = false;
            ImGui::BeginDisabled(); ImGui::Checkbox((std::string(m.name) + "##gpuno_" + m.name).c_str(), &off); ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr(gpulogic::noGpuReason(m.name)));
        }
        if (none) ImGui::TextDisabled("-");
        ImGui::TreePop();
    }
}
