// HF ALE screens: the common ALE channels (HFN / amateur, upper sideband dial), the nets known, Listen, then three views: the event
// log (UTC, frequency, from, to, call type, AMD text and commands, BER, SNR), the stations heard (address, last heard, count), and
// every word decoded (type, characters, vote and Golay errors).
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "dect2/ale/ale_proto.h"
#include "dect2/ale/ale_rx.h"
#include "dect2/ale/ale_tel.h"
#include "dect2/ale/ale_lqa.h"
#include "../hfbeacon/hfprop_ui.h"
#include "platform/platform.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    bool wasRunning = false, loaded = false;
    bool listen = false; int pushedListen = -1; float pushedVol = -1; bool pushedMute = false;   // "Listen": off until switched on
    int view = 0;                        // 0 events, 1 stations, 2 words, 3 link quality
    std::string lqaSel;                  // the station the link-quality view shows in detail
    int demod = dect2::HfDemodAuto;      // the front end's demodulator (HfDemod)
    char nets[128] = "";
    std::string pushedNets = "\x01";
};
State S;

bool live(const App& a) { return a.rx.standard == 50; }   // the engine reports its standard code minus one

void load() {
    if (S.loaded) return;
    S.loaded = true;
    S.listen = plat::prefs().getB("aleListen", false);
    S.demod = hfDemodLoad("aleDemod");
    const std::string n = plat::prefs().getS("aleNets", "");
    snprintf(S.nets, sizeof S.nets, "%s", n.c_str());
}

void utcText(double t, char* out, size_t n) {
    snprintf(out, n, "%s", dect2::timefmt::clock((int64_t)std::floor(t)).c_str());
}

std::vector<std::string> netList() {
    std::vector<std::string> v;
    std::string cur;
    for (const char* p = S.nets;; p++) {
        const char c = *p;
        if (c == 0 || c == ',' || c == ' ') { if (!cur.empty()) v.push_back(cur); cur.clear(); if (!c) break; continue; }
        cur += (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
    }
    return v;
}

ImVec4 typeColour(const std::string& t) {
    if (t == "Sounding") return ImVec4(0.60f, 0.70f, 0.78f, 1);
    if (t == "Response" || t == "Net response") return ImVec4(0.55f, 0.85f, 0.60f, 1);
    if (t == "Ack") return ImVec4(0.40f, 0.95f, 0.55f, 1);
    if (t == "Net" || t == "Group" || t == "AllCall" || t == "AnyCall") return ImVec4(0.95f, 0.75f, 0.45f, 1);
    return ImVec4(0.92f, 0.94f, 0.96f, 1);
}

void presets(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Channel"); flowNext(6 * gUi);
    int n = 0;
    const dect2::AlePreset* p = dect2::alePresets(n);
    for (int i = 0; i < n; i++) {
        const bool here = std::fabs(a.freqMhz - p[i].mhz) < 0.002;
        if (here) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
        char b[40]; snprintf(b, sizeof b, "%.3f##alep%d", p[i].mhz, i);
        if (ImGui::Button(b)) tuneFreq(a, p[i].mhz);
        if (here) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s ALE channel, %.3f MHz dial (upper sideband)", p[i].name, p[i].mhz);
        flowNext(4 * gUi);
    }
    ImGui::TextDisabled("|"); flowNext(8 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Nets"); flowNext(4 * gUi);
    ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::InputTextWithHint("##alenets", "HFN, ...", S.nets, sizeof S.nets)) plat::prefs().setS("aleNets", S.nets);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Net addresses you know, comma separated: calls to them are shown as net calls at once.\nOthers are found when a station other than the one called answers.");
    flowNext(8 * gUi);
    listenControls(a, S.listen, "aleListen", "ale", "Play the sideband (dial + 200 Hz to 3.8 kHz) on the sound card; decoding is the same either way");
    flowNext(8 * gUi);
    if (ImGui::Button("Clear##ale")) a.engine.ale().clearLog();
    flowEnd();
}

void eventsView(App& a) {
    const auto& ev = a.rx.ale.events;
    if (!ImGui::BeginTable("##alee", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("From"); ImGui::TableSetupColumn("To"); ImGui::TableSetupColumn("Type");
    ImGui::TableSetupColumn("BER"); ImGui::TableSetupColumn("dB"); ImGui::TableSetupColumn("AMD / commands", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    char b[32];
    for (const auto& e : ev) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); utcText(e.utc, b, sizeof b); ImGui::TextDisabled("%s", b);
        ImGui::TableNextColumn(); if (e.dialHz > 0) ImGui::Text("%.3f", e.dialHz / 1e6); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.36f, 1), "%s", e.from.empty() ? "?" : e.from.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(e.to.empty() ? "-" : e.to.c_str());
        ImGui::TableNextColumn(); ImGui::TextColored(typeColour(e.type), "%s", e.type.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s, %d words, %s, tone offset %+.0f Hz", e.type.c_str(), e.words, e.tis ? "TIS: an answer requested" : "TWS: no answer", e.cfoHz);
        ImGui::TableNextColumn(); ImGui::Text("%.1f%%", 100 * e.ber);
        ImGui::TableNextColumn(); ImGui::Text("%+.0f", e.snrDb);
        ImGui::TableNextColumn();
        ImGui::PushTextWrapPos(0);
        if (!e.amd.empty()) ImGui::TextColored(ImVec4(0.70f, 0.90f, 1.0f, 1), "%s", e.amd.c_str());
        if (!e.cmd.empty()) ImGui::TextDisabled("%s", e.cmd.c_str());
        if (e.amd.empty() && e.cmd.empty()) ImGui::TextDisabled(" ");
        ImGui::PopTextWrapPos();
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::PopFont();
    ImGui::EndTable();
    if (ev.empty()) ImGui::TextDisabled("no calls yet");
}

void stationsView(App& a) {
    const auto& st = a.rx.ale.stations;
    if (!ImGui::BeginTable("##ales", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Address"); ImGui::TableSetupColumn("Last heard"); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("Count"); ImGui::TableSetupColumn("dB");
    ImGui::TableSetupColumn("Last", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    char b[32];
    for (const auto& s : st) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.36f, 1), "%s", s.address.c_str());
        ImGui::TableNextColumn(); utcText(s.lastUtc, b, sizeof b); ImGui::TextDisabled("%s", b);
        ImGui::TableNextColumn(); if (s.lastDialHz > 0) ImGui::Text("%.3f", s.lastDialHz / 1e6); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::Text("%d", s.count);
        ImGui::TableNextColumn(); ImGui::Text("%+.0f", s.snrDb);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", s.lastType.c_str());
    }
    ImGui::PopFont();
    ImGui::EndTable();
    if (st.empty()) ImGui::TextDisabled("no stations yet");
}

void wordsView(App& a) {
    const auto& w = a.rx.ale.words;
    if (!ImGui::BeginTable("##alew", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("Text"); ImGui::TableSetupColumn("Vote"); ImGui::TableSetupColumn("Golay", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    char b[32];
    for (const auto& x : w) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); utcText(x.utc, b, sizeof b); ImGui::TextDisabled("%s", b);
        ImGui::TableNextColumn(); ImGui::Text("%s", dect2::ale::typeName(x.type));
        ImGui::TableNextColumn(); ImGui::Text("\"%s\"", x.text.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%d", x.voteErrs);
        ImGui::TableNextColumn(); ImGui::Text("%d", x.golayErrs);
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::PopFont();
    ImGui::EndTable();
    if (w.empty()) ImGui::TextDisabled("no words yet");
}

// ---------------------------------------------------------------- link quality (ale_lqa.h)

double utcNow() { return (double)std::time(nullptr); }

void lqaTick() {   // the link-quality memory is kept between runs
    static bool loaded = false;
    static double lastSave = 0;
    static uint64_t saved = 0;
    const std::string path = plat::dataDir() + "/ale-link-quality.txt";
    dect2::AleLqa& L = dect2::AleLqa::global();
    if (!loaded) {
        loaded = true;
        std::ifstream f(path, std::ios::binary);
        std::ostringstream ss; ss << f.rdbuf();
        L.load(ss.str());
        lastSave = utcNow();
        saved = L.total();
    }
    if (utcNow() - lastSave < 300 || L.total() == saved) return;
    lastSave = utcNow(); saved = L.total();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << L.save();
}

ImVec4 scoreColour(double s) { return s >= 66 ? ImVec4(0.45f, 0.85f, 0.5f, 1) : s >= 33 ? ImVec4(0.95f, 0.8f, 0.35f, 1) : ImVec4(0.8f, 0.5f, 0.45f, 1); }

void lqaView(App& a) {
    (void)a;
    dect2::AleLqa& L = dect2::AleLqa::global();
    const double now = utcNow();
    const std::vector<dect2::hfp::BandNow> prop = hfprop_ui::model().now(now);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Every call and sounding heard is a measurement of the path from that station on that frequency at that time of day. The best channel to reach each station now is worked out from those, from what was heard in the last half hour, and from the HF propagation monitor."));
    ImGui::PopTextWrapPos();
    const auto best = L.best(now, &prop);
    if (best.empty()) { ImGui::TextDisabled("%s", tr("Nothing measured yet: leave ALE running on a few channels (soundings come every half hour or so).")); return; }
    if (ImGui::BeginTable("##alelqa", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y * 0.55f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Station")); ImGui::TableSetupColumn(tr("Best now (MHz)")); ImGui::TableSetupColumn(tr("Score")); ImGui::TableSetupColumn("dB"); ImGui::TableSetupColumn(tr("Heard"));
        ImGui::TableSetupColumn(tr("Why"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& r : best) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(r.station.c_str(), S.lqaSel == r.station, ImGuiSelectableFlags_SpanAllColumns)) S.lqaSel = r.station;
            ImGui::TableNextColumn(); ImGui::Text("%.3f", r.hz / 1e6);
            ImGui::TableNextColumn(); ImGui::TextColored(scoreColour(r.score), "%.0f", r.score);
            ImGui::TableNextColumn(); ImGui::Text("%+.0f", (double)r.snrDb);
            ImGui::TableNextColumn(); ImGui::TextDisabled("%.0f min", std::max(0.0, (now - r.lastUtc) / 60.0));
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", r.why.c_str());
        }
        ImGui::EndTable();
    }
    if (S.lqaSel.empty()) return;
    ImGui::TextUnformatted(tr("All channels of"));
    ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.36f, 1), "%s", S.lqaSel.c_str());
    const auto ranks = L.rank(S.lqaSel, now, &prop);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const auto& r : ranks) {
        char b[96];
        snprintf(b, sizeof b, "%.3f MHz  %3.0f", r.hz / 1e6, r.score);
        ImGui::TextColored(scoreColour(r.score), "%s", b);
        ImGui::SameLine(150 * gUi);
        // the 24 hour profile: one bar per UTC hour (height = SNR), the current hour outlined
        const auto prof = L.profile(r.station, r.hz, now);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float bw = 8 * gUi, h = ImGui::GetTextLineHeight();
        for (int hr = 0; hr < 24; hr++) {
            const float x = p.x + hr * (bw + 1);
            if (prof[(size_t)hr].w > 0) {
                const float q = (float)std::max(0.0, std::min(1.0, (prof[(size_t)hr].snrDb + 5.0) / 30.0));
                dl->AddRectFilled(ImVec2(x, p.y - 2 + h * (1 - q)), ImVec2(x + bw, p.y - 2 + h), IM_COL32(110, 190, 120, 230));
            } else dl->AddRectFilled(ImVec2(x, p.y - 4 + h), ImVec2(x + bw, p.y - 2 + h), IM_COL32(70, 70, 70, 255));
        }
        { const int nowH = (int)std::fmod(now / 3600.0, 24.0); const float x = p.x + nowH * (bw + 1); dl->AddRect(ImVec2(x - 1, p.y - 4), ImVec2(x + bw + 1, p.y + h), IM_COL32(255, 255, 255, 200)); }
        ImGui::SameLine(150 * gUi + 24 * (bw + 1) + 10 * gUi);
        ImGui::TextDisabled("%s", r.why.c_str());
    }
    ImGui::TextDisabled("%s", tr("Bars: mean level at each UTC hour (-5 to 25 dB); the outlined hour is now."));
}

// ---------------------------------------------------------------- hooks

void tick(App& a) {
    load();
    lqaTick();
    if (!a.engine.running()) { S.wasRunning = false; return; }
    dect2::AleReceiver& r = a.engine.ale();
    if (!S.wasRunning || S.pushedNets != S.nets) { r.setNetAddresses(netList()); S.pushedNets = S.nets; }
    if (!S.wasRunning || (int)S.listen != S.pushedListen) { r.setListen(S.listen); S.pushedListen = S.listen; }
    r.setDemod(dect2::hfDemodResolve(S.demod, a.freqMhz));   // Auto follows the dial
    if (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute) {   // the sound controls are shared with the other modes
        r.setVolume(a.volume); r.setMuted(a.muted);
        S.pushedVol = a.volume; S.pushedMute = a.muted;
    }
    S.wasRunning = true;
}

void tuner(App& a, bool&) {
    load();
    hfDemodControl(a, S.demod, "aleDemod", "ale");
}

void tab(App& a) {
    load();
    presets(a);
    ImGui::Spacing();
    subNav("alev", S.view, {"Events", "Stations", "Words", "Link quality"});
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    if (S.view == 0) eventsView(a); else if (S.view == 1) stationsView(a); else if (S.view == 2) wordsView(a); else lqaView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    if (!ImGui::BeginTable("##alel", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Address"); ImGui::TableSetupColumn("Heard", ImGuiTableColumnFlags_WidthStretch);
    char b[32];
    for (const auto& s : a.rx.ale.stations) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(s.address.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); utcText(s.lastUtc, b, sizeof b); ImGui::TextDisabled("%s, %dx", b, s.count);
    }
    ImGui::EndTable();
    if (a.rx.ale.stations.empty()) ImGui::TextDisabled("no stations yet");
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::AleTelemetry& t = a.rx.ale;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("state", "%s", dect2::aleSummary(t).c_str());
    kv("word sync", "%s", t.state == 2 ? "in sync (392 ms words)" : "searching");
    kv("words", "%llu decoded, %llu missed in sync", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kv("last word", "%+.1f dB in 3 kHz, BER %.1f%%", t.snrDb, 100 * t.ber);
    kv("tone offset", "%+.1f Hz (AFC)", t.cfoHz);
    kv("clock", "%s", t.timeMode ? "the recording's start time" : "system clock (UTC)");
    kv("input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Sync", !on ? 0 : a.rx.ale.state == 2 ? 1 : 0); flowNext(10 * gUi);
    lamp("Words", !on ? 0 : a.rx.ale.dataValid ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::aleSummary(a.rx.ale).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "HF ALE";
    if (live(a)) l2 = dect2::aleSummary(a.rx.ale);
}

} // namespace

extern const ModeUi kAleUi;
const ModeUi kAleUi = {
    .sideTitle = "STATIONS",
    .tabName = "ALE",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .tick = tick,
};
