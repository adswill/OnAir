// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi HaLow (802.11ah) screens: the network list (SSID, BSSID, vendor, channel, bandwidth, MCS, streams, SNR, security as information, level, beacons,
// last seen), the live frame log with a frames-per-second plot, the statistics (frames by width, MCS, streams, preamble), the channel strip with the
// radio's span over the region's band, the region (ten plans, halow_chan.h), channel and receive-width pickers (Auto, or one of 1 / 2 / 4 / 8 / 16 MHz),
// and the channel scan. Receive only: nothing is ever sent, joined or answered; nothing is decrypted, the security type is only what the beacon announces.
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "dect2/halow/halow_proto.h"
#include "dect2/halow/halow_rx.h"
#include "dect2/halow/halow_tel.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

using dect2::halow::Region;

constexpr int kAutoNarrow = 0, kAutoAll = 99;   // receive width choices besides one width in MHz

struct State {
    int view = 0;                        // 0 networks, 1 frames, 2 statistics
    bool loaded = false, scan = false;
    int region = 0;                      // dect2::halow::Region
    int width = 1;                       // the channel width the picker lists: 1, 2, 4, 8, 16 MHz
    int rx = kAutoNarrow;                // receive width: Auto 1 - 4 MHz (6 Msps), Auto all widths (20 Msps), or one width in MHz
    int dwellSec = 6;
    double lastHop = 0;
    char filter[64] = "";
    float minRssi = -120;
    std::string sel;                     // BSSID clicked: the frame log shows only its frames
};
State S;

// Saves the region, list width and receive width.
void saveState() {
    plat::prefs().setI("hlReg2", S.region); plat::prefs().setI("hlWidth", S.width); plat::prefs().setI("hlRx", S.rx); plat::prefs().flush();
}

// Reads them once (the first version's region key is migrated).
void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.scan = plat::prefs().getB("hlScan", false);
    const int old = (int)plat::prefs().getI("hlRegion", 0);   // the first version: 0 EU, 1 US
    S.region = std::max(0, std::min((int)dect2::halow::kNumRegions - 1, (int)plat::prefs().getI("hlReg2", old == 1 ? (int)Region::RegUS : (int)Region::RegEU)));
    S.width = std::max(1, std::min(16, (int)plat::prefs().getI("hlWidth", 1)));
    S.rx = (int)plat::prefs().getI("hlRx", kAutoNarrow);
    if (S.rx != kAutoNarrow && S.rx != kAutoAll && S.rx != 1 && S.rx != 2 && S.rx != 4 && S.rx != 8 && S.rx != 16) S.rx = kAutoNarrow;
    S.dwellSec = std::max(1, std::min(60, (int)plat::prefs().getI("hlDwell", 6)));
}

int widthIdx(int mhz) { return mhz >= 16 ? 4 : mhz >= 8 ? 3 : mhz >= 4 ? 2 : mhz >= 2 ? 1 : 0; }
int rxMask() { return S.rx == kAutoNarrow || S.rx == kAutoAll ? 31 : 1 << widthIdx(S.rx); }

bool live(const App& a) { return a.rx.standard == 71; }   // the engine reports its standard code minus one

// UTC time of day of a wall-clock time.
std::string clock(int64_t t) {
    if (t <= 0) return "-";
    return dect2::timefmt::clock(t);
}

// A key and its value on one row.
void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

// The SSID, "(hidden)", or the compressed SSID of a network.
std::string nameOf(const dect2::HlNet& n) {
    if (n.hasSsid) return n.ssid;
    if (n.hidden) return tr("(hidden)");
    if (n.comp) return dect2::halow::compText(n.comp);
    return "-";
}

// True when a network passes the level and text filters.
bool passes(const dect2::HlNet& n) {
    if (n.rssi < S.minRssi) return false;
    if (!S.filter[0]) return true;
    const std::string f = lower(S.filter);
    return lower(n.ssid).find(f) != std::string::npos || lower(n.bssid).find(f) != std::string::npos || lower(n.vendor).find(f) != std::string::npos ||
           lower(n.security).find(f) != std::string::npos || lower(n.country).find(f) != std::string::npos;
}

const char* preName(int p) { return p == 0 ? "S1G_SHORT" : p == 1 ? "S1G_LONG" : "S1G_1M"; }

// ---------------------------------------------------------------- the radio's rate against the width asked for

// The radio rate the receive width calls for: Auto 1 - 4 MHz at 6 Msps (the 1 / 2 MHz primaries of a 4 MHz BSS sit off the centre), Auto all at 20 Msps, one width at its recommended rate (halow_phy.h)
double wantedRate() { return S.rx == kAutoNarrow ? 6e6 : S.rx == kAutoAll ? dect2::halow::recommendedRateHz(dect2::halow::W16) : dect2::halow::recommendedRateHz(widthIdx(S.rx)); }

// A line that says what the sample rate allows, in red when the chosen width does not fit
void rateNote(const App& a) {
    const double fs = a.engine.running() ? a.engine.sampleRate() : a.tune.sampleRate;
    if (fs <= 0) return;
    const int maxW = dect2::halow::maxWidthMhz(fs);
    const int need = S.rx == kAutoNarrow || S.rx == kAutoAll ? 0 : S.rx;
    if (need > maxW) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(pal::warnAmber(), tr("The radio runs at %.1f Msps: %d MHz channels need at least %.0f Msps (%.0f recommended). Pick the RATE in the top bar or another receive width."), fs / 1e6, need,
                           dect2::halow::minRateHz(widthIdx(need)) / 1e6, dect2::halow::recommendedRateHz(widthIdx(need)) / 1e6);
        ImGui::PopTextWrapPos();
    } else if (maxW < 16 && S.rx == kAutoAll) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled(tr("At %.1f Msps channels up to %d MHz are read; 16 MHz channels need 16 Msps or more (20 recommended)."), fs / 1e6, maxW);
        ImGui::PopTextWrapPos();
    }
}

// The note that this is receive only, and what each width needs.
void receiveNote() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Receive only: OnAir never transmits, probes, joins or answers a network, and nothing is decrypted. The security type is what the beacon "
                                 "announces, shown for information. Sub-GHz Wi-Fi HaLow (802.11ah) is for low power sensors and cameras at long range; 1, 2, 4, 8 and 16 MHz channels. "
                                 "1 and 2 MHz channels work from 2.4 Msps (an RTL-SDR), 4 MHz from 4 Msps, 8 MHz from 10 Msps, 16 MHz from 20 Msps."));
    ImGui::PopTextWrapPos();
}

// ---------------------------------------------------------------- the channel strip: the region's band, the channels, the radio's span and the networks

// The band of the region with the channels of the list width, the radio's span, the tuned frequency and the networks heard.
void channelStrip(App& a) {
    using namespace dect2::halow;
    const RegionInfo& ri = regionInfo(S.region);
    double lo = 1e9, hi = 0;
    for (const Band& b : ri.bands) { lo = std::min(lo, b.loMhz); hi = std::max(hi, b.hiMhz); }
    if (hi <= lo) return;
    const dect2::HalowTelemetry& t = a.rx.halow;
    const float h = 44 * gUi;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = std::max(60.f, ImGui::GetContentRegionAvail().x);
    ImGui::InvisibleButton("##hlstrip", ImVec2(w, h));
    const bool hover = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetMousePos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const double pad = 0.5;
    const auto X = [&](double mhz) { return p0.x + (float)((mhz - (lo - pad)) / (hi - lo + 2 * pad) * w); };
    const float y0 = p0.y + 12 * gUi, y1 = p0.y + h - 4 * gUi;
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(24, 28, 34, 255));
    for (const Band& b : ri.bands) dl->AddRectFilled(ImVec2(X(b.loMhz), y0), ImVec2(X(b.hiMhz), y1), IM_COL32(44, 56, 70, 255));
    // the channels of the list's width
    const auto ch = regionChannels(S.region, S.width);
    for (const Chan& c : ch) {
        const float x0 = X(c.mhz - c.widthMhz / 2.0), x1 = X(c.mhz + c.widthMhz / 2.0);
        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(110, 130, 150, 255));
        if (x1 - x0 > 22 * gUi) { char b[8]; snprintf(b, sizeof b, "%d", c.ccf); dl->AddText(ImVec2(x0 + 2 * gUi, y0 + 1), IM_COL32(150, 165, 180, 255), b); }
    }
    // the networks heard: a bar at their operating channel, brighter when recent
    for (const auto& n : t.nets) {
        if (n.freqMhz <= 0) continue;
        const double wd = n.opWidthMhz > 0 ? n.opWidthMhz : n.seenWidthMhz;
        const double ago = std::max(0.0, t.timeSec - n.lastSec);
        const ImU32 col = ago < 3 ? IM_COL32(90, 200, 120, 200) : IM_COL32(150, 150, 90, 130);
        dl->AddRectFilled(ImVec2(X(n.freqMhz - wd / 2), y1 - 7 * gUi), ImVec2(X(n.freqMhz + wd / 2), y1), col);
    }
    // the radio: the span it takes in, and the channel it is tuned to
    const double fs = (a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate) / 1e6;
    if (fs > 0) dl->AddRectFilled(ImVec2(X(a.freqMhz - fs / 2), p0.y + 1), ImVec2(X(a.freqMhz + fs / 2), y0 - 2), IM_COL32(80, 150, 230, 90));
    dl->AddLine(ImVec2(X(a.freqMhz), p0.y), ImVec2(X(a.freqMhz), p0.y + h), IM_COL32(240, 200, 80, 255), 1.5f);
    char head[96];
    snprintf(head, sizeof head, "%s  %.1f - %.1f MHz", ri.code, lo, hi);
    dl->AddText(ImVec2(p0.x + 3 * gUi, p0.y), IM_COL32(180, 190, 200, 255), head);
    if (hover) {
        const double mhz = (lo - pad) + (mouse.x - p0.x) / w * (hi - lo + 2 * pad);
        char tb[40];
        snprintf(tb, sizeof tb, "%.1f MHz", mhz);
        std::string tip = tb;
        for (const Chan& c : ch) if (std::fabs(c.mhz - mhz) <= c.widthMhz / 2.0) { char b[96]; snprintf(b, sizeof b, "\n%s %d: %.1f MHz, %d MHz wide, class %d", tr("channel"), c.ccf, c.mhz, c.widthMhz, operatingClass(S.region, c.widthMhz)); tip += b; }
        for (const auto& n : t.nets) if (n.freqMhz > 0 && std::fabs(n.freqMhz - mhz) <= std::max(1, n.opWidthMhz) / 2.0) tip += "\n" + nameOf(n) + "  " + std::to_string(std::max(n.opWidthMhz, n.seenWidthMhz)) + " MHz";
        ImGui::SetTooltip("%s", tip.c_str());
    }
}

// ---------------------------------------------------------------- views

// The filter row of the network list.
void filters(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Filter")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(160 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##hlf", tr("SSID, BSSID, vendor, security, country"), S.filter, sizeof S.filter);
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Level above")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(120 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::SliderFloat("##hlr", &S.minRssi, -120, 0, "%.0f dBFS");
    flowNext(10 * gUi);
    if (!S.sel.empty()) {
        ImGui::TextColored(pal::okGreen(), tr("%s only"), S.sel.c_str());
        ImGui::SameLine(0, 5 * gUi);
        if (ImGui::SmallButton(tr("Show all"))) S.sel.clear();
        flowNext(10 * gUi);
    }
    if (ImGui::Button(tr("Clear")) && live(a)) a.engine.halow().clearResults();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Forget every network and frame heard so far"));
    flowBreak();
}

// what a beacon announced about a network, for its tooltip
std::string detailOf(const dect2::HlNet& n) {
    char b[320];
    std::string s = n.last;
    snprintf(b, sizeof b, "\n%s %d MHz, %s", tr("last frame:"), n.seenWidthMhz, preName(n.pre));
    s += b;
    if (n.rateMbps > 0) { snprintf(b, sizeof b, ", MCS %d, %d %s, %.2f Mbit/s, SNR %.0f dB", n.mcs, n.nss, tr("stream(s)"), n.rateMbps, n.snrDb); s += b; }
    if (n.opClass) { snprintf(b, sizeof b, "\n%s %d, %s %d, %s %d MHz", tr("class"), n.opClass, tr("channel"), n.ccf, tr("operating width"), n.opWidthMhz); s += b; }
    if (n.primaryWidthMhz) { snprintf(b, sizeof b, ", %s %d MHz", tr("primary"), n.primaryWidthMhz); s += b; }
    if (n.primaryMhz > 0) { snprintf(b, sizeof b, ", %s %.1f MHz", tr("primary channel at"), n.primaryMhz); s += b; }
    if (!n.vendorIe.empty()) s += std::string("\n") + tr("vendor elements") + " " + n.vendorIe;
    if (!n.country.empty()) s += "\n" + std::string(tr("country")) + " " + n.country;
    if (n.dtimPeriod) { snprintf(b, sizeof b, ", DTIM %d", n.dtimPeriod); s += b; }
    if (n.txPowerDbm >= 0) { snprintf(b, sizeof b, ", %s %d dBm", tr("power constraint"), n.txPowerDbm); s += b; }
    if (n.hasRaw || n.hasTwt) s += std::string("\n") + (n.hasRaw ? "RAW " : "") + (n.hasTwt ? "TWT" : "");
    if (n.longPreamble >= 0) s += std::string("\n") + tr("long preamble") + (n.longPreamble ? ": yes" : ": no");
    if (n.basicMcs >= 0) { static const char* m[] = {"MCS 0-2", "MCS 0-7", "MCS 0-9", "-"}; s += std::string(", ") + tr("basic rate set") + " " + m[n.basicMcs & 3]; }
    s += "\n"; s += tr("click to show only this network's frames");
    return s;
}

// The network table.
void networksView(App& a) {
    receiveNote();
    rateNote(a);
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see the networks around you")); return; }
    filters(a);
    const dect2::HalowTelemetry& t = a.rx.halow;
    channelStrip(a);
    if (t.ppsHist.size() >= 2) {
        ImGui::TextDisabled("%s", tr("Frames per second"));
        ImGui::PlotLines("##hlpps", t.ppsHist.data(), (int)t.ppsHist.size(), 0, nullptr, 0, FLT_MAX, ImVec2(ImGui::GetContentRegionAvail().x, 40 * gUi));
    }
    if (!ImGui::BeginTable("##hlnet", 15, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("SSID"); ImGui::TableSetupColumn("BSSID"); ImGui::TableSetupColumn(tr("Vendor")); ImGui::TableSetupColumn(tr("Ch")); ImGui::TableSetupColumn("MHz");
    ImGui::TableSetupColumn(tr("Width")); ImGui::TableSetupColumn("MCS"); ImGui::TableSetupColumn("NSS"); ImGui::TableSetupColumn("SNR"); ImGui::TableSetupColumn(tr("Security"));
    ImGui::TableSetupColumn(tr("Beacon")); ImGui::TableSetupColumn(tr("Level")); ImGui::TableSetupColumn(tr("Beacons"));
    ImGui::TableSetupColumn(tr("Country")); ImGui::TableSetupColumn(tr("Last seen"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    int shown = 0;
    for (const auto& n : t.nets) {
        if (!passes(n)) continue;
        shown++;
        ImGui::TableNextRow();
        ImGui::PushID(n.bssid.c_str());
        const double ago = std::max(0.0, t.timeSec - n.lastSec);
        const ImVec4 c = ago < 3 ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::TableNextColumn();
        if (ImGui::Selectable("##row", S.sel == n.bssid, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.sel = S.sel == n.bssid ? std::string() : n.bssid;
        ImGui::SameLine(0, 0);
        ImGui::TextColored(c, "%s", nameOf(n).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", detailOf(n).c_str());
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(n.bssid.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(n.vendor.c_str());
        ImGui::TableNextColumn(); if (n.ccf) ImGui::Text("%d", n.ccf); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (n.freqMhz > 0) ImGui::Text("%.1f", n.freqMhz); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        if (n.opWidthMhz && n.opWidthMhz != n.seenWidthMhz) ImGui::Text("%d (op %d) MHz", n.seenWidthMhz, n.opWidthMhz); else ImGui::Text("%d MHz", n.seenWidthMhz);
        ImGui::TableNextColumn(); ImGui::Text("%d", n.mcs);
        ImGui::TableNextColumn(); ImGui::Text("%d", n.nss);
        ImGui::TableNextColumn(); ImGui::Text("%.0f dB", n.snrDb);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(n.security.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("What the beacon announces, for information. OnAir does not decrypt or test anything."));
        ImGui::TableNextColumn(); if (n.intervalTu) ImGui::Text("%d TU", n.intervalTu); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::Text("%.0f", n.rssi);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)n.beacons);
        ImGui::TableNextColumn(); if (!n.country.empty()) ImGui::TextUnformatted(n.country.c_str()); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::Text(ago < 1 ? tr("now") : tr("%.0f s ago"), ago);
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (t.nets.empty()) ImGui::TextDisabled("%s", tr("nothing heard yet"));
    else if (!shown) ImGui::TextDisabled("%s", tr("no network passes the filter"));
}

// The frame log.
void framesView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see the frames")); return; }
    const dect2::HalowTelemetry& t = a.rx.halow;
    const auto& log = t.log;
    if (!S.sel.empty()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(pal::okGreen(), tr("Showing %s only"), S.sel.c_str());
        ImGui::SameLine(0, 8 * gUi);
        if (ImGui::SmallButton(tr("Show all"))) S.sel.clear();
    }
    std::vector<size_t> rows;
    for (size_t i = 0; i < log.size(); i++) {
        if (!S.sel.empty() && log[i].bssid != S.sel) continue;
        rows.push_back(i);
    }
    if (ImGui::Button(tr("Copy"))) {
        std::string txt;
        for (size_t i : rows) {
            const auto& f = log[i];
            char b[760];
            snprintf(b, sizeof b, "%s\t%d MHz\t%s\tMCS %d\t%d\t%.0f dB\t%s\t%s\t%d\t%s\t%s\n", clock(f.wallTime).c_str(), f.widthMhz, preName(f.pre), f.mcs, f.nss, f.snrDb, f.type.c_str(), f.bssid.c_str(), f.len, f.decoded ? "ok" : "-", f.info.c_str());
            txt += b;
        }
        ImGui::SetClipboardText(txt.c_str());
    }
    ImGui::SameLine(0, 8 * gUi);
    ImGui::TextDisabled(tr("%.0f frames/s"), t.pps);
    if (!ImGui::BeginTable("##hllog", 10, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr(dect2::timefmt::headTime())); ImGui::TableSetupColumn(tr("Width")); ImGui::TableSetupColumn("MCS"); ImGui::TableSetupColumn("NSS"); ImGui::TableSetupColumn(tr("Preamble"));
    ImGui::TableSetupColumn("SNR"); ImGui::TableSetupColumn(tr("Type")); ImGui::TableSetupColumn("BSSID"); ImGui::TableSetupColumn(tr("Len")); ImGui::TableSetupColumn(tr("Details"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGuiListClipper clip;
    clip.Begin((int)rows.size());
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const auto& f = log[rows[(size_t)i]];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(f.wallTime).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d MHz", f.widthMhz);
            ImGui::TableNextColumn(); ImGui::Text("%d", f.mcs);
            ImGui::TableNextColumn(); ImGui::Text("%d", f.nss);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(preName(f.pre));
            ImGui::TableNextColumn(); ImGui::Text("%.0f dB", f.snrDb);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(f.type.c_str());
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(f.bssid.c_str()); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::Text("%d", f.len);
            ImGui::TableNextColumn(); if (f.decoded) ImGui::TextUnformatted(f.info.c_str()); else ImGui::TextDisabled("%s", f.info.c_str());
        }
    ImGui::EndTable();
    if (log.empty()) ImGui::TextDisabled("%s", tr("no frames yet"));
    else if (rows.empty()) ImGui::TextDisabled("%s", tr("no frame passes the selection"));
}

// SIG fields read by width, MCS, streams and preamble, and the PPDUs that are only counted
void statsView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see the statistics")); return; }
    rateNote(a);
    const dect2::HalowTelemetry& t = a.rx.halow;
    const double fs = a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate;
    if (ImGui::BeginTable("##hlst", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn(tr("Width")); ImGui::TableSetupColumn(tr("SIG fields read")); ImGui::TableSetupColumn(tr("Radio rate needed")); ImGui::TableSetupColumn(tr("State"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int w = 0; w < dect2::halow::kNumWidths; w++) {
            ImGui::TableNextRow();
            const bool tried = (t.widthMask >> w & 1) && fs >= dect2::halow::minRateHz(w) - 1;
            ImGui::TableNextColumn(); ImGui::Text("%d MHz", 1 << w);
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)t.perWidth[w]);
            ImGui::TableNextColumn(); ImGui::Text(tr("%.0f Msps (%.0f recommended)"), dect2::halow::minRateHz(w) / 1e6, dect2::halow::recommendedRateHz(w) / 1e6);
            ImGui::TableNextColumn();
            if (tried) ImGui::TextColored(pal::okGreen(), "%s", tr("listening")); else ImGui::TextDisabled("%s", (t.widthMask >> w & 1) ? tr("sample rate too low") : tr("not selected"));
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    std::string m;
    for (int i = 0; i <= 10; i++) { char b[40]; snprintf(b, sizeof b, "MCS %d: %llu   ", i, (unsigned long long)t.perMcs[i]); m += b; }
    kvRow(a, tr("by MCS"), "%s", m.c_str());
    kvRow(a, tr("by streams"), "1: %llu   2: %llu   3: %llu   4: %llu", (unsigned long long)t.perNss[1], (unsigned long long)t.perNss[2], (unsigned long long)t.perNss[3], (unsigned long long)t.perNss[4]);
    kvRow(a, tr("by preamble"), "S1G_SHORT: %llu   S1G_LONG: %llu   S1G_1M: %llu", (unsigned long long)t.perPre[0], (unsigned long long)t.perPre[1], (unsigned long long)t.perPre[2]);
    kvRow(a, tr("read from the SIG field only"), tr("%llu several streams, %llu STBC, %llu LDPC, %llu A-MPDU, %llu null data packets"), (unsigned long long)t.mimoFrames, (unsigned long long)t.stbcFrames, (unsigned long long)t.ldpcFrames,
          (unsigned long long)t.aggFrames, (unsigned long long)t.ndpFrames);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("The data field is decoded for one spatial stream with BCC coding at MCS 0 - 10. With several streams (a radio with one antenna cannot separate them), STBC, "
                                 "LDPC coding (parity-check matrices not included) and A-MPDU the frame is detected and counted, its contents are not read."));
    ImGui::PopTextWrapPos();
}

// ---------------------------------------------------------------- tuning

std::vector<dect2::halow::Chan> plan() { return dect2::halow::regionChannels(S.region, S.width); }

// Retunes to the next channel of a scan without resetting the tables.
void hopTo(App& a, double mhz) {
    if (!a.engine.running() || !live(a)) return;   // wait until activeStd is 71 (engine stdMode 72)
    a.freqMhz = mhz;
    a.tune.centerHz = mhz * 1e6;
    a.engine.retune(a.tune);   // no reset: the tables stay
}

// A new receive width: the radio rate follows it (applyBandwidth asks halowRateHz()), a radio is opened again when the rate changed
void applyRx(App& a) {
    saveState();
    a.engine.halow().setWidthMask(rxMask());
    const double old = a.tune.sampleRate;
    a.tune.centerHz = a.freqMhz * 1e6;
    applyBandwidth(a);
    if (a.engine.running() && a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[(size_t)a.devIdx].isRadio() && a.tune.sampleRate != old) startReceiver(a);
}

// Text of a receive width choice.
const char* rxLabel(int rx, char* b, size_t n) {
    if (rx == kAutoNarrow) snprintf(b, n, "%s", tr("Auto 1 - 4 MHz"));
    else if (rx == kAutoAll) snprintf(b, n, "%s", tr("Auto, all widths"));
    else snprintf(b, n, "%d MHz", rx);
    return b;
}

// Every frame: pushes the width choice to the receiver and runs the channel scan.
void tick(App& a) {
    loadState();
    a.engine.halow().setWidthMask(rxMask());
    const bool fileSrc = a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[(size_t)a.devIdx].kind == DeviceInfo::File;
    if (!a.engine.running() || !live(a) || !S.scan || fileSrc) { S.lastHop = ImGui::GetTime(); return; }
    if (ImGui::GetTime() - S.lastHop < S.dwellSec) return;
    S.lastHop = ImGui::GetTime();
    const auto ch = plan();
    if (ch.empty()) return;
    size_t next = 0;
    for (size_t i = 0; i < ch.size(); i++) if (std::fabs(ch[i].mhz - a.freqMhz) < 0.25) { next = (i + 1) % ch.size(); break; }
    hopTo(a, ch[next].mhz);
}

// Region, width, channel, receive width and scan controls.
void tuner(App& a, bool& retune) {
    using namespace dect2::halow;
    loadState();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Region"));
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(190 * gUi, ImGui::GetContentRegionAvail().x));
    auto regionText = [](int r, char* b, size_t n) {
        const RegionInfo& ri = regionInfo(r);
        std::string bands;
        for (const Band& bd : ri.bands) { char x[32]; snprintf(x, sizeof x, "%s%g - %g", bands.empty() ? "" : ", ", bd.loMhz, bd.hiMhz); bands += x; }
        snprintf(b, n, "%s  %s MHz", ri.code, bands.c_str());
    };
    char label[96];
    regionText(S.region, label, sizeof label);
    if (ImGui::BeginCombo("##hlreg", label)) {
        for (int r = 0; r < kNumRegions; r++) {
            regionText(r, label, sizeof label);
            if (ImGui::Selectable(label, S.region == r)) {
                S.region = r;
                const auto& ws = regionInfo(r).widths;
                if (std::find(ws.begin(), ws.end(), S.width) == ws.end()) S.width = ws.front();
                saveState();
                const auto first = plan();   // a new region retunes to its first channel of this width (live or stopped), like a channel pick
                if (!first.empty()) { tuneFreq(a, first.front().mhz); retune = true; }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", regionInfo(r).name);
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", regionInfo(S.region).name);
    flowNext(8 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Width"));
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(80 * gUi, ImGui::GetContentRegionAvail().x));
    char wl[24];
    snprintf(wl, sizeof wl, "%d MHz", S.width);
    if (ImGui::BeginCombo("##hlw", wl)) {
        for (int w : regionInfo(S.region).widths) {
            snprintf(wl, sizeof wl, "%d MHz", w);
            if (ImGui::Selectable(wl, S.width == w)) { S.width = w; saveState(); }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The channel width of the list below: S1G channels of this width in the region. What the receiver reads is set by Receive."));
    flowNext(10 * gUi);
    const auto ch = plan();
    int cur = 0;
    char cl[56] = "";
    snprintf(cl, sizeof cl, "%s", tr("other"));
    for (const auto& c : ch) if (std::fabs(c.mhz - a.freqMhz) < 0.25 + c.widthMhz / 2.0 - 0.5) { cur = c.ccf; snprintf(cl, sizeof cl, "%d (%.1f MHz)", c.ccf, c.mhz); }
    ImGui::TextDisabled("%s", tr("Channel"));
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(160 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##hlch", cl)) {
        for (const auto& c : ch) {
            char b[64];
            snprintf(b, sizeof b, "%d  %.1f MHz  (%s %d)", c.ccf, c.mhz, tr("class"), operatingClass(S.region, c.widthMhz));
            if (ImGui::Selectable(b, c.ccf == cur)) { tuneFreq(a, c.mhz); retune = true; }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("S1G channels of the region and width: centre = start frequency + 0.5 MHz x channel number, with the operating class of the plan"));
    flowNext(10 * gUi);
    ImGui::TextDisabled("%s", tr("Receive"));
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(130 * gUi, ImGui::GetContentRegionAvail().x));
    char rl[40];
    if (ImGui::BeginCombo("##hlrx", rxLabel(S.rx, rl, sizeof rl))) {
        for (int v : {kAutoNarrow, kAutoAll, 1, 2, 4, 8, 16}) {
            if (ImGui::Selectable(rxLabel(v, rl, sizeof rl), S.rx == v) && S.rx != v) { S.rx = v; applyRx(a); }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Which channel widths the receiver reads. Auto 1 - 4 MHz runs the radio at 6 Msps (it also looks for the 1 and 2 MHz primary channels of a 4 MHz network beside the centre). Auto, all widths needs 20 Msps (a HackRF, a LimeSDR ...) and reads 1 - 16 MHz.\n"
                                                         "One width sets the radio rate that width needs (2, 4, 5, 10 or 20 Msps) and ignores the other widths."));
    flowNext(10 * gUi);
    if (ImGui::Checkbox(tr("scan"), &S.scan)) { plat::prefs().setB("hlScan", S.scan); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Step through the channels of the list, listening to each for the dwell time (a radio that takes in a single channel misses what is\n"
                                                         "sent on the others meanwhile). Off: single channel."));
    if (S.scan) {
        ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(std::min(70 * gUi, ImGui::GetContentRegionAvail().x));
        if (ImGui::SliderInt("##hldw", &S.dwellSec, 1, 60, "%d s")) { plat::prefs().setI("hlDwell", S.dwellSec); plat::prefs().flush(); }
    }
    flowBreak();
    rateNote(a);
}

// The HaLow tab: Networks, Frames, Statistics.
void tab(App& a) {
    subNav("hlv", S.view, {tr("Networks"), tr("Frames"), tr("Statistics")});
    if (S.view == 0) networksView(a); else if (S.view == 1) framesView(a); else statsView(a);
}

// The network list on the right.
void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::HalowTelemetry& t = a.rx.halow;
    if (!ImGui::BeginTable("##hll", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn(tr("Network"), ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn(tr("Level"));
    for (const auto& n : t.nets) {
        ImGui::TableNextRow();
        ImGui::PushID(n.bssid.c_str());
        const bool recent = t.timeSec - n.lastSec < 3;
        ImGui::TableNextColumn();
        if (ImGui::Selectable("##row", S.sel == n.bssid, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.sel = S.sel == n.bssid ? std::string() : n.bssid;
        ImGui::SameLine(0, 0);
        ImGui::TextColored(recent ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", nameOf(n).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%.1f MHz, %d MHz wide, MCS %d x %d, %s", n.bssid.c_str(), n.freqMhz, n.seenWidthMhz, n.mcs, n.nss, n.security.c_str());
        ImGui::TableNextColumn(); ImGui::TextDisabled("%.0f", n.rssi);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// The Receiver tab: what the receiver is doing.
void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::HalowTelemetry& t = a.rx.halow;
    kvRow(a, tr("state"), "%s", dect2::halowSummary(t).c_str());
    if (t.centerMhz > 0) kvRow(a, tr("tuned"), "%.2f MHz%s%s%s%s", t.centerMhz, t.region >= 0 ? ", " : "", t.region >= 0 ? dect2::halow::regionInfo(t.region).code : "", t.chanHint ? ", S1G channel " : "", t.chanHint ? std::to_string(t.chanHint).c_str() : "");
    kvRow(a, tr("signal"), tr("tone SNR %.1f dB, offset %+.1f kHz"), t.snrDb, t.cfoHz / 1e3);
    kvRow(a, tr("preambles"), tr("%llu detected, %llu SIG fields read, %llu not readable"), (unsigned long long)t.preambles, (unsigned long long)t.sigOk, (unsigned long long)t.sigBad);
    kvRow(a, tr("frames"), tr("%llu good, %llu failed FCS, %llu only counted"), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.undecoded);
    kvRow(a, tr("by width"), "1 MHz %llu, 2 MHz %llu, 4 MHz %llu, 8 MHz %llu, 16 MHz %llu", (unsigned long long)t.perWidth[0], (unsigned long long)t.perWidth[1], (unsigned long long)t.perWidth[2], (unsigned long long)t.perWidth[3],
          (unsigned long long)t.perWidth[4]);
    kvRow(a, tr("reads"), tr("up to %d MHz channels at this rate"), t.maxWidthMhz);
    kvRow(a, tr("input"), "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kvRow(a, tr("signal time"), "%.1f s", t.timeSec);
    ImGui::Spacing();
    rateNote(a);
    receiveNote();
}

// The status bar.
void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Frames"), !on ? 0 : a.rx.halow.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::halowSummary(a.rx.halow).c_str()); ImGui::PopFont();
}

// The two lines of the top bar.
void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Wi-Fi HaLow (802.11ah)";
    if (live(a)) l2 = dect2::halowSummary(a.rx.halow);
}

} // namespace

// the radio rate the receive width asks for (applyBandwidth in state/app_state.cpp): 6 Msps for Auto 1 - 4 MHz, 20 Msps for Auto all widths, one width its own
double halowWantedRateHz() { loadState(); return wantedRate(); }
// the widest channel the receive width means (the spectrum's channel marker): 4 MHz for Auto 1 - 4 MHz, 16 MHz for Auto all widths
double halowWantedWidthMhz() { loadState(); return S.rx == kAutoNarrow ? 4 : S.rx == kAutoAll ? 16 : S.rx; }

extern const ModeUi kHalowUi;
const ModeUi kHalowUi = {
    .sideTitle = "NETWORKS",
    .tabName = "HaLow",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .tick = tick,
};
