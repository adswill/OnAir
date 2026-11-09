// NFC / RFID (13.56 MHz) screens: the frame log, the tag table (UID, type, ATQA / SAK, time seen, ATS) and decoded NDEF.
// Receive only. ISO/IEC 14443 Type A / Type B and ISO/IEC 15693 as far as the public specs allow.
#include "app.h"
#include "dect2/nfc/nfc_engine.h"
#include <algorithm>
#include <cstdio>
#include <string>

namespace {

bool live(const App& a) { return a.rx.standard == 149; }   // engine standard 150, active standard 149 (family 148)

const char* protoName(int p) { return p == 0 ? "14443-A" : p == 1 ? "14443-B" : "15693"; }

void tab(App& a) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("ISO/IEC 14443 A/B and ISO/IEC 15693 at 13.56 MHz, receive only. Reader frames are 100%% ASK (Miller); tag "
                        "answers load-modulate the 847.5 kHz (14443) or 423.75 kHz (15693) subcarrier. Needs an HF-capable receiver "
                        "at a few Msps so the subcarrier sidebands fit. Nothing is cloned and no encrypted content is decoded.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : tr("Tune to 13.56 MHz and start the receiver. NFC only carries a few centimetres: hold a loop antenna within a few cm of the reader.")); return; }

    const dect2::NfcTelemetry& t = a.rx.nfc;
    ImGui::Text("Field %.1f dBFS   subcarrier %.1f dBFS   frames %llu   CRC/parity errors %llu",
                (double)t.levelDb, (double)t.subcarrierDb, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    ImGui::Spacing();
    if (t.blocksOk == 0) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", tr("No frames yet. NFC only carries a few centimetres: hold a loop antenna within a few cm of the reader, with the card on the reader.")); ImGui::PopTextWrapPos(); ImGui::Spacing(); }

    // ---- tag table ----
    if (!t.tags.empty()) {
        ImGui::SeparatorText("Tags");
        if (ImGui::BeginTable("nfc_tags", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Borders)) {
            ImGui::TableSetupColumn("UID");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("ATQA");
            ImGui::TableSetupColumn("SAK");
            ImGui::TableSetupColumn("ATS");
            ImGui::TableSetupColumn("Seen");
            ImGui::TableHeadersRow();
            for (const dect2::NfcTag& tag : t.tags) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(tag.uid.empty() ? "-" : tag.uid.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(tag.type.c_str());
                ImGui::TableNextColumn(); if (tag.haveAtqa) ImGui::Text("0x%04X", tag.atqa); else ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); if (tag.haveSak) ImGui::Text("0x%02X", tag.sak); else ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(tag.ats.empty() ? "-" : tag.ats.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%dx", tag.seenCount);
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
    }

    // ---- decoded NDEF ----
    if (!t.ndef.empty()) {
        ImGui::SeparatorText("NDEF");
        for (const dect2::NfcNdef& n : t.ndef) {
            if (!n.uri.empty()) ImGui::Text("URI: %s", n.uri.c_str());
            else if (!n.text.empty()) ImGui::Text("Text (%s): %s", n.lang.c_str(), n.text.c_str());
            else ImGui::Text("record TNF %d, type \"%s\"", n.tnf, n.type.c_str());
        }
        ImGui::Spacing();
    }

    // ---- frame log ----
    ImGui::SeparatorText("Frame log");
    if (ImGui::BeginTable("nfc_frames", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 240 * gUi))) {
        ImGui::TableSetupColumn("Time (ms)", ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
        ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_WidthFixed, 56 * gUi);
        ImGui::TableSetupColumn("Proto", ImGuiTableColumnFlags_WidthFixed, 64 * gUi);
        ImGui::TableSetupColumn("Bytes");
        ImGui::TableSetupColumn("Decoded");
        ImGui::TableHeadersRow();
        for (auto it = t.log.rbegin(); it != t.log.rend(); ++it) {
            const dect2::NfcLogEntry& e = *it;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%.2f", e.timeSec * 1e3);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.fromReader ? "reader" : "tag");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(protoName(e.proto));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.hex.c_str());
            ImGui::TableNextColumn();
            if (e.crcChecked && !e.crcOk) ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "%s (CRC bad)", e.note.c_str());
            else if (!e.parityOk) ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1), "%s (parity bad)", e.note.c_str());
            else ImGui::TextUnformatted(e.note.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Input %.2f Msps, centre %.4f MHz", t.inputRate / 1e6, t.centerHz / 1e6);
}

void receiver(App& a) {
    if (!live(a)) return;
    const dect2::NfcTelemetry& t = a.rx.nfc;
    ImGui::Text("%s", dect2::nfcSummary(t).c_str());
    ImGui::Text("field %.1f dBFS, subcarrier %.1f dBFS", (double)t.levelDb, (double)t.subcarrierDb);
}

void list(App& a) {
    if (!live(a)) return;
    const dect2::NfcTelemetry& t = a.rx.nfc;
    for (const dect2::NfcTag& tag : t.tags) {
        ImGui::BulletText("%s", tag.uid.empty() ? "(no UID)" : tag.uid.c_str());
        ImGui::SameLine(); ImGui::TextDisabled("%s", tag.type.c_str());
    }
    if (t.tags.empty()) ImGui::TextDisabled("no tags yet");
}

void tick(App&) {}

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State");
    ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::nfcSummary(a.rx.nfc).c_str() : (a.engine.running() ? "starting" : "stopped"));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "NFC / RFID 13.56 MHz";
    if (live(a)) l2 = dect2::nfcSummary(a.rx.nfc);
}

} // namespace

extern const ModeUi kNfcUi;
const ModeUi kNfcUi = {
    .sideTitle = "TAGS",
    .tabName = "NFC",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
