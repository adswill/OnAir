// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF waveform ID screens: a spectrogram snippet, the ranked candidates, the measured parameters and a note per candidate with its source.
// Identification only: nothing is decoded, no message content is read, no encryption is touched. Candidates are estimates.
#include "app.h"
#include "dect2/milhf/milhf_engine.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>

namespace {

bool live(const App& a) { return a.rx.standard == 146; }   // family 145, engine standard 147, active standard 146

std::string clipMsg;

ImU32 heat(float v) {
    const float r = std::clamp(v * 2.4f - 0.6f, 0.f, 1.f), g = std::clamp(v * 2.4f - 1.4f, 0.f, 1.f), b = std::clamp(v * 3.0f, 0.f, 1.f) * (1.f - 0.5f * g);
    return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 255);
}

void spectrogram(const dect2::MilhfTelemetry& t) {
    const float w = std::max(200.f, ImGui::GetContentRegionAvail().x), h = 150.f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(8, 8, 14, 255));
    if (t.rows > 0 && (int)t.wf.size() >= t.rows * dect2::kMilhfCols) {
        int lo = 255, hi = 0;
        std::vector<uint8_t> s(t.wf.begin(), t.wf.begin() + t.rows * dect2::kMilhfCols);
        std::sort(s.begin(), s.end());
        lo = s[s.size() / 10];
        hi = std::max<int>(lo + 12, s[s.size() * 995 / 1000]);
        const float cw = w / dect2::kMilhfCols, rh = h / dect2::kMilhfRows;
        for (int r = 0; r < t.rows; r++)
            for (int c = 0; c < dect2::kMilhfCols; c++) {
                const float v = std::clamp(((float)t.wf[(size_t)r * dect2::kMilhfCols + c] - lo) / (float)(hi - lo), 0.f, 1.f);
                const float y = p.y + h - (t.rows - r) * rh;
                dl->AddRectFilled(ImVec2(p.x + c * cw, y), ImVec2(p.x + (c + 1) * cw + 0.5f, y + rh + 0.5f), heat(v));
            }
    }
    for (int hz = 0; hz <= 3500; hz += 500) {
        const float x = p.x + w * (float)hz / 3600.f;
        dl->AddLine(ImVec2(x, p.y + h - 6), ImVec2(x, p.y + h), IM_COL32(200, 200, 200, 180));
        char b[16];
        std::snprintf(b, sizeof b, "%d", hz);
        dl->AddText(ImVec2(x + 2, p.y + h - 18), IM_COL32(200, 200, 200, 200), b);
    }
    ImGui::Dummy(ImVec2(w, h));
    ImGui::TextDisabled("Audio 0 - 3600 Hz (upper sideband), newest at the bottom, last %.0f s", dect2::kMilhfRows * 0.2);
}

void saveClip(App& a) {
    namespace fs = std::filesystem;
    const std::string dir = plat::dataDir() + "/milhf-clips";
    std::error_code ec;
    fs::create_directories(dir, ec);
    char name[64];
    const std::time_t now = std::time(nullptr);
    std::strftime(name, sizeof name, "/clip-%Y%m%d-%H%M%S.wav", std::localtime(&now));
    std::string err;
    const std::string path = dir + name;
    if (a.engine.milhf().saveClip(path, err)) clipMsg = "Saved " + path;
    else clipMsg = "Not saved: " + err;
}

void tab(App& a) {
    if (!live(a)) {
        ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "tune an HF signal (upper sideband, 1.8 - 30 MHz) and start the receiver");
        return;
    }
    const dect2::MilhfTelemetry& t = a.rx.milhf;
    const dect2::milhf::Features& f = t.res.f;
    ImGui::TextUnformatted("HF waveform ID - Identification only");
    ImGui::TextWrapped("The candidates below are estimates from measured physical-layer features. Nothing is decoded, no message content is read and no encryption is touched.");
    ImGui::Spacing();
    spectrogram(t);
    ImGui::Spacing();
    if (!t.analysed) { ImGui::TextDisabled("collecting audio (a first estimate after about 3 seconds)"); return; }
    if (!f.signal) { ImGui::TextDisabled("No signal above the noise floor (%.0f dBFS)", t.levelDb); }

    ImGui::TextUnformatted("Candidates (best estimate first)");
    if (ImGui::BeginTable("##milhfc", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Waveform", ImGuiTableColumnFlags_WidthStretch, 2.f);
        ImGui::TableSetupColumn("Confidence", ImGuiTableColumnFlags_WidthFixed, 90.f * gUi);
        ImGui::TableSetupColumn("Measured basis", ImGuiTableColumnFlags_WidthStretch, 3.f);
        ImGui::TableHeadersRow();
        for (const auto& c : t.res.cands) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextWrapped("%s", c.name.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.0f %% (est.)", 100.0 * c.conf);
            ImGui::TableNextColumn(); ImGui::TextWrapped("%s", c.basis.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("Measured parameters");
    if (f.signal) {
        if (f.bwHz > 0) ImGui::Text("Occupied band: %.0f - %.0f Hz (%.0f Hz wide, centre %.0f Hz), SNR %.0f dB", f.loHz, f.hiHz, f.bwHz, f.centreHz, f.snrDb);
        if (f.baudHz > 0) ImGui::Text("Clock line: %.1f Bd (%.0f dB over its surroundings)", f.baudHz, f.baudDb);
        else ImGui::TextDisabled("Clock line: none found");
        if (f.order) ImGui::Text("Constellation guess at that clock: %s (rough)", f.order == 99 ? "higher order or unclear" : (std::to_string(f.order) + "-PSK").c_str());
        if (f.frameLagSym) ImGui::Text("Repetition period: %d symbols (%.1f ms), correlation %.2f", f.frameLagSym, f.frameLagMs, f.frameCorr);
        if (f.combScore >= 0.3) ImGui::Text("Tone comb: %d peaks, spacing %.1f Hz (regularity %.2f)", f.nTones, f.toneSpacingHz, f.combScore);
        if (f.shiftHz > 0) ImGui::Text("Two peaks %.0f Hz apart", f.shiftHz);
        if (f.pilot605) ImGui::Text("Steady tone at 605 Hz (%.0f dB)%s", f.pilot605Db, f.sync2915 ? ", and at 2915 Hz" : "");
        if (f.carrierDb > 0) ImGui::Text("Carrier line: %.1f Hz (%.0f dB)", f.carrierHz, f.carrierDb);
        if (f.envModDb > 0) ImGui::Text("Envelope modulation: %.1f Hz (%.0f dB)", f.envModHz, f.envModDb);
        if (f.bursts > 0) ImGui::Text("Bursts: %d, on %.0f %% of the time", f.bursts, 100.0 * f.duty);
        else ImGui::Text("Continuous (no bursts)");
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("Notes and sources");
    for (const auto& c : t.res.cands) {
        ImGui::PushID(c.name.c_str());
        ImGui::BulletText("%s", c.name.c_str());
        ImGui::Indent();
        ImGui::TextWrapped("%s", c.note.c_str());
        if (!c.url.empty()) {
            ImGui::TextDisabled("%s", c.url.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy link")) ImGui::SetClipboardText(c.url.c_str());
        }
        ImGui::Unindent();
        ImGui::PopID();
    }
    ImGui::Spacing();
    if (ImGui::Button("Save audio clip for sigidwiki")) saveClip(a);
    ImGui::SameLine();
    ImGui::TextDisabled("last 20 s of audio as a WAV, to share on the public Signal Identification Wiki");
    if (!clipMsg.empty()) ImGui::TextWrapped("%s", clipMsg.c_str());
}

void receiver(App& a) {
    if (!live(a)) return;
    const dect2::MilhfTelemetry& t = a.rx.milhf;
    ImGui::Text("%s", dect2::milhfSummary(t).c_str());
    ImGui::Text("audio %.0f Hz, level %.1f dBFS, identification only (estimates)", t.audioRate, t.levelDb);
}

void list(App&) {}
void tick(App&) {}

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State");
    ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::milhfSummary(a.rx.milhf).c_str() : (a.engine.running() ? "starting" : "stopped"));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "HF waveform ID";
    if (live(a)) l2 = dect2::milhfSummary(a.rx.milhf);
}

} // namespace

extern const ModeUi kMilhfUi;
const ModeUi kMilhfUi = {
    .sideTitle = "CANDIDATES",
    .tabName = "HF ID",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
