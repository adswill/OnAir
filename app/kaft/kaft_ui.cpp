// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAFT screens: the receiver panel (the transfer in progress, the profile ladder, counts, the folder received files are saved to and the files received)
// and the transmit options (callsign, file, width, profile, passes) that the TX panel draws for the KAFT signal. English texts only.
// KAFT only listens here: it sends no reply, so the panel says so and shows the profile the receiver would ask for.
#include "app.h"
#include "../panels/tx_state.h"
#include "dect2/kaft/kaft_tel.h"
#include "dect2/kaft/kaft_frame.h"
#include "dect2/kaft/kaft_txgen.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

bool live(const App& a) { return a.rx.standard == 126; }   // the engine reports its standard code minus one

const char* const kWidthNames[3] = {"1 MHz", "500 kHz", "200 kHz"};

const char* stateName(int s) { return s == 2 ? "receiving frames" : s == 1 ? "searching" : "no samples"; }

const char* sessionName(int s) {
    switch (s) {
    case 1: return "announced, waiting for data";
    case 2: return "receiving";
    case 3: return "complete, hash verified";
    case 4: return "failed: the hash check did not match";
    default: return "no transfer";
    }
}

std::string size(uint64_t b) {
    char s[32];
    if (b >= (1ull << 20)) snprintf(s, sizeof s, "%.2f MB", (double)b / 1048576.0);
    else if (b >= 1024) snprintf(s, sizeof s, "%.1f kB", (double)b / 1024.0);
    else snprintf(s, sizeof s, "%llu B", (unsigned long long)b);
    return s;
}

struct Pushed {
    bool init = false;
    char folder[512] = {};
    std::string sent;
} P;

void loadFolder() {
    if (P.init) return;
    P.init = true;
    snprintf(P.folder, sizeof P.folder, "%s", plat::basePrefs().getS("kaft.saveDir", "").c_str());
}

void ladder(const dect2::KaftTelemetry& t) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = 46 * gUi, h = 22 * gUi, gap = 4 * gUi;
    ImVec2 p = ImGui::GetCursorScreenPos();
    for (int i = 0; i < dect2::kaft::kNumProfiles; i++) {
        const ImVec2 a(p.x + (float)i * (w + gap), p.y), b(a.x + w, a.y + h);
        const bool cur = t.profile == i, req = t.requestedProfile == i;
        dl->AddRectFilled(a, b, cur ? IM_COL32(60, 150, 90, 255) : IM_COL32(40, 44, 54, 255), 3.f);
        if (req) dl->AddRect(a, b, IM_COL32(240, 200, 90, 255), 3.f, 0, 2.f);
        char lab[8];
        snprintf(lab, sizeof lab, "P%d", i);
        const ImVec2 ts = ImGui::CalcTextSize(lab);
        dl->AddText(ImVec2(a.x + (w - ts.x) * 0.5f, a.y + (h - ts.y) * 0.5f), IM_COL32(230, 232, 238, 255), lab);
    }
    ImGui::Dummy(ImVec2((w + gap) * dect2::kaft::kNumProfiles, h + 4 * gUi));
}

void tab(App& a) {
    loadFolder();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("KAFT is amateur file transfer in 200 kHz, 500 kHz or 1 MHz: ten profiles from BPSK 1/4 to 1024-QAM 9/10, frames of LDPC blocks each with a CRC-32, "
                        "a session (OPEN, data, FINISH) and a hash check of the whole file. This receiver only listens: it sends no reply, so a sender that waits for ACKs "
                        "cannot finish through it; the OnAir transmitter sends one-way and repeats the file, so a late start or a lost block is made up on a later pass. "
                        "A file is kept only when its hash matches. To transmit, switch to TX and pick KAFT.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    // the folder: edited here, pushed to the receiver in tick()
    ImGui::TextColored(pal::heading(), "%s", "Save received files to");
    ImGui::SetNextItemWidth(std::max(200.f * gUi, ImGui::GetContentRegionAvail().x - 190 * gUi));
    ImGui::InputText("##kaftfolder", P.folder, sizeof P.folder);
    ImGui::SameLine();
    if (ImGui::Button("Browse...##kaftfolder")) { const std::string d = plat::openFolderDialog(); if (!d.empty()) snprintf(P.folder, sizeof P.folder, "%s", d.c_str()); }
    if (!P.folder[0]) ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.4f, 1), "%s", "No folder chosen: a file that arrives is checked but not kept.");
    ImGui::Spacing();

    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::KaftTelemetry& t = a.rx.kaft;
    auto row = [](const char* k, const std::string& v) {
        ImGui::TextDisabled("%s", k);
        kvColumn(130 * gUi);
        ImGui::TextUnformatted(v.c_str());
    };
    char b[220];
    ImGui::TextColored(t.state == 2 ? ImVec4(0.45f, 0.85f, 0.5f, 1) : t.state == 1 ? ImVec4(0.95f, 0.8f, 0.4f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "%s", stateName(t.state));
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear list##kaft")) a.engine.kaft().clearFiles();
    ImGui::Spacing();

    ImGui::TextColored(pal::heading(), "%s", "Transfer");
    row("session", sessionName(t.session));
    if (t.session != 0) {
        row("file", t.fileName.empty() ? "-" : t.fileName);
        row("from", t.peer.empty() ? "-" : t.peer);
        const float frac = t.fileBytes ? (float)((double)t.haveBytes / (double)t.fileBytes) : 0.f;
        snprintf(b, sizeof b, "%s of %s", size(t.haveBytes).c_str(), size(t.fileBytes).c_str());
        ImGui::TextDisabled("%s", "progress");
        kvColumn(130 * gUi);
        ImGui::ProgressBar(std::max(0.f, std::min(1.f, frac)), ImVec2(240 * gUi, 0), b);
        if (t.resumedBytes) {
            snprintf(b, sizeof b, "%s were already held from an earlier pass or session", size(t.resumedBytes).c_str());
            row("resume", b);
        }
    }
    if (t.heldSessions) {
        snprintf(b, sizeof b, "%d unfinished transfer%s kept for a resume", t.heldSessions, t.heldSessions == 1 ? "" : "s");
        row("held", b);
    }

    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", "Link");
    row("width", t.width >= 0 && t.width < 3 ? kWidthNames[t.width] : "none yet");
    row("profile", t.profile >= 0 && t.profile < dect2::kaft::kNumProfiles ? dect2::kaft::kProfiles[t.profile].name : "none yet");
    snprintf(b, sizeof b, "%.1f dB", (double)t.esN0Db);
    row("Es/N0", t.width >= 0 ? b : "-");
    snprintf(b, sizeof b, "%+.0f Hz", (double)t.cfoHz);
    row("CFO", t.width >= 0 ? b : "-");
    row("last sender", t.callsign.empty() ? "-" : t.callsign);
    snprintf(b, sizeof b, "%llu data, %llu control, %llu ACK; %llu with a failed block", (unsigned long long)t.framesData, (unsigned long long)t.framesCtrl,
             (unsigned long long)t.framesAck, (unsigned long long)t.framesBad);
    row("frames", b);
    snprintf(b, sizeof b, "%llu good, %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    row("blocks", b);
    snprintf(b, sizeof b, "%.1f kbit/s", t.throughputBps / 1000.0);
    row("throughput", b);
    ImGui::TextDisabled("%s", "profile ladder");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", "(green: the profile of the frames; yellow outline: the profile this receiver would ask for - no return link, so it is not sent)");
    ladder(t);

    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", "Files received");
    if (t.files.empty()) { ImGui::TextDisabled("%s", "none yet"); return; }
    if (ImGui::BeginTable("kaftFiles", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("file");
        ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed, 80 * gUi);
        ImGui::TableSetupColumn("status");
        ImGui::TableHeadersRow();
        for (const dect2::KaftFile& f : t.files) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(f.name.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(size(f.bytes).c_str());
            ImGui::TableNextColumn();
            if (f.saved) ImGui::TextWrapped("%s", f.path.c_str());
            else ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.4f, 1), "%s", f.note.c_str());
        }
        ImGui::EndTable();
    }
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "KAFT";
    if (live(a)) l2 = dect2::kaftSummary(a.rx.kaft);
}

void tick(App& a) {
    loadFolder();
    if (!a.engine.running()) return;
    if (P.sent != P.folder) {   // the folder changed (or the first frame): hand it to the receiver and save it
        P.sent = P.folder;
        a.engine.kaft().setSaveFolder(P.sent);
        plat::basePrefs().setS("kaft.saveDir", P.sent.c_str());
    }
}

} // namespace

extern const ModeUi kKaftUi;
const ModeUi kKaftUi = {
    .sideTitle = "PROFILES",
    .tabName = "KAFT",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .summary = summary,
    .tick = tick,
};

namespace txui {

namespace {
void clampText(char* buf, size_t keep) { if (strlen(buf) > keep) buf[keep] = 0; }
}

// KAFT transmit options in the TX panel. The callsign, file, width, profile and passes are saved in the settings.
void kaftPanel(TxUi& s, bool running) {
    plat::Prefs& p = plat::basePrefs();
    if (!s.kaftLoaded) {
        s.kaftLoaded = true;
        snprintf(s.callsign, sizeof s.callsign, "%s", p.getS("tx.kaft.callsign", s.callsign).c_str());
        snprintf(s.kaftFile, sizeof s.kaftFile, "%s", p.getS("tx.kaft.file", "").c_str());
        s.kaftWidth = std::max(0, std::min(2, (int)p.getI("tx.kaft.width", 2)));
        s.kaftProfile = std::max(0, std::min(dect2::kaft::kNumProfiles - 1, (int)p.getI("tx.kaft.profile", 2)));
        s.kaftPasses = std::max(1, std::min(9, (int)p.getI("tx.kaft.passes", 2)));
    }
    ImGui::TextWrapped("%s", "KAFT: file transfer in a 200 kHz, 500 kHz or 1 MHz channel. The file goes out one-way: OPEN, the data frames, FINISH, and again for each pass; "
                              "no reply is waited for, so the profile is fixed and the whole file is sent several times (a receiver that missed blocks gets them on the next pass; one that has the file ignores the repeat). "
                              "The file is sent as it is (no compression codec is built in). Empty callsign sends the demo identity ONAIR; no file sends a short demo text file. "
                              "OnAir sets no limit on the frequency or the power.");
    ImGui::BeginDisabled(running);
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::InputText("Callsign (6 characters at most)", s.callsign, sizeof s.callsign, ImGuiInputTextFlags_CharsUppercase)) clampText(s.callsign, 6);
    ImGui::SetNextItemWidth(std::max(220.f * gUi, ImGui::GetContentRegionAvail().x - 260 * gUi));
    ImGui::InputText("File to send", s.kaftFile, sizeof s.kaftFile);
    ImGui::SameLine();
    if (ImGui::Button("Browse...##kaftfile")) { const std::string f = plat::openFileDialog(); if (!f.empty()) snprintf(s.kaftFile, sizeof s.kaftFile, "%s", f.c_str()); }
    ImGui::SameLine();
    if (ImGui::Button("Clear##kaftfile")) s.kaftFile[0] = 0;
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::BeginCombo("Width", kWidthNames[std::max(0, std::min(2, s.kaftWidth))])) {
        for (int i = 2; i >= 0; i--) {
            char lab[96];
            snprintf(lab, sizeof lab, "%s   (needs a rate of at least %g Msps)", kWidthNames[i], dect2::kaftMinRateHz(i) / 1e6);
            if (ImGui::Selectable(lab, s.kaftWidth == i)) s.kaftWidth = i;
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "In a file the sample rate is chosen under Destination; with a radio it follows the radio. A wider channel is faster.");
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::BeginCombo("Profile", dect2::kaft::kProfiles[std::max(0, std::min(dect2::kaft::kNumProfiles - 1, s.kaftProfile))].name)) {
        for (int i = 0; i < dect2::kaft::kNumProfiles; i++) {
            char lab[96];
            snprintf(lab, sizeof lab, "%s   (needs about %+.1f dB Es/N0)", dect2::kaft::kProfiles[i].name, dect2::kaft::kProfiles[i].reqEsN0Db);
            if (ImGui::Selectable(lab, s.kaftProfile == i)) s.kaftProfile = i;
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Fixed for the whole transmission: with no return link there is nothing to step it by. The Es/N0 figures are the specification's estimates, not measurements of this build.");
    ImGui::SetNextItemWidth(160 * gUi);
    ImGui::SliderInt("Passes", &s.kaftPasses, 1, 9);
    ImGui::EndDisabled();

    size_t bytes = 0;
    bool bad = false;
    if (s.kaftFile[0]) {
        std::error_code ec;
        const auto sz = std::filesystem::file_size(s.kaftFile, ec);
        bytes = ec ? 0 : (size_t)sz;
        bad = ec || bytes == 0;
        if (bad) ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1), "%s", "The file cannot be read or is empty.");
        else if (bytes > ((size_t)256 << 20)) ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1), "%s", "A KAFT file is at most 256 MiB here.");
    } else {
        bytes = 85;   // the demo text
    }
    if (!bad) {
        const double sec = dect2::kaftAirtimeSeconds(bytes, s.kaftProfile, s.kaftWidth, s.kaftPasses);
        ImGui::TextDisabled("%.1f kB, about %d:%02d of airtime for %d pass%s (an estimate)", (double)bytes / 1024.0, (int)sec / 60, (int)sec % 60, s.kaftPasses, s.kaftPasses == 1 ? "" : "es");
    }

    char sig[700];
    snprintf(sig, sizeof sig, "%s|%s|%d|%d|%d", s.callsign, s.kaftFile, s.kaftWidth, s.kaftProfile, s.kaftPasses);
    if (s.kaftSaved != sig) {
        s.kaftSaved = sig;
        p.setS("tx.kaft.callsign", s.callsign); p.setS("tx.kaft.file", s.kaftFile);
        p.setI("tx.kaft.width", s.kaftWidth); p.setI("tx.kaft.profile", s.kaftProfile); p.setI("tx.kaft.passes", s.kaftPasses);
    }
}

} // namespace txui
