// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAWS screens: the receiver panel (state, the preamble indicator for every slotted profile, the audio spectrum, the chat window with a compose box that
// continues in the TX view, the beacon list) and the transmit options (callsign, grid, recipient, message, kind, profile, passes, slot timing) that the TX
// panel draws for the KAWS signal. English texts only. KAWS is simulation-only: it has never been on the air.
#include "app.h"
#include "../panels/tx_state.h"
#include "dect2/kaws/kaws_tel.h"
#include "dect2/kaws/kaws_txgen.h"
#include "dect2/kaws/kaws_phy.h"
#include "dect2/kaws/kaws_sched.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace {

bool live(const App& a) { return a.rx.standard == 134; }   // the engine reports its standard code minus one

// Simulated thresholds (rust/onair_kaws, KAWS-MEASURED-v0.1): block error 10 % on the AWGN channel, SNR in 2.4 kHz. Not a measurement on a radio.
const double kSimAwgnDb[6] = {-22.4, -19.2, -16.1, -13.6, -9.6, -3.0};

const char* stateName(int s) { return s == 2 ? "receiving" : s == 1 ? "listening" : "no samples"; }

std::string hms(double utc) {
    if (utc <= 0) return "--:--:--";
    const std::time_t t = (std::time_t)utc;
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char b[16];
    snprintf(b, sizeof b, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return b;
}

std::string profileLabel(int p) {
    if (p < 0 || p >= dect2::kaws::kNumProfiles) return "none yet";
    char b[96];
    snprintf(b, sizeof b, "P%d %s (%s)", p, dect2::kaws::kProfiles[p].name, dect2::kaws::kProfiles[p].modulation);
    return b;
}

void spectrumView(const dect2::KawsTelemetry& t) {
    const float w = std::max(240.f * gUi, ImGui::GetContentRegionAvail().x - 8 * gUi), h = 90 * gUi;
    ImGui::Dummy(ImVec2(w, h));
    const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(18, 20, 26, 255));
    const int nb = (int)t.spectrum.size();
    if (nb > 0) {
        const float bw = (p1.x - p0.x) / (float)nb;
        for (int i = 0; i < nb; i++) {
            const float v = std::max(0.f, std::min(1.f, (t.spectrum[(size_t)i] + 60.f) / 60.f));
            const float hz = (float)i * 50.f + 25.f;
            const bool inBand = hz >= 300.f && hz <= 2700.f;   // the 2.4 kHz of KAWS sit at 300 .. 2700 Hz of audio
            dl->AddRectFilled(ImVec2(p0.x + bw * (float)i, p1.y - v * (p1.y - p0.y)), ImVec2(p0.x + bw * (float)(i + 1) - 1.f, p1.y),
                              inBand ? IM_COL32(120, 200, 255, 210) : IM_COL32(110, 114, 126, 190));
        }
    }
    dl->AddRect(p0, p1, IM_COL32(90, 94, 108, 255));
    const float x300 = p0.x + (p1.x - p0.x) * (300.f / 3000.f), x2700 = p0.x + (p1.x - p0.x) * (2700.f / 3000.f);
    dl->AddLine(ImVec2(x300, p0.y), ImVec2(x300, p1.y), IM_COL32(200, 170, 90, 160));
    dl->AddLine(ImVec2(x2700, p0.y), ImVec2(x2700, p1.y), IM_COL32(200, 170, 90, 160));
    dl->AddText(ImVec2(p0.x + 3, p0.y + 2), IM_COL32(150, 154, 166, 255), "0");
    dl->AddText(ImVec2(p1.x - 36 * gUi, p0.y + 2), IM_COL32(150, 154, 166, 255), "3 kHz");
}

void tab(App& a) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("KAWS is an amateur HF weak-signal mode, 2.4 kHz wide, for chat messages, beacons and (with a speech codec) voice. Tune the dial frequency in "
                        "upper sideband: the audio is read from the radio's signal. A burst is sent in a UTC slot (15 s to 120 s long depending on the profile) starting "
                        "1.5 s after the slot begins, so this receiver takes its slot grid from the computer clock, which must be UTC-accurate (NTP). It looks for the "
                        "woven-lattice preamble after each slot has ended, so a message shows up some seconds after its slot, and after up to two minutes for the weakest "
                        "profile. It only listens and sends no acknowledgement. KAWS is simulation-only: it has never been tried on a real HF path.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }

    const dect2::KawsTelemetry& t = a.rx.kaws;
    auto row = [](const char* k, const std::string& v) {
        ImGui::TextDisabled("%s", k);
        kvColumn(130 * gUi);
        ImGui::TextUnformatted(v.c_str());
    };
    char b[200];
    ImGui::TextColored(t.state == 2 ? ImVec4(0.45f, 0.85f, 0.5f, 1) : t.state == 1 ? ImVec4(0.95f, 0.8f, 0.4f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "%s", stateName(t.state));
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear##kaws")) a.engine.kaws().clearLists();
    ImGui::Spacing();
    row("UTC (computer)", hms(t.utcNow));
    row("profile", profileLabel(t.profile));
    snprintf(b, sizeof b, "%.1f dB in 2.4 kHz", (double)t.snrDb);
    row("SNR estimate", t.profile >= 0 ? b : "-");
    snprintf(b, sizeof b, "%+.1f Hz", (double)t.cfoHz);
    row("frequency offset", t.profile >= 0 ? b : "-");
    snprintf(b, sizeof b, "%+.2f s from slot start + 1.5 s", t.clockErrSec);
    row("slot timing", t.profile >= 0 ? b : "-");
    snprintf(b, sizeof b, "%llu good, %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    row("blocks", b);
    snprintf(b, sizeof b, "%llu chat, %llu beacons, %llu data frames", (unsigned long long)t.messages, (unsigned long long)t.beacons, (unsigned long long)t.dataFrames);
    row("received", b);
    snprintf(b, sizeof b, "%llu slot windows searched, %llu with a preamble", (unsigned long long)t.slotsTried, (unsigned long long)t.latticeHits);
    row("searches", b);

    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", "Preamble (woven lattice)");
    ImGui::TextColored(t.syncLock ? ImVec4(0.45f, 0.85f, 0.5f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "%s", t.syncLock ? "signature found in a recent slot" : "no signature in a recent slot");
    for (int p = 0; p < 6; p++) {
        const dect2::kaws::ProfileInfo& P = dect2::kaws::kProfiles[p];
        char lab[96];
        snprintf(lab, sizeof lab, "P%d %s, %d s slots", p, P.name, P.slotSec);
        ImGui::TextDisabled("%s", lab);
        kvColumn(230 * gUi);
        const bool seen = t.latticeSeen[p];
        const bool tried = t.latticeAgeSec[p] >= 0;
        if (!tried) ImGui::TextDisabled("%s", "not searched yet");
        else {
            ImGui::TextColored(seen ? ImVec4(0.45f, 0.85f, 0.5f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "%s", seen ? "found" : "not found");
            ImGui::SameLine();
            ImGui::TextDisabled("statistic %.1f, %d s ago", (double)t.latticeStat[p], (int)t.latticeAgeSec[p]);
        }
    }

    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", "Signal (audio spectrum)");
    spectrumView(t);
    ImGui::TextDisabled("%s", "The KAWS band is 300 to 2700 Hz of audio (the gold lines).");

    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", "Chat");
    if (t.chats.empty()) ImGui::TextDisabled("%s", "no message received yet");
    if (ImGui::BeginChild("kawsChat", ImVec2(0, std::min(220.f * gUi, 28.f * gUi + 20.f * gUi * (float)t.chats.size())), true)) {
        for (const dect2::KawsChat& c : t.chats) {
            ImGui::TextDisabled("%s", hms(c.utc).c_str());
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.f, 1), "%s", c.from.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("to %s  P%d  %.0f dB", c.to.c_str(), c.profile, (double)c.snrDb);
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(c.text.c_str());
            ImGui::PopTextWrapPos();
        }
    }
    ImGui::EndChild();

    // compose: the message is handed to the TX view, where the radio or the IQ file is chosen and the transmission is started on purpose
    static char to[16] = {}, msg[128] = {};
    ImGui::SetNextItemWidth(110 * gUi);
    ImGui::InputText("To##kawsto", to, sizeof to, ImGuiInputTextFlags_CharsUppercase);
    ImGui::SetNextItemWidth(std::max(220.f * gUi, ImGui::GetContentRegionAvail().x - 240 * gUi));
    ImGui::InputText("Message##kawsmsg", msg, sizeof msg);
    ImGui::SameLine();
    if (ImGui::Button("Compose in the TX view##kaws")) {
        txui::TxUi& s = txui::ui();
        snprintf(s.dest, sizeof s.dest, "%s", to);
        snprintf(s.text, sizeof s.text, "%s", msg);
        s.kawsKind = 0;
        s.kawsLoaded = true;
        txSelectMode(a, 133, nullptr);
        txViewFlag() = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Opens the TX view with KAWS chat and this text. Nothing is sent until you start the transmission there, with a radio you choose or an IQ file.");

    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", "Beacons");
    if (t.beaconList.empty()) { ImGui::TextDisabled("%s", "none received yet"); }
    else if (ImGui::BeginTable("kawsBeacons", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("UTC", ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
        ImGui::TableSetupColumn("call", ImGuiTableColumnFlags_WidthFixed, 90 * gUi);
        ImGui::TableSetupColumn("grid", ImGuiTableColumnFlags_WidthFixed, 60 * gUi);
        ImGui::TableSetupColumn("power", ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
        ImGui::TableSetupColumn("P / SNR", ImGuiTableColumnFlags_WidthFixed, 90 * gUi);
        ImGui::TableSetupColumn("text / position");
        ImGui::TableHeadersRow();
        for (const dect2::KawsBeaconEntry& e : t.beaconList) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(hms(e.utc).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.call.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.grid.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%+.1f dBm", e.powerDbm);
            ImGui::TableNextColumn(); ImGui::Text("P%d / %.0f dB", e.profile, (double)e.snrDb);
            ImGui::TableNextColumn();
            if (e.full) ImGui::Text("%.4f, %.4f  %s", e.lat, e.lon, e.text.c_str());
            else ImGui::TextDisabled("short beacon");
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", "Voice (P6, P7)");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", "Voice codec not installed. The voice profiles need a speech codec that you install yourself (docs/CODECS.md); this receiver does not decode voice superframes, so no voice is reported.");
    ImGui::PopTextWrapPos();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "KAWS";
    if (live(a)) l2 = dect2::kawsSummary(a.rx.kaws);
}

} // namespace

extern const ModeUi kKawsUi;
const ModeUi kKawsUi = {
    .sideTitle = "PROFILES",
    .tabName = "KAWS",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .summary = summary,
};

namespace txui {

namespace {
void clampText(char* buf, size_t keep) { if (strlen(buf) > keep) buf[keep] = 0; }
const char* const kKindNames[3] = {"Chat message", "Short beacon", "Full beacon"};
}

// KAWS transmit options in the TX panel. Everything is saved in the settings.
void kawsPanel(TxUi& s, bool running) {
    plat::Prefs& p = plat::basePrefs();
    if (!s.kawsLoaded) {
        s.kawsLoaded = true;
        snprintf(s.callsign, sizeof s.callsign, "%s", p.getS("tx.kaws.callsign", s.callsign).c_str());
        snprintf(s.grid, sizeof s.grid, "%s", p.getS("tx.kaws.grid", s.grid).c_str());
        snprintf(s.dest, sizeof s.dest, "%s", p.getS("tx.kaws.dest", s.dest).c_str());
        snprintf(s.text, sizeof s.text, "%s", p.getS("tx.kaws.text", s.text).c_str());
        s.kawsKind = std::max(0, std::min(2, (int)p.getI("tx.kaws.kind", 0)));
        s.kawsProfile = std::max(0, std::min(5, (int)p.getI("tx.kaws.profile", 3)));
        s.kawsPasses = std::max(1, std::min(9, (int)p.getI("tx.kaws.passes", 2)));
        s.kawsEirp = std::max(-40, std::min(60, (int)p.getI("tx.kaws.eirp", 30)));
        s.kawsAlign = p.getB("tx.kaws.align", true);
    }
    ImGui::TextWrapped("%s", "KAWS: HF weak-signal chat and beacons, 2.4 kHz wide, sent as upper-sideband audio (the dial frequency is the carrier of an SSB transmitter; "
                              "the signal sits 300 to 2700 Hz above it). A burst starts 1.5 s into a UTC slot of the profile, so the computer clock must be UTC-accurate (NTP). "
                              "The message goes out one-way, in the caller's (even) slots, and the whole message is sent several times: there is no acknowledgement. "
                              "KAWS is simulation-only: it has never been on the air. Empty callsign sends the demo identity ONAIR. OnAir sets no limit on the frequency or the power.");
    ImGui::BeginDisabled(running);
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::BeginCombo("Kind", kKindNames[std::max(0, std::min(2, s.kawsKind))])) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(kKindNames[i], s.kawsKind == i)) s.kawsKind = i;
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::InputText("Callsign (9 characters at most)", s.callsign, sizeof s.callsign, ImGuiInputTextFlags_CharsUppercase)) clampText(s.callsign, 9);
    ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::InputText("Grid (6 characters, like KO24RM)", s.grid, sizeof s.grid, ImGuiInputTextFlags_CharsUppercase)) clampText(s.grid, 6);
    if (s.kawsKind == 0) {
        ImGui::SetNextItemWidth(160 * gUi);
        if (ImGui::InputText("To (empty: CQ)", s.dest, sizeof s.dest, ImGuiInputTextFlags_CharsUppercase)) clampText(s.dest, 9);
    }
    if (s.kawsKind != 1) {
        ImGui::SetNextItemWidth(std::max(220.f * gUi, ImGui::GetContentRegionAvail().x - 160 * gUi));
        ImGui::InputText(s.kawsKind == 0 ? "Message (100 characters at most)" : "Text (10 characters at most)", s.text, sizeof s.text);
    }
    ImGui::SetNextItemWidth(320 * gUi);
    if (ImGui::BeginCombo("Profile", dect2::kaws::kProfiles[std::max(0, std::min(5, s.kawsProfile))].name)) {
        for (int i = 0; i < 6; i++) {
            const dect2::kaws::ProfileInfo& P = dect2::kaws::kProfiles[i];
            char lab[160];
            snprintf(lab, sizeof lab, "P%d %s, %s, %d s slots   (simulated: %+.1f dB on AWGN)", i, P.name, P.modulation, P.slotSec, kSimAwgnDb[i]);
            if (ImGui::Selectable(lab, s.kawsProfile == i)) s.kawsProfile = i;
        }
        ImGui::BeginDisabled(true);
        ImGui::Selectable("P6 Voice-700, P7 Voice-1200: voice codec not installed", false);
        ImGui::EndDisabled();
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "The profile is fixed for the whole transmission: with no return link there is nothing to step it by. The thresholds are from the simulator, not from a radio.");
    ImGui::SetNextItemWidth(160 * gUi);
    ImGui::SliderInt("Passes", &s.kawsPasses, 1, 9);
    if (s.kawsKind != 0) {
        ImGui::SetNextItemWidth(160 * gUi);
        ImGui::SliderInt("EIRP announced (dBm)", &s.kawsEirp, -40, 60);
        if (s.kawsKind == 1) ImGui::TextDisabled("%s", "The short beacon's power field spans -40 to -8.5 dBm in 0.5 dB steps; larger values are sent as -8.5 dBm.");
    }
    ImGui::Checkbox("Wait for the next slot (UTC) before the first burst", &s.kawsAlign);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "On: the file or the radio gets silence until the burst's slot starts, as a real transmission would. Off: the first burst starts at once (a receiver that follows the UTC grid will not find it).");
    ImGui::EndDisabled();

    TxRequest r;
    r.callsign = s.callsign; r.grid = s.grid; r.dest = s.dest; r.text = s.text; r.lat = s.lat; r.lon = s.lon;
    r.opt["kaws.kind"] = s.kawsKind; r.opt["kaws.profile"] = s.kawsProfile; r.opt["kaws.repeat"] = s.kawsPasses; r.opt["kaws.eirp"] = s.kawsEirp;
    dect2::KawsTxPlan plan;
    std::string err;
    if (dect2::kawsFromRequest(r, plan, err)) {
        const double sec = dect2::kawsAirtimeSeconds(plan);
        ImGui::TextDisabled("%d block%s, %d slot%s per pass (every second slot), about %d:%02d for %d pass%s (an estimate, without the wait for the first slot)", plan.blocks,
                            plan.blocks == 1 ? "" : "s", plan.slotsPerPass, plan.slotsPerPass == 1 ? "" : "s", (int)sec / 60, (int)sec % 60, plan.passes, plan.passes == 1 ? "" : "es");
    } else {
        ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1), "%s", err.c_str());
    }
    ImGui::TextDisabled("%s", "To an IQ file: pick the 12 ksps rate (or any rate from 12 ksps up) and a SigMF or WAV IQ format in the destination; the file holds the upper-sideband audio as complex samples.");

    char sig[900];
    snprintf(sig, sizeof sig, "%s|%s|%s|%s|%d|%d|%d|%d|%d", s.callsign, s.grid, s.dest, s.text, s.kawsKind, s.kawsProfile, s.kawsPasses, s.kawsEirp, s.kawsAlign ? 1 : 0);
    if (s.kawsSaved != sig) {
        s.kawsSaved = sig;
        p.setS("tx.kaws.callsign", s.callsign); p.setS("tx.kaws.grid", s.grid); p.setS("tx.kaws.dest", s.dest); p.setS("tx.kaws.text", s.text);
        p.setI("tx.kaws.kind", s.kawsKind); p.setI("tx.kaws.profile", s.kawsProfile); p.setI("tx.kaws.passes", s.kawsPasses); p.setI("tx.kaws.eirp", s.kawsEirp);
        p.setB("tx.kaws.align", s.kawsAlign);
    }
}

} // namespace txui
