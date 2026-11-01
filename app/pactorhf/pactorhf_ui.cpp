// PACTOR-II / -III / -IV screens (receive only, physical layer): the band presets, then three views: the signal (what the analysis found: carriers,
// baud, clock error, speed level, frame period and sync pattern), the symbols (the newest phase steps of the first carriers) and the events.
// The channel coding is proprietary and not decoded, so there is no text (docs/pactorhf.md).
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "gfx/gfx.h"
#include "i18n/i18n.h"
#include "dect2/pactorhf/pactorhf_rx.h"
#include "dect2/pactorhf/pactorhf_tel.h"
#include "dect2/pactor/pactor_rx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct State { int view = 0; };
State S[3];

template <int V> int active() { return 237 + V; }   // the engine reports its standard code minus one: 238, 239, 240 for V = 0, 1, 2
template <int V> bool live(const App& a) { return a.rx.standard == active<V>(); }
template <int V> int variant() { return V + 2; }

void presets(App& a, int v) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", tr("Physical layer only"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The channel coding of PACTOR-II, -III and -IV is proprietary (SCS) and not published. This mode finds the carriers, the symbol clock and the repeating frame structure and shows the symbols; it does not produce text."));
    flowNext(8 * gUi);
    ImGui::TextDisabled("%s", tr("Band"));
    flowNext(6 * gUi);
    int n = 0;
    const dect2::PactorPreset* p = dect2::pactorPresets(n);
    for (int i = 0; i < n; i++) {
        const bool here = std::fabs(a.freqMhz - p[i].mhz) < 0.002;
        if (here) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
        char b[40]; snprintf(b, sizeof b, "%s##pach%d_%d", p[i].name, v, i);
        if (ImGui::Button(b)) tuneFreq(a, p[i].mhz);
        if (here) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.4f MHz", p[i].mhz);
        flowNext(4 * gUi);
    }
    ImGui::TextDisabled("|"); flowNext(8 * gUi);
    char cl[40]; snprintf(cl, sizeof cl, "%s##pach%d", tr("Clear"), v);
    if (ImGui::Button(cl)) a.engine.pactorHf(v).clearResults();
    flowEnd();
}

void signalView(App& a, int v) {
    const dect2::PactorHfTelemetry& t = a.rx.pactorhf;
    auto kv = [&](const char* k, const char* fmt, auto... x) { ImGui::TextDisabled("%s", k); kvColumn(150 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, x...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv(tr("state"), "%s", dect2::pactorHfSummary(t).c_str());
    if (t.variant == 0) { ImGui::TextDisabled("%s", tr("no signal yet")); return; }
    if (t.variant != v) ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s %s", tr("The signal on the channel looks like"), dect2::pactorHfName(t.variant));
    kv(tr("symbol rate"), "%.2f baud (%+.0f ppm)", t.baud, t.clockPpm);
    kv(tr("carriers"), "%d%s", t.nCarriers, t.variant == 3 && t.level ? (std::string(", ") + tr("speed level") + " " + std::to_string(t.level)).c_str() : "");
    if (t.nCarriers > 1) kv(tr("spacing"), "%.0f Hz, %s %.0f Hz", t.spacingHz, tr("occupies about"), t.bwHz);
    kv(tr("centre"), "%.0f Hz", t.centreHz);
    kv(tr("clock line"), "%.0f dB", t.lineDb);
    if (t.variant == 4) {
        const char* c = t.constellation == 2 ? "BPSK" : t.constellation == 4 ? "QPSK" : t.constellation == 8 ? "8-PSK" : t.constellation ? "QAM" : "?";
        kv(tr("constellation"), "%s (%s)", c, tr("estimate, no equaliser"));
        kv(tr("analysis"), "%s", t.note.c_str());
    } else {
        for (size_t i = 0; i < t.carrierHz.size() && i < 18; i++) {
            char k[40]; snprintf(k, sizeof k, "%s %zu", tr("carrier"), i + 1);
            kv(k, "%.1f Hz, %s", t.carrierHz[i], t.carrierOrder[i] == 2 ? "D-BPSK" : t.carrierOrder[i] == 4 ? "D-QPSK" : t.carrierOrder[i] == 8 ? "D-8PSK" : "D-16PSK");
        }
        if (t.framePeriod) {
            kv(tr("frame repeats"), "%d %s = %.3f s (%d %s)", t.framePeriod, tr("symbols"), t.framePeriodSec, t.frameCopies, tr("copies"));
            if (t.syncLen > 0) kv(tr("constant part"), "%d %s from symbol %d: %s", t.syncLen, tr("symbols"), t.syncPos, t.syncHex.c_str());
        } else kv(tr("frame repeats"), "%s", tr("none found yet"));
    }
}

void symbolsView(App& a, int) {
    const dect2::PactorHfTelemetry& t = a.rx.pactorhf;
    if (t.rows.empty()) { ImGui::TextDisabled("%s", tr(t.variant == 4 ? "PACTOR-IV symbols are not extracted (no equaliser)" : "no symbols yet")); return; }
    ImGui::TextDisabled("%s", tr("The newest phase steps of the first carriers (one digit per symbol, 0 to 3 = steps of 90 degrees in D-QPSK):"));
    ImGui::PushFont(a.mono, 0);
    for (size_t i = 0; i < t.rows.size(); i++) {
        ImGui::TextDisabled("%zu", i + 1); ImGui::SameLine();
        ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(t.rows[i].c_str()); ImGui::PopTextWrapPos();
    }
    ImGui::PopFont();
}

void eventsView(App& a, int) {
    const auto& ev = a.rx.pactorhf.events;
    if (!ImGui::BeginTable("##pahe", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(tr("Event"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& e : ev) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(e.utc).c_str());
        ImGui::TableNextColumn(); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(e.text.c_str()); ImGui::PopTextWrapPos();
    }
    ImGui::EndTable();
    if (ev.empty()) ImGui::TextDisabled("%s", tr("nothing heard yet"));
}

template <int V> void tab(App& a) {
    presets(a, variant<V>());
    ImGui::Spacing();
    subNav(V == 0 ? "pah2v" : V == 1 ? "pah3v" : "pah4v", S[V].view, {"Signal", "Symbols", "Events"});
    if (!live<V>(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    if (S[V].view == 0) signalView(a, variant<V>()); else if (S[V].view == 1) symbolsView(a, variant<V>()); else eventsView(a, variant<V>());
}

template <int V> void list(App& a) {
    if (!live<V>(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const auto& ev = a.rx.pactorhf.events;
    for (const auto& e : ev) { ImGui::PushFont(a.mono, 0); ImGui::TextDisabled("%s", dect2::timefmt::clock(e.utc).c_str()); ImGui::PopFont(); ImGui::SameLine(); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(e.text.c_str()); ImGui::PopTextWrapPos(); }
    if (ev.empty()) ImGui::TextDisabled("%s", tr("nothing heard yet"));
}

template <int V> void receiver(App& a) {
    if (!live<V>(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::PactorHfTelemetry& t = a.rx.pactorhf;
    auto kv = [&](const char* k, const char* fmt, auto... x) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, x...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv(tr("state"), "%s", dect2::pactorHfSummary(t).c_str());
    kv(tr("analyses"), "%llu (%llu %s)", (unsigned long long)t.analyses, (unsigned long long)t.detections, tr("with a signal"));
    kv(tr("input"), "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kv(tr("signal time"), "%.1f s", t.timeSec);
}

template <int V> void status(App& a) {
    const bool run = a.engine.running(), on = live<V>(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Signal"), !on ? 0 : a.rx.pactorhf.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp(tr("Frames"), !on ? 0 : a.rx.pactorhf.dataValid ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::pactorHfSummary(a.rx.pactorhf).c_str()); ImGui::PopFont();
}

template <int V> void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = dect2::pactorHfName(variant<V>());
    if (live<V>(a)) l2 = dect2::pactorHfSummary(a.rx.pactorhf);
}

} // namespace

extern const ModeUi kPactor2Ui, kPactor3Ui, kPactor4Ui;
const ModeUi kPactor2Ui = {.sideTitle = "EVENTS", .tabName = "PACTOR-II", .tabIcon = Ic::Radio, .tab = tab<0>, .receiver = receiver<0>, .list = list<0>, .status = status<0>, .summary = summary<0>};
const ModeUi kPactor3Ui = {.sideTitle = "EVENTS", .tabName = "PACTOR-III", .tabIcon = Ic::Radio, .tab = tab<1>, .receiver = receiver<1>, .list = list<1>, .status = status<1>, .summary = summary<1>};
const ModeUi kPactor4Ui = {.sideTitle = "EVENTS", .tabName = "PACTOR-IV", .tabIcon = Ic::Radio, .tab = tab<2>, .receiver = receiver<2>, .list = list<2>, .status = status<2>, .summary = summary<2>};
