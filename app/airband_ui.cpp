// Airband screens: the channel list with live levels and squelch, adding and editing channels (validated to the 25 / 8.33 kHz raster),
// scan, squelch and hang, the activity log, and the band spectrum with a marker for every channel.
#include "app.h"
#include "plot.h"
#include "dect2/airband_gen.h"
#include "dect2/airband_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct State {
    bool loaded = false;
    std::vector<dect2::AirbandChannel> chans;
    float squelch = 6, hang = 0.5f;
    bool scan = false;
    bool dirty = true;                   // push the list and the settings to the receiver
    int view = 0;                        // 0 channels, 1 activity, 2 spectrum
    char addName[24] = "", addLabel[32] = "";
    std::string addErr;
    int editing = -1;
    char editLabel[32] = "";
    bool wasRunning = false;
    float pushedVol = -1; bool pushedMute = false;
};
State S;

bool live(const App& a) { return a.engine.running() && a.rx.standard == 27; }   // the engine reports its standard code minus one

std::string encode(const std::vector<dect2::AirbandChannel>& v) {
    std::string s;
    for (const auto& c : v) {
        char b[128];
        std::string l = c.label;
        for (auto& ch : l) if (ch == '|' || ch == ';') ch = ' ';
        snprintf(b, sizeof b, "%.1f|%d|%d|%d|%d|%s;", c.freqHz, c.is833 ? 1 : 0, c.muted ? 1 : 0, c.solo ? 1 : 0, c.priority ? 1 : 0, l.c_str());
        s += b;
    }
    return s;
}

std::vector<dect2::AirbandChannel> decode(const std::string& s) {
    std::vector<dect2::AirbandChannel> v;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ';')) {
        std::vector<std::string> f;
        std::stringstream is(item);
        std::string x;
        while (std::getline(is, x, '|')) f.push_back(x);
        if (f.size() < 5) continue;
        dect2::AirbandChannel c;
        c.freqHz = atof(f[0].c_str()); c.is833 = f[1] == "1"; c.muted = f[2] == "1"; c.solo = f[3] == "1"; c.priority = f[4] == "1";
        c.label = f.size() > 5 ? f[5] : "";
        if (dect2::airbandOnRaster(c.freqHz, c.is833)) v.push_back(c);
    }
    return v;
}

void save() {
    plat::Prefs& d = plat::prefs();
    d.setS("airChans", encode(S.chans));
    d.setD("airSquelch", S.squelch); d.setD("airHang", S.hang); d.setB("airScan", S.scan);
    S.dirty = true;
}

void load() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    if (d.has("airChans")) S.chans = decode(d.getS("airChans", ""));
    else S.chans = dect2::airbandChannelsFor(dect2::airbandTestLayout(), dect2::airbandTuning().defMhz * 1e6);
    S.squelch = (float)d.getD("airSquelch", 6); S.hang = (float)d.getD("airHang", 0.5); S.scan = d.getB("airScan", false);
}

std::string clock(int64_t t) {
    if (t <= 0) return "-";
    const time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char b[16];
    snprintf(b, sizeof b, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

const dect2::AirbandChannelState* stateOf(const App& a, size_t i) {
    if (!live(a) || i >= a.rx.airband.channels.size()) return nullptr;
    const auto& s = a.rx.airband.channels[i];
    return std::fabs(s.freqHz - S.chans[i].freqHz) < 1 ? &s : nullptr;
}

// ---------------------------------------------------------------- the controls

void controls(App& a) {
    if (ImGui::Checkbox("Scan", &S.scan)) save();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play one channel at a time: stop on the first that opens (the priority channel first), go on 2 s after it closed.\nOff: every open channel is mixed; an open priority channel pushes the others 12 dB down.");
    flowNext(12 * gUi);
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Squelch"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(140 * gUi);
    ImGui::SliderFloat("##airsq", &S.squelch, -5.f, 30.f, "%.0f dB");
    if (ImGui::IsItemDeactivatedAfterEdit()) save();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Carrier to noise a channel needs to open. Lower opens on weaker stations; the noise floor is tracked per channel.");
    flowNext(12 * gUi);
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Hang"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(110 * gUi);
    ImGui::SliderFloat("##airhang", &S.hang, 0.f, 3.f, "%.1f s");
    if (ImGui::IsItemDeactivatedAfterEdit()) save();
    flowNext(12 * gUi);
    if (live(a)) {
        const auto& t = a.rx.airband;
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Radio off by"); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::Text("%+.2f kHz%s", t.cfoHz / 1e3, t.tuneKnown ? "" : " ?"); ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The radio's tuning error, measured from the carriers of the listed channels and taken out of every channel.\n'?' until a carrier has been seen.");
    }
    flowEnd();
}

void addRow(App& a) {
    ImGui::SetNextItemWidth(90 * gUi);
    const bool enter = ImGui::InputTextWithHint("##airadd", "118.005", S.addName, sizeof S.addName, ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Channel name as dialled (118.025 is a 25 kHz channel; 118.030 / .035 / .040 are the 8.33 kHz channels of that block),\nor a frequency in MHz on the 8.33 kHz raster (118.0083).");
    flowNext(6 * gUi);
    ImGui::SetNextItemWidth(110 * gUi);
    ImGui::InputTextWithHint("##airlab", "Tower", S.addLabel, sizeof S.addLabel);
    flowNext(6 * gUi);
    if (ImGui::Button("Add") || enter) {
        double f; bool n833;
        if (!dect2::airbandParse(S.addName, f, n833)) S.addErr = "not a channel: 118.000 - 136.990 on the 25 or 8.33 kHz raster";
        else {
            bool dup = false;
            for (const auto& c : S.chans) dup |= std::fabs(c.freqHz - f) < 1 && c.is833 == n833;
            if (dup) S.addErr = "already in the list";
            else {
                dect2::AirbandChannel c; c.freqHz = f; c.is833 = n833; c.label = S.addLabel;
                S.chans.push_back(c);
                std::sort(S.chans.begin(), S.chans.end(), [](const auto& x, const auto& y) { return x.freqHz < y.freqHz; });
                S.addName[0] = S.addLabel[0] = 0; S.addErr.clear();
                save();
            }
        }
    }
    flowNext(12 * gUi);
    if (ImGui::Button("Test channels")) { S.chans = dect2::airbandChannelsFor(dect2::airbandTestLayout(), a.freqMhz * 1e6); save(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The channels of the built-in test signal around the frequency tuned now.");
    flowEnd();
    if (!S.addErr.empty()) ImGui::TextColored(pal::warnAmber(), "%s", S.addErr.c_str());
}

// ---------------------------------------------------------------- the views

void channelView(App& a) {
    controls(a);
    addRow(a);
    ImGui::Spacing();
    const int nc = 9;
    if (!ImGui::BeginTable("##airch", nc, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("Channel"); ImGui::TableSetupColumn("Name"); ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 110 * gUi);
    ImGui::TableSetupColumn("Squelch"); ImGui::TableSetupColumn("Carrier"); ImGui::TableSetupColumn("Heard");
    ImGui::TableSetupColumn("M"); ImGui::TableSetupColumn("S"); ImGui::TableSetupColumn("P");
    ImGui::TableHeadersRow();
    int remove = -1;
    for (size_t i = 0; i < S.chans.size(); i++) {
        dect2::AirbandChannel& c = S.chans[i];
        const dect2::AirbandChannelState* s = stateOf(a, i);
        ImGui::PushID((int)i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushFont(a.mono, 0);
        ImGui::Text("%s", dect2::airbandName(c.freqHz, c.is833).c_str());
        ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.4f MHz, %s spacing", c.freqHz / 1e6, c.is833 ? "8.33 kHz" : "25 kHz");
        ImGui::SameLine(); ImGui::TextDisabled("%s", c.is833 ? "8.33" : "25");
        ImGui::TableNextColumn();
        if (S.editing == (int)i) {
            ImGui::SetNextItemWidth(100 * gUi);
            if (ImGui::InputText("##ed", S.editLabel, sizeof S.editLabel, ImGuiInputTextFlags_EnterReturnsTrue)) { c.label = S.editLabel; S.editing = -1; save(); }
        } else {
            ImGui::TextUnformatted(c.label.empty() ? "-" : c.label.c_str());
            if (ImGui::IsItemClicked()) { S.editing = (int)i; snprintf(S.editLabel, sizeof S.editLabel, "%s", c.label.c_str()); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to rename; right-click to remove the channel.");
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) remove = (int)i;
        }
        ImGui::TableNextColumn();
        {
            const float snr = s ? s->snrDb : -99;
            const float frac = std::max(0.f, std::min(1.f, (snr + 5) / 45));
            char ov[32] = "";
            if (s && s->inBand) snprintf(ov, sizeof ov, "%.0f dB", snr);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, s && s->open ? pal::accent() : pal::grey(0.45f));
            ImGui::ProgressBar(frac, ImVec2(110 * gUi, 0), ov);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && s) ImGui::SetTooltip("carrier to noise %.1f dB (squelch %.0f dB), channel power %.1f dBFS", s->snrDb, S.squelch, s->levelDb);
        }
        ImGui::TableNextColumn();
        if (!s) ImGui::TextDisabled("-");
        else if (!s->inBand) ImGui::TextColored(pal::warnAmber(), "out of band");
        else if (s->open) ImGui::TextColored(s->heterodyne ? pal::warnAmber() : pal::okGreen(), "%s%s", s->heterodyne ? "OPEN 2 stn" : "OPEN", s->playing ? " >" : "");
        else ImGui::TextDisabled("closed");
        ImGui::TableNextColumn();
        if (s && s->open) ImGui::Text("%+.0f Hz", s->carrierHz); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        if (s) ImGui::Text("%llu", (unsigned long long)s->transmissions); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (ImGui::Checkbox("##m", &c.muted)) save();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mute: listened to and logged, not played.");
        ImGui::TableNextColumn(); if (ImGui::Checkbox("##s", &c.solo)) save();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Solo: while any channel is solo, only the solo channels play.");
        ImGui::TableNextColumn();
        if (ImGui::Checkbox("##p", &c.priority)) { if (c.priority) for (size_t k = 0; k < S.chans.size(); k++) if (k != i) S.chans[k].priority = false; save(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Priority: takes over the scan when it opens, and pushes the other channels down in the mix.");
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (remove >= 0) { S.chans.erase(S.chans.begin() + remove); S.editing = -1; save(); }
    if (S.chans.empty()) ImGui::TextDisabled("no channel: add one above");
}

void activityView(App& a) {
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the activity"); ImGui::PopTextWrapPos(); return; }
    const auto& log = a.rx.airband.activity;
    if (ImGui::Button("Copy")) {
        std::string txt;
        for (const auto& e : log) {
            char b[200];
            snprintf(b, sizeof b, "%s\t%s\t%s\t%.1f s\t%.0f dB\t%.1f dBFS\n", clock(e.wallTime).c_str(), e.name.c_str(), e.label.c_str(), e.durSec, e.snrDb, e.levelDb);
            txt += b;
        }
        ImGui::SetClipboardText(txt.c_str());
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy the log to the clipboard, one transmission per line.");
    if (!ImGui::BeginTable("##airlog", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time UTC"); ImGui::TableSetupColumn("Channel"); ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("Length"); ImGui::TableSetupColumn("C/N"); ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& e : log) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(e.wallTime).c_str());
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(e.name.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(e.label.empty() ? "-" : e.label.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.1f s", e.durSec);
        ImGui::TableNextColumn(); ImGui::Text("%.0f dB%s", e.snrDb, e.heterodyne ? " 2 stn" : "");
        ImGui::TableNextColumn(); ImGui::Text("%.1f dBFS", e.levelDb);
    }
    ImGui::EndTable();
    if (log.empty()) ImGui::TextDisabled("nothing heard yet");
}

void spectrumView(App& a) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Every channel of the list: green while open, the light band its filter (8.33 kHz: +-3.4 kHz, 25 kHz: +-5 kHz).");
    ImGui::PopTextWrapPos();
    if (!plt::BeginPlot("##airspec", ImVec2(-1, ImGui::GetContentRegionAvail().y), plt::Flags_NoLegend | plt::Flags_NoTitle)) return;
    plt::SetupAxes("frequency (MHz)", "dBFS/bin");
    const double fs = (a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate) / 1e6;
    const double rc = radioCenterMhz(a);
    plt::SetupAxisLimits(plt::X1, rc - fs / 2, rc + fs / 2, plt::Cond_Once);
    plt::SetupAxisLimits(plt::Y1, a.yMin, a.yMax, plt::Cond_Once);
    plt::SetupAxisFormat(plt::X1, "%.3f");
    for (size_t i = 0; i < S.chans.size(); i++) {
        const auto& c = S.chans[i];
        const dect2::AirbandChannelState* s = stateOf(a, i);
        const double f = c.freqHz / 1e6 + (live(a) ? a.rx.airband.cfoHz / 1e6 : 0), hw = (c.is833 ? 3400 : 5000) / 1e6;
        char id[24];
        snprintf(id, sizeof id, "b%zu", i);
        plt::Spec bs; bs.FillColor = s && s->open ? ImVec4(0.3f, 0.8f, 0.4f, 0.25f) : pal::accent(0.12f);
        plt::PlotVBand(id, f - hw, f + hw, bs);
        snprintf(id, sizeof id, "m%zu", i);
        plt::Spec ms; ms.LineColor = s && s->open ? ImVec4(0.3f, 0.85f, 0.4f, 0.9f) : pal::grey(0.5f);
        plt::PlotInfLines(id, &f, 1, ms);
        const std::string lb = c.label.empty() ? dect2::airbandName(c.freqHz, c.is833) : c.label;
        plt::PlotText(lb.c_str(), f, a.yMax, ImVec2(0, 10 + (float)(i % 3) * 13));
    }
    if (!a.smooth.empty()) {
        auto x = xs(a);
        std::vector<double> y(a.smooth.begin(), a.smooth.end());
        plt::Spec ss; ss.LineColor = pal::accent(); ss.LineWeight = 1.2f;
        plt::PlotLine("spectrum", x.data(), y.data(), (int)std::min(x.size(), y.size()), ss);
    }
    plt::EndPlot();
}

// ---------------------------------------------------------------- hooks

void tick(App& a) {
    load();
    const bool run = a.engine.running() && a.rx.standard == 27;
    if (!a.engine.running()) { S.wasRunning = false; return; }
    dect2::AirbandReceiver& r = a.engine.airband();
    if (!S.wasRunning || S.dirty) {
        r.setChannels(S.chans); r.setSquelchDb(S.squelch); r.setHangSec(S.hang); r.setScan(S.scan);
        S.dirty = false;
    }
    if (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute) {   // the sound controls are shared with the other modes
        r.setVolume(a.volume); r.setMuted(a.muted);
        S.pushedVol = a.volume; S.pushedMute = a.muted;
    }
    S.wasRunning = true;
    (void)run;
}

void tab(App& a) {
    load();
    subNav("airv", S.view, {"Channels", "Activity", "Spectrum"});
    if (S.view == 0) channelView(a); else if (S.view == 1) activityView(a); else spectrumView(a);
}

void list(App& a) {
    load();
    if (!ImGui::BeginTable("##airl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Channel"); ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch);
    for (size_t i = 0; i < S.chans.size(); i++) {
        const auto& c = S.chans[i];
        const dect2::AirbandChannelState* s = stateOf(a, i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(dect2::airbandName(c.freqHz, c.is833).c_str()); ImGui::PopFont();
        ImGui::TableNextColumn();
        const std::string lb = c.label.empty() ? std::string("") : c.label + "  ";
        if (s && s->open) ImGui::TextColored(pal::okGreen(), "%sOPEN", lb.c_str());
        else ImGui::TextDisabled("%s%s", lb.c_str(), s && !s->inBand ? "out of band" : "");
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::AirbandTelemetry& t = a.rx.airband;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    int inBand = 0, open = 0;
    for (const auto& c : t.channels) { inBand += c.inBand; open += c.open; }
    kv("state", "%s", dect2::airbandSummary(t).c_str());
    kv("channels", "%zu listed, %d in the band, %d open", t.channels.size(), inBand, open);
    kv("tuning error", "%+.0f Hz%s", t.cfoHz, t.tuneKnown ? " (measured)" : " (not measured yet)");
    kv("squelch", "%.0f dB carrier to noise, hang %.1f s", t.squelchDb, t.hangSec);
    kv("audio", "%s", t.scan ? (t.scanChan >= 0 ? "scan: stopped on a channel" : "scan: scanning") : "mix of the open channels");
    kv("transmissions", "%llu", (unsigned long long)t.blocksOk);
    kv("radio centre", "%.4f MHz", t.centerHz / 1e6);
    kv("input", "%.3f Msps, %.1f dBFS; channels at %.0f Hz", t.inputRate / 1e6, t.levelDb, t.channelRate);
    kv("signal time", "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Open", !on ? 0 : a.rx.airband.state == 2 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::airbandSummary(a.rx.airband).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Airband";
    if (live(a)) l2 = dect2::airbandSummary(a.rx.airband);
}

} // namespace

extern const ModeUi kAirbandUi;
const ModeUi kAirbandUi = {
    .sideTitle = "CHANNELS",
    .tabName = "Channels",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
