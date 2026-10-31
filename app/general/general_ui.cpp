// General receiver screens: the listening frequency with a step selector, the demodulator (NFM, WFM, AM, USB, LSB, CW), filter width
// presets, squelch (auto or level), AGC or a fixed gain, de-emphasis, CW pitch, the shared volume, an S-meter and recording to WAV,
// over the spectrum and the waterfall. On those plots the channel is a marker: a click puts it there (inside the band the radio
// covers, without retuning; outside it the radio is retuned), its filter edges can be dragged.
#include "app.h"
#include "gfx/plot.h"
#include "memory/memory_options.h"
#include "dect2/general/general_gen.h"
#include "dect2/general/general_rx.h"
#include "dect2/general/general_tel.h"
#include "dect2/ham/ham_voice.h"
#include "dect2/geo/maidenhead.h"
#include "state/freq_model.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include "dect2/general/swbc.h"
#include <string>
#include <vector>

bool freqStepArrow(const char* id, ImGuiDir dir);

namespace {

const double kSteps[] = {10, 100, 1000, 2500, 5000, 6250, 8333.333333, 9000, 10000, 12500, 25000, 50000, 100000};
const char* const kStepNames[] = {"10 Hz", "100 Hz", "1 kHz", "2.5 kHz", "5 kHz", "6.25 kHz", "8.33 kHz", "9 kHz", "10 kHz", "12.5 kHz", "25 kHz", "50 kHz", "100 kHz"};
constexpr int kNumSteps = (int)(sizeof kSteps / sizeof *kSteps);
const double kDeemph[] = {0, 50, 75, 750};
const char* const kDeemphNames[] = {"off", "50 us", "75 us", "750 us"};

struct State {
    bool loaded = false;
    int demod = dect2::GenNfm;
    double width[dect2::GenDemodCount] = {};
    int step[dect2::GenDemodCount] = {};
    bool sqOn = true, sqAuto = true;
    float sqDb = -60;
    bool agc = true;
    float gainDb = 20;
    int deNfm = 0, deWfm = 1;
    float pitch = 700;
    int amDet = 0;               // AM detector: 0 envelope, 1 synchronous, 2 sync upper sideband, 3 sync lower
    bool amAuto = true;          // AM Auto quality (on by default)
    int amNr = -1;               // its noise reduction: -1 auto, 0 off, else the strength
    bool amEnh = false;          // synthetic treble ("enhanced"), experimental
    bool nb = false;             // noise blanker
    float nbDb = 12;
    double chanOff = 0;          // the channel relative to the dial frequency (a.freqMhz)
    double lastDial = -1;        // a new dial frequency typed elsewhere puts the channel back on it
    int drag = -1, dragPlot = -1;   // the filter edge held (0 low, 1 high) and on which plot
    bool dragged = false;
    std::string recErr;
    bool txRec = false;
    float preRoll = 1.5f;        // seconds of signal kept from before the squelch opens
    float txHang = 1.5f;         // the file stays open this long after the squelch shuts
    int txIq = -1;               // -1 follows the mode (on for USB / LSB), 0 off, 1 on
    int hvRegion = 0;            // the ham voice plan: 0 picks the IARU region from the locator or the settings, 1 2 3 chosen (Advanced)
    float afHp = 0, afLp = 0, afNotch = 0;   // audio filters in Hz, 0 = off
    bool nrOn[dect2::GenDemodCount] = {};    // neural noise remover per demodulator (speech modes only), off by default
    float nrStr[dect2::GenDemodCount] = {80, 80, 80, 80, 80, 80};   // its strength in percent
    bool mp3 = false;            // recordings as MP3 through LAME (when libmp3lame is installed)
    bool flac = false;           // recordings as FLAC (lossless, smaller, with tags) instead of WAV
    // more channels from the same capture (Engine::generalExtra): each is silent, has its own demodulator and, if wanted, its own recordings
    int nExtra = 0;
    double exOff[3] = {100e3, -100e3, 200e3};       // relative to the dial
    int exDemod[3] = {dect2::GenNfm, dect2::GenNfm, dect2::GenNfm};
    bool exRec[3] = {false, false, false};
    int exDrag = -1;
    dect2::GeneralTelemetry exTel[3];
    uint64_t exSeq[3] = {0, 0, 0};
};
State S;

void tip(const char* t) { if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", t); }
bool isSw(const App& a) { return a.family == kShortwaveFamily; }   // Shortwave listens with this same screen
bool live(const App& a) { return a.rx.standard == (isSw(a) ? 213 : 56); }   // the engine reports its standard code minus one
const dect2::GeneralTelemetry& T(const App& a) { return isSw(a) ? a.rx.shortwave.general : a.rx.general; }
dect2::GeneralReceiver& grx(App& a) { return isSw(a) ? a.engine.shortwave().general() : a.engine.general(); }

bool nrOffered(int demod) { return demod == dect2::GenNfm || demod == dect2::GenWfm || demod == dect2::GenAm; }   // speech: not SSB data, CW or music
const int kDefStep[dect2::GenDemodCount] = {9, 11, 2, 1, 1, 1};   // 12.5 kHz NFM, 50 kHz WFM, 1 kHz AM, 100 Hz SSB / CW

void load() {
    if (S.loaded) return;
    S.loaded = true;
    auto& p = plat::prefs();
    S.demod = (int)std::clamp(p.getI("genDemod", dect2::GenNfm), 0L, (long)dect2::GenDemodCount - 1);
    for (int d = 0; d < dect2::GenDemodCount; d++) {
        char k[24];
        snprintf(k, sizeof k, "genWidth%d", d); S.width[d] = dect2::generalClampWidth(d, p.getD(k, dect2::generalDefaultWidth(d)));
        snprintf(k, sizeof k, "genStep%d", d); S.step[d] = (int)std::clamp(p.getI(k, kDefStep[d]), 0L, (long)kNumSteps - 1);
    }
    S.flac = p.getB("genFlac", false);
    S.mp3 = p.getB("genMp3", false) && dect2::GeneralReceiver::mp3Available();
    S.afHp = (float)p.getD("genAfHp", 0); S.afLp = (float)p.getD("genAfLp", 0); S.afNotch = (float)p.getD("genAfNotch", 0);
    for (int d = 0; d < dect2::GenDemodCount; d++) {
        char k[24];
        snprintf(k, sizeof k, "genNrOn%d", d); S.nrOn[d] = nrOffered(d) && p.getB(k, false);
        snprintf(k, sizeof k, "genNrStr%d", d); S.nrStr[d] = (float)std::clamp(p.getD(k, 80), 0.0, 100.0);
    }
    S.sqOn = p.getB("genSqOn", true); S.sqAuto = p.getB("genSqAuto", true); S.sqDb = (float)p.getD("genSqDb", -60);
    S.preRoll = (float)std::clamp(p.getD("genPreRoll", 1.5), 0.0, 3.0);
    S.txHang = (float)std::clamp(p.getD("genTxHang", 1.5), 0.0, 3.0);
    S.txIq = (int)p.getI("genTxIq", -1);
    if (S.txIq < -1 || S.txIq > 1) S.txIq = -1;
    S.agc = p.getB("genAgc", true); S.gainDb = (float)p.getD("genGain", 20);
    S.deNfm = (int)std::clamp(p.getI("genDeNfm", 0), 0L, 3L); S.deWfm = (int)std::clamp(p.getI("genDeWfm", 1), 0L, 3L);
    S.pitch = (float)std::clamp(p.getD("genPitch", 700), 200.0, 1500.0);
    S.amDet = (int)std::clamp(p.getI("genAmDet", 0), 0L, 3L);
    S.amAuto = p.getB("genAmAuto", true); S.amNr = (int)std::clamp(p.getI("genAmNr", -1), -1L, 100L); S.amEnh = p.getB("genAmEnh", false);
    S.nb = p.getB("genNb", false); S.nbDb = (float)std::clamp(p.getD("genNbDb", 12), 6.0, 30.0);
    S.chanOff = p.getD("genOffset", 0);
    S.hvRegion = (int)std::clamp(p.getI("ham.voiceRegion", 0), 0L, 3L);
}
void save() {
    auto& p = plat::prefs();
    p.setI("genDemod", S.demod);
    p.setI("ham.voiceRegion", S.hvRegion);
    for (int d = 0; d < dect2::GenDemodCount; d++) {
        char k[24];
        snprintf(k, sizeof k, "genWidth%d", d); p.setD(k, S.width[d]);
        snprintf(k, sizeof k, "genStep%d", d); p.setI(k, S.step[d]);
    }
    p.setB("genSqOn", S.sqOn); p.setB("genSqAuto", S.sqAuto); p.setD("genSqDb", S.sqDb);
    p.setB("genAgc", S.agc); p.setD("genGain", S.gainDb);
    p.setI("genDeNfm", S.deNfm); p.setI("genDeWfm", S.deWfm); p.setD("genPitch", S.pitch); p.setD("genOffset", S.chanOff);
    p.setI("genAmDet", S.amDet); p.setB("genNb", S.nb); p.setD("genNbDb", S.nbDb);
    p.setB("genAmAuto", S.amAuto); p.setI("genAmNr", S.amNr); p.setB("genAmEnh", S.amEnh);
}

double listenHz(const App& a) { return a.freqMhz * 1e6 + S.chanOff; }
bool channelIq(int demod) { return S.txIq < 0 ? (demod == dect2::GenUsb || demod == dect2::GenLsb) : S.txIq > 0; }
double stepHz() { return kSteps[S.step[S.demod]]; }
void edges(double& lo, double& hi) { dect2::generalFilterEdges(S.demod, S.width[S.demod], lo, hi); }

// Put the channel on hz: inside the band the radio covers it only moves the marker; outside, the radio is retuned onto it
void setListen(App& a, double hz, bool centre = false) {   // centre: the target button is on, so a click retunes the radio even inside the band (#301)
    hz = std::max(1e3, hz);
    const double fs = a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate;
    const double rc = radioCenterMhz(a) * 1e6;
    double lo, hi;
    edges(lo, hi);
    const fmodel::ListenPlan p = fmodel::planListen(hz, rc, fs, lo, hi, a.tuningMode == 1);
    if (p.retune || (centre && a.tuningMode != 1)) {
        S.chanOff = 0;
        tuneFreq(a, hz / 1e6);
        S.lastDial = a.freqMhz;
    } else S.chanOff = p.vfoHz - a.freqMhz * 1e6;
    save();
}
double snap(double hz) { const double s = stepHz(); return std::round(hz / s) * s; }

void setDemod(int d) {
    if (d == S.demod) return;
    S.demod = d;
    save();
}

std::vector<std::string> recTags(const App& a, double hz) {
    const std::time_t now = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y-%m-%d", std::localtime(&now));
    char t[96];
    snprintf(t, sizeof t, "TITLE=%.4f MHz %s", hz / 1e6, dect2::generalDemodName(S.demod));
    (void)a;
    return {t, std::string("DATE=") + ts, "COMMENT=Recorded with OnAir"};
}

void startRecording(App& a) {
    std::error_code ec;
    const std::string dir = plat::dataDir() + "/audio-recordings";
    std::filesystem::create_directories(dir, ec);
    const std::time_t now = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", std::localtime(&now));
    char name[128];
    snprintf(name, sizeof name, "/onair_%s_%.4fMHz_%s.%s", dect2::generalDemodName(S.demod), listenHz(a) / 1e6, ts, S.mp3 ? "mp3" : S.flac ? "flac" : "wav");
    grx(a).setRecordFormat(S.flac, recTags(a, listenHz(a))); grx(a).setRecordMp3(S.mp3);
    if (!grx(a).startRecording(dir + name)) S.recErr = "cannot write " + dir + name;
    else S.recErr.clear();
}

// the S-meter: a bar from -130 to 0 dBFS with the squelch threshold marked
void sMeter(const App& a) {
    const auto& t = T(a);
    const bool on = live(a) && t.levelDb > -199;
    const float w = std::min(360 * gUi, ImGui::GetContentRegionAvail().x), h = ImGui::GetFrameHeight() * 0.7f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(26, 30, 36, 255), 3.f);
    auto xOf = [&](float db) { return p.x + w * std::clamp((db + 130.f) / 130.f, 0.f, 1.f); };
    if (on) {
        const ImU32 col = t.squelchOpen ? IM_COL32(90, 200, 120, 255) : IM_COL32(110, 130, 150, 255);
        dl->AddRectFilled(p, ImVec2(xOf(t.levelDb), p.y + h), col, 3.f);
        if (t.noiseDb > -199) dl->AddLine(ImVec2(xOf(t.noiseDb), p.y), ImVec2(xOf(t.noiseDb), p.y + h), IM_COL32(200, 200, 200, 140), 1.f);
        if (t.squelchDb > -199) dl->AddLine(ImVec2(xOf(t.squelchDb), p.y - 2), ImVec2(xOf(t.squelchDb), p.y + h + 2), IM_COL32(240, 170, 60, 255), 2.f);
    }
    for (int db = -120; db <= -10; db += 10) dl->AddLine(ImVec2(xOf((float)db), p.y + h * 0.65f), ImVec2(xOf((float)db), p.y + h), IM_COL32(0, 0, 0, 120), 1.f);
    ImGui::Dummy(ImVec2(w, h));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Channel level, -130 to 0 dBFS (after the channel filter).\nGrey line: the noise in the channel; amber: the squelch threshold.");
    ImGui::SameLine(0, 8 * gUi);
    if (on) {
        if (t.dataValid) ImGui::Text("%.1f dBFS  SNR %.1f dB", t.levelDb, t.snrDb);
        else ImGui::Text("%.1f dBFS", t.levelDb);
        ImGui::SameLine(0, 8 * gUi);
        ImGui::TextColored(t.squelchOpen ? ImVec4(0.45f, 0.85f, 0.55f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "%s", t.squelchOpen ? "open" : "closed");
    } else ImGui::TextDisabled("%s", a.engine.running() ? "no level yet" : "stopped");
}

void extraChannelsUi(App& a);
bool amBroadcast(const App& a);
void audioFiltersUi(App& a);
void noiseRemoverUi(App& a);


// ---- the ham voice band plan: one click tunes a segment and sets the demodulator and the filter (core/src/ham/ham_voice.cpp, docs/ham-voice.md) ----
// The IARU region of the station: the one chosen in the Ham setup, else from the Ham locator, the receiver position or the setup location, else Region 1.
int stationRegion(bool* known = nullptr) {
    auto& p = plat::prefs();
    int r = (int)p.getI("ham.region", 0);
    if (r < 1 || r > 3) r = dect2::ham::hamIaruRegionForGrid(p.getS("ham.grid", ""));
    if (r < 1 && receiverLocationSet()) r = dect2::ham::hamIaruRegionForPosition(p.getD("adsbLat", 0), p.getD("adsbLon", 0));
    if (r < 1) {
        const std::string loc = p.getS("setupLoc", "");
        double la, lo;
        if (!loc.empty()) r = dect2::fromMaidenhead(loc, la, lo) || dect2::parseCoordinates(loc, la, lo) ? dect2::ham::hamIaruRegionForPosition(la, lo) : 0;
    }
    if (known) *known = r >= 1;
    return r >= 1 ? r : 1;
}
int voiceRegion() { return S.hvRegion ? S.hvRegion : stationRegion(); }
int demodOf(dect2::ham::VoiceMode m) { return m == dect2::ham::VoiceMode::Nfm ? dect2::GenNfm : m == dect2::ham::VoiceMode::Usb ? dect2::GenUsb : dect2::GenLsb; }
void hamVoiceTune(App& a, const dect2::ham::HamVoiceSeg& g) {
    const int d = demodOf(g.mode);
    setDemod(d);
    S.width[d] = dect2::generalClampWidth(d, g.bwHz);
    S.chanOff = 0;
    tuneFreq(a, g.tuneHz / 1e6);
    S.lastDial = a.freqMhz;
    save();
}
void hamVoiceUi(App& a) {
    static const char* const kReg[] = {"Auto (from my locator)", "IARU Region 1 (Europe, Africa, Middle East, northern Asia)", "IARU Region 2 (the Americas)", "IARU Region 3 (south and east Asia, Oceania)"};
    const int reg = voiceRegion();
    const auto segs = dect2::ham::hamVoiceSegments(reg);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Ham voice"));
    flowNext(6 * gUi);
    const double hz = listenHz(a);
    for (size_t i = 0; i < segs.size(); i++) {
        const auto& g = segs[i];
        const bool on = hz >= g.loHz && hz <= g.hiHz && S.demod == demodOf(g.mode);
        ImGui::PushID((int)i);
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::SmallButton(g.button)) hamVoiceTune(a, g);
        if (on) ImGui::PopStyleColor();
        ImGui::PopID();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s %s: %.4f to %.4f MHz, %s\n%s %.4f MHz, %s, %.4g kHz", g.band, g.label, g.loHz / 1e6, g.hiHz / 1e6,
            g.mode == dect2::ham::VoiceMode::Nfm ? "FM" : g.mode == dect2::ham::VoiceMode::Usb ? "upper sideband" : "lower sideband", tr("Click: tune"), g.tuneHz / 1e6,
            dect2::generalDemodName(demodOf(g.mode)), g.bwHz / 1e3);
        flowNext(3 * gUi);
    }
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Amateur voice by the IARU band plan of your region: lower sideband below 10 MHz, upper sideband above (60 m upper), FM for the 10 m, 6 m, 2 m and 70 cm segments. A click tunes the segment's calling or middle frequency and sets the demodulator and filter. The segments are the IARU plans; your country's plan can be narrower. 30 m has no voice; 11 m is CB, not amateur. The region follows your locator (Ham setup, receiver position or setup location); choose it by hand under Advanced. See docs/ham-voice.md."));
    flowNext(6 * gUi);
    if (ImGui::TreeNodeEx(trw("Advanced##hvadv"), ImGuiTreeNodeFlags_NoTreePushOnOpen)) {
        ImGui::SetNextItemWidth(380 * gUi);
        int sel = S.hvRegion;
        if (ImGui::Combo(tr("Region##hvreg"), &sel, kReg, 4)) { S.hvRegion = sel; save(); }
        bool known = false; const int auto_ = stationRegion(&known);
        ImGui::TextDisabled("%s", S.hvRegion == 0 ? (known ? (auto_ == 1 ? "Region 1 found from your locator" : auto_ == 2 ? "Region 2 found from your locator" : "Region 3 found from your locator") : "No locator set (Ham setup or the setup wizard): Region 1 until you choose") : "Chosen by hand");
    }
    flowEnd();
}

void controls(App& a) {
    load();
    // the listening frequency and the step
    ImGui::PushFont(a.mono, 0);
    ImGui::Text("%.4f MHz", listenHz(a) / 1e6);
    ImGui::PopFont();
    tip("The frequency listened to: the dial plus where the channel marker sits.\nClick the spectrum or the waterfall to move it; drag the edges of the light band to change the filter.");
    flowNext(10 * gUi);
    if (freqStepArrow("##gdn", ImGuiDir_Left)) setListen(a, snap(listenHz(a)) - stepHz());
    tip("Down one step. Hold for a continuous change.");
    ImGui::SameLine(0, 2 * gUi);
    if (freqStepArrow("##gup", ImGuiDir_Right)) setListen(a, snap(listenHz(a)) + stepHz());
    tip("Up one step. Hold for a continuous change.");
    ImGui::SameLine(0, 4 * gUi);
    ImGui::SetNextItemWidth(95 * gUi);
    if (ImGui::Combo("##gstep", &S.step[S.demod], kStepNames, kNumSteps)) save();
    tip("Frequency step: the arrows move by it and a click on the spectrum snaps to it");
    flowNext(6 * gUi);
    if (ImGui::SmallButton("Centre")) { S.chanOff = 0; tuneFreq(a, snap(listenHz(a)) / 1e6); S.lastDial = a.freqMhz; save(); }
    tip("Tune the radio so the channel sits on the dial (the marker back in the middle)");
    flowEnd();

    // the demodulator
    for (int d = 0; d < dect2::GenDemodCount; d++) {
        if (d) ImGui::SameLine();
        if (ImGui::RadioButton(dect2::generalDemodName(d), S.demod == d)) setDemod(d);
    }
    if (amBroadcast(a)) {   // medium wave and the shortwave broadcast bands (kHz: the band centre, or a common frequency)
        static const struct { const char* name; double khz; } kPre[] = {{"MW 531", 531}, {"MW 999", 999}, {"MW 1368", 1368}, {"MW 1566", 1566}};
        ImGui::TextDisabled("presets");
        for (const auto& pr : kPre) {
            ImGui::SameLine(0, 4 * gUi);
            if (ImGui::SmallButton(pr.name)) { S.chanOff = 0; tuneFreq(a, pr.khz / 1e3); S.lastDial = a.freqMhz; save(); }
        }
        size_t nsw = 0;
        const dect2::SwBand* sw = dect2::swBroadcastBands(nsw);
        for (size_t i = 0; i < nsw; i++) {   // each band: centre, AM with the synchronous detector, 9 kHz filter, 5 kHz step
            ImGui::SameLine(0, 4 * gUi);
            ImGui::PushID((int)i);
            if (ImGui::SmallButton(sw[i].name)) {
                S.demod = dect2::GenAm; S.chanOff = 0;
                S.width[dect2::GenAm] = dect2::kSwAmWidthHz;
                S.step[dect2::GenAm] = 4;   // 5 kHz
                if (!S.amAuto) S.amDet = 1;
                tuneFreq(a, dect2::swBandCentreKhz(sw[i]) / 1e3); S.lastDial = a.freqMhz; save();
            }
            {
                std::string t = std::string(sw[i].name) + (sw[i].tropical ? " (tropical zone only)" : "");
                for (int r = 1; r <= 3; r++) {
                    double s0, e0;
                    t += "\nRegion " + std::to_string(r) + ": " + (dect2::swBandInRegion(sw[i], r, s0, e0) ? std::to_string((int)s0) + " - " + std::to_string((int)e0) + " kHz" : std::string("no broadcasting"));
                }
                if (*sw[i].note) t += std::string("\n") + sw[i].note;
                tip(t.c_str());
            }
            ImGui::PopID();
        }
        ImGui::TextDisabled("MW steps are 9 kHz (Europe, Asia) or 10 kHz (Americas); the shortwave buttons use 5 kHz. Needs a radio that reaches medium wave and shortwave (HF / direct sampling).");
    }
    // filter width presets
    ImGui::SameLine(0, 16 * gUi);
    ImGui::TextDisabled("filter");
    int np = 0;
    const double* pr = dect2::generalWidthPresets(S.demod, np);
    for (int i = 0; i < np; i++) {
        ImGui::SameLine(0, 4 * gUi);
        char b[32];
        snprintf(b, sizeof b, "%.4g k##gw%d", pr[i] / 1e3, i);
        const bool sel = std::fabs(S.width[S.demod] - pr[i]) < 1;
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::SmallButton(b)) { S.width[S.demod] = pr[i]; save(); }
        if (sel) ImGui::PopStyleColor();
    }
    ImGui::SameLine(0, 8 * gUi);
    ImGui::Text("%.4g kHz", S.width[S.demod] / 1e3);
    hamVoiceUi(a);

    // squelch, AGC, de-emphasis, pitch
    if (ImGui::Checkbox("Squelch", &S.sqOn)) save();
    tip("Mute the noise between transmissions");
    flowNext(6 * gUi);
    ImGui::BeginDisabled(!S.sqOn);
    ImGui::SetNextItemWidth(80 * gUi);
    int sm = S.sqAuto ? 0 : 1;
    if (comboTr("##gsqm", &sm, {N_("Auto"), N_("Level")})) { S.sqAuto = sm == 0; save(); }
    tip("Auto: USB and LSB open when the voice passband is 6 dB over its own noise (speech, no carrier needed). NFM, AM and the others open 6 dB over the noise beside the channel. Level: opens at the threshold in dBFS.");
    if (!S.sqAuto) {
        ImGui::SameLine(0, 4 * gUi);
        ImGui::SetNextItemWidth(140 * gUi);
        ImGui::SliderFloat("##gsql", &S.sqDb, -130, 0, "%.0f dBFS");
        if (ImGui::IsItemDeactivatedAfterEdit()) save();
        ImGui::SameLine(0, 4 * gUi);
        if (ImGui::SmallButton("Set") && live(a) && T(a).levelDb > -199) { S.sqDb = std::round(T(a).levelDb + 3); save(); }
        tip("Threshold 3 dB over the level now (set it while no one transmits)");
    }
    ImGui::EndDisabled();
    const bool fm = S.demod == dect2::GenNfm || S.demod == dect2::GenWfm;
    flowNext(14 * gUi);
    if (fm) {
        ImGui::TextDisabled("de-emphasis"); flowNext(4 * gUi);
        int& de = S.demod == dect2::GenNfm ? S.deNfm : S.deWfm;
        ImGui::SetNextItemWidth(85 * gUi);
        if (ImGui::Combo("##gde", &de, kDeemphNames, 4)) save();
        tip("FM de-emphasis: 50 us in Europe, 75 us in the Americas for broadcast FM; 750 us for pre-emphasised narrow FM");
    } else {
        if (ImGui::Checkbox("AGC", &S.agc)) save();
        tip("Automatic gain: AM follows the carrier, SSB and CW the envelope (fast attack, slow release)");
        if (!S.agc) {
            flowNext(4 * gUi);
            ImGui::SetNextItemWidth(130 * gUi);
            ImGui::SliderFloat("##ggain", &S.gainDb, 0, 80, "gain %.0f dB");
            if (ImGui::IsItemDeactivatedAfterEdit()) save();
        }
        if (S.demod == dect2::GenCw) {
            flowNext(10 * gUi);
            ImGui::SetNextItemWidth(130 * gUi);
            ImGui::SliderFloat("##gpitch", &S.pitch, 200, 1500, "pitch %.0f Hz");
            if (ImGui::IsItemDeactivatedAfterEdit()) save();
            tip("BFO: the tone a carrier on the channel frequency gives");
        }
        if (S.demod == dect2::GenAm) {
            flowNext(10 * gUi);
            if (ImGui::Checkbox(tr("Auto quality"), &S.amAuto)) save();
            tip("Locks onto the carrier, picks the cleaner sideband when the other one has interference, sets the audio bandwidth from what is received, cuts clicks, removes whistles and hum, reduces noise and keeps the loudness steady.\nAM has no noise immunity like FM: this removes what is not the programme, it cannot bring back sound that the noise covers.");
            if (S.amAuto) {
                flowNext(6 * gUi);
                ImGui::SetNextItemWidth(120 * gUi);
                int nrSel = S.amNr < 0 ? 0 : S.amNr == 0 ? 1 : S.amNr <= 40 ? 2 : 3;
                if (comboTr("##gamnr", &nrSel, {N_("Noise: auto"), N_("Noise: off"), N_("Noise: gentle"), N_("Noise: strong")})) { S.amNr = nrSel == 0 ? -1 : nrSel == 1 ? 0 : nrSel == 2 ? 30 : 90; save(); }
                tip("Noise reduction strength. Auto follows the signal to noise ratio (off on a clean station).");
                flowNext(6 * gUi);
                if (ImGui::Checkbox(tr("Enhanced treble"), &S.amEnh)) save();
                tip("Experimental: adds synthetic treble above what the station sends (made from the audio below it). Not faithful; off by default.");
                if (live(a) && T(a).amActive && !T(a).amStatus.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", T(a).amStatus.c_str()); ImGui::PopTextWrapPos(); }
            }
            flowNext(10 * gUi);
            ImGui::SetNextItemWidth(130 * gUi);
            if (S.amAuto ? comboTr("##gamdet", &S.amDet, {N_("Detector: auto"), N_("Synchronous"), N_("Sync upper only"), N_("Sync lower only")})
                         : comboTr("##gamdet", &S.amDet, {N_("Envelope"), N_("Synchronous"), N_("Sync upper only"), N_("Sync lower only")})) save();
            tip("Envelope: the classic AM detector, best with two stations on one frequency.\nSynchronous: locks onto the carrier; no distortion when the carrier fades on short wave, a little less noise on a weak station.\nUpper / lower only: one sideband, to leave out a station next to it on the other side.");
        }
    }
    if (S.demod != dect2::GenWfm) {
        flowNext(10 * gUi);
        if (ImGui::Checkbox("Blanker", &S.nb)) save();
        tip("Noise blanker: cuts out clicks and pulses (ignition, electric fences, power line noise) before the channel filter. Off for clean signals.");
        if (S.nb) {
            flowNext(4 * gUi);
            ImGui::SetNextItemWidth(110 * gUi);
            ImGui::SliderFloat("##gnbdb", &S.nbDb, 6, 30, "above %.0f dB");
            if (ImGui::IsItemDeactivatedAfterEdit()) save();
            tip("How far above the average a pulse has to be to be cut out. Lower catches weaker pulses but can clip strong speech.");
        }
    }
    flowNext(14 * gUi);
    if (iconFlat(a.muted ? Ic::Mute : Ic::Speaker, a.muted ? "Unmute" : "Mute")) a.muted = !a.muted;
    ImGui::SameLine(0, 4 * gUi);
    ImGui::SetNextItemWidth(100 * gUi);
    float vol = a.volume * 100.f;
    if (ImGui::SliderFloat("##gvol", &vol, 0, 100, "%.0f %%")) a.volume = vol / 100.f;
    tip("Volume (shared with the other modes)");
    flowEnd();

    // S-meter and recording
    sMeter(a);
    const bool rec = live(a) ? T(a).recording : grx(a).recording();
    if (!rec) {
        ImGui::BeginDisabled(!a.engine.running());
        if (ImGui::SmallButton(S.mp3 ? "Record MP3" : S.flac ? "Record FLAC" : "Record WAV")) startRecording(a);
        ImGui::EndDisabled();
        tip("Record what you hear (after the squelch, before the volume) to a 16-bit 48 kHz WAV file in the audio-recordings folder");
    } else if (ImGui::SmallButton("Stop recording")) grx(a).stopRecording();
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Checkbox("FLAC", &S.flac)) { plat::prefs().setB("genFlac", S.flac); grx(a).setRecordFormat(S.flac, recTags(a, listenHz(a))); grx(a).setRecordMp3(S.mp3); }
    tip("Save recordings as FLAC (lossless, about half the size of WAV, with the frequency and date as tags) instead of WAV");
    ImGui::SameLine(0, 6 * gUi);
    ImGui::BeginDisabled(!dect2::GeneralReceiver::mp3Available());
    if (ImGui::Checkbox("MP3", &S.mp3)) { plat::prefs().setB("genMp3", S.mp3); grx(a).setRecordMp3(S.mp3); }
    ImGui::EndDisabled();
    tip(dect2::GeneralReceiver::mp3Available() ? "Save recordings as MP3 (64 kbit/s mono, small files); wins over FLAC" : "MP3 needs the LAME library (libmp3lame): install it (brew install lame, apt install libmp3lame0, or put libmp3lame.dll in the OnAir codecs folder, %LOCALAPPDATA%\\OnAir\\codecs, which updates do not touch). OnAir does not ship it.");
    {   // one file per transmission, for the voice modes
        ImGui::SameLine(0, 8 * gUi);
        if (ImGui::Checkbox("One file per transmission", &S.txRec)) {
            std::error_code ec;
            const std::string dir = plat::dataDir() + "/audio-recordings";
            std::filesystem::create_directories(dir, ec);
            auto& r = grx(a);
            r.setRecordFormat(S.flac, recTags(a, listenHz(a))); r.setRecordMp3(S.mp3);
            r.setListenHz(listenHz(a));
            r.setTransmissionGate(S.preRoll, S.txHang, channelIq(S.demod), true);
            r.setTransmissionRecording(S.txRec, dir, "");
        }
        tip("One file each time the squelch opens. It starts with the pre-roll, so the first syllable is kept, and it stays open through the hang so a pause between words is not a new file. The name has the time, the frequency and the mode, and a line is added to transmissions.csv. Clips under half a second are dropped. Needs the squelch on.");
        if (S.txRec) { ImGui::SameLine(0, 6 * gUi); ImGui::TextDisabled("%d saved", grx(a).transmissionFiles()); }
    }
    if (live(a) && (T(a).recording || !T(a).recordPath.empty())) {
        ImGui::SameLine(0, 8 * gUi);
        if (T(a).recording) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "REC %.1f s", T(a).recordSec);
        else ImGui::TextDisabled("saved");
        ImGui::SameLine(0, 6 * gUi);
        ImGui::TextDisabled("%s", T(a).recordPath.c_str());
    }
    const std::string& err = S.recErr.empty() && live(a) ? T(a).recordError : S.recErr;
    if (!err.empty()) { ImGui::SameLine(0, 8 * gUi); ImGui::TextColored(pal::warnAmber(), "%s", err.c_str()); }
    ImGui::SetNextItemWidth(155 * gUi);
    ImGui::SliderFloat("##gpr", &S.preRoll, 0.f, 3.f, "pre-roll %.1f s");
    if (ImGui::IsItemDeactivatedAfterEdit()) plat::prefs().setD("genPreRoll", S.preRoll);
    tip("Seconds of signal kept from before the squelch opens, written at the start of each transmission file (0 to 3).");
    ImGui::SameLine(0, 8 * gUi);
    ImGui::SetNextItemWidth(140 * gUi);
    ImGui::SliderFloat("##ghang", &S.txHang, 0.f, 3.f, "hang %.1f s");
    if (ImGui::IsItemDeactivatedAfterEdit()) plat::prefs().setD("genTxHang", S.txHang);
    tip("How long the file stays open after the squelch shuts. About 1.5 s keeps one SSB transmission together across a gap between words. The mute itself still waits 150 ms.");
    ImGui::SameLine(0, 8 * gUi);
    bool iq = channelIq(S.demod);
    if (ImGui::Checkbox("Channel IQ", &iq)) { S.txIq = iq ? 1 : 0; plat::prefs().setI("genTxIq", S.txIq); }
    tip("Save the channel's complex baseband next to the audio (48 kHz float IQ, about 23 MB a minute), so the carrier can be measured afterwards. On for USB and LSB unless you change it; off for NFM, where those files would be large.");
    audioFiltersUi(a);
    noiseRemoverUi(a);
    extraChannelsUi(a);
}

// ---- audio filters: high-pass, low-pass and a notch for a whistle (they apply to what is heard and recorded)
void audioFiltersUi(App& a) {
    (void)a;
    bool ch = false;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Audio filters");
    auto one = [&](const char* id, const char* lab, float* v, float lo, float hi, const char* tipText) {
        ImGui::SameLine(0, 8 * gUi);
        ImGui::TextDisabled("%s", lab);
        ImGui::SameLine(0, 3 * gUi);
        ImGui::SetNextItemWidth(62 * gUi);
        if (ImGui::DragFloat(id, v, 10.f, 0.f, hi, *v > 0 ? "%.0f Hz" : "off")) { if (*v > 0 && *v < lo) *v = lo; ch = true; }
        tip(tipText);
    };
    one("##afhp", "high-pass", &S.afHp, 50.f, 3000.f, "Cut the low rumble below this frequency (drag; 0 = off)");
    one("##aflp", "low-pass", &S.afLp, 500.f, 12000.f, "Cut the hiss above this frequency (drag; 0 = off). 3000 Hz is typical for voice");
    one("##afnt", "notch", &S.afNotch, 100.f, 12000.f, "Remove one steady whistle (carrier, heterodyne) at this audio frequency (drag; 0 = off)");
    if (ch) { auto& p = plat::prefs(); p.setD("genAfHp", S.afHp); p.setD("genAfLp", S.afLp); p.setD("genAfNotch", S.afNotch); }
}

// ---- neural noise remover (RNNoise): NFM, WFM and AM only, remembered per demodulator, off until switched on
void noiseRemoverUi(App& a) {
    (void)a;
    if (!nrOffered(S.demod)) return;
    const int d = S.demod;
    bool ch = ImGui::Checkbox(tr("Noise remover (neural)##gennr"), &S.nrOn[d]);
    if (S.nrOn[d]) {
        ImGui::SameLine(0, 8 * gUi);
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::SliderFloat("##gennrs", &S.nrStr[d], 0.f, 100.f, "%.0f%%")) ch = true;
        tip(tr("Mix of cleaned and original audio: right removes more noise, left keeps more of the original. Adds about 20 ms of delay."));
    }
    ImGui::SameLine(0, 8 * gUi);
    ImGui::TextDisabled("%s", tr("Speech only; do not use on music or data modes."));
    if (ch) { auto& p = plat::prefs(); char k[24]; snprintf(k, sizeof k, "genNrOn%d", d); p.setB(k, S.nrOn[d]); snprintf(k, sizeof k, "genNrStr%d", d); p.setD(k, S.nrStr[d]); }
}

// ---- more channels from the same capture
void extraChannelsUi(App& a) {
    if (isSw(a)) return;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Channels");
    ImGui::SameLine();
    ImGui::BeginDisabled(S.nExtra <= 0);
    if (ImGui::SmallButton("-##exn")) { S.nExtra--; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Text("%d", S.nExtra + 1);
    ImGui::SameLine();
    ImGui::BeginDisabled(S.nExtra >= dect2::Engine::kGeneralExtra);
    if (ImGui::SmallButton("+##exn")) {
        S.exDemod[S.nExtra] = S.demod;
        S.exOff[S.nExtra] = S.chanOff + (S.nExtra % 2 ? -1 : 1) * 100e3 * (1 + S.nExtra / 2);
        S.nExtra++;
    }
    ImGui::EndDisabled();
    tip("Listen to more than one channel of the capture at once. Each extra channel is another demodulator and costs CPU time; drag the amber lines on the spectrum onto the signals. You hear the first channel; the others show their level and can record each transmission to a file.");
    if (S.nExtra > 0) {
        ImGui::SameLine();
        ImGui::TextColored(pal::warnAmber(), "Each channel adds CPU load: use a lower sample rate when the computer struggles.");
    }
    for (int i = 0; i < S.nExtra && i < dect2::Engine::kGeneralExtra; i++) {
        ImGui::PushID(i);
        const auto& t = S.exTel[i];
        ImGui::Text("Ch %d  %.4f MHz", i + 2, (a.freqMhz * 1e6 + S.exOff[i]) / 1e6);
        ImGui::SameLine(0, 8 * gUi);
        ImGui::SetNextItemWidth(70 * gUi);
        if (ImGui::BeginCombo("##exd", dect2::generalDemodName(S.exDemod[i]))) {
            for (int d = 0; d < dect2::GenDemodCount; d++) if (ImGui::Selectable(dect2::generalDemodName(d), d == S.exDemod[i])) S.exDemod[i] = d;
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, 8 * gUi);
        if (live(a) && t.levelDb > -199) ImGui::TextColored(t.squelchOpen ? ImVec4(0.45f, 0.85f, 0.55f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "%.1f dBFS %s", t.levelDb, t.squelchOpen ? "open" : "closed");
        else ImGui::TextDisabled("no level yet");
        ImGui::SameLine(0, 8 * gUi);
        if (ImGui::SmallButton("Listen")) {   // swap with the channel that is heard
            std::swap(S.exOff[i], S.chanOff);
            std::swap(S.exDemod[i], S.demod);
            save();
        }
        tip("Hear this channel (it swaps places with the first one)");
        ImGui::SameLine(0, 8 * gUi);
        if (ImGui::Checkbox("Rec", &S.exRec[i])) {
            std::error_code ec;
            const std::string dir = plat::dataDir() + "/audio-recordings";
            std::filesystem::create_directories(dir, ec);
            auto& x = a.engine.generalExtra(i);
            const double hz = a.freqMhz * 1e6 + S.exOff[i];
            x.setRecordFormat(S.flac, recTags(a, hz)); x.setRecordMp3(S.mp3);
            x.setListenHz(hz);
            x.setTransmissionGate(S.preRoll, S.txHang, channelIq(S.exDemod[i]), true);
            x.setTransmissionRecording(S.exRec[i], dir, "");
        }
        tip("Record each transmission on this channel to its own file (time, frequency and mode in the name; audio-recordings folder)");
        if (S.exRec[i]) { ImGui::SameLine(0, 6 * gUi); ImGui::TextDisabled("%d saved", a.engine.generalExtra(i).transmissionFiles()); }
        ImGui::PopID();
    }
}

bool amBroadcast(const App& a) { const dect2::SubMode* sm = curSub(a); return sm && !strcmp(sm->id, "am"); }

void tick(App& a) {
    load();
    {   // the AM broadcast entry opens in AM (once per visit; the demodulator can be changed afterwards)
        static bool wasAm = false;
        const bool am = amBroadcast(a);
        if (am && !wasAm && S.demod != dect2::GenAm) { S.demod = dect2::GenAm; save(); }
        wasAm = am;
    }
    if (S.lastDial >= 0 && std::fabs(a.freqMhz - S.lastDial) > 1e-9) { S.chanOff = 0; save(); }   // the dial was moved elsewhere
    S.lastDial = a.freqMhz;
    auto& r = grx(a);
    if (isSw(a)) { a.engine.shortwave().setDemod(S.demod); a.engine.shortwave().setChannelOffsetHz(S.chanOff); }   // the decoders follow the channel marker
    else { r.setDemod(S.demod); r.setChannelOffsetHz(S.chanOff); }
    r.setWidthHz(S.width[S.demod]);
    r.setSquelch(S.sqOn, S.sqAuto, S.sqDb);
    r.setAgc(S.agc, S.gainDb);
    r.setDeemphUs(S.demod == dect2::GenNfm ? kDeemph[S.deNfm] : S.demod == dect2::GenWfm ? kDeemph[S.deWfm] : 0);
    r.setCwPitchHz(S.pitch);
    r.setAmDetector(S.amDet);
    r.setAmAuto(S.amAuto, S.amNr, 0, S.amEnh);
    r.setNoiseBlanker(S.nb && S.demod != dect2::GenWfm, S.nbDb);
    r.setVolume(a.volume); r.setMuted(a.muted);
    r.setAudioFilters(S.afHp, S.afLp, S.afNotch);
    r.setNoiseRemover(nrOffered(S.demod) && S.nrOn[S.demod], S.nrStr[S.demod] / 100.f);
    r.setListenHz(listenHz(a));
    r.setTransmissionGate(S.preRoll, S.txHang, channelIq(S.demod), true);
    if (isSw(a)) return;   // Shortwave has no extra channels: its decoders are the extra listeners
    a.engine.setGeneralExtraCount(S.nExtra);
    for (int i = 0; i < S.nExtra && i < dect2::Engine::kGeneralExtra; i++) {   // the other channels: the squelch and gain of the main one, their own offset and demodulator
        auto& x = a.engine.generalExtra(i);
        x.setSilent(true);
        x.setDemod(S.exDemod[i]);
        x.setWidthHz(S.width[S.exDemod[i]]);
        x.setChannelOffsetHz(S.exOff[i]);
        x.setSquelch(S.sqOn, S.sqAuto, S.sqDb);
        x.setAgc(S.agc, S.gainDb);
        x.setDeemphUs(S.exDemod[i] == dect2::GenNfm ? kDeemph[S.deNfm] : S.exDemod[i] == dect2::GenWfm ? kDeemph[S.deWfm] : 0);
        x.setCwPitchHz(S.pitch);
        x.setAmDetector(S.amDet);
        x.setAmAuto(S.amAuto, S.amNr, 0, S.amEnh);
        x.setNoiseBlanker(S.nb && S.exDemod[i] != dect2::GenWfm, S.nbDb);
        x.setAudioFilters(S.afHp, S.afLp, S.afNotch);
        x.setNoiseRemover(nrOffered(S.exDemod[i]) && S.nrOn[S.exDemod[i]], S.nrStr[S.exDemod[i]] / 100.f);
        x.setListenHz(a.freqMhz * 1e6 + S.exOff[i]);
        x.setTransmissionGate(S.preRoll, S.txHang, channelIq(S.exDemod[i]), true);
        dect2::GeneralTelemetry g;
        if (x.telemetry(g, S.exSeq[i])) { S.exSeq[i] = g.seq; S.exTel[i] = g; }
    }
}

void tab(App& a) {
    controls(a);
    if (!gSpectrumInShell) spectrumWaterfall(a, specWaterfallSplit(), false);
}

void receiver(App& a) {
    const auto& t = T(a);
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    ImGui::TextWrapped("%s", dect2::generalSummary(t).c_str());
    ImGui::Text("Listening on %.6f MHz (%+.3f kHz from the dial)", listenHz(a) / 1e6, t.chanOffsetHz / 1e3);
    ImGui::Text("Input %.3f Msps, demodulated at %.0f kHz", t.rate / 1e6, t.demodRate / 1e3);
    ImGui::Text("Filter %+.2f to %+.2f kHz around the channel", t.filterLoHz / 1e3, t.filterHiHz / 1e3);
    if (t.noiseDb > -199) ImGui::Text("Noise in the channel %.1f dBFS", t.noiseDb); else ImGui::TextDisabled("Noise not measured yet");
    if (t.squelchDb > -199) ImGui::Text("Squelch threshold %.1f dBFS", t.squelchDb);
    if (t.demod <= dect2::GenWfm && t.squelchOpen) ImGui::Text("FM offset %+.0f Hz", t.cfoHz);
    ImGui::Text("Audio %.1f dBFS", t.audioDb);
}

void list(App& a) {
    load();
    ImGui::TextDisabled("%s %.4g kHz", dect2::generalDemodName(S.demod), S.width[S.demod] / 1e3);
    ImGui::Text("%.4f MHz", listenHz(a) / 1e6);
    if (live(a)) ImGui::TextDisabled("%s", T(a).squelchOpen ? "squelch open" : "squelch closed");
}

void status(App& a) {
    StatusPanel panel;
    const bool on = live(a);
    lamp("RX", on ? (T(a).state == 2 ? 1 : T(a).state == 1 ? 2 : 3) : 0, (int)Ic::Speaker); flowNext(12 * gUi);
    ImGui::TextUnformatted(on ? dect2::generalSummary(T(a)).c_str() : (a.engine.running() ? "starting" : "stopped"));
    flowEnd();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    char b[96];
    snprintf(b, sizeof b, "%s %.4f MHz", dect2::generalDemodName(S.demod), listenHz(a) / 1e6);
    l1 = b;
    if (live(a)) l2 = dect2::generalSummary(T(a));
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const auto& t = T(a);
    out.push_back({"LEVEL  dBFS", "%.0f", (double)t.levelDb, -130, 0, t.squelchOpen ? 1 : 0});
    out.push_back({"SNR  dB", "%.0f", (double)t.snrDb, 0, 40, !t.dataValid ? 0 : t.snrDb >= 15 ? 1 : t.snrDb >= 6 ? 2 : 3});
}

void synth(App& a, bool& changed) {
    dect2::SynthConfig& sc = a.tune.synth;
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("one station on the dial frequency, modulated by a 1 kHz tone (CW: a steady carrier), over noise"); ImGui::PopTextWrapPos(); }
    int m = std::clamp(sc.modeOpt[0], 0, (int)dect2::GenDemodCount - 1);
    ImGui::SetNextItemWidth(120 * gUi);
    if (ImGui::Combo("Modulation##gsyn", &m, "NFM\0WFM\0AM\0USB\0LSB\0CW\0")) { sc.modeOpt[0] = m; changed = true; }
    bool quiet = sc.modeOpt[1] == 1;
    if (ImGui::Checkbox("Noise only##gsyn", &quiet)) { sc.modeOpt[1] = quiet ? 1 : 0; changed = true; }
    tip("No station: the squelch must stay closed");
}

} // namespace


// ---- the Memory panel (app/memory): the listening frequency and the options a memory keeps for this mode
double generalListenHz(const App& a) { load(); return listenHz(a); }
void generalSetListenHz(App& a, double hz) { load(); setListen(a, hz); }   // the big readout: the VFO inside the band, the radio only past its edge
void generalRecallHz(App& a, double hz) { load(); S.chanOff = 0; a.freqMhz = hz / 1e6; S.lastDial = a.freqMhz; save(); }   // the channel back on the dial, the dial on hz
// ---- the remote API (app/platform/remote_api_app.cpp): the demodulator and the squelch of the General receiver
int generalApiDemod() { load(); return S.demod; }
// Shortwave (app/shortwave): the General screen's pieces
void generalControlsPanel(App& a) { controls(a); }
void generalTick(App& a) { tick(a); }
void generalApiSetWidthHz(double hz) { load(); S.width[S.demod] = dect2::generalClampWidth(S.demod, hz); save(); }
double generalApiWidthHz() { load(); return S.width[S.demod]; }
dect2::GeneralReceiver& generalRx(App& a) { return grx(a); }
bool generalApiSetDemod(int d) { load(); if (d < 0 || d >= dect2::GenDemodCount) return false; setDemod(d); return true; }
void generalApiSquelch(bool on, bool autoLevel, double db) { load(); S.sqOn = on; S.sqAuto = autoLevel; if (db > -140 && db < 20) S.sqDb = (float)db; save(); }
bool generalApiRecord(App& a, bool on, std::string& err) {
    load();
    if (!on) { grx(a).stopRecording(); return true; }
    if (grx(a).recording()) { err = "already recording"; return false; }
    startRecording(a);
    if (!S.recErr.empty()) { err = S.recErr; return false; }
    return true;
}

namespace {
std::string numS(double v) { char b[32]; snprintf(b, sizeof b, "%.6g", v); return b; }
double numV(const std::string& s, double def) { char* e = nullptr; const double d = strtod(s.c_str(), &e); return e == s.c_str() ? def : d; }
const bool gGenOptions = [] {
    auto is = [](const App& a) { return isGeneralFamily(a.family); };
    MemoryOptionReg r1({"genDemod", is, [](const App&) { load(); return std::to_string(S.demod); },
                        [](App&, const std::string& v) { load(); S.demod = std::clamp((int)numV(v, S.demod), 0, (int)dect2::GenDemodCount - 1); save(); }});
    MemoryOptionReg r2({"genWidthHz", is, [](const App&) { load(); return numS(S.width[S.demod]); },
                        [](App&, const std::string& v) { load(); S.width[S.demod] = dect2::generalClampWidth(S.demod, numV(v, S.width[S.demod])); save(); }});
    MemoryOptionReg r3({"genSquelch", is, [](const App&) { load(); return std::string(S.sqOn ? (S.sqAuto ? "auto" : "level") : "off"); },
                        [](App&, const std::string& v) { load(); S.sqOn = v != "off"; S.sqAuto = v != "level"; save(); }});
    MemoryOptionReg r4({"genSquelchDb", is, [](const App&) { load(); return numS(S.sqDb); },
                        [](App&, const std::string& v) { load(); S.sqDb = (float)std::clamp(numV(v, S.sqDb), -130.0, 0.0); save(); }});
    MemoryOptionReg r5({"genAgc", is, [](const App&) { load(); return std::string(S.agc ? "1" : "0"); }, [](App&, const std::string& v) { load(); S.agc = v == "1"; save(); }});
    MemoryOptionReg r6({"genGainDb", is, [](const App&) { load(); return numS(S.gainDb); },
                        [](App&, const std::string& v) { load(); S.gainDb = (float)std::clamp(numV(v, S.gainDb), -20.0, 80.0); save(); }});
    MemoryOptionReg r7({"genPitchHz", is, [](const App&) { load(); return numS(S.pitch); },
                        [](App&, const std::string& v) { load(); S.pitch = (float)std::clamp(numV(v, S.pitch), 200.0, 1500.0); save(); }});
    MemoryOptionReg r8({"genDeemph", is, [](const App&) { load(); return std::to_string(S.demod == dect2::GenNfm ? S.deNfm : S.deWfm); },
                        [](App&, const std::string& v) { load(); int& d = S.demod == dect2::GenNfm ? S.deNfm : S.deWfm; d = std::clamp((int)numV(v, d), 0, 3); save(); }});
    return true;
}();
}  // namespace

// ---- the channel marker on the spectrum and the waterfall (plots.cpp)

double generalChannelWidthMhz(const App&) { load(); double lo, hi; edges(lo, hi); return (hi - lo) / 1e6; }

const char* generalDemodLabel() { load(); return dect2::generalDemodName(S.demod); }

// the same demodulator choice as the controls above, drawn in the spectrum's BW popup
void generalDemodPicker() {
    load();
    for (int d = 0; d < dect2::GenDemodCount; d++) {
        if (d) ImGui::SameLine();
        if (ImGui::RadioButton(dect2::generalDemodName(d), S.demod == d)) setDemod(d);
    }
}

void generalChannelBand(const App& a, bool fill) {
    load();
    double lo, hi;
    edges(lo, hi);
    const double f = listenHz(a) / 1e6, x0 = f + lo / 1e6, x1 = f + hi / 1e6;
    if (specLookActive(a)) specLookBand(a, x0, x1, f, fill, S.drag >= 0);   // the spectrum look's passband (the counter below still drawn)
    else {
        if (fill) { plt::Spec band; band.FillColor = pal::accent(pal::dev() ? 0.14f : 0.16f); plt::PlotVBand("band", x0, x1, band); }
        const double e[2] = {x0, x1};
        plt::Spec es; es.LineColor = pal::accent(fill ? 0.55f : 0.70f); es.LineWeight = S.drag >= 0 ? 2.f : 1.f;
        plt::PlotInfLines("bandedges", e, 2, es);
        const double c[1] = {f};
        plt::Spec cs; cs.LineColor = ImVec4(0.95f, 0.35f, 0.30f, 0.85f); cs.LineWeight = 1.2f;
        plt::PlotInfLines("channel", c, 1, cs);
    }
    if (fill && S.nExtra > 0) {   // the counter at the top right of the spectrum
        char b[40]; snprintf(b, sizeof b, "%d decoders", S.nExtra + 1);
        const ImVec2 pp = plt::GetPlotPos(), ps = plt::GetPlotSize(), ts = ImGui::CalcTextSize(b);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 q(pp.x + ps.x - ts.x - 12 * gUi, pp.y + 6 * gUi);
        dl->AddRectFilled(ImVec2(q.x - 5 * gUi, q.y - 2 * gUi), ImVec2(q.x + ts.x + 5 * gUi, q.y + ts.y + 2 * gUi), IM_COL32(20, 22, 26, 200), 4.f * gUi);
        dl->AddText(q, IM_COL32(245, 190, 70, 255), b);
    }
    for (int i = 0; i < S.nExtra && i < 3; i++) {   // the other channels: a band in another colour, its centre can be dragged
        double elo, ehi;
        dect2::generalFilterEdges(S.exDemod[i], S.width[S.exDemod[i]], elo, ehi);
        const double fx = (a.freqMhz * 1e6 + S.exOff[i]) / 1e6;
        char id[24];
        if (fill) { plt::Spec b; b.FillColor = ImVec4(0.95f, 0.75f, 0.25f, 0.16f); snprintf(id, sizeof id, "exband%d", i); plt::PlotVBand(id, fx + elo / 1e6, fx + ehi / 1e6, b); }
        const double cx[1] = {fx};
        plt::Spec xs; xs.LineColor = ImVec4(0.95f, 0.75f, 0.25f, S.exDrag == i ? 1.f : 0.85f); xs.LineWeight = S.exDrag == i ? 2.f : 1.2f;
        snprintf(id, sizeof id, "exch%d", i);
        plt::PlotInfLines(id, cx, 1, xs);
    }
}

bool generalDragging(int plotId) { return S.drag >= 0 && S.dragPlot == plotId; }

void generalPlotMouse(App& a, int plotId) {
    load();
    const bool hov = plt::IsPlotHovered();
    if (!hov && !(S.drag >= 0 && S.dragPlot == plotId) && S.exDrag < 0) return;
    const plt::Rect lim = plt::GetPlotLimits();
    const ImVec2 pp = plt::GetPlotPos(), ps = plt::GetPlotSize();
    const double span = std::max(1e-9, lim.X.Max - lim.X.Min);
    auto px = [&](double mhz) { return pp.x + (float)((mhz - lim.X.Min) / span * ps.x); };
    const double mhz = plt::GetPlotMousePos().x, mx = ImGui::GetIO().MousePos.x;
    if (S.exDrag >= 0) {   // an extra channel's centre is held
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsMouseDown(0)) {
            S.exOff[S.exDrag] = snap(mhz * 1e6) - a.freqMhz * 1e6;
            ImGui::SetTooltip("channel %d: %.4f MHz", S.exDrag + 2, (a.freqMhz * 1e6 + S.exOff[S.exDrag]) / 1e6);
        } else S.exDrag = -1;
        return;
    }
    if (hov) for (int i = 0; i < S.nExtra && i < 3; i++) {
        if (std::fabs(mx - px((a.freqMhz * 1e6 + S.exOff[i]) / 1e6)) < 6 * gUi) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            ImGui::SetTooltip("drag to move channel %d (%.4f MHz)", i + 2, (a.freqMhz * 1e6 + S.exOff[i]) / 1e6);
            if (ImGui::IsMouseClicked(0)) S.exDrag = i;
            return;
        }
    }
    double lo, hi;
    edges(lo, hi);
    const double f = listenHz(a) / 1e6;
    const double el = f + lo / 1e6, eh = f + hi / 1e6;
    // which edges can be held: both for the symmetric filters, only the outer one for USB / LSB (the inner sits 300 Hz from the dial)
    const bool canLo = S.demod != dect2::GenUsb, canHi = S.demod != dect2::GenLsb;
    int edgeNear = -1;
    const float grab = 5 * gUi;
    if (canLo && std::fabs(mx - px(el)) < grab) edgeNear = 0;
    if (canHi && std::fabs(mx - px(eh)) < grab && (edgeNear < 0 || std::fabs(mx - px(eh)) < std::fabs(mx - px(el)))) edgeNear = 1;
    if (S.drag >= 0 && S.dragPlot == plotId) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsMouseDown(0)) {
            const double d = (mhz - f) * 1e6;
            double w;
            if (S.demod == dect2::GenUsb) w = d - dect2::kGeneralSsbLowCutHz;
            else if (S.demod == dect2::GenLsb) w = -d - dect2::kGeneralSsbLowCutHz;
            else w = 2 * std::fabs(d);
            S.width[S.demod] = std::round(dect2::generalClampWidth(S.demod, w) / 10) * 10;
            ImGui::SetTooltip("filter %.4g kHz", S.width[S.demod] / 1e3);
        } else { S.drag = -1; S.dragPlot = -1; S.dragged = true; save(); }
        return;
    }
    if (!hov) return;
    if (edgeNear >= 0) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsMouseClicked(0)) { S.drag = edgeNear; S.dragPlot = plotId; return; }
        ImGui::SetTooltip("drag to change the filter (%.4g kHz)", S.width[S.demod] / 1e3);
        return;
    }
    if (ImGui::IsMouseClicked(0)) S.dragged = false;
    const double to = snap(mhz * 1e6);
    const ImVec2 dd = ImGui::GetMouseDragDelta(0, 0);
    if (ImGui::IsMouseReleased(0) && !S.dragged && std::fabs(dd.x) < 4 && std::fabs(dd.y) < 4) { setListen(a, to, a.centreTuning); return; }
    if (std::fabs(to - listenHz(a)) > 0.5) {
        const double lx[2] = {to / 1e6, to / 1e6}, ly[2] = {lim.Y.Min, lim.Y.Max};
        plt::Spec ls; ls.LineColor = ImVec4(0.95f, 0.25f, 0.25f, 0.5f); ls.LineWeight = 1.f;
        plt::PlotLine("##gtunehere", lx, ly, 2, ls);
        ImGui::SetTooltip("click to listen on %.4f MHz", to / 1e6);
    }
}

extern const ModeUi kGeneralUi;
const ModeUi kGeneralUi = {
    .sideTitle = "CHANNEL",
    .tabName = "Listen",
    .tabIcon = Ic::Speaker,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .synth = synth,
    .tick = tick,
    .meters = meters,
};
