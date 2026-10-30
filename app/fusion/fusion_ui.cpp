// Yaesu System Fusion screens (work in progress): lock, frame counts, call mode, source / destination callsigns, repeater, the slow data
// text and the state of the speech codec plug-in. The voice needs a codec that the user installs (codec id "ysf", docs/CODECS.md).
#include "app.h"
#include "ui/kit/mode_frame.h"
#include "ui/kit/session_list.h"
#include "voice/voice_monitor_ui.h"
#include "codec/speech_codec_ui.h"
#include "dect2/fusion/fusion_rx.h"
#include <cstdio>
#include <memory>
#include <string>

namespace {

bool live(const App& a) { return a.rx.standard == 129; }   // the engine reports its standard code (130) minus one

struct State {
    bool wasRunning = false;
    float pushedVol = -1;
    bool pushedMute = false;
    bool codecGiven = false;                          // the plug-in was handed to the receiver
    std::unique_ptr<dect2::codec::SpeechCodec> codec;
} S;

const char* frameName(int fi) { return fi == 0 ? "header" : fi == 1 ? "communication" : fi == 2 ? "terminator" : fi == 3 ? "test" : "-"; }
const char* dataName(int dt) { return dt == 0 ? "V/D mode 1" : dt == 1 ? "data FR" : dt == 2 ? "V/D mode 2" : dt == 3 ? "voice FR" : "-"; }
const char* callName(int cm) { return cm == 0 ? "group / CQ" : cm == 3 ? "individual" : cm < 0 ? "-" : "reserved"; }
const char* routeName(int mr) { return mr == 0 ? "direct" : mr == 1 ? "downlink (uplink idle)" : mr == 2 ? "downlink (uplink busy)" : mr < 0 ? "-" : "reserved"; }

void field(const char* label, const std::string& v) {
    if (v.empty()) ImGui::TextDisabled("%s: -", label);
    else ImGui::Text("%s: %s", label, v.c_str());
}

void tick(App& a) {
    if (!a.engine.running()) { S.wasRunning = false; return; }
    dect2::FusionReceiver& r = a.engine.fusion();
    // Load the voice codec while the mode runs, whichever tab is drawn (the tab only shows the state).
    if (live(a) && !S.codecGiven &&
        appcodec::speechCodecPoll("ysf", S.codec).status == dect2::codec::CodecStatus::Ready && S.codec) {
        r.setSpeechCodec(std::move(S.codec), std::string());
        S.codecGiven = true;
    }
    if (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute) {
        r.setVolume(a.volume); r.setMuted(a.muted);
        S.pushedVol = a.volume; S.pushedMute = a.muted;
    }
    S.wasRunning = true;
}

void codecBlock(App& a) {
    const dect2::FusionTelemetry& t = a.rx.fusion;
    ImGui::Spacing();
    ImGui::TextUnformatted("Voice");
    appcodec::voiceCodecLine("ysf", "AMBE+2 (Yaesu)");
    if (!S.codecGiven) {
        // the gate draws the not-installed text, the notice dialog or the load error, and hands over the codec once it is ready
        if (appcodec::speechCodecGate("ysf", S.codec)) {
            a.engine.fusion().setSpeechCodec(std::move(S.codec), std::string());
            S.codecGiven = true;
        }
    }
    if (ImGui::TreeNode("Install helper##ysf")) { appcodec::codecInstallHelper("ysf"); ImGui::TreePop(); }
    if (S.codecGiven) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.6f, 1), "codec ready: %llu of %llu voice frames decoded", (unsigned long long)t.voiceDecoded, (unsigned long long)t.voiceSubFrames);
    else ImGui::TextDisabled("no audio until the codec above is ready. Callsigns and data are still decoded (%llu voice frames received)", (unsigned long long)t.voiceSubFrames);
}

void tab(App& a) {
    {   // the voice monitor's strip, the same in every voice mode
        const dect2::FusionTelemetry& t = a.rx.fusion;
        voiceui::Link l;
        l.live = live(a); l.locked = l.live && t.dataValid; l.codecName = "AMBE+2 (Yaesu)"; l.codecId = "ysf";
        l.codecReady = S.codecGiven;
        const double tot = (double)t.dchOk + (double)t.dchBad;
        l.errorRate = l.locked && tot > 20 ? (float)(t.dchBad / tot) : -1.f;   // the frames whose data channel failed its check
        voiceui::strip(a, "ysf", "Yaesu System Fusion", l);
    }
    kit::modeHeader({128, "Yaesu Fusion", live(a) ? 1 : a.engine.running() ? 2 : 0, -1000.f, a.freqMhz * 1e6});
    static int fview = 0;
    kit::modeNav("ysfv", fview, {{tr("Live"), 0}, {tr("Calls"), 1}});
    if (fview == 1) { kit::sessionItems(128, 1, "ysf"); return; }
    if (!live(a)) {
        ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "tune to a Yaesu Fusion channel (2 m or 70 cm, 12.5 kHz) and start the receiver");
        codecBlock(a);
        return;
    }
    const dect2::FusionTelemetry& t = a.rx.fusion;
    ImGui::TextUnformatted("Yaesu System Fusion (WIP): C4FM 4800 symbols/s, 12.5 kHz");
    ImGui::Spacing();
    ImGui::Text("Lock: %s", t.dataValid ? "receiving" : t.blocksOk ? "idle (last transmission below)" : "listening");
    ImGui::Text("Frames: %llu good, %llu bad (headers %llu, communication %llu, terminators %llu)", (unsigned long long)t.blocksOk,
                (unsigned long long)t.blocksBad, (unsigned long long)t.headers, (unsigned long long)t.comms, (unsigned long long)t.terminators);
    ImGui::Text("Data channel units: %llu good, %llu bad", (unsigned long long)t.dchOk, (unsigned long long)t.dchBad);
    ImGui::Text("Level %.0f dBFS, offset %+.0f Hz, SNR %.0f dB%s", t.levelDb, t.cfoHz, t.snrDb, t.inverted ? ", spectrum mirrored" : "");
    ImGui::Spacing();
    ImGui::Text("Last frame: %s, %s, frame %d of %d", frameName(t.frameType), dataName(t.dataType), t.fn + 1, t.ft + 1);
    ImGui::Text("Call mode: %s", callName(t.callMode));
    ImGui::Text("Routing: %s%s", routeName(t.routing), t.voip == 1 ? ", via internet" : "");
    if (t.squelch > 0) ImGui::Text("Squelch code: %d", t.squelch);
    ImGui::Spacing();
    field("Source", t.src);
    field("Destination", t.dest);
    field("Repeater downlink", t.down);
    field("Repeater uplink", t.up);
    if (t.haveCsd[2]) ImGui::Text("Supplementary: %s | %s | %s | %s", t.rem[0].c_str(), t.rem[1].c_str(), t.rem[2].c_str(), t.rem[3].c_str());
    ImGui::Spacing();
    ImGui::TextUnformatted("Position / slow data text");
    if (t.slowText.empty()) ImGui::TextDisabled("none received (the GPS data format is not in the public specification, so only the text is shown)");
    else ImGui::TextWrapped("%s", t.slowText.c_str());
    codecBlock(a);
    ImGui::Spacing();
    ImGui::TextDisabled("Input %.0f Hz, centre %.4f MHz", t.inputRate, t.centerHz / 1e6);
    wideCarriersTable(a, 0);   // whole span: every Fusion carrier in the span (callsigns; the sound of the one listened to)
}

void receiver(App& a) {
    if (!live(a)) return;
    const dect2::FusionTelemetry& t = a.rx.fusion;
    ImGui::Text("%s", dect2::fusionSummary(t).c_str());
    ImGui::Text("frames %llu, level %.1f dBFS, offset %+.1f Hz", (unsigned long long)t.blocksOk, t.levelDb, t.cfoHz);
}

void list(App&) {}

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State");
    ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::fusionSummary(a.rx.fusion).c_str() : (a.engine.running() ? "starting" : "stopped"));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Yaesu Fusion (WIP)";
    if (live(a)) l2 = dect2::fusionSummary(a.rx.fusion);
}

} // namespace

extern const ModeUi kFusionUi;
const ModeUi kFusionUi = {
    .sideTitle = "CALLS",
    .tabName = "Fusion",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
