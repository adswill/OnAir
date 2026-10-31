// ATIS screens (the Automatic Transmitter Identification System of the European inland waterways, not AIS): the messages heard (ten digit
// identity, country, the call sign it spells, the ship name from your own table), the stations (one row per identity), and a converter between
// call signs and ATIS numbers. The voice of the channel plays as narrow FM (Listen); the ATIS comes right after the voice.
#include "app.h"
#include "i18n/i18n.h"
#include "dect2/atis/atis_proto.h"
#include "dect2/atis/atis_rx.h"
#include "dect2/atis/atis_tel.h"
#include "dect2/common/time_fmt.h"
#include "dect2/marine/marine_mmsi.h"
#include "platform/platform.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;
    bool listen = true, loaded = false, namesLoaded = false;
    int sel = -1;
    char nameBuf[64] = "";
    char csBuf[16] = "PC8075", midBuf[8] = "244", numBuf[16] = "";
    int selSerialDigits = 0;
    std::string selDigits;
    double namesSaved = 0;
};
State S;

bool live(const App& a) { return a.rx.standard == 212; }   // the engine reports its standard code minus one

void load() {
    if (S.loaded) return;
    S.loaded = true;
    S.listen = plat::prefs().getB("atisListen", true);
}

void loadNames() {
    if (S.namesLoaded) return;
    S.namesLoaded = true;
    for (const char* f : {"/mmsi_names.tsv", "/ship_names.tsv"}) {
        std::ifstream in(std::filesystem::u8path(plat::dataDir() + f), std::ios::binary);
        if (!in) continue;
        std::stringstream ss; ss << in.rdbuf();
        dect2::marine::ShipDb::global().importText(ss.str());
    }
    dect2::marine::ShipDb::global().takeDirty();
}

void saveNames() {
    if (!dect2::marine::ShipDb::global().takeDirty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(plat::dataDir()), ec);
    std::ofstream out(std::filesystem::u8path(plat::dataDir() + "/ship_names.tsv"), std::ios::binary);
    out << dect2::marine::ShipDb::global().exportTsv();
}

void banner(App& a) {
    load();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Tune an inland VHF channel (156.3 - 162.0 MHz). The ATIS follows each transmission."));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("ATIS (ETSI EN 300 698-1 annex B) is sent by the radio of a ship on the European inland waterways when the operator lets go of the push-to-talk key: 1200 baud, 1300 / 2100 Hz tones in the DSC code. It is not AIS. The number is 9 + the country digits + the call sign (or 9 + the MMSI on radios programmed that way). The voice plays as narrow FM; the check symbol and the second copy of every symbol repair most errors."));
    flowNext(8 * gUi);
    listenControls(a, S.listen, "atisListen", "atis", tr("Play the channel as narrow FM; decoding is the same either way"));
    flowNext(8 * gUi);
    char cl[40]; snprintf(cl, sizeof cl, "%s##atis", tr("Clear"));
    if (ImGui::Button(cl)) a.engine.atis().clearResults();
    flowEnd();
}

std::string who(const std::string& country, const std::string& name) {
    std::string r = name;
    if (!country.empty()) r += (r.empty() ? "" : ", ") + country;
    return r;
}

void messagesView(App& a) {
    const auto& ms = a.rx.atis.messages;
    if (!ImGui::BeginTable("##atism", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(tr("ATIS number")); ImGui::TableSetupColumn(tr("Country")); ImGui::TableSetupColumn(tr("Call sign"));
    ImGui::TableSetupColumn(tr("Name")); ImGui::TableSetupColumn(tr("Check")); ImGui::TableSetupColumn("dB", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    for (const auto& m : ms) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(m.utc).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%s", m.digits.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%s", m.country.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%s", m.callsign.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%s", m.name.c_str());
        ImGui::TableNextColumn();
        if (m.checkOk) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), "%s", m.repaired ? tr("ok, repaired") : tr("ok"));
        else ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1), "%s", tr("failed"));
        ImGui::TableNextColumn(); ImGui::Text("%.0f", m.levelDb);
    }
    ImGui::PopFont();
    ImGui::EndTable();
    if (ms.empty()) ImGui::TextDisabled("%s", tr("no messages yet"));
}

void stationsView(App& a) {
    loadNames();
    const auto& st = a.rx.atis.stations;
    const float h = std::max(80.f * gUi, ImGui::GetContentRegionAvail().y - 34 * gUi);
    if (ImGui::BeginTable("##atiss", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("ATIS number")); ImGui::TableSetupColumn(tr("Country")); ImGui::TableSetupColumn(tr("Call sign")); ImGui::TableSetupColumn(tr("Name"));
        ImGui::TableSetupColumn(tr("Last heard")); ImGui::TableSetupColumn(tr("Heard"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGui::PushFont(a.mono, 0);
        for (size_t i = 0; i < st.size(); i++) {
            const auto& s = st[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable((s.digits + "##as" + std::to_string(i)).c_str(), S.selDigits == s.digits, ImGuiSelectableFlags_SpanAllColumns)) {
                S.selDigits = s.digits;
                snprintf(S.nameBuf, sizeof S.nameBuf, "%s", s.name.c_str());
            }
            ImGui::TableNextColumn(); ImGui::Text("%s", s.country.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%s", s.callsign.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%s", s.name.c_str());
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(s.lastSeen).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%llux", (unsigned long long)s.count);
        }
        ImGui::PopFont();
        ImGui::EndTable();
    }
    if (st.empty()) ImGui::TextDisabled("%s", tr("no stations yet"));
    if (!S.selDigits.empty()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", tr("Name for the selected number")); ImGui::SameLine();
        ImGui::SetNextItemWidth(200 * gUi);
        ImGui::InputText("##atisname", S.nameBuf, sizeof S.nameBuf);
        ImGui::SameLine();
        if (ImGui::Button(tr("Save name##atis")) && S.nameBuf[0]) {
            const std::string mmsi = S.selDigits.substr(1);
            dect2::marine::ShipDb::global().importText(mmsi + "\t" + S.nameBuf + "\n");
            saveNames();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Kept in your ship name table (shared with the DSC and AIS screens), under the nine digits after the 9"));
    }
}

void converterView(App& a) {
    ImGui::TextWrapped("%s", tr("Call sign to ATIS number (ETSI EN 300 698-1 annex C): 9, the country digits (MID), the second letter of the call sign as 01 to 26, the four digits."));
    ImGui::SetNextItemWidth(100 * gUi); ImGui::InputText(tr("Call sign##atiscs"), S.csBuf, sizeof S.csBuf);
    ImGui::SetNextItemWidth(60 * gUi); ImGui::InputText(tr("MID (country digits)##atismid"), S.midBuf, sizeof S.midBuf);
    const int mid = atoi(S.midBuf);
    const std::string id = dect2::atis::idFromCallsign(S.csBuf, mid);
    ImGui::PushFont(a.mono, 0);
    if (id.empty()) ImGui::TextDisabled("%s", tr("two letters and four digits, for example PC8075"));
    else ImGui::Text("ATIS %s  (%s)", id.c_str(), dect2::marine::midCountry(mid));
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::TextWrapped("%s", tr("ATIS number to call sign"));
    ImGui::SetNextItemWidth(160 * gUi); ImGui::InputText(tr("ATIS number##atisnum"), S.numBuf, sizeof S.numBuf);
    const dect2::atis::Id d = dect2::atis::idFromDigits(S.numBuf);
    ImGui::PushFont(a.mono, 0);
    if (!d.valid) ImGui::TextDisabled("%s", tr("ten digits"));
    else {
        ImGui::Text("%s  %s", d.callsign.c_str(), d.country.c_str());
        ImGui::Text("%s %s", tr("as an MMSI:"), d.mmsi.c_str());
    }
    ImGui::PopFont();
}

void tab(App& a) {
    banner(a);
    ImGui::Spacing();
    subNav("atisv", S.view, {"Messages", "Stations", "Converter"});
    if (S.view == 2) { converterView(a); return; }
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    if (S.view == 0) messagesView(a); else stationsView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const auto& st = a.rx.atis.stations;
    if (!ImGui::BeginTable("##atisl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn(tr("ATIS number")); ImGui::TableSetupColumn(tr("Heard"), ImGuiTableColumnFlags_WidthStretch);
    for (const auto& s : st) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::Text("%s", s.digits.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s %s, %llux", s.callsign.c_str(), dect2::timefmt::clock(s.lastSeen).c_str(), (unsigned long long)s.count);
    }
    ImGui::EndTable();
    if (st.empty()) ImGui::TextDisabled("%s", tr("no stations yet"));
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::AtisTelemetry& t = a.rx.atis;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv(tr("state"), "%s", dect2::atisSummary(t).c_str());
    kv(tr("messages"), "%llu ok, %llu %s", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, tr("check failed"));
    kv(tr("other DSC calls"), "%llu", (unsigned long long)t.otherCalls);
    kv(tr("carrier offset"), "%+.0f Hz", t.cfoHz);
    kv(tr("channel"), "%.1f dBFS, %.0f dB %s", t.levelDb, t.snrDb, tr("over the floor"));
    kv(tr("input"), "%.3f Msps", t.inputRate / 1e6);
    kv(tr("signal time"), "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Tones"), !on ? 0 : a.rx.atis.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp(tr("ATIS"), !on ? 0 : a.rx.atis.dataValid ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::atisSummary(a.rx.atis).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "ATIS";
    if (live(a)) l2 = dect2::atisSummary(a.rx.atis);
}

void tick(App& a) {
    load();
    dect2::AtisReceiver& r = a.engine.atis();
    r.setListen(S.listen); r.setVolume(a.volume); r.setMuted(a.muted);
}

} // namespace

extern const ModeUi kAtisUi;
const ModeUi kAtisUi = {
    .sideTitle = "STATIONS",
    .tabName = "ATIS",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
