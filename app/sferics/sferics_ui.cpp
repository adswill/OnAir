// Sferics (lightning) screens: strikes, flashes and strikes per minute, a 60 minute activity histogram, the list of the last strikes with
// their time, strength and width, and the noise floor. One receiver sees when and how strong, not where: no map. Simple view: the meters,
// the histogram and the list side by side; Advanced: the same with the details. The threshold, the mains filter and the merge time are
// the decoder options in the side panel; "Clear counts" zeroes the counters.
#include "app.h"
#include "i18n/i18n.h"
#include "dect2/common/time_fmt.h"
#include "dect2/sferics/sferics_gen.h"
#include "dect2/sferics/sferics_rx.h"
#include "platform/platform.h"
#include "ui/kit/kit.h"
#include "ui/kit/mode_frame.h"
#include "ui/kit/mode_panel.h"
#include <algorithm>
#include <cstdio>
#include <string>

namespace {

struct State {
    bool loaded = false, wasRunning = false;
    float thrDb = 10.f, mergeMs = 0.5f;
    bool mains = true;
    float pThr = -1, pMerge = -1;
    int pMains = -1;
};
State S;

bool live(const App& a) { return a.rx.standard == 244; }   // the engine reports its standard code minus one

void load() {
    if (S.loaded) return;
    S.loaded = true;
    S.thrDb = (float)plat::prefs().getD("sfericsThr", 10.0);
    S.mergeMs = (float)plat::prefs().getD("sfericsMerge", 0.5);
    S.mains = plat::prefs().getB("sfericsMains", true);
}

std::string ago(double s) {
    char b[48];
    if (s < 0) return "-";
    if (s < 90) snprintf(b, sizeof b, "%.0f s", s);
    else if (s < 5400) snprintf(b, sizeof b, "%.0f min", s / 60);
    else snprintf(b, sizeof b, "%.1f h", s / 3600);
    return b;
}

void notRunning(const App& a) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); }

void meters(App& a) {
    const dect2::SfericsTelemetry& t = a.rx.sferics;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(150 * gUi); ImGui::PushFont(a.mono, 0); ImGui::Text(fmt, v...); ImGui::PopFont(); };
    kv(tr("state"), "%s", dect2::sfericsSummary(t).c_str());
    kv(tr("activity"), "%s", tr(dect2::sfericsActivity(t.perMin1)));
    kv(tr("strikes per minute (1 min)"), "%.0f", t.perMin1);
    kv(tr("strikes per minute (10 min)"), "%.1f", t.perMin10);
    kv(tr("strikes"), "%llu", (unsigned long long)t.strikes);
    kv(tr("flashes"), "%llu", (unsigned long long)t.flashes);
    kv(tr("last strike"), "%s", ago(t.lastAgoSec).c_str());
    if (t.mainsActive) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", tr("A regular 100 / 120 Hz impulse train (mains interference) is being ignored.")); ImGui::PopTextWrapPos(); }
}

void histogram(App& a) {
    const dect2::SfericsTelemetry& t = a.rx.sferics;
    ImGui::TextDisabled("%s", tr("Strikes per minute, last 60 minutes (right = now)"));
    if (t.perMinute.empty()) return;
    float mx = 1;
    std::vector<float> v(t.perMinute.begin(), t.perMinute.end());
    for (float f : v) mx = std::max(mx, f);
    ImGui::PlotHistogram("##sferhist", v.data(), (int)v.size(), 0, nullptr, 0.f, mx * 1.1f, ImVec2(ImGui::GetContentRegionAvail().x, std::max(70.f, 90 * gUi)));
}

void recent(App& a) {
    const dect2::SfericsTelemetry& t = a.rx.sferics;
    if (!ImGui::BeginTable("##sferr", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(tr("Over floor (dB)")); ImGui::TableSetupColumn(tr("Width (us)")); ImGui::TableSetupColumn(tr("Flash"));
    ImGui::TableSetupColumn(tr("Signal time (s)"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    for (const dect2::SfericsStrike& s : t.recent) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(s.utc).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.1f", (double)s.peakDb);
        ImGui::TableNextColumn(); ImGui::Text("%.0f", (double)s.widthUs);
        ImGui::TableNextColumn(); ImGui::Text("%d", s.flash);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%.4f", s.timeSec);
    }
    ImGui::PopFont();
    ImGui::EndTable();
    if (t.recent.empty()) ImGui::TextDisabled("%s", tr("no strikes yet"));
}

void intro() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Lightning shows as short radio crackles (sferics) on every band below about 30 MHz, strongest at VLF / LF and in the evening on HF. "
                                 "Tune a quiet frequency (3 to 8 MHz, or an unused AM broadcast channel) and watch the strikes. One receiver gives the time and strength of each strike, not its position. Receive only."));
    ImGui::PopTextWrapPos();
}

void tab(App& a) {
    load();
    kit::modeHeader({243, "Sferics", live(a) ? 1 : a.engine.running() ? 2 : 0, -1000.f, a.freqMhz * 1e6});
    intro();
    ImGui::Spacing();
    if (!live(a)) { notRunning(a); return; }
    meters(a);
    ImGui::Spacing();
    histogram(a);
    ImGui::Spacing();
    recent(a);
}

void command(App& a) {
    load();
    kit::modeHeader({243, "Sferics", live(a) ? 1 : a.engine.running() ? 2 : 0, -1000.f, a.freqMhz * 1e6});
    if (!live(a)) { intro(); notRunning(a); return; }
    modepanel::panes({{"##sferact", tr("Activity"), [&a] { meters(a); ImGui::Spacing(); histogram(a); }, 1.f, 260.f},
                      {"##sferstr", tr("Last strikes"), [&a] { recent(a); }, 1.f, 220.f}}, 340.f);
}

void receiver(App& a) {
    if (!live(a)) { notRunning(a); return; }
    const dect2::SfericsTelemetry& t = a.rx.sferics;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(150 * gUi); ImGui::PushFont(a.mono, 0); ImGui::Text(fmt, v...); ImGui::PopFont(); };
    kv(tr("noise floor"), "%.1f dBFS", (double)t.floorDb);
    kv(tr("threshold"), "%.0f dB over the floor", (double)t.thresholdDb);
    kv(tr("last strike strength"), "%.1f dB over the floor", (double)t.snrDb);
    kv(tr("mains impulses ignored"), "%llu", (unsigned long long)t.mains);
    kv(tr("too long (over 5 ms), ignored"), "%llu", (unsigned long long)t.tooLong);
    kv(tr("input"), "%.3f Msps", t.inputRate / 1e6);
    kv(tr("signal time"), "%.1f s", t.timeSec);
}

void list(App&) {}

void decoder(App& a, bool&) {
    load();
    (void)a;
    ImGui::TextDisabled("%s", tr("THRESHOLD")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(100 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::SliderFloat("##sferthr", &S.thrDb, 6.f, 30.f, "%.0f dB")) { plat::prefs().setD("sfericsThr", S.thrDb); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("How far a crackle must stand over the noise floor to count. Lower finds weaker (more distant) strikes but also counts noise."));
    if (ImGui::Checkbox(tr("Ignore mains interference"), &S.mains)) { plat::prefs().setB("sfericsMains", S.mains); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Recognises a regular 100 / 120 Hz impulse train (lamp dimmers, chargers, power lines) and leaves it out of the count."));
    ImGui::TextDisabled("%s", tr("MERGE")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(100 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::SliderFloat("##sfermerge", &S.mergeMs, 0.1f, 5.f, "%.1f ms")) { plat::prefs().setD("sfericsMerge", S.mergeMs); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Crackles closer together than this are one strike (a return stroke often rings in several pulses)."));
    if (ImGui::SmallButton(tr("Clear counts##sfer"))) a.engine.sferics().clearCounts();
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    static const char* scenes[] = {"Thunderstorm", "Mains interference only", "Storm and mains", "Quiet"};
    ImGui::SetNextItemWidth(150 * gUi);
    int o = std::clamp(sc.modeOpt[0], 0, 3);
    if (ImGui::BeginCombo("##sfersc", tr(scenes[o]))) {
        for (int i = 0; i < 4; i++) if (ImGui::Selectable(tr(scenes[i]), o == i)) { sc.modeOpt[0] = i; changed = true; }
        ImGui::EndCombo();
    }
    bool hz60 = sc.modeOpt[1] == 1;
    if (ImGui::Checkbox(tr("60 Hz mains"), &hz60)) { sc.modeOpt[1] = hz60 ? 1 : 0; changed = true; }
    float r = sc.modeVal[0] > 0 ? (float)sc.modeVal[0] : 1.5f;
    ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::SliderFloat("##sferrate", &r, 0.1f, 20.f, "%.1f / s", ImGuiSliderFlags_Logarithmic)) { sc.modeVal[0] = r; changed = true; }
}

void tick(App& a) {
    load();
    dect2::SfericsReceiver& r = a.engine.sferics();
    if (S.pThr != S.thrDb) { S.pThr = S.thrDb; r.setThresholdDb(S.thrDb); }
    if (S.pMains != (S.mains ? 1 : 0)) { S.pMains = S.mains ? 1 : 0; r.setMainsFilter(S.mains); }
    if (S.pMerge != S.mergeMs) { S.pMerge = S.mergeMs; r.setMergeMs(S.mergeMs); }
    const bool run = a.engine.running();
    if (run && !S.wasRunning) { S.pThr = S.pMerge = -1; S.pMains = -1; }   // a new start: send the options again
    S.wasRunning = run;
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    lamp("IQ", run ? 1 : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Strikes"), !on ? 0 : a.rx.sferics.state == 3 ? 1 : 2); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::sfericsSummary(a.rx.sferics).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Sferics";
    if (live(a)) l2 = dect2::sfericsSummary(a.rx.sferics);
}

} // namespace

extern const ModeUi kSfericsUi;
const ModeUi kSfericsUi = {
    .sideTitle = "STRIKES",
    .tabName = "Sferics",
    .tabIcon = Ic::Wave,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .decoder = decoder,
    .synth = synth,
    .tick = tick,
    .command = command,
};
