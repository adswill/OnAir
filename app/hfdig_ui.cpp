// HF digital screens: the shell. One main tab with a view per decoder (RTTY, SSTV, FreeDV); each view is drawn by that decoder's own
// file (hfdig_rtty_ui.cpp, hfdig_sstv_ui.cpp, hfdig_freedv_ui.cpp). The shell also keeps the sound controls in step for FreeDV's speech.
#include "app.h"
#include "dect2/hfdig_tel.h"

// the views of the decoders, each in its own file
void hfdigRttyTab(App& a, const dect2::HfdigTelemetry& t);
void hfdigSstvTab(App& a, const dect2::HfdigTelemetry& t);
void hfdigFreedvTab(App& a, const dect2::HfdigTelemetry& t);

namespace {

struct State {
    int view = 0;                        // 0 RTTY, 1 SSTV, 2 FreeDV
    bool wasRunning = false;
    float pushedVol = -1;
    bool pushedMute = false;
};
State S;

bool live(const App& a) { return a.engine.running() && a.rx.standard == 26; }   // the engine reports its standard code minus one

void tick(App& a) {
    const bool run = a.engine.running();
    if (run && a.rx.standard == 26 && (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute)) {   // the sound controls are shared with the other modes
        a.engine.hfdig().setVolume(a.volume);
        a.engine.hfdig().setMuted(a.muted);
        S.pushedVol = a.volume; S.pushedMute = a.muted;
    }
    S.wasRunning = run;
}

void tab(App& a) {
    subNav("hfdv", S.view, {"RTTY", "SSTV", "FreeDV"});
    const dect2::HfdigTelemetry& t = a.rx.hfdig;
    if (S.view == 0) hfdigRttyTab(a, t);
    else if (S.view == 1) hfdigSstvTab(a, t);
    else hfdigFreedvTab(a, t);
}

void list(App& a) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", a.engine.running() ? "decoder not built yet" : "start the receiver");
    ImGui::PopTextWrapPos();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::HfdigTelemetry& t = a.rx.hfdig;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("state", "%s", "no decoder yet");
    kv("input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kv("sideband", "%.1f dBFS (200 - 3800 Hz above the dial frequency)", t.audioDb);
    kv("signal time", "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Decoder", 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : "no decoder yet"); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "HF digital";
    if (live(a)) l2 = dect2::hfdigSummary(a.rx.hfdig);
}

} // namespace

extern const ModeUi kHfdigUi;
const ModeUi kHfdigUi = {
    .sideTitle = "MESSAGES",
    .tabName = "Decoders",
    .tabIcon = Ic::Doc,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
