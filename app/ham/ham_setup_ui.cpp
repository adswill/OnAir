// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "ease/ease_ui.h"
// The Ham view's station setup wizard, the in-app Ham guide and the keyboard shortcut table: see ham_ops_ui.h.
#include "ham_ops_ui.h"
#include "ham_station_ui.h"
#include "../platform/platform.h"
#include "dect2/geo/maidenhead.h"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>

using namespace dect2;

namespace {
int suggestRegion(const char* grid) {
    double la, lo;
    if (!fromMaidenhead(grid, la, lo)) return 0;
    if (lo < -25) return 2;                                   // the Americas
    if ((lo > 60 && la < 45) || (lo > 90 && la < 10)) return 3;   // south and east Asia, Oceania
    return 1;                                                 // Europe, Africa, the Middle East, northern Asia
}
const char* kRegion[] = {"not chosen", "IARU Region 1 (Europe, Africa, Middle East, northern Asia)", "IARU Region 2 (the Americas)",
                         "IARU Region 3 (south and east Asia, Oceania)"};
}  // namespace

void hamSetupWizard(App& a, char* call, size_t callN, char* grid, size_t gridN, bool& licence, const std::function<void()>& rigWidgets,
                    const std::function<void()>& saveStation) {
    (void)a;
    HamOps& o = hamOps();
    static int step = 0;
    static char lic[32] = {}, pwr[12] = {}, ant[64] = {};
    if (o.openWizard) {
        o.openWizard = false;
        step = 0;
        snprintf(lic, sizeof lic, "%s", o.licClass.c_str()); snprintf(pwr, sizeof pwr, "%s", o.power.c_str()); snprintf(ant, sizeof ant, "%s", o.antenna.c_str());
        ImGui::OpenPopup("##hamsetup");
    }
    ImGui::SetNextWindowSize(ImVec2(620 * gUi, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("##hamsetup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    static const char* kSteps[] = {"Station", "Licence and band plan", "Power and antenna", "Rig and audio", "Done"};
    for (int i = 0; i < 5; i++) {
        if (i) { ImGui::SameLine(); ImGui::TextDisabled(">"); ImGui::SameLine(); }
        if (i == step) ImGui::TextColored(ImVec4(0.4f, 0.8f, 1, 1), "%d %s", i + 1, tr(kSteps[i])); else ImGui::TextDisabled("%d %s", i + 1, tr(kSteps[i]));
    }
    ImGui::Separator();
    bool canNext = true;
    const ImGuiInputTextFlags U = ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank;
    switch (step) {
    case 0: {
        ImGui::TextWrapped("%s", tr("Your callsign and your grid locator go into every message and every log entry."));
        {   // once: what the rest of the program already knows, for the fields still blank
            std::string cs = call, gs = grid;
            if (easeui::stationFillOnce(a, "stationFilled.ham", cs, gs)) { snprintf(call, callN, "%s", cs.c_str()); snprintf(grid, gridN, "%s", gs.c_str()); }
        }
        ImGui::SetNextItemWidth(140 * gUi);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputText(tr("Callsign"), call, callN, U);
        ImGui::SetNextItemWidth(100 * gUi);
        ImGui::InputText(tr("Grid"), grid, gridN, U);
        if (receiverLocationSet()) {   // 4.0: the locator from the receiver position (View > Map, GPS)
            double la = 0, lo = 0;
            receiverLocation(la, lo);
            const std::string g = toMaidenhead(la, lo, 6);
            if (g != grid) { ImGui::SameLine(); if (ImGui::SmallButton((std::string(tr("From my position: ")) + g).c_str())) snprintf(grid, gridN, "%s", g.c_str()); }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("4 or 6 characters, e.g. KO24 or KO24pq. The Map view of OnAir shows the locator of any place."));
        const bool cOk = ham::hamCallValid(call), gOk = ham::hamGridValid(grid);
        if (call[0] && !cOk) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", tr("That does not look like a callsign."));
        if (grid[0] && !gOk) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", tr("A grid is 2 letters, 2 digits and optionally 2 letters (KO24 / KO24pq)."));
        if (cOk) {
            const ham::DxccHit h = o.dxcc.lookup(call);
            if (h.e) ImGui::TextDisabled("%s: %s, CQ %d, ITU %d", tr("Callsign of"), h.e->name.c_str(), h.cqz, h.ituz);
        }
        canNext = cOk && gOk;
        break;
    }
    case 1: {
        ImGui::TextWrapped("%s", tr("OnAir transmits only after you press Arm. It warns you when no licence is set or the signal lies outside an amateur band or your country's plan, and then leaves the decision to you: you are responsible for being allowed to transmit. Your own licence class and band plan are yours to follow."));
        ImGui::TextWrapped("%s", tr("You are also responsible for your hardware: overdriving the output or a long duty cycle can damage the radio. While you transmit, and for 10 minutes after, nothing is uploaded to the OnAir map server."));
        ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::BeginCombo(tr("Licence class"), lic[0] ? lic : tr("choose or type"))) {
            for (const char* c : {"Full (CEPT / HAREC)", "Extra", "General", "Intermediate", "Technician", "Foundation / Novice", "Other"})
                if (ImGui::Selectable(c, !strcmp(lic, c))) snprintf(lic, sizeof lic, "%s", c);
            ImGui::EndCombo();
        }
        ImGui::SameLine(); ImGui::SetNextItemWidth(160 * gUi);
        ImGui::InputText("##licfree", lic, sizeof lic);
        if (o.region == 0) o.region = suggestRegion(grid);
        ImGui::SetNextItemWidth(420 * gUi);
        ImGui::Combo(tr("Band plan"), &o.region, kRegion, 4);
        if (const int s = suggestRegion(grid); s && s != o.region) { ImGui::SameLine(); if (ImGui::SmallButton(tr("Use the region of my grid"))) o.region = s; }
        ImGui::TextDisabled("%s", tr("The band bar warns when the dial is outside your region's band."));
        hamStationWizardExtras(grid, gridN);   // 4.0: country (sets the region), the grid from your position, antennas
        ImGui::Checkbox(tr("I hold an amateur licence for this callsign and will follow its rules"), &licence);
        if (!licence) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", tr("Without it OnAir warns you when you transmit: you are responsible for being allowed to."));
        canNext = true;   // receiving and logging work without a licence
        break;
    }
    case 2:
        ImGui::TextWrapped("%s", tr("Written into the log (TX_PWR) and shown to you; OnAir does not set the rig's power."));
        ImGui::SetNextItemWidth(80 * gUi); ImGui::InputText(tr("Power W"), pwr, sizeof pwr, ImGuiInputTextFlags_CharsDecimal);
        ImGui::SetNextItemWidth(320 * gUi); ImGui::InputText(tr("Antenna"), ant, sizeof ant);
        if (ImGui::CollapsingHeader(trw("Country file"))) hamCountryUi();
        if (ImGui::CollapsingHeader(trw("Callsign lookup (optional)"))) hamCallbookSettings();
        break;
    case 3:
        ImGui::TextWrapped("%s", tr("PTT: a Hamlib rig keys the transmitter over CAT; none means VOX (the rig keys on the audio). Audio out is the sound card that feeds the rig. Receive audio comes from the radio source chosen in the toolbar (an SDR, or the rig's sound card as an audio source)."));
        if (rigWidgets) rigWidgets();
        ImGui::TextDisabled("%s", tr("Start with a low level and raise it until the rig shows a little power and no ALC."));
        break;
    default:
        ImGui::Text("%s  %s   %s %s", call, grid, tr("licence class"), lic[0] ? lic : "-");
        ImGui::Text("%s", kRegion[std::max(0, std::min(3, o.region))]);
        ImGui::Text("%s %s W   %s %s", tr("power"), pwr[0] ? pwr : "-", tr("antenna"), ant[0] ? ant : "-");
        ImGui::TextWrapped("%s", tr("Next: pick a band in the band bar, wait for the decodes, then double click a CQ (or press F7 to answer the best one). Arm TX before the first transmission. F1 opens the guide."));
        break;
    }
    ImGui::Separator();
    if (step > 0 && ImGui::Button(tr("Back"), ImVec2(90 * gUi, 0))) step--;
    if (step > 0) ImGui::SameLine();
    ImGui::BeginDisabled(!canNext);
    if (ImGui::Button(step < 4 ? tr("Next") : tr("Finish"), ImVec2(90 * gUi, 0)) || (canNext && ImGui::IsKeyPressed(ImGuiKey_Enter) && step != 1)) {
        o.licClass = lic; o.power = pwr; o.antenna = ant;
        if (step < 4) step++;
        else { o.setupDone = true; ImGui::CloseCurrentPopup(); }
        hamSaveStation();
        if (saveStation) saveStation();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Close"), ImVec2(90 * gUi, 0))) { o.licClass = lic; o.power = pwr; o.antenna = ant; hamSaveStation(); if (saveStation) saveStation(); ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

void hamShortcutsHelp() {
    static const char* k[][2] = {
        {"Esc", "Halt TX: stop at once, disarm, PTT off (works everywhere in the Ham view)"},
        {"F1", "this guide"}, {"F2", "station setup"}, {"F3", "logbook"}, {"F4", "clear the DX call (stop the QSO, the rest stays)"},
        {"F5", "map"}, {"F6", "call CQ"}, {"F7", "answer the best CQ of the last slot (new DXCC first)"}, {"F8", "spots"},
        {"Enter", "call the station picked in the decode list; in the log: edit the QSO"}, {"Up / Down", "move through the decode list"},
        {"Alt+Up / Alt+Down", "next band up / down"}, {"Ctrl+L", "log a QSO by hand"}, {"Ctrl+F", "search the log"},
        {"Ctrl+Z / Ctrl+Y", "undo / redo the last log change"}, {"Del", "delete the picked QSO (undo brings it back)"},
        {"Ctrl+1 .. Ctrl+6", "send message Tx1 .. Tx6 in the next slot"},
    };
    if (!ImGui::BeginTable("##keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) return;
    for (auto& r : k) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(r[0]); ImGui::TableNextColumn(); ImGui::TextUnformatted(tr(r[1])); }
    ImGui::EndTable();
    ImGui::TextDisabled("%s", tr("Arm TX has no shortcut on purpose: it is always a click."));
}

void hamGuideTab(App& a) {
    (void)a;
    ImGui::BeginChild("##hamguide");
    auto sec = [](const char* title, const char* body, bool open = false) {
        if (ImGui::CollapsingHeader(trw(title), open ? ImGuiTreeNodeFlags_DefaultOpen : 0)) { ImGui::Indent(); ImGui::TextWrapped("%s", tr(body)); ImGui::Unindent(); ImGui::Spacing(); }
    };
    sec("Getting started",
        "1. Station setup (F2): callsign, grid, licence, band plan, power, rig and audio.\n"
        "2. Receive: choose the radio source in the toolbar (an SDR, or the rig's sound card), then a band in the band bar of the Operate tab. "
        "The decodes appear in the list after the first slot.\n"
        "3. Answer a CQ: double click it, press Call on the picked station, or F7 for the best CQ of the last slot. Calling CQ yourself: CQ or F6.\n"
        "4. Arm TX. Nothing is sent before that. Auto-seq answers the replies (report, R-report, RR73, 73) and logs the QSO.", true);
    sec("Safety",
        "Every transmission needs Arm pressed (never saved, cleared by Halt and by the watchdog). No licence set, a signal (dial + audio frequency + bandwidth) "
        "outside the amateur bands or your country's plan, or an odd callsign or grid are warnings: OnAir says so and still sends, because the decision and "
        "the responsibility are yours. Halt TX (Esc) stops at once and releases PTT. The "
        "watchdog halts after 6 minutes without an operator action; a call is given up after its retries. The band bar warns when the dial is outside "
        "your IARU region's band; your national rules and licence class are yours to follow.");
    sec("The decode list",
        "Colours: red, a message to you; magenta, a new DXCC entity; pink, an entity new on this band; blue, a new grid; green, a CQ; grey, a "
        "station already worked on this band and mode. The Info column says why. A right click offers call, look up, show on the map. Contest mode "
        "marks dupes with DUPE.");
    sec("Auto-sequence and the timeline",
        "The steps Tx1 (grid) .. Tx5 (73), or Tx6 (CQ), are shown above the messages with the current one lit, and the message of the next slot. "
        "TX slot even or odd is the parity you transmit in; calling a station takes the other one by itself. Hold keeps your TX audio frequency when "
        "you call someone. The timeline shows the last and next slots: blue received, red sent, grey outline your next transmissions. "
        "Ctrl+1 .. Ctrl+6 sends one of the six messages in the next slot instead of the automatic one; a free text is cut to 13 characters.");
    sec("Logbook",
        "Every finished QSO is saved at once in onair_ham.adi (ADIF, in the OnAir data folder). Search by any word, or field:value (band:20m "
        "mode:FT8 call:K1 dxcc:291 cont:EU state:TX qsl:n). Double click or Enter edits, Del deletes, Ctrl+Z undoes. Import reads any program's "
        "ADIF and skips dupes; export writes the QSOs shown. LoTW: export and sign the file with TQSL.");
    sec("Awards",
        "DXCC, WAS, grids and CQ zones: worked and confirmed per band and per mode class (CW, phone, digital), with a mode and band filter. Confirmed "
        "means QSL received or LoTW; mark it with a right click in the log or import the confirmations from an ADIF file. Statistics: QSOs per band, "
        "mode, continent and hour, the longest distance.");
    sec("Country file",
        "The callsign lookup uses a built-in table of about 200 entities written for OnAir from the public prefix allocations. For all 340, load a "
        "cty.dat file you have (Logbook > Country file); OnAir never downloads it.");
    sec("Contest mode",
        "Choose the contest and your exchange; every logged QSO gets the contest id, the sent exchange and the serial number. The dupe sheet lists "
        "the calls per band, the decode list and the QSO dialog mark dupes, and Export Cabrillo writes the log for the contest robot.");
    sec("Map and spots",
        "The map shows the stations heard with a grid, coloured as the decode list, the great-circle path (short and, if chosen, long) to the "
        "station picked and to the running QSO with distance and bearing, and the grayline (night side shaded, the terminator, the sun). Spots lists "
        "every station heard in the last hour from your own decodes; nothing is fetched from the network.");
    sec("Cluster (online spots)",
        "DX cluster, Reverse Beacon Network, POTA, SOTA and PSK Reporter, each off until you switch it on (the cluster and RBN ask for your "
        "callsign; PSK Reporter is asked who heard your callsign). The list marks what each station would add to your log (new DXCC, band, "
        "mode, zone, state, grid, prefix); a click tunes there and puts the call in the QSO panel. Alerts: needed stations and your watch list.");
    sec("Callsign lookup",
        "The QSO panel shows the name, QTH and grid of the station from your earlier QSOs and the local cache, and from QRZ.com or HamQTH when "
        "you enter your own account in the station setup. The account stays on this computer and goes only to that service.");
    sec("Activations (POTA, SOTA, WWFF, IOTA)",
        "Activate tab: your reference goes into every QSO logged while the activation runs, with the count against the program's minimum and "
        "the POTA file / SOTA CSV for you to upload. A station picked from a POTA or SOTA spot gets its reference in the log.");
    sec("Contests on FT8 / FT4",
        "Operate: choose ARRL Field Day or RTTY Roundup and your exchange. The sequence sends the exchange instead of grid and report, and the "
        "log keeps it for the Cabrillo export.");
    sec("Macros",
        "The Messages box keeps your own texts. {MYCALL} {MYGRID} {CALL} {GRID} {RST} {SNR} {SERIAL} {EXCH} {NAME} {QTH} are filled in. Click "
        "Send next to send one in the next slot (FT8 free text: 13 characters).");
    if (ImGui::CollapsingHeader(trw("Keyboard shortcuts"), ImGuiTreeNodeFlags_DefaultOpen)) { ImGui::Indent(); hamShortcutsHelp(); ImGui::Unindent(); }
    ImGui::EndChild();
}
