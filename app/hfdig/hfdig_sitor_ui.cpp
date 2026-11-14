// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SITOR screens: the SITOR-B (FEC) text of NAVTEX and HF coast stations as it is received, with the tone spacing, the symbol rate and the
// character counts, and the SITOR-A / AMTOR (ARQ) monitor: selective call, blocks and repeats, link events and the text without repeats. The receiver is core/include/dect2/sitor/sitor_live.h (family 119, engine standard 121, active standard 120).
#include "app.h"
#include "i18n/i18n.h"
#include "dect2/sitor/sitor_history.h"
#include "dect2/sitor/sitor_tel.h"
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <string>

namespace {

bool live(const App& a) { return a.rx.standard == 120; }

dect2::SitorHistory gHist;   // the text shown: kept when the frequency changes
double gHistFreq = -1;

void feedHistory(const App& a) {
    if (gHistFreq >= 0 && std::fabs(gHistFreq - a.freqMhz) > 1e-9) {
        char b[96];
        const time_t tt = time(nullptr);
        struct tm g;
#ifdef _WIN32
        gmtime_s(&g, &tt);
#else
        gmtime_r(&tt, &g);
#endif
        snprintf(b, sizeof b, "--- %.4f MHz, %02d:%02d:%02d UTC ---", a.freqMhz, g.tm_hour, g.tm_min, g.tm_sec);
        gHist.retune(b);
    }
    gHistFreq = a.freqMhz;
    gHist.update(a.rx.sitor.text);
}

void tab(App& a) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("SITOR-B (CCIR 476, ITU-R M.476): 100 baud FSK with 170 Hz shift, every character sent twice. The tones, their polarity and the "
                        "character phase are found automatically. Receive only.");
    ImGui::TextDisabled("%s", tr("SITOR-A / AMTOR (ARQ) links are monitored too: both stations, repeats dropped. Tested on simulated signals."));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::SitorTelemetry& t = a.rx.sitor;
    auto row = [](const char* k, const char* v) {
        ImGui::TextDisabled("%s", k);
        kvColumn(130 * gUi);
        ImGui::TextUnformatted(v);
    };
    char b[160];
    row("state", t.state == 2 ? "locked" : t.state == 1 ? "FSK found, not locked" : "no FSK signal");
    snprintf(b, sizeof b, "%.0f Hz shift, %.0f baud%s", t.shiftHz, t.baud, t.inverted ? ", tones swapped" : "");
    row("signal", b);
    snprintf(b, sizeof b, "%.1f dB", static_cast<double>(t.snrDb));
    row("SNR", b);
    snprintf(b, sizeof b, "%llu good, %llu bad code words", static_cast<unsigned long long>(t.blocksOk), static_cast<unsigned long long>(t.blocksBad));
    row("words", b);
    if (t.arqLocked || !t.arqText.empty()) {
        snprintf(b, sizeof b, "%s, %llu blocks, %llu repeats, %llu mutilated", t.arqLocked ? tr("locked") : tr("lost"), static_cast<unsigned long long>(t.arqNew),
                 static_cast<unsigned long long>(t.arqRepeats), static_cast<unsigned long long>(t.arqBad));
        row(tr("SITOR-A"), b);
        if (!t.selcall.empty()) row(tr("selective call"), t.selcall.c_str());
    }
    ImGui::Spacing();
    feedHistory(a);
    if (ImGui::SmallButton(tr("Clear"))) gHist.clear();
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Save..."))) {
        const std::string path = saveFileDialog("sitor.txt");
        if (!path.empty()) {
            std::ofstream o(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary);
            o << gHist.text;
            if (!t.arqText.empty()) o << "\n--- SITOR-A ---\n" << t.arqText << "\n";
        }
    }
    ImGui::BeginChild("sitortext", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(gHist.text.c_str());
    if (!t.arqText.empty()) { ImGui::Separator(); ImGui::TextDisabled("%s", tr("SITOR-A")); ImGui::TextUnformatted(t.arqText.c_str()); }
    ImGui::PopTextWrapPos();
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

void list(App& a) {
    if (!live(a)) return;
    const std::string& l = a.rx.sitor.arqLog;
    if (l.empty()) { ImGui::TextDisabled("%s", tr("no SITOR-A link heard yet")); return; }
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(l.c_str());
    ImGui::PopTextWrapPos();
}
void tick(App&) {}

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State");
    ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::sitorSummary(a.rx.sitor).c_str() : (a.engine.running() ? "starting" : "stopped"));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "SITOR";
    if (live(a)) l2 = dect2::sitorSummary(a.rx.sitor);
}

} // namespace

extern const ModeUi kSitorUi;
const ModeUi kSitorUi = {
    .sideTitle = "TEXT",
    .tabName = "SITOR",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
