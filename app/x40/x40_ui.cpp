// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The screens of the 4.0 signal modes (families 881 - 885, docs/X40-4.0.md): one table of what was decoded, one Receiver page. One click: the mode tunes and needs no settings.
#include "app.h"
#include "dect2/x40/x40_rx.h"
#include "dect2/x40/x40_tel.h"
#include "dect2/x40/stl_cfg.h"
#include "dect2/x40/scada_proto.h"
#include "../ism/ook_scatter_ui.h"
#include <algorithm>
#include <cstdio>
#include <string>

void scadaTab(App& a);    // scada_ui.cpp: SCADA radio telemetry, the Messages and Devices tabs
void scadaList(App& a);

namespace {

using dect2::X40Row;
using dect2::X40Telemetry;

template <int V> bool live(const App& a) { return a.rx.standard == dect2::x40::activeOfVariant(V); }

template <int V> void tab(App& a) {
    if constexpr (V == dect2::x40::Scada) { scadaTab(a); return; }   // its stored messages stay readable while stopped
    if (!live<V>(a)) { ImGui::TextDisabled("%s", tr(a.engine.running() ? "starting" : "stopped")); return; }
    const X40Telemetry& t = a.rx.x40;
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(t.headline.c_str()); ImGui::PopFont();
    if constexpr (V == dect2::x40::OokPulse) {
        static OokScatterState scatter;
        std::vector<dect2::ookscatter::Burst> bs(t.ookBursts.begin(), t.ookBursts.end());
        ookScatterPanel(bs, (int)bs.size() - 1, scatter);
        ImGui::Separator();
    }
    if (t.rows.empty()) { ImGui::TextDisabled("%s", tr("nothing decoded yet")); return; }
    if (ImGui::BeginTable("##x40", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupColumn(tr("Time")); ImGui::TableSetupColumn(tr("Kind")); ImGui::TableSetupColumn(tr("ID")); ImGui::TableSetupColumn(tr("Decoded"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const X40Row& r : t.rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%.1f", r.timeSec);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(tr(r.kind.c_str()));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.id.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.text.c_str());
        }
        ImGui::EndTable();
    }
}

template <int V> void receiver(App& a) {
    if (!live<V>(a)) { ImGui::TextDisabled("%s", tr(a.engine.running() ? "starting" : "stopped")); return; }
    const X40Telemetry& t = a.rx.x40;
    auto kv = [&](const char* k, const std::string& v) { ImGui::TextDisabled("%s", tr(k)); kvColumn(150 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(v.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    char b[160];
    kv(N_("state"), t.headline);
    snprintf(b, sizeof b, "%llu good, %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); kv(N_("frames"), b);
    snprintf(b, sizeof b, "%.1f dB", t.snrDb); kv(N_("signal"), b);
    snprintf(b, sizeof b, "%.0f Hz", t.cfoHz); kv(N_("offset"), b);
    snprintf(b, sizeof b, "%.3f Msps", t.inputRate / 1e6); kv(N_("input"), b);
    for (const auto& f : t.facts) kv(f.first.c_str(), f.second);
}

template <int V> void list(App& a) {
    if constexpr (V == dect2::x40::Scada) { scadaList(a); return; }
    if (!live<V>(a)) { ImGui::TextDisabled("%s", tr(a.engine.running() ? "starting" : "stopped")); return; }
    const X40Telemetry& t = a.rx.x40;
    int shown = 0;
    for (const X40Row& r : t.rows) { ImGui::Text("%s  %s", r.kind.c_str(), r.id.c_str()); if (++shown >= 40) break; }
    if (!shown) ImGui::TextDisabled("%s", tr("nothing yet"));
}

// STL link: the sound meters and the link settings (one click needs none of them: the defaults are a +-60 kHz composite link in a 250 kHz channel)
void stlControls() {
    dect2::StlConfig& c = dect2::stlConfig();
    const float pk = c.peakDbfs.load(), rms = c.rmsDbfs.load();
    ImGui::TextDisabled("%s", tr("Sound level"));
    char b[48];
    snprintf(b, sizeof b, "%.0f dBFS", pk); ImGui::ProgressBar(std::min(1.f, std::max(0.f, (pk + 60.f) / 60.f)), ImVec2(-1, 0), b);
    snprintf(b, sizeof b, "%.0f dBFS", rms); ImGui::ProgressBar(std::min(1.f, std::max(0.f, (rms + 60.f) / 60.f)), ImVec2(-1, 0), b);
    float vol = c.volume.load(); bool mute = c.muted.load();
    ImGui::SetNextItemWidth(140 * gUi);
    if (ImGui::SliderFloat("##stlvol", &vol, 0.f, 1.f, "%.2f")) c.volume = vol;
    ImGui::SameLine(); if (ImGui::Checkbox(tr("Mute"), &mute)) c.muted = mute;
    if (!ImGui::TreeNode(trw("Link settings (advanced)"))) return;
    float dev = (float)(c.deviationHz.load() / 1000), wid = (float)(c.channelWidthHz.load() / 1000);
    ImGui::TextDisabled("%s", tr("Deviation at 100 % (kHz)")); ImGui::SetNextItemWidth(140 * gUi);
    if (ImGui::DragFloat("##stldev", &dev, 1.f, 10.f, 300.f, "%.0f kHz")) c.deviationHz = (double)dev * 1000;
    bool autoW = c.autoWidth.load();
    if (ImGui::Checkbox(tr("Automatic channel width and centre"), &autoW)) c.autoWidth = autoW;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Follows the link: Carson's rule on the measured deviation and the multiplex content (RDS, SCA), or the occupied width, plus a margin; the carrier offset is removed first."));
    if (autoW) ImGui::TextDisabled("%.0f kHz", c.widthNowHz.load() / 1000);
    else {
        ImGui::TextDisabled("%s", tr("Channel width (kHz)")); ImGui::SetNextItemWidth(140 * gUi);
        if (ImGui::DragFloat("##stlwid", &wid, 5.f, 20.f, 1000.f, "%.0f kHz")) c.channelWidthHz = (double)wid * 1000;
    }
    static const int deemph[3] = {75, 50, 0};
    static const char* const deNames[3] = {N_("75 us (US)"), N_("50 us"), N_("Flat (no de-emphasis)")};
    int di = c.deemphasisUs.load() == 50 ? 1 : c.deemphasisUs.load() == 0 ? 2 : 0;
    ImGui::SetNextItemWidth(200 * gUi);
    if (ImGui::BeginCombo("##stlde", tr(deNames[di]))) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(tr(deNames[i]), di == i)) c.deemphasisUs = deemph[i];
        ImGui::EndCombo();
    }
    static const char* const modeNames[3] = {N_("Automatic (stereo when the pilot is locked)"), N_("Composite stereo"), N_("Mono")};
    int mi = c.mode.load(); if (mi < 0 || mi > 2) mi = 0;
    ImGui::SetNextItemWidth(200 * gUi);
    if (ImGui::BeginCombo("##stlmode", tr(modeNames[mi]))) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(tr(modeNames[i]), mi == i)) c.mode = i;
        ImGui::EndCombo();
    }
    static const char* const scaNames[3] = {N_("the programme"), "SCA 67 kHz", "SCA 92 kHz"};
    int si = c.scaListen.load(); if (si < -1 || si > 1) si = -1;
    ImGui::SetNextItemWidth(200 * gUi);
    if (ImGui::BeginCombo("##stlsca", tr(scaNames[si + 1]))) {
        for (int i = -1; i < 2; i++) if (ImGui::Selectable(tr(scaNames[i + 1]), si == i)) c.scaListen = i;
        ImGui::EndCombo();
    }
    static const char* const regNames[3] = {N_("Auto (RBDS on odd tenths)"), N_("RDS (Europe)"), N_("RBDS (US)")};
    int ri = c.rdsRegion.load(); if (ri < 0 || ri > 2) ri = 2;
    ImGui::SetNextItemWidth(200 * gUi);
    if (ImGui::BeginCombo("##stlreg", tr(regNames[ri]))) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(tr(regNames[i]), ri == i)) c.rdsRegion = i;
        ImGui::EndCombo();
    }
    ImGui::TreePop();
}

template <int V> void decoder(App& a, bool&) {
    if (ImGui::SmallButton(trw("Clear"))) {
        a.engine.x40().clearResults(V);
        if constexpr (V == dect2::x40::Scada) { dect2::scada::Store& st = dect2::scada::store(); std::lock_guard<std::mutex> lk(st.mu); st.tr.clear(); st.phy.clear(); st.version++; }
    }
    if constexpr (V == dect2::x40::Stl) stlControls();
}

template <int V> void status(App& a) {
    const bool run = a.engine.running(), on = live<V>(a);
    lamp("Decode", !on ? 0 : a.rx.x40.state >= 2 ? 1 : 0);
    flowNext(10 * gUi);
    ImGui::TextUnformatted(!on ? tr(run ? "starting" : "stopped") : a.rx.x40.headline.c_str());
}

template <int V> void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = dect2::x40::variantName(V);
    if (live<V>(a)) l2 = a.rx.x40.headline;
}

template <int V> constexpr ModeUi makeUi() {
    ModeUi u{};
    u.sideTitle = "HEARD"; u.tabName = "Decoded"; u.tabIcon = Ic::Signal;
    u.tab = tab<V>; u.receiver = receiver<V>; u.list = list<V>; u.status = status<V>; u.summary = summary<V>; u.decoder = decoder<V>;
    return u;
}

} // namespace

extern const ModeUi kX40Ui[55];
const ModeUi kX40Ui[55] = {makeUi<0>(), makeUi<1>(), makeUi<2>(), makeUi<3>(), makeUi<4>(), makeUi<5>(), makeUi<6>(), makeUi<7>(), makeUi<8>(), makeUi<9>(), makeUi<10>(), makeUi<11>(), makeUi<12>(), makeUi<13>(), makeUi<14>(), makeUi<15>(), makeUi<16>(), makeUi<17>(), makeUi<18>(), makeUi<19>(), makeUi<20>(), makeUi<21>(), makeUi<22>(), makeUi<23>(), makeUi<24>(), makeUi<25>(), makeUi<26>(), makeUi<27>(), makeUi<28>(), makeUi<29>(), makeUi<30>(), makeUi<31>(), makeUi<32>(), makeUi<33>(), makeUi<34>(), makeUi<35>(), makeUi<36>(), makeUi<37>(), makeUi<38>(), makeUi<39>(), makeUi<40>(), makeUi<41>(), makeUi<42>(), makeUi<43>(), makeUi<44>(), makeUi<45>(), makeUi<46>(), makeUi<47>(), makeUi<48>(), makeUi<49>(), makeUi<50>(), makeUi<51>(), makeUi<52>(), makeUi<53>(), makeUi<54>()};
