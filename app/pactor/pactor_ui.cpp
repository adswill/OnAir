// PACTOR-I screens (experimental, monitor only): the band presets, Listen, then three views: the packets and control signals heard (UTC,
// kind, speed, packet number, flags, level, text), the text of both stations, and the call signs (connect packets, calls named in the
// text). PACTOR-II, -III and -IV are proprietary and not supported (docs/pactor.md).
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "gfx/gfx.h"
#include "i18n/i18n.h"
#include "dect2/pactor/pactor_rx.h"
#include "dect2/pactor/pactor_tel.h"
#include "dect2/pactor/pactor_proto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct State {
    bool wasRunning = false, loaded = false;
    bool listen = false; int pushedListen = -1; float pushedVol = -1; bool pushedMute = false;
    int view = 0;                        // 0 packets, 1 text, 2 stations
    int demod = dect2::HfDemodAuto;
    char huffPath[512] = {};
    std::string huffMsg;
};
State S;

bool live(const App& a) { return a.rx.standard == 204; }   // the engine reports its standard code minus one

void load() {
    if (S.loaded) return;
    S.loaded = true;
    S.listen = plat::prefs().getB("pactorListen", false);
    S.demod = hfDemodLoad("pactorDemod");
    const std::string p = plat::prefs().getS("pactorHuffman", "");
    snprintf(S.huffPath, sizeof S.huffPath, "%s", p.c_str());
    if (!p.empty() && dect2::pactor::huffLoad(p)) S.huffMsg = tr("Huffman table loaded");
}

const ImVec4 kCallCol(1.0f, 0.82f, 0.36f, 1);

ImVec4 kindColour(const std::string& k) {
    if (k == "data") return ImVec4(0.55f, 0.85f, 0.95f, 1);
    if (k == "CS1") return ImVec4(0.40f, 0.95f, 0.55f, 1);
    if (k == "CS2" || k == "bad packet") return ImVec4(0.95f, 0.55f, 0.45f, 1);
    if (k == "CS3" || k == "CS4" || k == "QRT") return ImVec4(0.95f, 0.78f, 0.45f, 1);
    if (k == "connect") return kCallCol;
    return ImVec4(0.80f, 0.84f, 0.88f, 1);
}

void presets(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", tr("Experimental"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("PACTOR-I is decoded from its public description only; no real recording was available. PACTOR-II, -III and -IV are proprietary and not supported."));
    flowNext(8 * gUi);
    ImGui::TextDisabled("%s", tr("Band"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("PACTOR has no calling frequency: these buttons only jump to the digital part of each band."));
    flowNext(6 * gUi);
    int n = 0;
    const dect2::PactorPreset* p = dect2::pactorPresets(n);
    for (int i = 0; i < n; i++) {
        const bool here = std::fabs(a.freqMhz - p[i].mhz) < 0.002;
        if (here) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
        char b[40]; snprintf(b, sizeof b, "%s##pacp%d", p[i].name, i);
        if (ImGui::Button(b)) tuneFreq(a, p[i].mhz);
        if (here) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.4f MHz", p[i].mhz);
        flowNext(4 * gUi);
    }
    ImGui::TextDisabled("|"); flowNext(8 * gUi);
    listenControls(a, S.listen, "pactorListen", "pactor", tr("Play the sideband (dial + 200 Hz to 3.8 kHz) on the sound card; decoding is the same either way"));
    flowNext(8 * gUi);
    char cl[40]; snprintf(cl, sizeof cl, "%s##pactor", tr("Clear"));
    if (ImGui::Button(cl)) a.engine.pactor().clearResults();
    flowEnd();
}

std::string detail(const dect2::PactorFrameLog& f) {
    char b[160];
    std::string s;
    if (f.kind == "bad packet") return tr("CRC failed (kept for memory ARQ)");
    if (f.kind.compare(0, 2, "CS") == 0) return tr(f.text.c_str());
    if (f.repeat) s += f.fec ? tr("unproto / FEC repeat  ") : tr("repeat  ");
    if (f.memArq) s += tr("memory ARQ  ");
    if (f.breakIn) s += tr("break-in  ");
    if (f.qrt) s += tr("QRT  ");
    if (f.kind == "connect") { snprintf(b, sizeof b, "%s %s", tr("calls"), f.call.c_str()); return s + b; }
    return s + (f.huffman ? "[H] " : "[A] ") + f.text;
}

void packetsView(App& a) {
    const auto& fr = a.rx.pactor.frames;
    if (!ImGui::BeginTable("##pacf", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(tr("Kind")); ImGui::TableSetupColumn(tr("Baud")); ImGui::TableSetupColumn("#");
    ImGui::TableSetupColumn(tr("Side")); ImGui::TableSetupColumn("dB"); ImGui::TableSetupColumn(tr("Detail"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    for (const auto& f : fr) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(f.utc).c_str());
        ImGui::TableNextColumn(); ImGui::TextColored(kindColour(f.kind), "%s", f.kind.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("tones %.0f / %.0f Hz%s%s", f.toneHz, f.toneHz + 200, f.inverted ? ", swapped" : "", f.header >= 0 ? (f.header == 0x55 ? ", header 55" : ", header AA") : "");
        ImGui::TableNextColumn(); ImGui::Text("%d", f.baud);
        ImGui::TableNextColumn(); if (f.count < 0) ImGui::TextDisabled("-"); else ImGui::Text("%d", f.count);
        ImGui::TableNextColumn(); ImGui::Text("%d", f.sender);
        ImGui::TableNextColumn(); if (f.crcOk && f.snrDb > 0) ImGui::Text("%.0f", f.snrDb); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(detail(f).c_str()); ImGui::PopTextWrapPos();
    }
    ImGui::PopFont();
    ImGui::EndTable();
    if (fr.empty()) ImGui::TextDisabled("%s", tr("no packets yet"));
}

void textView(App& a) {
    const auto& t = a.rx.pactor;
    if (!t.hasFullHuffman) {
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", tr("Huffman table incomplete: unknown codes show as ~. Put the table of the public ARRL PACTOR page (Annex, section 9) in a file and load it:"));
        ImGui::SetNextItemWidth(320 * gUi);
        ImGui::InputText("##pachuff", S.huffPath, sizeof S.huffPath);
        ImGui::SameLine();
        if (ImGui::Button(tr("Load table##pac"))) {
            const bool ok = dect2::pactor::huffLoad(S.huffPath);
            S.huffMsg = ok ? tr("Huffman table loaded") : tr("could not read the table");
            if (ok) plat::prefs().setS("pactorHuffman", S.huffPath);
        }
        if (!S.huffMsg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", S.huffMsg.c_str()); }
    }
    if (ImGui::BeginChild("##pact", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        ImGui::PushFont(a.mono, 0);
        if (t.text.empty()) ImGui::TextDisabled("%s", tr("no data yet"));
        else { ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(t.text.c_str()); ImGui::PopTextWrapPos(); }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
        ImGui::PopFont();
    }
    ImGui::EndChild();
}

void stationsView(App& a) {
    const auto& st = a.rx.pactor.stations;
    if (!ImGui::BeginTable("##pacs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Station")); ImGui::TableSetupColumn(tr("Last heard")); ImGui::TableSetupColumn(tr("Count")); ImGui::TableSetupColumn(tr("Seen as"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    for (const auto& s : st) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextColored(kCallCol, "%s", s.call.c_str());
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(s.lastSeen).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)s.count);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", tr(s.how.c_str()));
    }
    ImGui::PopFont();
    ImGui::EndTable();
    if (st.empty()) ImGui::TextDisabled("%s", tr("no stations yet"));
}

void tick(App& a) {
    load();
    if (!a.engine.running()) { S.wasRunning = false; return; }
    dect2::PactorReceiver& r = a.engine.pactor();
    if (!S.wasRunning || (int)S.listen != S.pushedListen) { r.setListen(S.listen); S.pushedListen = S.listen; }
    r.setDemod(dect2::hfDemodResolve(S.demod, a.freqMhz));
    if (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute) {
        r.setVolume(a.volume); r.setMuted(a.muted);
        S.pushedVol = a.volume; S.pushedMute = a.muted;
    }
    S.wasRunning = true;
}

void tuner(App& a, bool&) {
    load();
    hfDemodControl(a, S.demod, "pactorDemod", "pactor");
}

void tab(App& a) {
    load();
    presets(a);
    ImGui::Spacing();
    subNav("pacv", S.view, {"Packets", "Text", "Stations"});
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    if (S.view == 0) packetsView(a); else if (S.view == 1) textView(a); else stationsView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    if (!ImGui::BeginTable("##pacl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn(tr("Station")); ImGui::TableSetupColumn(tr("Heard"), ImGuiTableColumnFlags_WidthStretch);
    for (const auto& s : a.rx.pactor.stations) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextColored(kCallCol, "%s", s.call.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s, %llux", dect2::timefmt::clock(s.lastSeen).c_str(), (unsigned long long)s.count);
    }
    ImGui::EndTable();
    if (a.rx.pactor.stations.empty()) ImGui::TextDisabled("%s", tr("no stations yet"));
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::PactorTelemetry& t = a.rx.pactor;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv(tr("state"), "%s", dect2::pactorSummary(t).c_str());
    if (t.pairLowHz > 0) kv(tr("tone pair"), "%.0f / %.0f Hz", t.pairLowHz, t.pairLowHz + 200);
    kv(tr("packets"), "%llu OK (%llu at 200 baud), %llu %s", (unsigned long long)t.blocksOk, (unsigned long long)t.fastPackets, (unsigned long long)t.blocksBad, tr("failed"));
    kv(tr("control signals"), "%llu", (unsigned long long)t.csCount);
    kv(tr("repeats"), "%llu", (unsigned long long)t.repeats);
    kv(tr("memory ARQ saves"), "%llu", (unsigned long long)t.memArqSaves);
    kv(tr("changeovers"), "%llu", (unsigned long long)t.changeovers);
    kv(tr("connect packets"), "%llu", (unsigned long long)t.connects);
    kv(tr("input"), "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kv(tr("signal time"), "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Signal"), !on ? 0 : a.rx.pactor.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp(tr("Packets"), !on ? 0 : a.rx.pactor.dataValid ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::pactorSummary(a.rx.pactor).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "PACTOR-I";
    if (live(a)) l2 = dect2::pactorSummary(a.rx.pactor);
}

} // namespace

extern const ModeUi kPactorUi;
const ModeUi kPactorUi = {
    .sideTitle = "STATIONS",
    .tabName = "PACTOR-I",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .tick = tick,
};
