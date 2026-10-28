// TETRAPOL screens: the cells (D_SYSTEM_INFO), the calls on the traffic channel being received and the group calls a control channel
// announces, the frame layer (polarity, band coding, scramble) and the voice strip. Receive only. Voice: clear calls only, through the user's
// own codec plug-in (onair-codec-tetrapol, docs/CODECS.md); encrypted calls and calls of unknown encryption state are never decoded.
// The call table is for this screen only: it is never saved, exported or logged, and it shows group and channel numbers only for clear calls.
#include "app.h"
#include "codec/speech_codec_ui.h"
#include "platform/platform.h"
#include "dect2/audio/audioout.h"
#include "dect2/dsp/resampler.h"
#include "dect2/tetrapol/tetrapol_rx.h"
#include "dect2/tetrapol/tetrapol_tel.h"
#include "dect2/tetrapol/tetrapol_voice.h"
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

bool freqStepArrow(const char* id, ImGuiDir dir);

namespace {

struct State {
    int sel = -1;
    int spacing = 0;
    int polarity = 0;     // 0 automatic, 1 normal, 2 inverted
    int band = 0;         // 0 automatic, 1 VHF, 2 UHF
    bool listen = true;
    bool record = false;
    int appliedPol = -1, appliedBand = -1, appliedListen = -1, appliedRec = -1;
};
State S;

dect2::AudioOut gVoiceOut;
bool gWired = false;
constexpr int kOutRate = 48000;

const char* kHelp =
    "TETRAPOL (PAS 0001-2 / 0001-3), receive only.\n\n"
    "This mode is not TETRA. It reads the GMSK 8 kbit/s frames of a TETRAPOL carrier (10 / 12.5 kHz channels). On a control channel it shows "
    "D_SYSTEM_INFO (country, system, BN, cell, location area, scramble) and counts the group calls announced, with their group and channel "
    "numbers only when the call is clear. On a traffic channel it follows the call.\n\n"
    "Polarity: the uplink is sent with the opposite polarity to the downlink, and receivers differ too. Automatic tries both and keeps the "
    "one whose frames pass their CRC; Normal / Inverted force one. The VHF / UHF frame coding and the scrambling constant are found the same way.\n\n"
    "Voice: most TETRAPOL networks encrypt their calls. A call is played only when its own signalling, heard in the clear on this channel, "
    "says it is a clear call (key index 0). Encrypted calls and calls whose state is not known (joined late) are never decoded: no key is "
    "used, guessed or stored. OnAir does not include the TETRAPOL speech codec (RP-CELP, patented). To hear clear calls you install a codec "
    "plug-in yourself (onair-codec-tetrapol, see the Install helper and docs/CODECS.md); without it calls are found and counted, silently.\n\n"
    "Record calls writes one WAV per clear call into the recordings folder (the file name holds the date and time only). The call table is "
    "never saved or exported.\n\n"
    "Listening to radio services is regulated in many countries: follow the rules where you live.";

bool live(const App& a) { return a.rx.standard == 80; }

std::string ago(int64_t t) {
    if (t <= 0) return "-";
    const long d = (long)(std::time(nullptr) - t);
    char b[32];
    if (d < 60) snprintf(b, sizeof b, "%ld s", std::max(0L, d));
    else if (d < 3600) snprintf(b, sizeof b, "%ld min", d / 60);
    else snprintf(b, sizeof b, "%ld h", d / 3600);
    return b;
}

const char* verdictShort(int v) {
    return v == 1 ? "clear" : v == 2 ? "encrypted (not decoded)" : "unknown (not decoded)";
}

void helpButton() {
    if (ImGui::SmallButton("?##tpolhelp")) ImGui::OpenPopup("##tpolhelppop");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("What this mode reads");
    if (ImGui::BeginPopup("##tpolhelppop")) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32);
        ImGui::TextUnformatted(kHelp);
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
}

void wire(App& a) {
    if (gWired) return;
    gWired = true;
    // the codec's PCM (8 kHz for the plug-in format) goes to the speaker at 48 kHz through the rational resampler
    a.engine.tetrapol().setPcmOut([&a](const int16_t* pcm, size_t n, int rate) {
        static dect2::RationalResampler rs;
        static int rsRate = 0;
        static std::vector<dect2::cf32> in, out;
        if (rate <= 0 || !n) return;
        if (rate != rsRate) { rsRate = rate; rs.configure(rate, kOutRate); }
        if (!gVoiceOut.running()) { gVoiceOut.start(kOutRate); gVoiceOut.setStartThreshold(kOutRate / 5); }
        gVoiceOut.setVolume(a.volume);
        gVoiceOut.setMuted(a.muted);
        in.resize(n);
        for (size_t i = 0; i < n; i++) in[i] = dect2::cf32(pcm[i] / 32768.f, 0.f);
        out.clear();
        rs.process(in.data(), n, out);
        std::vector<float> st(out.size() * 2);
        for (size_t i = 0; i < out.size(); i++) st[2 * i] = st[2 * i + 1] = out[i].real();
        gVoiceOut.write(st.data(), (int)out.size());
    });
}

void apply(App& a) {
    dect2::TetrapolReceiver& rx = a.engine.tetrapol();
    if (S.appliedPol != S.polarity) { rx.setPolarity(S.polarity); S.appliedPol = S.polarity; }
    if (S.appliedBand != S.band) { rx.setBand(S.band); S.appliedBand = S.band; }
    if (S.appliedListen != (int)S.listen) { rx.setListen(S.listen); S.appliedListen = S.listen; }
    if (S.appliedRec != (int)S.record) {
        std::string folder;
        if (S.record) {
            folder = plat::dataDir() + "/recordings";
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(folder), ec);
        }
        rx.setRecordFolder(folder);
        S.appliedRec = S.record;
    }
}

void voiceStrip(App& a) {
    wire(a);
    const dect2::TetrapolTelemetry& t = a.rx.tetrapol;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Voice");
    ImGui::SameLine();
    if (ImGui::SmallButton("Install helper##tpol")) ImGui::OpenPopup("tpolhelper");
    if (ImGui::BeginPopup("tpolhelper")) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        appcodec::codecInstallHelper("tetrapol");
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    appcodec::voiceCodecLine("tetrapol", "TETRAPOL RP-CELP");
    if (!a.engine.tetrapol().hasCodec()) {
        // the notice is accepted before anything is loaded; nothing is decoded without a codec
        std::unique_ptr<dect2::codec::SpeechCodec> c;
        if (appcodec::speechCodecPoll("tetrapol", c).status == dect2::codec::CodecStatus::Ready && c) a.engine.tetrapol().setCodec(std::move(c));
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Listen to");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
    const char* ls[2] = {"clear calls", "nothing (mute)"};
    int li = S.listen ? 0 : 1;
    if (ImGui::Combo("##tpollisten", &li, ls, 2)) S.listen = li == 0;
    ImGui::SameLine();
    ImGui::Checkbox("Record calls##tpol", &S.record);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("One WAV per clear call in the recordings folder (date and time in the name, no numbers)");
    if (!t.wavPath.empty()) { ImGui::SameLine(); ImGui::TextDisabled("recording %s", t.wavPath.c_str()); }
    ImGui::TextDisabled("%s", t.voiceStatus.empty() ? "no call" : t.voiceStatus.c_str());
}

void frameLine(const dect2::TetrapolTelemetry& t) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Polarity");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
    const char* pol[3] = {"automatic", "normal", "inverted"};
    ImGui::Combo("##tpolpol", &S.polarity, pol, 3);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("TETRAPOL uplinks are sent with the opposite polarity to downlinks. Automatic tries both.");
    ImGui::SameLine();
    ImGui::TextUnformatted("Band coding");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
    const char* bd[3] = {"automatic", "VHF", "UHF"};
    ImGui::Combo("##tpolband", &S.band, bd, 3);
    ImGui::TextDisabled("%s; polarity %s%s, %s coding, scramble %s, frames ok %llu / bad %llu, SNR %.1f dB, offset %+.0f Hz",
                        t.synced ? "frame sync" : "looking for frames", t.polarity == 1 ? "normal" : t.polarity == 2 ? "inverted" : "-",
                        t.polarityAuto ? " (found)" : " (set)", t.band == 1 ? "VHF" : t.band == 2 ? "UHF" : "-",
                        t.scramble >= 0 ? std::to_string(t.scramble).c_str() : "-", (unsigned long long)t.blocksOk,
                        (unsigned long long)t.blocksBad, t.snrDb, t.cfoHz);
}

void tab(App& a) {
    apply(a);
    helpButton();
    ImGui::SameLine();
    ImGui::TextUnformatted("TETRAPOL (receive only)");
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to hear TETRAPOL"); return; }
    const dect2::TetrapolTelemetry& t = a.rx.tetrapol;
    ImGui::TextDisabled("%s", dect2::tetrapolSummary(t).c_str());
    frameLine(t);
    voiceStrip(a);
    const float h = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginTable("##tpolcells", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0, h * 0.3f))) {
        ImGui::TableSetupColumn("Country"); ImGui::TableSetupColumn("System"); ImGui::TableSetupColumn("Cell");
        ImGui::TableSetupColumn("LA"); ImGui::TableSetupColumn("BN"); ImGui::TableSetupColumn("Scramble");
        ImGui::TableSetupColumn("SNR"); ImGui::TableSetupColumn("Last");
        ImGui::TableHeadersRow();
        auto num = [](int v) { if (v >= 0) ImGui::Text("%d", v); else ImGui::TextUnformatted("-"); };
        for (size_t i = 0; i < t.cells.size(); i++) {
            const auto& c = t.cells[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); num(c.country);
            ImGui::TableNextColumn(); num(c.systemId);
            ImGui::TableNextColumn(); num(c.cellId);
            ImGui::TableNextColumn(); num(c.locArea);
            ImGui::TableNextColumn(); num(c.bnId);
            ImGui::TableNextColumn(); num(c.scramble);
            ImGui::TableNextColumn(); ImGui::Text("%.1f dB", c.snrDb);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(c.lastWall).c_str());
            if (ImGui::IsItemHovered()) S.sel = (int)i;
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Calls on this channel (screen only, never saved)");
    if (ImGui::BeginTable("##tpolcalls", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0, h * 0.22f))) {
        ImGui::TableSetupColumn("Started"); ImGui::TableSetupColumn("Length"); ImGui::TableSetupColumn("Encryption");
        ImGui::TableSetupColumn("Voice frames"); ImGui::TableSetupColumn("Decoded"); ImGui::TableSetupColumn("Recording", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& c : t.calls) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s%s", ago(c.startWall).c_str(), c.active ? " (on air)" : "");
            ImGui::TableNextColumn(); ImGui::Text("%.1f s", c.lastSec - c.startSec);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(verdictShort(c.verdict));
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)c.voiceFrames);
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)c.decoded);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.wavPath.empty() ? "-" : c.wavPath.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Group calls announced on a control channel (numbers shown for clear calls only)");
    if (ImGui::BeginTable("##tpolact", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0, h * 0.18f))) {
        ImGui::TableSetupColumn("Group"); ImGui::TableSetupColumn("Channel"); ImGui::TableSetupColumn("Encryption");
        ImGui::TableSetupColumn("Heard"); ImGui::TableSetupColumn("Last");
        ImGui::TableHeadersRow();
        for (const auto& c : t.activations) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); if (c.groupId >= 0) ImGui::Text("%d", c.groupId); else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); if (c.channelId >= 0) ImGui::Text("%d", c.channelId); else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(verdictShort(c.verdict));
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)c.heard);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(c.lastWall).c_str());
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Log");
    for (const auto& e : t.events) {
        ImGui::TextUnformatted(e.text.c_str());
        if (&e - t.events.data() > 12) break;
    }
}

int spacingHz() {   // 12500 or 10000: the channel raster of the country (saved)
    if (!S.spacing) S.spacing = plat::prefs().getI("tpolSpacing", 12500) == 10000 ? 10000 : 12500;
    return S.spacing;
}

// the channel raster beside the frequency box: the arrows step by one channel (hold for a continuous change), and the choice is saved
void tuner(App& a, bool& retune) {
    int sp = spacingHz();
    ImGui::TextDisabled("Channels");
    ImGui::SameLine(0, 5 * gUi);
    static const char* const items[] = {"12.5 kHz", "10 kHz"};
    int cur = sp == 10000 ? 1 : 0;
    ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::Combo("##tpolsp", &cur, items, 2)) {
        S.spacing = cur == 1 ? 10000 : 12500;
        plat::prefs().setI("tpolSpacing", S.spacing);
        a.engine.tetrapol().setSpacingHz(S.spacing);
        sp = S.spacing;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Channel spacing of the network: 12.5 kHz (France and most of Europe) or 10 kHz. The arrows step by one channel.");
    const double stepMhz = sp / 1e6;
    auto snap = [&](double mhz) { return std::round(mhz / stepMhz) * stepMhz; };
    ImGui::SameLine(0, 6 * gUi);
    if (freqStepArrow("##tpoldn", ImGuiDir_Left)) { tuneFreq(a, snap(snap(a.freqMhz) - stepMhz)); retune = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("the channel below. Hold for a continuous change.");
    ImGui::SameLine(0, 4 * gUi);
    if (freqStepArrow("##tpolup", ImGuiDir_Right)) { tuneFreq(a, snap(snap(a.freqMhz) + stepMhz)); retune = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("the channel above. Hold for a continuous change.");
}

void receiver(App& a) { a.engine.tetrapol().setSpacingHz(spacingHz()); }
void list(App&) {}
void tick(App& a) {
    // the codec loads (and is handed to the receiver) whatever tab is shown
    apply(a);
    if (!live(a)) return;
    wire(a);
    appcodec::voiceCodecTick("tetrapol");
    if (!a.engine.tetrapol().hasCodec()) {
        std::unique_ptr<dect2::codec::SpeechCodec> c;
        if (appcodec::speechCodecPoll("tetrapol", c).status == dect2::codec::CodecStatus::Ready && c) a.engine.tetrapol().setCodec(std::move(c));
    }
}
void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::tetrapolSummary(a.rx.tetrapol).c_str() : (a.engine.running() ? "starting" : "stopped"));
}
void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "TETRAPOL";
    if (live(a)) l2 = dect2::tetrapolSummary(a.rx.tetrapol);
}

} // namespace

extern const ModeUi kTetrapolUi;
const ModeUi kTetrapolUi = {
    .sideTitle = "CELLS",
    .tabName = "TETRAPOL",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,   // in the order ModeUi declares them (GCC refuses another order)
    .tick = tick,
};
