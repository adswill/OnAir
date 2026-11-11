// W1A voiceband modes screens (families 800 - 802, core/src/w1a, docs/W1A-3.9.md): HF packet 300 Bd (AX.25 frames), the voice inversion
// scrambler (detection, carrier estimate, descrambled audio to a WAV file) and the ELT 121.5 MHz swept-tone detector. Receive only.
// Simulated signals only so far.
#include "app.h"
#include "state/recdir.h"
#include "dect2/w1a/w1a_rx.h"
#include "dect2/w1a/w1a_tel.h"
#include <chrono>
#include <string>

namespace {

int kindOf(const App& a) { return a.rx.standard - 801; }
bool live(const App& a, int kind) { return a.rx.standard == 801 + kind; }

const char* note(int kind) {
    switch (kind) {
    case 0: return N_("HF packet radio, full decode: AX.25 frames in 300 Bd AFSK, tones 1600 / 1800 Hz (200 Hz shift), upper sideband. Every frame with a good CRC is shown "
                      "as sender>destination,path:text; a tuning error of up to a quarter of the shift is followed.");
    case 1: return N_("Analog voice scrambler, full-band inversion (every audio frequency f becomes fc - f). Tune a narrow-FM voice channel. The long-term spectrum of the voiced "
                      "audio is compared with speech as heard and flipped about each carrier from 2.7 to 3.7 kHz; the carrier is found to about 100 - 200 Hz. Split-band "
                      "inversion and rolling-code scramblers are not covered.");
    default: return N_("Emergency locator transmitter on 121.5 MHz, detection only: an AM carrier with a tone that sweeps downwards from about 1600 to 300 Hz two to four "
                       "times a second. The sweep, its rate and its depth are measured; the old analogue beacons carry no data. Tune the AM channel.");
    }
}

// the parameter names the decoders report, listed so they are translated
const char* const kParamKeys[] = {N_("modulation"), N_("link layer"), N_("stations"), N_("tone offset"), N_("scrambling"), N_("fit as heard"), N_("fit inverted"),
                                  N_("carrier"), N_("signal"), N_("sweep rate"), N_("sweep range"), N_("sweeps counted")};
const char* paramKey(const std::string& k) { for (const char* p : kParamKeys) if (k == p) return tr(p); return k.c_str(); }

std::string newWavPath() {
    const long long t = (long long)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return recordingsSub("descrambled") + "/descrambled-" + std::to_string(t) + ".wav";
}

void tab(App& a) {
    const int kind = kindOf(a);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr(note(kind >= 0 && kind < dect2::kW1aKinds ? kind : 0)));
    ImGui::TextDisabled("%s", kind == 0 ? tr("Tune the dial frequency, upper sideband. Checked on simulated signals only.")
                                        : tr("Checked on simulated signals only: no real recording went through this mode."));
    ImGui::PopTextWrapPos();
    if (kind < 0 || kind >= dect2::kW1aKinds) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver")); return; }
    const dect2::W1aTelemetry& t = a.rx.w1a;
    ImGui::TextUnformatted(dect2::w1aSummary(t).c_str());
    if (!t.params.empty() && ImGui::BeginTable("##w1ap", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        for (const auto& p : t.params) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", paramKey(p.first));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.second.c_str());
        }
        ImGui::EndTable();
    }
    if (kind == 0) {
        ImGui::Text("%s: %llu  %s: %llu", tr("frames"), (unsigned long long)t.blocksOk, tr("failed the CRC"), (unsigned long long)t.blocksBad);
        ImGui::BeginChild("##w1at", ImVec2(0, 180 * gUi), ImGuiChildFlags_Borders);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(t.text.empty() ? "-" : t.text.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
    if (kind == 1 && !t.spec.empty()) {
        ImGui::TextDisabled("%s", tr("Long-term audio spectrum, 0 - 4.8 kHz"));
        ImGui::PlotLines("##w1as", t.spec.data(), (int)t.spec.size(), 0, nullptr, -70.f, 0.f, ImVec2(-1, 70 * gUi));
    }
    if (ImGui::Button(tr("Clear"))) a.engine.w1a(kind).clearResults();
    ImGui::SeparatorText(tr("Events"));
    for (const auto& e : t.events) ImGui::Text("%7.1f s  %s", e.timeSec, e.text.c_str());
    if (kind == 1 && ImGui::CollapsingHeader(trw("Advanced"))) {
        static float fc = 0;
        ImGui::SetNextItemWidth(140 * gUi);
        if (ImGui::InputFloat(tr("Carrier (Hz, 0 = automatic)"), &fc, 0, 0, "%.0f")) a.engine.w1a(1).setForced(fc);
        if (!t.recording) {
            if (ImGui::Button(tr("Record the descrambled audio"))) a.engine.w1a(1).setRecordPath(newWavPath());
        } else {
            if (ImGui::Button(tr("Stop recording"))) a.engine.w1a(1).setRecordPath("");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", t.recordPath.c_str());
        }
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("The audio is written as a 12 kHz, 16-bit mono WAV file in the recordings folder."));
        ImGui::PopTextWrapPos();
    }
}
void receiver(App& a) { const int k = kindOf(a); ImGui::TextUnformatted(k >= 0 && k < dect2::kW1aKinds && live(a, k) ? dect2::w1aSummary(a.rx.w1a).c_str() : tr("stopped")); }
void status(App& a) { receiver(a); }
void summary(const App& a, std::string& l1, std::string& l2) {
    const int k = kindOf(a);
    l1 = dect2::w1aName(k >= 0 && k < dect2::kW1aKinds ? k : 0);
    if (k >= 0 && k < dect2::kW1aKinds) l2 = dect2::w1aSummary(a.rx.w1a);
}

} // namespace

#define W1A_UI(var, name) \
    extern const ModeUi var; \
    const ModeUi var = {.sideTitle = "Voiceband", .tabName = name, .tabIcon = Ic::Signal, .tab = tab, .receiver = receiver, .status = status, .summary = summary};
W1A_UI(kW1aPacketUi, "HF packet")
W1A_UI(kW1aInvertUi, "Inversion")
W1A_UI(kW1aEltUi, "ELT")
