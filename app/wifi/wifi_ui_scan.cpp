// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi band selector, channel list and hop scan (see wifi_ui_int.h). The radio listens to one 20 MHz channel at a time (20 Msps; a wide capture
// of a radio that offers 50 Msps or more hears several at once); the hop steps through the chosen list, dwelling a little longer than a
// beacon interval (102.4 ms) on each. Receive only.
#include "wifi_ui_int.h"
#include "dect2/wifi/wifi_chan.h"
#include <algorithm>
#include <cstdio>

namespace wfui {

State S;

// Loads the saved band, domain and hop settings once.
void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.hop = plat::prefs().getB("wifiHop", false);
    S.band = std::max(0, std::min(2, (int)plat::prefs().getI("wifiBand", 0)));
    S.domain = std::max(0, std::min(dect2::wifi::kDomains - 1, (int)plat::prefs().getI("wifiDomain", 0)));
    S.dwellMs = std::max(100, std::min(2000, (int)plat::prefs().getI("wifiDwellMs", 200)));
    S.pscOnly = plat::prefs().getB("wifiPsc", true);
    S.skipDfs = plat::prefs().getB("wifiSkipDfs", false);
}

static void save() {
    plat::prefs().setB("wifiHop", S.hop); plat::prefs().setI("wifiBand", S.band); plat::prefs().setI("wifiDomain", S.domain);
    plat::prefs().setI("wifiDwellMs", S.dwellMs); plat::prefs().setB("wifiPsc", S.pscOnly); plat::prefs().setB("wifiSkipDfs", S.skipDfs);
    plat::prefs().flush();
}

bool live(const App& a) { return a.rx.standard == 55; }   // the engine reports its standard code minus one

// The channels the hop steps through.
std::vector<int> hopList() {
    std::vector<int> v;
    for (int c : dect2::wifi::channelList(S.band, S.domain)) {
        if (S.band == dect2::wifi::Band6 && S.pscOnly && !dect2::wifi::isPsc(c)) continue;
        if (S.band == dect2::wifi::Band5 && S.skipDfs && dect2::wifi::isDfs(c)) continue;
        if (S.band == dect2::wifi::Band24 && c > 13) continue;
        v.push_back(c);
    }
    return v;
}

// Tunes to a channel; the frequency box follows.
void tuneChannel(App& a, int band, int ch) {
    const double mhz = dect2::wifi::bandChannelMhz(band, ch);
    if (mhz <= 0) return;
    a.freqMhz = mhz;
    a.tune.centerHz = mhz * 1e6;
    a.engine.retune(a.tune);
}

// Why this radio cannot listen: out of its specified frequency range, or too slow a sample rate for a 20 MHz channel.
std::string radioNote(const App& a) {
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) return "";
    const DeviceInfo& d = a.devices[(size_t)a.devIdx];
    if (d.kind == DeviceInfo::File || d.kind == DeviceInfo::Synthetic) return "";
    char b[200];
    if (d.maxFreqHz > 0 && a.freqMhz * 1e6 > d.maxFreqHz) {
        snprintf(b, sizeof b, tr("This radio is specified up to %.0f MHz: %.0f MHz is out of its range"), d.maxFreqHz / 1e6, a.freqMhz);
        return b;
    }
    if (d.minFreqHz > 0 && a.freqMhz * 1e6 < d.minFreqHz) {
        snprintf(b, sizeof b, tr("This radio is specified from %.0f MHz: %.0f MHz is out of its range"), d.minFreqHz / 1e6, a.freqMhz);
        return b;
    }
    if (d.maxRateHz > 0 && d.maxRateHz < 20e6 - 1) {
        snprintf(b, sizeof b, tr("A 20 MHz channel needs 20 Msps; this radio reaches only %.1f Msps"), d.maxRateHz / 1e6);
        return b;
    }
    return "";
}

// Band, regulatory domain, channel, hop and dwell.
void tunerControls(App& a, bool& retune) {
    using namespace dect2::wifi;
    loadState();
    const double mhz = a.freqMhz;
    const int curBand = bandOfMhz(mhz);
    ImGui::TextDisabled("%s", tr("Band"));
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(90 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##wfband", bandName(S.band))) {
        for (int b = 0; b < 3; b++)
            if (ImGui::Selectable(bandName(b), b == S.band)) {
                S.band = b; save();
                const std::vector<int> l = hopList();
                if (!l.empty()) { tuneChannel(a, b, l.front()); retune = true; }
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("2.4 GHz: channels 1 - 14. 5 GHz: 36 - 177 (with DFS). 6 GHz: 1 - 233 (Wi-Fi 6E / 7)."));
    ImGui::SameLine(0, 8 * gUi);
    ImGui::TextDisabled("%s", tr("Domain"));
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(130 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##wfdom", domainName(S.domain))) {
        for (int d = 0; d < kDomains; d++) if (ImGui::Selectable(domainName(d), d == S.domain)) { S.domain = d; save(); }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The channels each region allows. All lists every channel."));
    ImGui::TextDisabled("%s", tr("Channel"));
    const int cur = curBand == S.band ? bandChannelOf(S.band, mhz) : 0;
    char label[48];
    if (cur) snprintf(label, sizeof label, "%d (%.0f MHz)", cur, bandChannelMhz(S.band, cur)); else snprintf(label, sizeof label, "%s", tr("other"));
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(190 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##wfch", label)) {
        for (int c : channelList(S.band, S.domain)) {
            char b[80];
            snprintf(b, sizeof b, "%3d  %.0f MHz  %s%s%s", c, bandChannelMhz(S.band, c), unii(S.band, c), S.band == Band5 && isDfs(c) ? " DFS" : "", S.band == Band6 && isPsc(c) ? " PSC" : "");
            if (ImGui::Selectable(b, c == cur)) { tuneChannel(a, S.band, c); retune = true; }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("One 20 MHz channel at a time. Wider networks beacon on their 20 MHz primary channel. 6 GHz: channels 5, 21, 37 ... (PSC) carry FILS discovery."));
    ImGui::SameLine(0, 8 * gUi);
    if (ImGui::Checkbox(tr("hop##wfhop"), &S.hop)) save();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Step through the band's channels; dwell longer than 102.4 ms (the beacon interval)."));
    if (S.hop) {
        ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(std::min(100 * gUi, ImGui::GetContentRegionAvail().x));
        if (ImGui::SliderInt("##wfdw", &S.dwellMs, 100, 2000, "%d ms")) save();
        if (S.band == Band6) { ImGui::SameLine(0, 5 * gUi); if (ImGui::Checkbox(tr("PSC only##wfpsc"), &S.pscOnly)) save(); }
        if (S.band == Band5) { ImGui::SameLine(0, 5 * gUi); if (ImGui::Checkbox(tr("skip DFS##wfdfs"), &S.skipDfs)) save(); }
    }
    const std::string note = radioNote(a);
    if (!note.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::warnAmber(), "%s", note.c_str()); ImGui::PopTextWrapPos(); }
}

// Moves to the next channel of the hop list when the dwell time is over.
void hopTick(App& a) {
    loadState();
    if (!a.engine.running() || !S.hop || a.devices[(size_t)a.devIdx].kind == DeviceInfo::File) { S.lastHop = ImGui::GetTime(); return; }
    if (ImGui::GetTime() - S.lastHop < S.dwellMs / 1000.0) return;
    S.lastHop = ImGui::GetTime();
    const std::vector<int> l = hopList();
    if (l.empty()) return;
    const int cur = dect2::wifi::bandChannelOf(S.band, a.freqMhz);
    size_t next = 0;
    for (size_t i = 0; i < l.size(); i++) if (l[i] == cur) next = (i + 1) % l.size();
    tuneChannel(a, S.band, l[next]);
}

} // namespace wfui
