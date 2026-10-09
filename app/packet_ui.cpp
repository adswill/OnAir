// APRS / Packet screens: skeleton. The state lamp and a note that the decoder is not built yet; the mode's worker replaces them.
#include "app.h"
#include "dect2/packet_tel.h"

namespace {

bool live(const App& a) { return a.engine.running() && a.rx.standard == 25; }   // the engine reports its standard code minus one

void note(const App& a) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", a.engine.running() ? "decoder not built yet" : "start the receiver");
    ImGui::PopTextWrapPos();
}

void tab(App& a) {
    lamp("Decoder", 0);
    ImGui::SameLine(0, 12 * gUi);
    note(a);
}

void list(App& a) { note(a); }

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::PacketTelemetry& t = a.rx.packet;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("state", "%s", "no decoder yet");
    kv("input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
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
    l1 = "APRS / Packet";
    if (live(a)) l2 = dect2::packetSummary(a.rx.packet);
}

} // namespace

extern const ModeUi kPacketUi;
const ModeUi kPacketUi = {
    .sideTitle = "STATIONS",
    .tabName = "Packets",
    .tabIcon = Ic::Doc,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
};
