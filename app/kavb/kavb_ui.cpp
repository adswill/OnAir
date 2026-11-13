// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAVB screens: the receiver panel (state, the symbol rate it listens to, frame and FEC counts, constellation, the packets that arrived, a byte counter and an
// optional file for the payload) and the transmit options (callsign, a file of bytes or the synthetic test stream, profile A to D, FEC profile) that the TX panel
// draws for the KAVB signal. English texts only. No AV1 or Opus decoder is built in: the panel says so and shows the packets as they are.
#include "app.h"
#include "../panels/tx_state.h"
#include "dect2/kavb/kavb_tel.h"
#include "dect2/kavb/kavb_txgen.h"
#include "dect2/kavb/kavb_av.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

bool live(const App& a) { return a.rx.standard == 124; }   // the engine reports its standard code minus one

const char* const kProfileNames[4] = {"A  700 ksym/s, 16-QAM 3/4", "B  1050 ksym/s, 16-QAM 3/4", "C  1400 ksym/s, 16-QAM 3/4", "D  1400 ksym/s, 64-QAM 3/4"};
const char* const kTypeNames[5] = {"video", "audio", "text", "picture", "data"};

struct Panel {
    bool loaded = false;
    int listen = 0;
    char outFile[512] = {};
    bool saving = false;
};
Panel P;

const char* stateName(int s) { return s == 2 ? "locked on frames" : s == 1 ? "searching" : "no samples"; }

std::string clock(double sec) {
    char b[24];
    snprintf(b, sizeof b, "%d:%02d", (int)sec / 60, (int)sec % 60);
    return b;
}

std::string fecName(int i) {
    const dect2::kavb::FecProfile f = dect2::kavb::fecProfile(std::max(0, std::min(dect2::kavb::kNumFecProfiles - 1, i)));
    char b[24];
    snprintf(b, sizeof b, "%d + %d", f.data, f.parity);
    return b;
}

void constellation(const dect2::KavbTelemetry& t) {
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

void tick(App& a) {
    plat::Prefs& p = plat::basePrefs();
    if (!P.loaded) {
        P.loaded = true;
        P.listen = std::max(0, std::min(3, (int)p.getI("kavb.listen", 0)));
        snprintf(P.outFile, sizeof P.outFile, "%s", p.getS("kavb.outFile", "").c_str());
    }
    a.engine.kavb().setListenProfile(P.listen);   // an atomic store; the receiver rebuilds its resampler only when the value changes
}

void tab(App& a) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("KAVB is amateur digital video, 1 to 2 MHz wide: single-carrier 16-QAM or 64-QAM frames with LDPC, a V-shaped sweep signature, and video, audio and data "
                        "packets in forward-error-correction groups. This build receives the frames and the packets but has no AV1 video or Opus audio decoder, so no picture or sound is shown: "
                        "the panel lists the packets as they arrive, counts their payload bytes and can write the payload to a file. Video decoder not installed. "
                        "The receiver listens to one symbol rate at a time (C and D share one); the profile of each frame is read from its header. To transmit, switch to TX and pick KAVB.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }

    const dect2::KavbTelemetry& t = a.rx.kavb;
    auto row = [](const char* k, const std::string& v) {
        ImGui::TextDisabled("%s", k);
        kvColumn(130 * gUi);
        ImGui::TextUnformatted(v.c_str());
    };
    char b[240];
    ImGui::TextColored(t.state == 2 ? ImVec4(0.45f, 0.85f, 0.5f, 1) : t.state == 1 ? ImVec4(0.95f, 0.8f, 0.4f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "%s", stateName(t.state));
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear##kavb")) a.engine.kavb().clearPackets();
    ImGui::Spacing();
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::BeginCombo("Listen to", kProfileNames[P.listen])) {
        for (int i = 0; i < 4; i++) {
            if (ImGui::Selectable(kProfileNames[i], P.listen == i)) { P.listen = i; plat::basePrefs().setI("kavb.listen", i); }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "The symbol rate the receiver resamples to. A frame of another symbol rate is ignored. C and D have the same rate; the header tells them apart.");
    row("profile", t.profile >= 0 ? std::string(1, (char)('A' + t.profile)) + (t.robust ? " (robust, QPSK 1/2)" : "") : std::string("none yet"));
    snprintf(b, sizeof b, "%.1f dB", (double)t.esn0Db);
    row("Es/N0", t.profile >= 0 ? b : "-");
    snprintf(b, sizeof b, "%+.1f kHz  (coarse search %+.1f kHz)", (double)t.cfoHz / 1000.0, (double)t.coarseHz / 1000.0);
    row("carrier offset", t.profile >= 0 ? b : "-");
    row("station", t.callsign.empty() ? "-" : t.callsign);
    snprintf(b, sizeof b, "%llu good, %llu with bad blocks, %llu headers rejected", (unsigned long long)t.framesGood, (unsigned long long)t.framesBad, (unsigned long long)t.headerRejects);
    row("frames", b);
    snprintf(b, sizeof b, "%llu good, %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    row("blocks", b);
    snprintf(b, sizeof b, "%s", t.profile >= 0 ? fecName(t.fecProfile).c_str() : "-");
    row("FEC group", b);
    snprintf(b, sizeof b, "%llu complete, %llu restored by FEC, %llu lost, %llu cells missing", (unsigned long long)t.groupsOk, (unsigned long long)t.groupsRecovered, (unsigned long long)t.groupsLost,
             (unsigned long long)t.cellsMissing);
    row("FEC groups", b);
    snprintf(b, sizeof b, "%llu (%llu dropped by CRC)", (unsigned long long)t.packets, (unsigned long long)t.packetsDropped);
    row("packets", b);
    snprintf(b, sizeof b, "video %llu, audio %llu, text %llu, picture %llu, data %llu", (unsigned long long)t.packetsByType[0], (unsigned long long)t.packetsByType[1],
             (unsigned long long)t.packetsByType[2], (unsigned long long)t.packetsByType[3], (unsigned long long)t.packetsByType[4]);
    row("by type", b);
    snprintf(b, sizeof b, "%llu bytes, %.1f kbit/s", (unsigned long long)t.payloadBytes, t.throughputBps / 1000.0);
    row("payload", b);
    ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.4f, 1), "%s", "Video decoder not installed: the AV1 and Opus payload is counted, not played.");

    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", "Save the payload");
    ImGui::SetNextItemWidth(std::max(220.f * gUi, ImGui::GetContentRegionAvail().x - 260 * gUi));
    ImGui::InputText("File##kavbout", P.outFile, sizeof P.outFile);
    ImGui::SameLine();
    if (ImGui::Button("Browse...##kavbout")) { const std::string f = plat::saveFileDialog("kavb_payload.bin"); if (!f.empty()) snprintf(P.outFile, sizeof P.outFile, "%s", f.c_str()); }
    if (!P.saving) {
        if (ImGui::Button("Start saving##kavb") && P.outFile[0]) {
            a.engine.kavb().setOutputFile(P.outFile);
            plat::basePrefs().setS("kavb.outFile", P.outFile);
            P.saving = true;
        }
    } else if (ImGui::Button("Stop saving##kavb")) {
        a.engine.kavb().setOutputFile("");
        P.saving = false;
    }
    ImGui::SameLine();
    if (!t.savePath.empty()) ImGui::TextDisabled("%llu bytes written to %s", (unsigned long long)t.savedBytes, t.savePath.c_str());
    else ImGui::TextDisabled("%s", "Raw payload bytes of every packet but the station ID, in arrival order (a file sent by OnAir comes back byte for byte when nothing was lost; video units are not a playable file).");
    if (!t.saveError.empty()) ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1), "%s", t.saveError.c_str());

    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", "Constellation of the last frame");
    if (t.constellation.empty()) ImGui::TextDisabled("%s", "no frame yet");
    else constellation(t);

    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", "Packets");
    if (t.recent.empty()) { ImGui::TextDisabled("%s", "none received yet"); return; }
    if (ImGui::BeginTable("kavbPackets", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("time", ImGuiTableColumnFlags_WidthFixed, 50 * gUi);
        ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
        ImGui::TableSetupColumn("stream", ImGuiTableColumnFlags_WidthFixed, 60 * gUi);
        ImGui::TableSetupColumn("seq", ImGuiTableColumnFlags_WidthFixed, 60 * gUi);
        ImGui::TableSetupColumn("bytes");
        ImGui::TableHeadersRow();
        for (const dect2::KavbPacketInfo& p : t.recent) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(p.atSec).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.type >= 0 && p.type < 5 ? kTypeNames[p.type] : "?");
            ImGui::TableNextColumn(); ImGui::Text("%d", p.stream);
            ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)p.seq);
            ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)p.bytes);
        }
        ImGui::EndTable();
    }
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "KAVB";
    if (live(a)) l2 = dect2::kavbSummary(a.rx.kavb);
}

} // namespace

extern const ModeUi kKavbUi;
const ModeUi kKavbUi = {
    .sideTitle = "PROFILES",
    .tabName = "KAVB",
    .tabIcon = Ic::Tv,
    .tab = tab,
    .summary = summary,
    .tick = tick,
};

namespace txui {

namespace {
void clampText(char* buf, size_t keep) { if (strlen(buf) > keep) buf[keep] = 0; }
}

// KAVB transmit options in the TX panel. The callsign, file, profile, FEC profile and test stream length are saved in the settings.
void kavbPanel(TxUi& s, bool running) {
    plat::Prefs& p = plat::basePrefs();
    if (!s.kavbLoaded) {
        s.kavbLoaded = true;
        snprintf(s.callsign, sizeof s.callsign, "%s", p.getS("tx.kavb.callsign", s.callsign).c_str());
        snprintf(s.kavbFile, sizeof s.kavbFile, "%s", p.getS("tx.kavb.file", "").c_str());
        s.kavbProfile = std::max(0, std::min(3, (int)p.getI("tx.kavb.profile", 0)));
        s.kavbFec = std::max(0, std::min(dect2::kavb::kNumFecProfiles - 1, (int)p.getI("tx.kavb.fec", 1)));
        s.kavbSeconds = std::max(1, std::min(600, (int)p.getI("tx.kavb.seconds", 5)));
    }
    ImGui::TextWrapped("%s", "KAVB: amateur digital video, 1 to 2 MHz wide. No AV1 or Opus encoder is built in, so a camera is not a source: the payload is either the bytes of a file, "
                              "sent as data packets (a KAVB receiver that saves the payload gets the file back), or a synthetic test stream with video- and audio-sized units. "
                              "The station ID goes out as a text packet. The transmission ends when the file or the test stream is sent. Empty callsign sends ONAIR. "
                              "OnAir sets no limit on the frequency or the power.");
    ImGui::BeginDisabled(running);
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::InputText("Callsign (7 characters at most)", s.callsign, sizeof s.callsign, ImGuiInputTextFlags_CharsUppercase)) clampText(s.callsign, 7);
    ImGui::SetNextItemWidth(std::max(220.f * gUi, ImGui::GetContentRegionAvail().x - 260 * gUi));
    ImGui::InputText("File (empty: the test stream)", s.kavbFile, sizeof s.kavbFile);
    ImGui::SameLine();
    if (ImGui::Button("Browse...##kavbfile")) { const std::string f = plat::openFileDialog(); if (!f.empty()) snprintf(s.kavbFile, sizeof s.kavbFile, "%s", f.c_str()); }
    ImGui::SameLine();
    if (ImGui::Button("Clear##kavbfile")) s.kavbFile[0] = 0;
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::BeginCombo("Profile", kProfileNames[std::max(0, std::min(3, s.kavbProfile))])) {
        for (int i = 0; i < 4; i++) if (ImGui::Selectable(kProfileNames[i], s.kavbProfile == i)) s.kavbProfile = i;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "A higher profile carries more data but needs a stronger signal and a wider channel. The radio rate must be at least 1.6 times the symbol rate.");
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::BeginCombo("FEC group (data + parity cells)", fecName(s.kavbFec).c_str())) {
        for (int i = 0; i < dect2::kavb::kNumFecProfiles; i++) if (ImGui::Selectable(fecName(i).c_str(), s.kavbFec == i)) s.kavbFec = i;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Parity cells let a receiver restore lost cells of a group. 4 + 0 is no protection.");
    if (!s.kavbFile[0]) {
        ImGui::SetNextItemWidth(160 * gUi);
        ImGui::SliderInt("Test stream (seconds)", &s.kavbSeconds, 1, 600);
    }
    ImGui::EndDisabled();

    size_t bytes = 0;
    if (s.kavbFile[0]) {
        std::error_code ec;
        const auto sz = std::filesystem::file_size(s.kavbFile, ec);
        bytes = ec ? 0 : (size_t)sz;
        if (ec) ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1), "%s", "The file cannot be read.");
    }
    const double sec = dect2::kavbAirtimeSeconds(bytes, s.kavbProfile, s.kavbFec, s.kavbSeconds);
    ImGui::TextDisabled("%s, about %d:%02d of airtime (an estimate), radio rate at least %.2f MHz", s.kavbFile[0] ? "file" : "test stream", (int)sec / 60, (int)sec % 60,
                        dect2::kavbMinRate(s.kavbProfile) / 1e6);
    if (bytes > ((size_t)64 << 20)) ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1), "%s", "A KAVB file is at most 64 MiB.");

    char sig[900];
    snprintf(sig, sizeof sig, "%s|%s|%d|%d|%d", s.callsign, s.kavbFile, s.kavbProfile, s.kavbFec, s.kavbSeconds);
    if (s.kavbSaved != sig) {
        s.kavbSaved = sig;
        p.setS("tx.kavb.callsign", s.callsign); p.setS("tx.kavb.file", s.kavbFile);
        p.setI("tx.kavb.profile", s.kavbProfile); p.setI("tx.kavb.fec", s.kavbFec); p.setI("tx.kavb.seconds", s.kavbSeconds);
    }
}

} // namespace txui
