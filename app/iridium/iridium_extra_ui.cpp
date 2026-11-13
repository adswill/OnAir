// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Iridium ACARS, packet dump and pager screens. Nothing here is verified against a real Iridium recording (see
// core/include/dect2/iridium/iridium_acars.h): the screens show what the decoder made of the frames, with its flags.
#include "app.h"
#include "iridium_extra_ui.h"
#include "dect2/iridium/iridium_acars.h"
#include "dect2/iridium/iridium_tel.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>

namespace iridiumui {
namespace {

using dect2::IridiumTelemetry;

struct State {
    char acText[96] = "";
    int acDir = 0;                   // 0 all, 1 from aircraft, 2 to aircraft
    bool acPosOnly = false, acProblems = false;
    uint64_t acSel = 0;              // serial of the selected message
    char pkText[96] = "";
    int pkDir = 0, pkType = 0;       // type: 0 all, then IDA, IIP, IIQ, IIR, IIU, IMS, ITL, MSG
    bool pkFrames = false, pkProblems = false, pkUnassembled = false;
    uint64_t pkSel = 0;
    char pgText[96] = "";
    int pgFmt = 0;                   // 0 all, 1 alphanumeric, 2 numeric
    bool pgComplete = false;
    std::string note;
};
State S;

bool live(const App& a) { return a.rx.standard == 20; }

std::string utc(int64_t wall) {
    const time_t s = (time_t)wall;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &s);
#else
    gmtime_r(&s, &g);
#endif
    char b[24];
    snprintf(b, sizeof b, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
bool has(const std::string& hay, const std::string& needleLower) { return needleLower.empty() || lower(hay).find(needleLower) != std::string::npos; }

void saveText(const std::string& name, const std::string& body) {
    const std::string p = saveFileDialog(name.c_str());
    if (p.empty()) return;
    std::ofstream f(std::filesystem::path(reinterpret_cast<const char8_t*>(p.c_str())), std::ios::binary);
    if (!f) { S.note = "cannot write " + p; return; }
    f << body;
    S.note = "saved " + p;
}

std::string clean(std::string s) { for (auto& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' '; return s; }

const char* regKindName(int k) { return k == 1 ? "tail" : k == 2 ? "ICAO address" : k == 3 ? "other" : "none"; }

// ---------------------------------------------------------------- ACARS

std::vector<const dect2::IridiumAcarsMsg*> acarsFiltered(const IridiumTelemetry& t) {
    std::vector<const dect2::IridiumAcarsMsg*> v;
    const std::string q = lower(S.acText);
    for (auto it = t.acars.rbegin(); it != t.acars.rend(); ++it) {
        const auto& m = *it;
        const bool fromAircraft = !m.downlink;
        if (S.acDir == 1 && !fromAircraft) continue;
        if (S.acDir == 2 && fromAircraft) continue;
        if (S.acPosOnly && !m.hasPos) continue;
        if (S.acProblems && m.crc != 2 && m.parity != 2) continue;
        if (!q.empty()) {
            char hx[8] = "";
            if (m.icao) snprintf(hx, sizeof hx, "%06X", m.icao & 0xFFFFFF);
            if (!has(m.reg + " " + m.flight + " " + m.label + " " + m.text + " " + m.labelText + " " + hx + " " + m.seq, q)) continue;
        }
        v.push_back(&m);
    }
    return v;
}

std::string acarsTsv(const std::vector<const dect2::IridiumAcarsMsg*>& v) {
    std::string s = "utc\tmhz\tdirection\treg\ticao\tflight\tmode\tack\tlabel\tblock\tcrc\tparity\tblocks\trepeats\ttext\n";
    char b[320];
    for (auto m : v) {
        char hx[8] = "";
        if (m->icao) snprintf(hx, sizeof hx, "%06X", m->icao & 0xFFFFFF);
        snprintf(b, sizeof b, "%s\t%.4f\t%s\t%s\t%s\t%s\t%c\t%c\t%s\t%c\t%s\t%s\t%d\t%d\t", utc(m->wall).c_str(), m->freqHz / 1e6, m->downlink ? "to aircraft" : "from aircraft",
                 m->reg.c_str(), hx, m->flight.c_str(), m->mode ? m->mode : ' ', m->ack >= 32 ? m->ack : '-', m->label.c_str(), m->blockId >= 32 ? m->blockId : '-',
                 m->crc == 1 ? "ok" : m->crc == 2 ? "bad" : "none", m->parity == 1 ? "ok" : m->parity == 2 ? "bad" : "none", m->blocks, m->repeats);
        s += b;
        s += clean(m->text) + "\n";
    }
    return s;
}

void filterRow(const char* id, char* text, size_t n) {
    ImGui::SetNextItemWidth(std::min(220 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint(id, "search", text, n);
}

} // namespace

void acarsView(App& a) {
    const IridiumTelemetry& t = a.rx.iridium;
    { ImGui::PushTextWrapPos(0);
      ImGui::TextDisabled("ACARS carried in Iridium short burst data. Also in Message search and the aircraft log; positions from the aircraft appear on the aircraft map as \"Iridium ACARS\". %llu packets joined, %llu repeats dropped, %llu with a bad check, %llu with bad parity. Unverified without a real recording.",
                          (unsigned long long)t.sbdPackets, (unsigned long long)t.acarsDuplicates, (unsigned long long)t.acarsCrcBad, (unsigned long long)t.acarsParityBad);
      ImGui::PopTextWrapPos(); }
    filterRow("##irac_q", S.acText, sizeof S.acText);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120 * gUi);
    ImGui::Combo("##irac_d", &S.acDir, "All\0From aircraft\0To aircraft\0");
    ImGui::SameLine(); ImGui::Checkbox("Positions##irac", &S.acPosOnly);
    ImGui::SameLine(); ImGui::Checkbox("Check failed##irac", &S.acProblems);
    const auto v = acarsFiltered(t);
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy##irac")) ImGui::SetClipboardText(acarsTsv(v).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Save...##irac")) saveText("iridium-acars.tsv", acarsTsv(v));
    if (!S.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", S.note.c_str()); }

    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float rw = std::min(340.f * gUi, W * 0.4f);
    ImGui::BeginChild("##irac_l", ImVec2(W - rw - 8 * gUi, H));
    const float th = ImGui::GetContentRegionAvail().y * 0.62f;
    if (ImGui::BeginTable("##irac_t", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable, ImVec2(0, th))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn("Dir"); ImGui::TableSetupColumn("Registration"); ImGui::TableSetupColumn("Flight");
        ImGui::TableSetupColumn("Mode"); ImGui::TableSetupColumn("Ack"); ImGui::TableSetupColumn("Label"); ImGui::TableSetupColumn("Blk");
        ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (auto m : v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID((int)(m->serial & 0x7fffffff));
            if (ImGui::Selectable(utc(m->wall).c_str(), S.acSel == m->serial, ImGuiSelectableFlags_SpanAllColumns)) S.acSel = m->serial;
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m->downlink ? "to a/c" : "from a/c");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m->reg.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m->flight.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%c", m->mode ? m->mode : ' ');
            ImGui::TableNextColumn(); ImGui::Text("%c", m->ack == 0x15 ? '!' : m->ack == 0x06 ? '^' : (m->ack >= 32 ? m->ack : '-'));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m->label.c_str());
            if (!m->labelText.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m->labelText.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%c", m->blockId >= 32 ? m->blockId : '-');
            ImGui::TableNextColumn();
            const bool bad = m->crc == 2 || m->parity == 2;
            if (bad) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.65f, 0.3f, 1.f));
            ImGui::TextUnformatted(clean(m->text).c_str());
            if (bad) ImGui::PopStyleColor();
        }
        if (v.empty()) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled(t.acars.empty() ? (live(a) ? "no Iridium ACARS heard yet" : "start the receiver") : "nothing matches"); }
        ImGui::EndTable();
    }
    // aircraft heard
    ImGui::TextDisabled("Aircraft (%zu)", t.aircraft.size());
    if (ImGui::BeginTable("##irac_ac", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Registration"); ImGui::TableSetupColumn("ICAO"); ImGui::TableSetupColumn("Flight"); ImGui::TableSetupColumn("Msgs");
        ImGui::TableSetupColumn("Last"); ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Alt ft");
        ImGui::TableHeadersRow();
        for (auto it = t.aircraft.rbegin(); it != t.aircraft.rend(); ++it) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(it->reg.c_str());
            ImGui::TableNextColumn(); if (it->icao) ImGui::Text("%06X", it->icao & 0xFFFFFF); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(it->flight.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%u", it->messages);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(it->lastLabel.c_str());
            ImGui::TableNextColumn();
            if (it->hasPos) ImGui::Text("%.3f, %.3f  (%s)", it->lat, it->lon, it->posKind.c_str()); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (it->hasPos && it->hasAlt) ImGui::Text("%d", it->altFt); else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##irac_r", ImVec2(0, H), ImGuiChildFlags_Borders);
    const dect2::IridiumAcarsMsg* sel = nullptr;
    for (const auto& m : t.acars) if (m.serial == S.acSel) sel = &m;
    if (!sel) {
        ImGui::PushTextWrapPos(0); ImGui::TextDisabled("click a message"); ImGui::PopTextWrapPos();
    } else {
        ImGui::TextColored(pal::heading(), "%s", sel->reg.empty() ? "(no registration)" : sel->reg.c_str());
        auto kv = [&](const char* k, const std::string& val) { ImGui::TextDisabled("%s", k); kvColumn(95 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(val.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
        char b[96];
        kv("direction", sel->downlink ? "to the aircraft (Iridium downlink)" : "from the aircraft (Iridium uplink)");
        kv("address", std::string(regKindName(sel->regKind)) + (sel->icao ? " " + [&] { snprintf(b, sizeof b, "%06X", sel->icao & 0xFFFFFF); return std::string(b); }() : std::string()));
        if (!sel->flight.empty()) kv("flight", sel->flight);
        if (!sel->seq.empty()) kv("sequence", sel->seq);
        snprintf(b, sizeof b, "%s %s  block %c  mode %c", sel->label.c_str(), sel->labelText.c_str(), sel->blockId >= 32 ? sel->blockId : '-', sel->mode ? sel->mode : ' ');
        kv("label", b);
        snprintf(b, sizeof b, "check %s, parity %s, %d block%s%s%s", sel->crc == 1 ? "ok" : sel->crc == 2 ? "FAILED" : "not present", sel->parity == 1 ? "ok" : sel->parity == 2 ? "FAILED" : "not used",
                 sel->blocks, sel->blocks == 1 ? "" : "s", sel->more ? ", more blocks expected" : "", sel->repeats ? ", repeated" : "");
        kv("quality", b);
        if (sel->hasPos) { snprintf(b, sizeof b, "%.4f, %.4f%s", sel->lat, sel->lon, sel->hasAlt ? (" alt " + std::to_string(sel->altFt) + " ft").c_str() : ""); kv("position", std::string(b) + "  " + sel->posKind); }
        if (!sel->decoded.empty()) kv("decoded", sel->decoded);
        ImGui::Separator();
        ImGui::PushFont(a.mono, 0); ImGui::TextWrapped("%s", sel->text.c_str()); ImGui::PopFont();
        if (ImGui::SmallButton("copy text##irac")) ImGui::SetClipboardText(sel->text.c_str());
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------- packets and frames

namespace {
const char* kTypeNames[] = {"All types", "IDA", "IIP", "IIQ", "IIR", "IIU", "IMS", "ITL", "MSG"};
bool typeMatch(int sel, uint8_t type) {
    if (sel == 0) return true;
    return std::strcmp(dect2::iridiumTypeLabel(type), kTypeNames[sel]) == 0;
}
std::string hexAscii(const std::vector<uint8_t>& v) {
    std::string s;
    char b[16];
    for (size_t i = 0; i < v.size(); i += 16) {
        snprintf(b, sizeof b, "%04zx  ", i); s += b;
        std::string asc;
        for (size_t k = 0; k < 16; k++) {
            if (i + k < v.size()) { snprintf(b, sizeof b, "%02x ", v[i + k]); s += b; asc += (v[i + k] >= 32 && v[i + k] < 127) ? (char)v[i + k] : '.'; }
            else s += "   ";
        }
        s += " " + asc + "\n";
    }
    return s;
}
} // namespace

void packetsView(App& a) {
    const IridiumTelemetry& t = a.rx.iridium;
    { ImGui::PushTextWrapPos(0);
      ImGui::TextDisabled("Data traffic of the Iridium links: single IDA / LAP frames (what the demodulator and the error codes made of each burst) and the packets they were joined into. Broadcast frames (IRA, IBC, ISY) and voice are not listed here.");
      ImGui::PopTextWrapPos(); }
    if (ImGui::RadioButton("Packets", !S.pkFrames)) S.pkFrames = false;
    ImGui::SameLine(); if (ImGui::RadioButton("Frames", S.pkFrames)) S.pkFrames = true;
    ImGui::SameLine(); filterRow("##irpk_q", S.pkText, sizeof S.pkText);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hex (\"7608\"), text, or a status word (\"repeat\", \"crc\").");
    ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi); ImGui::Combo("##irpk_d", &S.pkDir, "Both ways\0Downlink\0Uplink\0");
    if (S.pkFrames) { ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi); ImGui::Combo("##irpk_ty", &S.pkType, "All types\0IDA\0IIP\0IIQ\0IIR\0IIU\0IMS\0ITL\0MSG\0"); }
    ImGui::SameLine(); ImGui::Checkbox("Problems##irpk", &S.pkProblems);
    if (S.pkFrames) { ImGui::SameLine(); ImGui::Checkbox("Not assembled##irpk", &S.pkUnassembled); }

    const std::string q = lower(S.pkText);
    std::string dump;     // the filtered list as text for Copy / Save
    std::vector<const dect2::IridiumPacketRec*> pks;
    std::vector<const dect2::IridiumFrameRec*> frs;
    char b[512];
    if (!S.pkFrames) {
        for (auto it = t.packets.rbegin(); it != t.packets.rend(); ++it) {
            const auto& p = *it;
            if (S.pkDir == 1 && !p.downlink) continue;
            if (S.pkDir == 2 && p.downlink) continue;
            if (S.pkProblems && p.status != "incomplete" && p.status != "not SBD") continue;
            if (!q.empty() && !has(p.hex + " " + p.status + " " + dect2::iridiumAsciiDump(dect2::iridiumHexBytes([&] { std::string h; for (char c : p.hex) if (c != ' ') h += c; return h; }())), q)) continue;
            pks.push_back(&p);
        }
        dump = "utc\tmhz\tdirection\tfragments\tbytes\tsbd_type\tstatus\thex\n";
        for (auto p : pks) {
            snprintf(b, sizeof b, "%s\t%.4f\t%s\t%d\t%u\t%s\t%s\t", utc(p->wall).c_str(), p->freqHz / 1e6, p->downlink ? "downlink" : "uplink", p->fragments, p->length,
                     p->sbdType >= 0 ? (std::string("0x") + [&] { char x[8]; snprintf(x, sizeof x, "%04x", p->sbdType); return std::string(x); }()).c_str() : "-", p->status.c_str());
            dump += b; dump += p->hex + "\n";
        }
    } else {
        for (auto it = t.frameDump.rbegin(); it != t.frameDump.rend(); ++it) {
            const auto& f = *it;
            if (S.pkDir == 1 && !f.downlink) continue;
            if (S.pkDir == 2 && f.downlink) continue;
            if (!typeMatch(S.pkType, f.type)) continue;
            if (S.pkProblems && f.status == "ok") continue;
            if (S.pkUnassembled && (f.packet || f.type != 6)) continue;
            if (!q.empty() && !has(f.hex + " " + f.status + " " + dect2::iridiumAsciiDump(dect2::iridiumHexBytes(f.hex)), q)) continue;
            frs.push_back(&f);
        }
        dump = "utc\tmhz\tdirection\ttype\tstatus\tcorrected\tctr\tmore\tbytes\tpacket\thex\n";
        for (auto f : frs) {
            snprintf(b, sizeof b, "%s\t%.4f\t%s\t%s\t%s\t%d\t%d\t%d\t%u\t%llu\t", utc(f->wall).c_str(), f->freqHz / 1e6, f->downlink ? "downlink" : "uplink", dect2::iridiumTypeLabel(f->type),
                     f->status.c_str(), f->corrected, f->ctr, f->cont ? 1 : 0, f->length, (unsigned long long)f->packet);
            dump += b; dump += f->hex + "\n";
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy##irpk")) ImGui::SetClipboardText(dump.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Save...##irpk")) saveText(S.pkFrames ? "iridium-frames.tsv" : "iridium-packets.tsv", dump);
    if (!S.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", S.note.c_str()); }

    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float rw = std::min(430.f * gUi, W * 0.42f);
    ImGui::BeginChild("##irpk_l", ImVec2(W - rw - 8 * gUi, H));
    if (!S.pkFrames) {
        if (ImGui::BeginTable("##irpk_p", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("Dir"); ImGui::TableSetupColumn("Frames");
            ImGui::TableSetupColumn("Bytes"); ImGui::TableSetupColumn("Status"); ImGui::TableSetupColumn("Data", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (auto p : pks) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID((int)(p->serial & 0x7fffffff));
                if (ImGui::Selectable(utc(p->wall).c_str(), S.pkSel == p->serial, ImGuiSelectableFlags_SpanAllColumns)) S.pkSel = p->serial;
                ImGui::PopID();
                ImGui::TableNextColumn(); ImGui::Text("%.4f", p->freqHz / 1e6);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(p->downlink ? "down" : "up");
                ImGui::TableNextColumn(); ImGui::Text("%d", p->fragments);
                ImGui::TableNextColumn(); ImGui::Text("%u", p->length);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(p->status.c_str());
                ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(p->hex.substr(0, 90).c_str()); ImGui::PopFont();
            }
            if (pks.empty()) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled(t.packets.empty() ? (live(a) ? "no data packets yet" : "start the receiver") : "nothing matches"); }
            ImGui::EndTable();
        }
    } else {
        if (ImGui::BeginTable("##irpk_f", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("Dir"); ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Decode"); ImGui::TableSetupColumn("Ctr"); ImGui::TableSetupColumn("Bytes"); ImGui::TableSetupColumn("Packet");
            ImGui::TableSetupColumn("Data", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (auto f : frs) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID((int)(f->serial & 0x7fffffff));
                if (ImGui::Selectable(utc(f->wall).c_str(), S.pkSel == f->serial, ImGuiSelectableFlags_SpanAllColumns)) S.pkSel = f->serial;
                ImGui::PopID();
                ImGui::TableNextColumn(); ImGui::Text("%.4f", f->freqHz / 1e6);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(f->downlink ? "down" : "up");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::iridiumTypeLabel(f->type));
                ImGui::TableNextColumn(); ImGui::Text("%s%s", f->status.c_str(), f->corrected ? " *" : "");
                if (f->corrected && ImGui::IsItemHovered()) ImGui::SetTooltip("%d bit%s corrected by the error code", f->corrected, f->corrected == 1 ? "" : "s");
                ImGui::TableNextColumn(); if (f->ctr >= 0) ImGui::Text("%d%s", f->ctr, f->cont ? "+" : ""); else ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); ImGui::Text("%u", f->length);
                ImGui::TableNextColumn(); if (f->packet) ImGui::Text("#%llu", (unsigned long long)f->packet); else ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(f->hex.substr(0, 60).c_str()); ImGui::PopFont();
            }
            if (frs.empty()) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled(t.frameDump.empty() ? (live(a) ? "no data frames yet" : "start the receiver") : "nothing matches"); }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##irpk_r", ImVec2(0, H), ImGuiChildFlags_Borders);
    if (!S.pkFrames) {
        const dect2::IridiumPacketRec* sel = nullptr;
        for (const auto& p : t.packets) if (p.serial == S.pkSel) sel = &p;
        if (!sel) { ImGui::TextDisabled("click a packet"); }
        else {
            ImGui::TextColored(pal::heading(), "Packet #%llu", (unsigned long long)sel->serial);
            ImGui::TextDisabled("%s, %d frame%s, %u bytes, %s", sel->downlink ? "downlink" : "uplink", sel->fragments, sel->fragments == 1 ? "" : "s", sel->length, sel->status.c_str());
            std::string h;
            for (char c : sel->hex) if (c != ' ' && c != '.') h += c;
            const std::vector<uint8_t> bytes = dect2::iridiumHexBytes(h);
            const std::string hd = hexAscii(bytes);
            ImGui::Separator();
            ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(hd.c_str()); ImGui::PopFont();
            if (sel->length > bytes.size()) ImGui::TextDisabled("(first %zu bytes)", bytes.size());
            if (ImGui::SmallButton("copy##irpkp")) ImGui::SetClipboardText(hd.c_str());
            ImGui::Spacing();
            ImGui::TextDisabled("Frames joined");
            for (const auto& f : t.frameDump) if (f.packet == sel->serial) ImGui::Text("ctr %d%s  %s%s", f.ctr, f.cont ? "+" : " ", f.status.c_str(), f.corrected ? " (corrected)" : "");
        }
    } else {
        const dect2::IridiumFrameRec* sel = nullptr;
        for (const auto& f : t.frameDump) if (f.serial == S.pkSel) sel = &f;
        if (!sel) { ImGui::TextDisabled("click a frame"); }
        else {
            ImGui::TextColored(pal::heading(), "%s frame", dect2::iridiumTypeLabel(sel->type));
            ImGui::TextDisabled("%s, %.4f MHz, %s, %d bit%s corrected", sel->downlink ? "downlink" : "uplink", sel->freqHz / 1e6, sel->status.c_str(), sel->corrected, sel->corrected == 1 ? "" : "s");
            const std::string hd = hexAscii(dect2::iridiumHexBytes(sel->hex));
            ImGui::Separator();
            ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(hd.c_str()); ImGui::PopFont();
            if (ImGui::SmallButton("copy##irpkf")) ImGui::SetClipboardText(hd.c_str());
        }
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------- pager and paging

void pagerView(App& a) {
    const IridiumTelemetry& t = a.rx.iridium;
    { ImGui::PushTextWrapPos(0);
      ImGui::TextDisabled("Pager channel. Messages are joined from the MSG frames (alphanumeric or numeric). The ring alerts (IRA) name the temporary identities (TMSI) that are being paged; they are not phone numbers and say nothing more about who is paged. %llu paged in all.",
                          (unsigned long long)t.pagedTotal);
      ImGui::PopTextWrapPos(); }
    filterRow("##irpg_q", S.pgText, sizeof S.pgText);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Text, a pager number (RIC), or a TMSI in hex.");
    ImGui::SameLine(); ImGui::SetNextItemWidth(130 * gUi); ImGui::Combo("##irpg_f", &S.pgFmt, "All formats\0Alphanumeric\0Numeric\0");
    ImGui::SameLine(); ImGui::Checkbox("Complete only##irpg", &S.pgComplete);
    const std::string q = lower(S.pgText);

    std::vector<const dect2::IridiumPagerMsg*> ms;
    for (auto it = t.messages.rbegin(); it != t.messages.rend(); ++it) {
        if (S.pgFmt == 1 && it->fmt != 5) continue;
        if (S.pgFmt == 2 && it->fmt != 3) continue;
        if (S.pgComplete && !it->complete) continue;
        if (!q.empty() && !has(it->text + " " + std::to_string(it->ric), q)) continue;
        ms.push_back(&*it);
    }
    std::string dump = "ric\tseq\tformat\tcomplete\ttext\n";
    char b[96];
    for (auto m : ms) {
        snprintf(b, sizeof b, "%d\t%d\t%s\t%s\t", m->ric, m->seq, m->fmt == 3 ? "numeric" : m->fmt == 5 ? "alphanumeric" : "?", m->complete ? "yes" : "no");
        dump += b; dump += clean(m->text) + "\n";
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy##irpg")) ImGui::SetClipboardText(dump.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Save...##irpg")) saveText("iridium-pager.tsv", dump);
    if (!S.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", S.note.c_str()); }

    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float rw = std::min(330.f * gUi, W * 0.38f);
    ImGui::BeginChild("##irpg_l", ImVec2(W - rw - 8 * gUi, H));
    if (ImGui::BeginTable("##irpg_m", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("RIC"); ImGui::TableSetupColumn("Seq"); ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (auto m : ms) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (t.hasTime) { const time_t s = (time_t)(t.iridiumUtc - (t.timeSec - m->time)); struct tm g;
#ifdef _WIN32
                gmtime_s(&g, &s);
#else
                gmtime_r(&s, &g);
#endif
                ImGui::Text("%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec); }
            else ImGui::Text("%.0f s", m->time);
            ImGui::TableNextColumn(); ImGui::Text("%d", m->ric);
            ImGui::TableNextColumn(); ImGui::Text("%d", m->seq);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m->fmt == 3 ? "numeric" : m->fmt == 5 ? "text" : "?");
            ImGui::TableNextColumn();
            if (m->complete) ImGui::TextUnformatted(clean(m->text).c_str()); else ImGui::TextDisabled("%s (incomplete)", clean(m->text).c_str());
        }
        if (ms.empty()) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled(t.messages.empty() ? (live(a) ? "no pager messages yet" : "start the receiver") : "nothing matches"); }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##irpg_r", ImVec2(0, H), ImGuiChildFlags_Borders);
    // identities paged in the ring alerts kept (at most 100 alerts)
    struct Tm { int count = 0; int sat = -1, beam = -1; double last = 0; };
    std::map<uint32_t, Tm> ids;
    for (const auto& r : t.ringAlerts) for (uint32_t id : r.tmsis) { Tm& x = ids[id]; x.count++; x.sat = r.sat; x.beam = r.beam; x.last = std::max(x.last, r.time); }
    ImGui::TextDisabled("Paged identities (TMSI), %zu in the last %zu ring alerts", ids.size(), t.ringAlerts.size());
    std::vector<std::pair<uint32_t, Tm>> rows(ids.begin(), ids.end());
    std::sort(rows.begin(), rows.end(), [](const auto& x, const auto& y) { return x.second.last > y.second.last; });
    if (ImGui::BeginTable("##irpg_t", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("TMSI"); ImGui::TableSetupColumn("Pages"); ImGui::TableSetupColumn("Sat"); ImGui::TableSetupColumn("Beam");
        ImGui::TableHeadersRow();
        for (const auto& r : rows) {
            char hx[12];
            snprintf(hx, sizeof hx, "%08X", r.first);
            if (!q.empty() && !has(hx, q)) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(hx); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::Text("%d", r.second.count);
            ImGui::TableNextColumn(); ImGui::Text("%d", r.second.sat);
            ImGui::TableNextColumn(); ImGui::Text("%d", r.second.beam);
        }
        if (rows.empty()) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("none yet"); }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

} // namespace iridiumui
