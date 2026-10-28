// TETRA screens: the base station table (frequency, network code, location area, colour code, encryption, last heard), the details of
// the selected carrier, its neighbour cells, the carriers seen in the spectrum, a scan over a span, and the log. Only what base stations
// broadcast in the clear to every terminal is shown: nothing is decrypted, no voice, no short data, no subscriber identities.
#include "app.h"
#include "voice/voice_monitor_ui.h"
#include "dect2/voice/voice_monitor.h"
#include "codec/speech_codec_ui.h"
#include "dect2/audio/audioout.h"
#include "dect2/dsp/resampler.h"
#include "dect2/tetra/tetra_traffic.h"
#include "dect2/tetra/tetra_voice_codec.h"
#include "state/results_export.h"
#include "dect2/gsmr/gsmr_rx.h"
#include "dect2/tetra/tetra_pdu.h"
#include "dect2/tetra/tetra_rx.h"
#include "dect2/tetra/tetra_slots.h"
#include "dect2/tetra/tetra_tel.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;                 // 0 cells, 1 neighbours, 2 carriers, 3 log
    int sel = -1;                 // selected cell (index in cells)
    int span = 0;                 // 0 390 - 395, 1 410 - 430, 2 450 - 470, 3 custom
    double lo = 380, hi = 400;    // custom span, MHz
    bool scanning = false;
    std::vector<double> plan;     // centres, MHz
    size_t step = 0;
    double stepAt = 0;
    float dwell = 6.0f;           // a capture is 2.3 s, and the search takes about as long again
};
State S;

// Unencrypted voice: the sink hands each clear traffic frame to the user-installed speech codec and the PCM goes to the speaker.
// Encrypted calls and calls of unknown encryption state never reach it (tetra_voice.h). OnAir ships no codec.
dect2::tetra::TetraVoiceSink gVoice;
dect2::AudioOut gVoiceOut;
bool gVoiceWired = false;
constexpr int kVoiceOutRate = 48000;
// The audio of a capture that starts from a silent output waits for the end of the capture (CaptureLeadIn, tetra_voice_codec.h). Shared by the
// receiver's thread (the frames, the end of the capture) and the UI thread (the release of a capture whose end never came, e.g. after a retune).
struct VoicePlayout {
    std::mutex mu;
    dect2::tetra::CaptureLeadIn lead{kVoiceOutRate, 2, dect2::TetraReceiver::kCaptureSec};
    std::chrono::steady_clock::time_point holdSince{};
    std::vector<float> out;
} gPlayout;
std::string gVoiceAccess;   // "MCC 234 MNC 10 CC 5" of the cell whose voice is played (under gPlayout.mu)
struct Listen { int mode = 0, tn = 0; double carrierHz = 0; bool pushed = false; } gListen;   // issue #267: whole span / one channel, timeslot, carrier

// the capture is decoded (or, from the UI, its end never came): the held audio goes to the speaker with its lead of silence. gPlayout.mu held.
void voiceRelease() {
    if (!gPlayout.lead.holding()) return;
    gPlayout.out.clear();
    gPlayout.lead.captureEnd(gPlayout.out);
    gVoiceOut.write(gPlayout.out.data(), (int)(gPlayout.out.size() / 2));
}

void wireVoice(App& a) {
    if (gVoiceWired) return;
    gVoiceWired = true;
    // Played at 48 kHz: AudioOut's start and underrun thresholds are counted in frames for 48 kHz, and at the codec's 8 kHz they meant
    // 1.8 s and 4.8 s of waiting. The codec's rate is the plug-in's to choose, so it goes through the rational resampler, not a fixed
    // factor. The speech comes a capture (2.3 s) at a time: the audio that starts from a silent output is held until the capture is
    // decoded and then goes out behind as much silence as the capture lacked, so the sound does not run dry before the next capture.
    gVoice.setPcmOut([&a](const int16_t* pcm, size_t n, int rate) {
        static dect2::RationalResampler rs;
        static int rsRate = 0;
        static std::vector<dect2::cf32> in, out;
        static std::vector<float> rec;
        if (rate <= 0) return;
        // the voice monitor (voice_monitor.h): the stream is named by its carrier and timeslot (TETRA identities are not decoded)
        dect2::voice::Monitor& mon = dect2::voice::monitor("tetra");
        dect2::voice::CallInfo ci;
        ci.slot = a.engine.tetra().voiceTimeslot();
        ci.carrierHz = a.engine.tetra().voiceCarrierHz();
        if (ci.carrierHz > 0 && ci.slot > 0) { char b[48]; snprintf(b, sizeof b, "%.4f/TS%d", ci.carrierHz / 1e6, ci.slot); ci.dstText = b; }
        { std::lock_guard<std::mutex> lk(gPlayout.mu); ci.access = gVoiceAccess; }
        if (mon.offer(ci) != dect2::voice::Verdict::Play) return;
        rec.resize(n);
        for (size_t i = 0; i < n; i++) rec[i] = pcm[i] / 32768.f;
        mon.audio(ci, rec.data(), n, 1, rate);   // the call's recording (when on), at the codec's rate
        if (rate != rsRate) { rsRate = rate; rs.configure(rate, kVoiceOutRate); }
        const std::string dev = mon.settings().device;
        std::lock_guard<std::mutex> lk(gPlayout.mu);
        if (gVoiceOut.running() && gVoiceOut.device() != dev) gVoiceOut.stop();   // the user picked another output for TETRA
        if (!gVoiceOut.running()) { gVoiceOut.setDevice(dev); gVoiceOut.start(kVoiceOutRate); }
        gVoiceOut.setVolume(a.volume * mon.volume()); gVoiceOut.setMuted(a.muted);
        const bool idle = !gVoiceOut.playing() && gVoiceOut.bufferedFrames() == 0;
        if (idle && !gPlayout.lead.holding()) {   // a stream starts: its first capture waits for its end
            rs.reset();
            gPlayout.holdSince = std::chrono::steady_clock::now();
        }
        in.resize(n);
        for (size_t i = 0; i < n; i++) in[i] = dect2::cf32(pcm[i] / 32768.f, 0.f);
        out.clear();
        rs.process(in.data(), n, out);
        std::vector<float> st(out.size() * 2);
        for (size_t i = 0; i < out.size(); i++) st[2 * i] = st[2 * i + 1] = out[i].real();
        gPlayout.out.clear();
        gPlayout.lead.push(st.data(), out.size(), idle, gPlayout.out);
        if (!gPlayout.out.empty()) gVoiceOut.write(gPlayout.out.data(), (int)(gPlayout.out.size() / 2));
    });
    gVoice.setCaptureEnd([] { std::lock_guard<std::mutex> lk(gPlayout.mu); voiceRelease(); });
    gVoice.setFrameDecoder(dect2::tetra::makeTrafficFrameDecoder());   // channel decoding from EN 300 395-2 clause 5.5; the codec itself is the user's plug-in
    a.engine.tetra().setVoiceSink(&gVoice);
}

void voiceStrip(App& a) {
    wireVoice(a);
    {   // audio held for the end of a capture that never came (a retune in the middle of it): out after a second
        std::lock_guard<std::mutex> lk(gPlayout.mu);
        if (gPlayout.lead.holding() && std::chrono::steady_clock::now() - gPlayout.holdSince > std::chrono::seconds(1)) voiceRelease();
    }
    const dect2::TetraTelemetry& t = a.rx.tetra;
    const double playHz = a.engine.tetra().voiceCarrierHz();
    {   // the cell of the stream being played, for the monitor's card
        std::string acc;
        for (const auto& c : t.cells) if (playHz > 0 && std::fabs(c.freqHz - playHz) < 6e3 && c.mcc >= 0) {
            char b[64]; snprintf(b, sizeof b, "MCC %d MNC %d CC %d", c.mcc, c.mnc, c.colourCode); acc = b; break;
        }
        std::lock_guard<std::mutex> lk(gPlayout.mu);
        gVoiceAccess = acc;
    }
    {   // the voice monitor's strip, the same in every voice mode
        voiceui::Link l;
        l.live = a.rx.standard == 47; l.locked = l.live && t.dataValid; l.codecName = "TETRA ACELP"; l.codecId = "tetra";
        l.codecReady = gVoice.hasCodec();   // standard 47: TETRA (live() is below)
        const double tot = (double)t.blocksOk + (double)t.blocksBad;
        l.errorRate = l.locked && tot > 20 ? (float)(t.blocksBad / tot) : -1.f;   // signalling blocks that failed their CRC
        voiceui::strip(a, "tetra", "TETRA", l);
    }
    {   // what is listened to (issue #267): the whole span or one 25 kHz channel, the timeslot, the carrier
        bool ch = !gListen.pushed;
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Listen to");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(190 * gUi);
        const char* lm[2] = {"the whole span", "this channel only (25 kHz)"};
        if (ImGui::BeginCombo("##tetralisten", lm[gListen.mode])) {
            for (int i = 0; i < 2; i++) if (ImGui::Selectable(lm[i], gListen.mode == i)) { gListen.mode = i; ch = true; }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("This channel only: only the carrier at the dial frequency is decoded. The same call sent by other sites in the span\n"
                                                      "is not heard again, and a capture is decoded much faster, so less speech is lost between captures.");
        flowNext(8 * gUi);
        ImGui::SetNextItemWidth(110 * gUi);
        const char* ts[5] = {"any timeslot", "timeslot 1", "timeslot 2", "timeslot 3", "timeslot 4"};
        if (ImGui::BeginCombo("##tetrats", ts[gListen.tn])) {
            for (int i = 0; i < 5; i++) if (ImGui::Selectable(ts[i], gListen.tn == i)) { gListen.tn = i; ch = true; }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Any: the first timeslot with clear voice is followed. A number: only that timeslot is played.");
        flowNext(8 * gUi);
        ImGui::SetNextItemWidth(170 * gUi);
        char cur[48];
        if (gListen.carrierHz > 0) snprintf(cur, sizeof cur, "%.4f MHz", gListen.carrierHz / 1e6); else snprintf(cur, sizeof cur, "strongest carrier");
        if (ImGui::BeginCombo("##tetracar", cur)) {
            if (ImGui::Selectable("strongest carrier", gListen.carrierHz <= 0)) { gListen.carrierHz = 0; ch = true; }
            for (const auto& c : t.cells) {
                char b[96]; snprintf(b, sizeof b, "%.4f MHz%s", c.freqHz / 1e6, c.encryption() ? "  (encryption)" : "");
                if (ImGui::Selectable(b, std::fabs(gListen.carrierHz - c.freqHz) < 1e3)) { gListen.carrierHz = c.freqHz; ch = true; }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Strongest: the strongest carrier with clear voice is followed. A carrier: only its voice is played.");
        if (playHz > 0) { flowNext(8 * gUi); ImGui::TextDisabled("playing %.4f MHz, timeslot %d", playHz / 1e6, a.engine.tetra().voiceTimeslot()); }
        flowEnd();
        if (ch) {
            a.engine.tetra().setSingleCarrier(gListen.mode == 1);
            a.engine.tetra().setVoiceTimeslot(gListen.tn);
            a.engine.tetra().setVoiceCarrier(gListen.carrierHz);
            gListen.pushed = true;
        }
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Voice");
    ImGui::SameLine();
    if (ImGui::SmallButton("Install helper##tetra")) ImGui::OpenPopup("tetrahelper");
    if (ImGui::BeginPopup("tetrahelper")) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        appcodec::codecInstallHelper("tetra");
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    appcodec::voiceCodecLine("tetra", "TETRA ACELP");
    ImGui::SameLine();
    if (!gVoice.hasCodec()) {
        // The notice gate runs first; a codec is loaded only after the user accepted it. Nothing is decoded without a codec.
        std::unique_ptr<dect2::codec::SpeechCodec> c;
        if (appcodec::speechCodecGate("tetra", c) && c) gVoice.setCodec(std::move(c));
        return;
    }
    ImGui::TextUnformatted(gVoice.status().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(clear calls only; %s / %s)", dect2::tetra::voiceVerdictText(dect2::tetra::VoiceVerdict::EncryptedNotDecoded),
                        dect2::tetra::voiceVerdictText(dect2::tetra::VoiceVerdict::StateUnknownNotDecoded));
}

bool live(const App& a);

// "Listen to": which timeslot of the carrier is played (the choice is saved). Encrypted slots are shown muted and are never decoded.
void slotPanel(App& a) {
    namespace T = dect2::tetra;
    static const char* modes[] = {"first clear voice", "slot 1", "slot 2", "slot 3", "slot 4", "talkgroup", "ISSI"};
    int cur = a.tetraSlotMode == 1 ? a.tetraSlotNum + 0 : a.tetraSlotMode == 2 ? 5 : a.tetraSlotMode == 3 ? 6 : 0;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Carriers");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150 * gUi);
    if (ImGui::BeginCombo("##tetrasingle", a.tetraSingle ? "Single carrier" : "Search span")) {
        if (ImGui::Selectable("Search span", !a.tetraSingle)) a.tetraSingle = 0;
        if (ImGui::Selectable("Single carrier", a.tetraSingle != 0)) a.tetraSingle = 1;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", a.tetraSingle ? "only the 25 kHz channel at the dial frequency is filtered out and decoded" : "every carrier in the span is searched for");
    if (!a.tetraSingle) {   // 3.4: how much of the capture is searched; a wide capture (Advanced > Custom bandwidth / sample rate) lists and decodes every carrier in it
        ImGui::SameLine(0, 14 * gUi);
        ImGui::TextDisabled("%s", "searching the middle");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60 * gUi);
        int pct = a.tetraSearchPct;
        if (ImGui::InputInt("##tetrasearch", &pct, 0, 0)) { a.tetraSearchPct = std::max(1, std::min(99, pct)); a.engine.tetra().setSearchBand(a.tetraSearchPct / 100.0); }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", "% of the capture");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "72 % is the flat part of a radio's decimation filter. Raise it on a wide capture whose edges are flat too. The wider the capture (Advanced: Custom bandwidth and sample rate), the more carriers are listed and decoded.");
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Listen to");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150 * gUi);
    if (ImGui::BeginCombo("##tetraslot", modes[cur])) {
        for (int i = 0; i < 7; i++) if (ImGui::Selectable(modes[i], cur == i)) {
            if (i == 0) a.tetraSlotMode = 0;
            else if (i <= 4) { a.tetraSlotMode = 1; a.tetraSlotNum = i; }
            else a.tetraSlotMode = i == 5 ? 2 : 3;
        }
        ImGui::EndCombo();
    }
    if (a.tetraSlotMode >= 2) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110 * gUi);
        ImGui::InputInt("##tetraid", &a.tetraSlotId, 0, 0);
        if (a.tetraSlotId < 0) a.tetraSlotId = 0;
        if (a.tetraSlotId == 0) { ImGui::SameLine(); ImGui::TextDisabled("enter the id"); }
    }
    if (a.tetraSlotMode >= 2)
        ImGui::TextDisabled("Talkgroup and ISSI come from the set-up of calls that are not encrypted; encrypted calls show none. They stay on this computer.");
    if (!live(a)) return;
    // the strongest recently heard cell with a main carrier shows its four slots
    const dect2::TetraCell* best = nullptr;
    for (const auto& c : a.rx.tetra.cells) if (!best || c.lastSec > best->lastSec) best = &c;
    if (!best) return;
    const double now = (double)std::time(nullptr);
    for (int i = 0; i < 4; i++) {
        const T::SlotInfo& k = best->slots[i];
        char heard[32] = "never";
        if (k.lastHeardSec >= 0) snprintf(heard, sizeof heard, "%.0f s ago", std::max(0.0, now - k.lastHeardSec));
        char ids[64] = "";
        if (k.talkgroup) snprintf(ids, sizeof ids, " TG %u", k.talkgroup);
        if (k.issi) snprintf(ids + strlen(ids), sizeof ids - strlen(ids), " ISSI %u", k.issi);
        const bool enc = k.state == T::SlotState::Encrypted;
        if (enc) ImGui::PushStyleColor(ImGuiCol_Text, pal::warnAmber());
        ImGui::Text("slot %d: %s%s, last heard %s%s", i + 1, T::slotStateText(k.state), ids, heard,
                    (a.tetraSlotMode == 1 && a.tetraSlotNum == i + 1) ? "  <- listening" : "");
        if (enc) ImGui::PopStyleColor();
    }
}

const char* airEncShort(const dect2::TetraCell& c) {
    switch (c.airEnc()) {
        case 1: return tr("none advertised");
        case 2: return tr("class 2");
        case 3: return tr("class 3");
        case 4: return tr("available");
        default: return tr("unknown");
    }
}

const char* kHelp =
    "TETRA network information (ETSI EN 300 392-2), receive only.\n\n"
    "This mode reads only what a TETRA base station broadcasts in the clear to every terminal: the network code (MCC, MNC), colour code, "
    "location area, the cell's carriers and duplex spacing, its neighbour cells, the services it offers and whether the network uses air "
    "interface encryption is advertised. The same information is shown by open-source tools such as osmo-tetra.\n\n"
    "Wide captures: under Advanced in the top bar, tick Custom for the bandwidth and for the sample rate and set any value; there is no upper limit in OnAir, the radio decides what it can do. "
    "A HackRF at 10 to 20 Msps captures 7 to 14 MHz of the middle of the band (the rest is its filter's roll-off): every TETRA carrier inside is listed and its control channel decoded, with the processor load growing "
    "slowly with the number of carriers (one shared FFT cuts them out). \"searching the middle N %\" sets how much of the capture is searched. A capture needs about 8 bytes per sample for 2.3 s (0.4 GB at 20 Msps).\n\n"
    "It does not decrypt anything, does not transmit, does not join or disturb any network, and does not decode or play voice, short "
    "data messages or subscriber identities. Signalling that carries an encryption mode is only counted. A cell is described as using air interface encryption only when it advertises class 2 or 3, or its signalling says so; class 1 cells and scrambling are not encryption.\n\n"
    "You are responsible for following your country's rules on receiving radio signals.";

bool live(const App& a) { return a.rx.standard == 47; }

std::string ago(int64_t t) {
    if (t <= 0) return "-";
    const long d = (long)(std::time(nullptr) - t);
    char b[32];
    if (d < 60) snprintf(b, sizeof b, "%ld s", std::max(0L, d)); else if (d < 3600) snprintf(b, sizeof b, "%ld min", d / 60); else snprintf(b, sizeof b, "%ld h", d / 3600);
    return b;
}

std::string netText(const dect2::TetraCell& c) {
    if (c.mcc < 0) return "-";
    char b[96];
    const char* country = c.mcc == 1 ? "test network" : dect2::gsmrCountry(c.mcc);
    if (*country) snprintf(b, sizeof b, "%d-%d %s", c.mcc, c.mnc, country); else snprintf(b, sizeof b, "%d-%d", c.mcc, c.mnc);
    return b;
}

void spanOf(double& lo, double& hi) {
    switch (S.span) {
    case 0: lo = 390; hi = 395; break;
    case 1: lo = 410; hi = 430; break;
    case 2: lo = 450; hi = 470; break;
    case 4: lo = 380; hi = 400; break;   // the whole 380 - 400 MHz range, in steps of the capture (a wide capture covers it in one or two)
    default: lo = std::min(S.lo, S.hi); hi = std::max(S.lo, S.hi); break;
    }
}

void makePlan(const App& a) {
    S.plan.clear();
    double lo, hi;
    spanOf(lo, hi);
    const double w = std::max(0.15, a.tune.sampleRate / 1e6 * std::max(0.05, a.tetraSearchPct / 100.0));   // the part of the sample band searched whole
    for (double c = lo + w / 2; c - w / 2 < hi && S.plan.size() < 400; c += w) S.plan.push_back(std::round(c * 1000) / 1000);
}

void helpButton() {
    if (ImGui::SmallButton("?##tetrahelp")) ImGui::OpenPopup("##tetrahelppop");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("What this mode reads, and what it never does");
    if (ImGui::BeginPopup("##tetrahelppop")) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32);
        ImGui::TextUnformatted(kHelp);
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
}

void scanBar(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Span");
    flowNext(16 * gUi);
    static const char* spans[] = {"390 - 395 MHz (emergency services)", "410 - 430 MHz", "450 - 470 MHz", "Custom", "380 - 400 MHz (whole range)"};
    ImGui::SetNextItemWidth(220 * gUi);
    ImGui::Combo("##tspan", &S.span, spans, 5);
    if (S.span == 3) {
        flowNext(10 * gUi);
        ImGui::SetNextItemWidth(80 * gUi); ImGui::InputDouble("##tlo", &S.lo, 0, 0, "%.3f");
        ImGui::SameLine(); ImGui::TextDisabled("to"); ImGui::SameLine();
        ImGui::SetNextItemWidth(80 * gUi); ImGui::InputDouble("MHz##thi", &S.hi, 0, 0, "%.3f");
    }
    flowNext(10 * gUi);
    if (!S.scanning) {
        if (runningButton(a, tr("Scan"), true)) { makePlan(a); S.step = 0; S.stepAt = 0; S.scanning = !S.plan.empty(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Step the radio over the span; every TETRA carrier in each step is listed with its network information. Receive only.");
    } else {
        if (ImGui::SmallButton("Stop scan")) S.scanning = false;
        ImGui::SameLine(); ImGui::TextDisabled("%zu / %zu", S.step + 1, S.plan.size());
    }
    flowNext(10 * gUi);
    if (ImGui::SmallButton("Clear##tclr")) { a.engine.tetra().clearCells(); S.sel = -1; }
    flowNext(10 * gUi);
    {
        static std::string note;
        resexp::Table t; t.cols = {"time", "frequency_hz", "mode", "network", "location_area", "colour_code", "encryption", "carrier", "level_db", "snr_db"};
        const std::string now = resexp::stamp();
        for (const auto& c : a.rx.tetra.cells)
            t.add({now, resexp::num(c.nominalHz, 0), "TETRA", netText(c), c.la >= 0 ? std::to_string(c.la) : "", std::to_string(c.colourCode),
                   c.hasSysinfo ? airEncShort(c) : "", c.hasSysinfo ? (c.isMain ? "main" : "other") : "",
                   resexp::num(c.levelDb, 0), resexp::num(c.snrDb, 0)});
        resexp::saveButton(t, "tetra", note, "svtetra");
        if (!note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", note.c_str()); }
    }
    flowNext(4 * gUi);
    helpButton();
    flowEnd();
}

void cellTable(App& a) {
    const dect2::TetraTelemetry& t = a.rx.tetra;
    const bool wide = ImGui::GetContentRegionAvail().x > 860 * gUi;
    const int nc = wide ? 9 : 6;
    const ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV;
    if (!ImGui::BeginTable("##tcells", nc, fl, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    if (wide) for (const char* h : {"MHz", "Network", "LA", "Colour", "Air encryption", "Carrier", "Level", "SNR", "Last heard"}) ImGui::TableSetupColumn(h);
    else for (const char* h : {"MHz", "Network", "LA", "Colour", "Air encryption", "Heard"}) ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < t.cells.size(); i++) {
        const auto& c = t.cells[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        char l[64]; snprintf(l, sizeof l, "%.4f##t%zu", c.nominalHz / 1e6, i);
        if (ImGui::Selectable(l, S.sel == (int)i, ImGuiSelectableFlags_SpanAllColumns)) S.sel = (int)i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s, colour code %d, offset %+.0f Hz, heard %s ago", netText(c).c_str(), c.colourCode, c.cfoHz, ago(c.lastWall).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(netText(c).c_str());
        ImGui::TableNextColumn(); if (c.la >= 0) ImGui::Text("%d", c.la); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::Text("%d", c.colourCode);
        ImGui::TableNextColumn();
        if (c.airEnc() >= 2) ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", airEncShort(c)); else if (c.hasSysinfo) ImGui::TextUnformatted(airEncShort(c)); else ImGui::TextDisabled("%s", airEncShort(c));
        if (wide) { ImGui::TableNextColumn(); if (c.hasSysinfo) ImGui::TextUnformatted(c.isMain ? "main" : "other"); else ImGui::TextDisabled("-"); }
        if (wide) { ImGui::TableNextColumn(); ImGui::Text("%.0f", c.levelDb); }
        if (wide) { ImGui::TableNextColumn(); ImGui::Text("%.0f", c.snrDb); }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(c.lastWall).c_str());
    }
    ImGui::EndTable();
}

void detail(App& a) {
    ImGui::PushTextWrapPos(0);
    struct Pop { ~Pop() { ImGui::PopTextWrapPos(); } } pop_;
    const dect2::TetraTelemetry& t = a.rx.tetra;
    if (S.sel < 0 || S.sel >= (int)t.cells.size()) { ImGui::TextDisabled("select a carrier"); return; }
    const auto& c = t.cells[(size_t)S.sel];
    ImGui::Text("%.4f MHz, network %s, colour code %d, system code %d", c.nominalHz / 1e6, netText(c).c_str(), c.colourCode, c.systemCode);
    static const char* share[] = {"continuous transmission", "carrier sharing", "MCCH sharing", "traffic carrier sharing"};
    static const char* load[] = {"unknown", "low", "medium", "high"};
    ImGui::Text("%s, cell load %s, late entry %s, neighbour broadcast %s; last sync timeslot %d frame %d multiframe %d",
                share[c.sharingMode & 3], load[c.cellLoad & 3], c.lateEntry ? "yes" : "no", (c.nbrBroadcast & 2) ? "yes" : "no", c.tn, c.fn, c.mn);
    ImGui::Text("level %.1f dBFS, SNR %.1f dB, offset %+.0f Hz; %llu sync bursts, %llu SYSINFO, %llu blocks good, %llu failed",
                c.levelDb, c.snrDb, c.cfoHz, (unsigned long long)c.bschOk, (unsigned long long)c.bnchOk, (unsigned long long)c.blocksOk, (unsigned long long)c.blocksBad);
    if (c.hasSysinfo) {
        ImGui::Text("Location area %d; main carrier %d = %.4f MHz (band %d, offset %d)%s; uplink %s", c.la, c.mainCarrier, c.mainHz / 1e6, c.band, c.offset,
                    c.isMain ? ", this carrier" : "", c.uplinkHz > 0 ? (std::to_string(c.uplinkHz / 1e6).substr(0, 8) + " MHz").c_str() : "unknown");
        ImGui::Text("secondary control channels %d, max terminal power level %d, access level min %d, access parameter %d, radio downlink timeout %d",
                    c.numScch, c.msTxPwrMax, c.rxLevAccessMin, c.accessParam, c.dlTimeout);
        ImGui::Text("services: %s", dect2::tetra::serviceText(c.bsService).c_str());
        if (c.cckFlag) ImGui::Text("common cipher key identifier %d", c.hyperframeOrCck); else ImGui::Text("hyperframe %d", c.hyperframeOrCck);
        if (c.secValid) ImGui::Text("security: authentication %s, class 1 %s, class 2 %s, class 3 %s", c.authRequired ? "required" : "not required",
                                    c.secClass1 ? "yes" : "no", c.secClass2 ? "yes" : "no", c.secClass3 ? "yes" : "no");
    }
    ImGui::Text(tr("Air-interface encryption: %s"), airEncShort(c));
    ImGui::Text(tr("%llu signalling PDUs with an encryption mode set (not decrypted); %llu blocks unreadable (CRC failed)"), (unsigned long long)c.encryptedPdus, (unsigned long long)c.blocksBad);
    std::string ts;
    for (int i = 0; i < 4; i++) { ts += "TS" + std::to_string(i + 1) + " " + dect2::tetra::dlUsageText(c.tsUsage[i]); if (i < 3) ts += ", "; }
    ImGui::Text("timeslots: %s", ts.c_str());
    if (!c.netTime.empty()) ImGui::Text("network time: %s", c.netTime.c_str());
}

void neighbourView(App& a) {
    const dect2::TetraTelemetry& t = a.rx.tetra;
    if (!ImGui::BeginChild("##tnbr")) { ImGui::EndChild(); return; }
    bool any = false;
    for (const auto& c : t.cells) {
        if (!c.hasNwrk) continue;
        any = true;
        char h[96]; snprintf(h, sizeof h, "%.4f MHz %s LA %d##n%.0f", c.nominalHz / 1e6, netText(c).c_str(), c.la, c.freqHz);
        if (!ImGui::TreeNode(h)) continue;
        for (const auto& n : c.neighbours) {
            if (n.freqHz > 0) ImGui::BulletText("cell %d: carrier %d (%.4f MHz), LA %d%s", n.cellId, n.carrier, n.freqHz / 1e6, n.la, n.synced ? ", synchronised" : "");
            else ImGui::BulletText("cell %d: carrier %d, LA %d", n.cellId, n.carrier, n.la);
        }
        if (c.neighbours.empty()) ImGui::TextDisabled("no neighbours listed");
        ImGui::TreePop();
    }
    if (!any) ImGui::TextDisabled("no neighbour cell broadcast read yet (D-NWRK-BROADCAST on the main control channel)");
    ImGui::EndChild();
}

void carrierView(App& a) {
    const dect2::TetraTelemetry& t = a.rx.tetra;
    if (t.carriers.empty()) { ImGui::TextDisabled("no carriers in the last block"); return; }
    for (const auto& c : t.carriers)
        ImGui::BulletText("%.4f MHz, %.0f dB over the floor, %s", c.freqHz / 1e6, c.levelDb, c.synced ? "TETRA synchronisation" : "no TETRA synchronisation in this block");
}

void logView(App& a) {
    for (const auto& e : a.rx.tetra.events) ImGui::TextUnformatted(e.text.c_str());
    if (a.rx.tetra.events.empty()) ImGui::TextDisabled("nothing yet");
}

void tick(App& a) {
    // Load the voice codec while the mode runs, whichever tab is drawn (the strip only shows the state).
    if (live(a) && a.engine.running() && !gVoice.hasCodec()) {
        wireVoice(a);
        std::unique_ptr<dect2::codec::SpeechCodec> c;
        if (appcodec::speechCodecPoll("tetra", c).status == dect2::codec::CodecStatus::Ready && c) gVoice.setCodec(std::move(c));
    }
    if (a.engine.running()) {   // the saved 'Listen to' choice reaches the receiver whichever tab is drawn
        dect2::tetra::SlotSelect sel; sel.mode = (dect2::tetra::SlotMode)a.tetraSlotMode; sel.slot = a.tetraSlotNum; sel.id = (uint32_t)a.tetraSlotId;
        a.engine.tetra().setVoiceSlot(sel);
        a.engine.tetra().setSingleCarrier(a.tetraSingle != 0);
        a.engine.tetra().setSearchBand(a.tetraSearchPct / 100.0);
    }
    if (!S.scanning) return;
    if (!a.engine.running()) { S.scanning = false; return; }
    const double now = ImGui::GetTime();
    if (S.stepAt == 0 || now - S.stepAt >= S.dwell) {
        if (S.stepAt != 0) S.step++;
        if (S.step >= S.plan.size()) { S.scanning = false; return; }
        tuneFreq(a, S.plan[S.step]);
        S.stepAt = now;
    }
}

void tab(App& a) {
    scanBar(a);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Receive only: the public information every TETRA base station broadcasts to all terminals (network code, location area, services, encryption advertised). Nothing is decrypted; no messages or identities are decoded. Clear (unencrypted) voice can be played only with a speech codec you install yourself; encrypted calls are never decoded.");
    ImGui::PopTextWrapPos();
    voiceStrip(a);
    slotPanel(a);
    subNav("tetrav", S.view, {"Cells", "Neighbours", "Carriers", "Log"});
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to find the TETRA base stations around you"); return; }
    if (S.view == 0) {
        const float h = ImGui::GetContentRegionAvail().y;
        if (ImGui::BeginChild("##tct", ImVec2(0, h * 0.55f))) cellTable(a);
        ImGui::EndChild();
        detail(a);
    } else if (S.view == 1) neighbourView(a);
    else if (S.view == 2) carrierView(a);
    else logView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    for (const auto& c : a.rx.tetra.cells) ImGui::Text("%.4f %-10s %s", c.nominalHz / 1e6, netText(c).c_str(), c.airEnc() >= 2 ? "enc" : "");
    if (a.rx.tetra.cells.empty()) ImGui::TextDisabled("none yet");
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::TetraTelemetry& t = a.rx.tetra;
    ImGui::Text("%s", dect2::tetraSummary(t).c_str());
    ImGui::Text("%llu blocks searched, %llu signalling blocks decoded, %llu failed", (unsigned long long)t.blocks, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    ImGui::Text("input %.2f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    ImGui::TextDisabled("pi/4-DQPSK 18 ksym/s, 25 kHz channels; %.1f MHz of the band at a time: raise the sample rate and bandwidth (Advanced > Custom) or use Scan for a wider span", t.inputRate / 1e6 * a.tetraSearchPct / 100.0);
}

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::tetraSummary(a.rx.tetra).c_str() : (a.engine.running() ? "starting" : "stopped"));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "TETRA";
    if (live(a)) l2 = dect2::tetraSummary(a.rx.tetra);
}

// The test signal: one cell, two cells, or (3.4) a wide capture with many carriers at different levels and tuning errors; the capture's width comes from
// Advanced > Custom bandwidth / sample rate (a 10 MHz bandwidth carries a dozen carriers across 7 MHz)
void synthOptions(App& a, bool& changed) {
    dect2::SynthConfig& sc = a.tune.synth;
    ImGui::TextDisabled("%s", "TETRA test signal");
    flowNext(10 * gUi);
    static const char* scenes[] = {"one cell (wide capture from 4 Msps)", "two cells", "wide capture"};
    int sc0 = std::max(0, std::min(2, sc.modeOpt[0]));
    if (sc0 == 0 && a.tune.sampleRate >= 4e6) sc0 = 2;
    ImGui::SetNextItemWidth(120 * gUi);
    if (ImGui::BeginCombo("##tetrascene", scenes[sc0])) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(scenes[i], i == sc0)) { sc.modeOpt[0] = i; changed = true; }
        ImGui::EndCombo();
    }
    if (sc0 == 2) {
        flowNext(10 * gUi);
        int n = sc.modeOpt[1] > 0 ? sc.modeOpt[1] : 12;
        ImGui::SetNextItemWidth(70 * gUi);
        if (ImGui::InputInt("carriers##tetran", &n, 0, 0)) { sc.modeOpt[1] = std::max(1, n); changed = true; }
    }
    flowNext(14 * gUi); ImGui::TextDisabled("%s", "SNR"); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    float snr = (float)sc.snrDb; if (ImGui::SliderFloat("##tsnr", &snr, 5, 45, "%.0f dB")) { sc.snrDb = snr; changed = true; }
    flowNext(); ImGui::TextDisabled("%s", "CFO"); ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi);
    float cfo = (float)(sc.cfoHz / 1e3); if (ImGui::SliderFloat("##tcfo", &cfo, -20, 20, "%.2f kHz")) { sc.cfoHz = cfo * 1e3; changed = true; }
}

} // namespace

extern const ModeUi kTetraUi;
const ModeUi kTetraUi = {
    .sideTitle = "CELLS",
    .tabName = "TETRA",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .synth = synthOptions,
    .tick = tick,
};
