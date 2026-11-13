// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAPP screens: the receiver panel (state, the last burst, counts, a profile histogram, the constellation of the last burst and the list of messages
// that arrived whole) and the transmit options (callsign, a text or a file, profile, passes) that the TX panel draws for the KAPP signal. English texts only.
// KAPP only listens here: it sends no acknowledgement, so the panel says so.
#include "app.h"
#include "../panels/tx_state.h"
#include "dect2/kapp/kapp_tel.h"
#include "dect2/kapp/kapp_txgen.h"
#include "dect2/kapp/kapp_frame.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

bool live(const App& a) { return a.rx.standard == 125; }   // the engine reports its standard code minus one

const char* const kProfileNames[9] = {"0  BPSK 1/4", "1  QPSK 1/4", "2  QPSK 1/2", "3  QPSK 3/4", "4  16-QAM 1/2", "5  16-QAM 3/4", "6  64-QAM 2/3", "7  256-QAM 3/4", "8  1024-QAM 5/6"};

const char* stateName(int s) { return s == 2 ? "receiving bursts" : s == 1 ? "listening" : "no samples"; }

std::string clock(double sec) {
    char b[24];
    snprintf(b, sizeof b, "%d:%02d", (int)sec / 60, (int)sec % 60);
    return b;
}

void constellation(const dect2::KappTelemetry& t) {
    const float side = 180 * gUi;
    ImGui::Dummy(ImVec2(side, side));
    const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(18, 20, 26, 255));
    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    dl->AddLine(ImVec2(p0.x, c.y), ImVec2(p1.x, c.y), IM_COL32(70, 74, 86, 255));
    dl->AddLine(ImVec2(c.x, p0.y), ImVec2(c.x, p1.y), IM_COL32(70, 74, 86, 255));
    const float scale = side * 0.5f / 2.0f;   // the plot spans -2 .. +2 around the unit average power
    for (const auto& s : t.constellation) {
        const float x = std::max(-2.f, std::min(2.f, s.first)), y = std::max(-2.f, std::min(2.f, s.second));
        dl->AddCircleFilled(ImVec2(c.x + x * scale, c.y - y * scale), 1.6f * gUi, IM_COL32(120, 200, 255, 190));
    }
    dl->AddRect(p0, p1, IM_COL32(90, 94, 108, 255));
}

void tab(App& a) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("KAPP is amateur packet data 100 kHz wide: bursts of LDPC blocks from BPSK 1/4 to 1024-QAM 5/6, each block with its own CRC, "
                        "and a message is rebuilt block by block (selective repeat). This receiver only listens: it sends no acknowledgement, so a sender that waits for "
                        "ACKs cannot finish through it; a one-way sender repeats the message instead (the OnAir transmitter does). A message has no check of its own beyond the block CRCs. "
                        "To transmit, switch to TX and pick KAPP.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }

    const dect2::KappTelemetry& t = a.rx.kapp;
    auto row = [](const char* k, const std::string& v) {
        ImGui::TextDisabled("%s", k);
        kvColumn(130 * gUi);
        ImGui::TextUnformatted(v.c_str());
    };
    char b[200];
    ImGui::TextColored(t.state == 2 ? ImVec4(0.45f, 0.85f, 0.5f, 1) : t.state == 1 ? ImVec4(0.95f, 0.8f, 0.4f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "%s", stateName(t.state));
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear##kapp")) a.engine.kapp().clearPackets();
    ImGui::Spacing();
    row("profile", t.profile >= 0 && t.profile < 9 ? kProfileNames[t.profile] : "none yet");
    snprintf(b, sizeof b, "%.1f dB", (double)t.snrDb);
    row("Es/N0", t.profile >= 0 ? b : "-");
    snprintf(b, sizeof b, "%+.0f Hz", (double)t.cfoHz);
    row("CFO", t.profile >= 0 ? b : "-");
    row("last sender", t.callsign.empty() ? "-" : t.callsign);
    snprintf(b, sizeof b, "%llu data, %llu ACK heard", (unsigned long long)t.bursts, (unsigned long long)t.acks);
    row("bursts", b);
    snprintf(b, sizeof b, "%llu good, %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    row("blocks", b);
    snprintf(b, sizeof b, "%llu found, %llu header CRC ok, %llu failed", (unsigned long long)t.detections, (unsigned long long)t.headerOk, (unsigned long long)t.headerFail);
    row("preambles", b);
    snprintf(b, sizeof b, "%.1f kbit/s", t.throughputBps / 1000.0);
    row("throughput", b);
    snprintf(b, sizeof b, "%llu complete, %llu repeats ignored, %llu bytes", (unsigned long long)t.messages, (unsigned long long)t.repeats, (unsigned long long)t.bytesDelivered);
    row("messages", b);
    snprintf(b, sizeof b, "%u bytes of %u incomplete", (unsigned)t.partialBytes, (unsigned)t.partialMessages);
    row("in progress", b);

    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", "Bursts per profile");
    uint64_t mx = 1;
    for (uint64_t v : t.profileBursts) mx = std::max(mx, v);
    for (int i = 0; i < 9; i++) {
        ImGui::TextDisabled("%s", kProfileNames[i]);
        kvColumn(130 * gUi);
        snprintf(b, sizeof b, "%llu", (unsigned long long)t.profileBursts[i]);
        ImGui::ProgressBar((float)t.profileBursts[i] / (float)mx, ImVec2(160 * gUi, 0), b);
    }

    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", "Constellation of the last burst");
    if (t.constellation.empty()) ImGui::TextDisabled("%s", "no data burst yet");
    else constellation(t);

    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", "Messages");
    if (t.packets.empty()) { ImGui::TextDisabled("%s", "none received yet"); return; }
    if (ImGui::BeginTable("kappPackets", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("time", ImGuiTableColumnFlags_WidthFixed, 50 * gUi);
        ImGui::TableSetupColumn("from", ImGuiTableColumnFlags_WidthFixed, 80 * gUi);
        ImGui::TableSetupColumn("bytes", ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
        ImGui::TableSetupColumn("profile / Es/N0", ImGuiTableColumnFlags_WidthFixed, 110 * gUi);
        ImGui::TableSetupColumn("content");
        ImGui::TableHeadersRow();
        for (const dect2::KappPacket& p : t.packets) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(p.atSec).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.callsign.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)p.bytes);
            ImGui::TableNextColumn(); ImGui::Text("%d / %.1f dB", p.profile, (double)p.snrDb);
            ImGui::TableNextColumn();
            if (p.binary) ImGui::TextDisabled("(binary data) %s", p.preview.c_str());
            else ImGui::TextWrapped("%s", p.preview.c_str());
        }
        ImGui::EndTable();
    }
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "KAPP";
    if (live(a)) l2 = dect2::kappSummary(a.rx.kapp);
}

} // namespace

extern const ModeUi kKappUi;
const ModeUi kKappUi = {
    .sideTitle = "PROFILES",
    .tabName = "KAPP",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .summary = summary,
};

namespace txui {

namespace {
void clampText(char* buf, size_t keep) { if (strlen(buf) > keep) buf[keep] = 0; }
}

// KAPP transmit options in the TX panel. The callsign, profile, passes and file are saved in the settings.
void kappPanel(TxUi& s, bool running) {
    plat::Prefs& p = plat::basePrefs();
    if (!s.kappLoaded) {
        s.kappLoaded = true;
        snprintf(s.callsign, sizeof s.callsign, "%s", p.getS("tx.kapp.callsign", s.callsign).c_str());
        snprintf(s.text, sizeof s.text, "%s", p.getS("tx.kapp.text", s.text).c_str());
        snprintf(s.kappFile, sizeof s.kappFile, "%s", p.getS("tx.kapp.file", "").c_str());
        s.kappProfile = std::max(0, std::min(8, (int)p.getI("tx.kapp.profile", 2)));
        s.kappPasses = std::max(1, std::min(9, (int)p.getI("tx.kapp.passes", 2)));
    }
    ImGui::TextWrapped("%s", "KAPP: packet data, 100 kHz wide. The message goes out as bursts of blocks, one-way: no acknowledgement is waited for, so the whole message is sent "
                              "several times (a receiver that missed a block gets it on the next pass; one that has the message ignores the repeat). "
                              "Empty callsign sends the demo identity ONAIR; an empty message sends a short demo text. OnAir sets no limit on the frequency or the power.");
    ImGui::BeginDisabled(running);
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::InputText("Callsign (7 characters at most)", s.callsign, sizeof s.callsign, ImGuiInputTextFlags_CharsUppercase)) clampText(s.callsign, 7);
    ImGui::SetNextItemWidth(std::max(220.f * gUi, ImGui::GetContentRegionAvail().x - 160 * gUi));
    ImGui::InputText("Message (text)", s.text, sizeof s.text);
    ImGui::SetNextItemWidth(std::max(220.f * gUi, ImGui::GetContentRegionAvail().x - 260 * gUi));
    ImGui::InputText("File (sent instead of the text)", s.kappFile, sizeof s.kappFile);
    ImGui::SameLine();
    if (ImGui::Button("Browse...##kappfile")) { const std::string f = plat::openFileDialog(); if (!f.empty()) snprintf(s.kappFile, sizeof s.kappFile, "%s", f.c_str()); }
    ImGui::SameLine();
    if (ImGui::Button("Clear##kappfile")) s.kappFile[0] = 0;
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::BeginCombo("Profile", kProfileNames[std::max(0, std::min(8, s.kappProfile))])) {
        for (int i = 0; i < 9; i++) {
            char lab[96];
            snprintf(lab, sizeof lab, "%s   (needs about %+.1f dB Es/N0)", kProfileNames[i], (double)dect2::kapp::kProfiles[(size_t)i].needEsN0Db);
            if (ImGui::Selectable(lab, s.kappProfile == i)) s.kappProfile = i;
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "The profile is fixed for the whole transmission: with no return link there is nothing to step it by. A low profile reaches further, a high one is faster.");
    ImGui::SetNextItemWidth(160 * gUi);
    ImGui::SliderInt("Passes", &s.kappPasses, 1, 9);
    ImGui::EndDisabled();

    size_t bytes = strlen(s.text);
    if (s.kappFile[0]) {
        std::error_code ec;
        const auto sz = std::filesystem::file_size(s.kappFile, ec);
        bytes = ec ? 0 : (size_t)sz;
        if (ec) ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1), "%s", "The file cannot be read.");
    }
    if (bytes == 0) bytes = 75;   // the demo text
    const double sec = dect2::kappAirtimeSeconds(bytes, s.kappProfile, s.kappPasses);
    ImGui::TextDisabled("%zu bytes, about %d:%02d of airtime for %d pass%s (an estimate)", bytes, (int)sec / 60, (int)sec % 60, s.kappPasses, s.kappPasses == 1 ? "" : "es");
    if (bytes > ((size_t)1 << 23)) ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1), "%s", "A KAPP message is at most 8 MiB.");

    char sig[900];
    snprintf(sig, sizeof sig, "%s|%s|%s|%d|%d", s.callsign, s.text, s.kappFile, s.kappProfile, s.kappPasses);
    if (s.kappSaved != sig) {
        s.kappSaved = sig;
        p.setS("tx.kapp.callsign", s.callsign); p.setS("tx.kapp.text", s.text); p.setS("tx.kapp.file", s.kappFile);
        p.setI("tx.kapp.profile", s.kappProfile); p.setI("tx.kapp.passes", s.kappPasses);
    }
}

} // namespace txui
