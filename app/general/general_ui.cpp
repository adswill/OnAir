// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// General receiver screens: the listening frequency with a step selector, the demodulator (NFM, WFM, AM, USB, LSB, CW), filter width
// presets, squelch (auto or level), AGC or a fixed gain, de-emphasis, CW pitch, the shared volume, an S-meter and recording to WAV,
// over the spectrum and the waterfall. On those plots the channel is a marker: a click puts it there (inside the band the radio
// covers, without retuning; outside it the radio is retuned), its filter edges can be dragged.
#include "app.h"
#include "dect2/general/sig_guess.h"
#include "gfx/plot.h"
#include "memory/memory_options.h"
#include "dect2/general/general_gen.h"
#include "dect2/general/general_rx.h"
#include "dect2/general/general_tel.h"
#include "dect2/ham/ham_voice.h"
#include "dect2/geo/maidenhead.h"
#include "state/freq_model.h"
#include "gfx/drag_tune.h"
#include "general/general_layout.h"
#include "general/general_panels.h"
#include "ui/touch.h"
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
    float nrStr[dect2::GenDemodCount] = {80, 80, 80, 80, 80, 80, 80, 80, 80};   // its strength in percent
    // 4.0: each demodulator keeps its own squelch, AGC and gain (and its width and step above); sqOn ... gainDb are the current one's
    bool sqOnD[dect2::GenDemodCount] = {}, sqAutoD[dect2::GenDemodCount] = {}, agcD[dect2::GenDemodCount] = {};
    float sqDbD[dect2::GenDemodCount] = {}, gainD[dect2::GenDemodCount] = {};
    bool snapOn = true;          // clicks and drags of the VFO snap to the step (genSnap)
    bool autoDemod = false;      // Auto: the demodulator and the filter from the band plan for the tuned frequency (genAutoDemod, off by default)
    double autoLastHz = -1;      // where Auto last looked (it acts when the frequency moves to another band)
    std::string autoWhy;         // what it picked, for the tooltip
    std::string autoBand;        // the band it picked for (a new band: a new pick; inside one band the user's choice stays)
    int vfoGrab = -1;            // the VFO body is held on this plot (a drag moves it, keeping where it was grabbed)
    double vfoGrabOff = 0;       // the mouse frequency less the VFO frequency at the press (Hz)
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
// 4.0: the same controls are drawn in the sidebar of the General screen (gSide: one narrow column) and in the old wide rows. In the sidebar the
// next control goes on the same line only when it fits (flowNext) and sliders and combos take the column's width.
bool gSide = false;
void nxt(float px) { if (gSide) flowNext(px < 0 ? -1.f : px * gUi); else if (px < 0) ImGui::SameLine(); else ImGui::SameLine(0, px * gUi); }
void wid(float px) { ImGui::SetNextItemWidth(gSide ? -FLT_MIN : px * gUi); }
bool isSw(const App& a) { return a.family == kShortwaveFamily; }   // Shortwave listens with this same screen
bool live(const App& a) { return a.rx.standard == (isSw(a) ? 213 : 56); }   // the engine reports its standard code minus one
const dect2::GeneralTelemetry& T(const App& a) { return isSw(a) ? a.rx.shortwave.general : a.rx.general; }
dect2::GeneralReceiver& grx(App& a) { return isSw(a) ? a.engine.shortwave().general() : a.engine.general(); }

bool nrOffered(int demod) { return demod == dect2::GenNfm || demod == dect2::GenWfm || demod == dect2::GenAm; }   // speech: not SSB data, CW or music
const int kDefStep[dect2::GenDemodCount] = {9, 11, 2, 1, 1, 1, 1, 2, 1};   // 12.5 kHz NFM, 50 kHz WFM, 1 kHz AM, 100 Hz SSB / CW / DSB / CW-L, 1 kHz RAW

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
    for (int d = 0; d < dect2::GenDemodCount; d++) {   // a profile from before 4.0 has only the shared values: every demodulator starts from them
        char k[24];
        snprintf(k, sizeof k, "genSqOn%d", d); S.sqOnD[d] = p.getB(k, S.sqOn);
        snprintf(k, sizeof k, "genSqAuto%d", d); S.sqAutoD[d] = p.getB(k, S.sqAuto);
        snprintf(k, sizeof k, "genSqDb%d", d); S.sqDbD[d] = (float)std::clamp(p.getD(k, S.sqDb), -130.0, 0.0);
        snprintf(k, sizeof k, "genAgc%d", d); S.agcD[d] = p.getB(k, S.agc);
        snprintf(k, sizeof k, "genGain%d", d); S.gainD[d] = (float)std::clamp(p.getD(k, S.gainDb), -20.0, 80.0);
    }
    S.sqOn = S.sqOnD[S.demod]; S.sqAuto = S.sqAutoD[S.demod]; S.sqDb = S.sqDbD[S.demod]; S.agc = S.agcD[S.demod]; S.gainDb = S.gainD[S.demod];
    S.snapOn = p.getB("genSnap", true);
    S.autoDemod = p.getB("genAutoDemod", false);
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
    p.setB("genAgc", S.agc); p.setD("genGain", S.gainDb);   // (the shared keys too: an older version reads these)
    S.sqOnD[S.demod] = S.sqOn; S.sqAutoD[S.demod] = S.sqAuto; S.sqDbD[S.demod] = S.sqDb; S.agcD[S.demod] = S.agc; S.gainD[S.demod] = S.gainDb;
    for (int d = 0; d < dect2::GenDemodCount; d++) {
        char k[24];
        snprintf(k, sizeof k, "genSqOn%d", d); p.setB(k, S.sqOnD[d]);
        snprintf(k, sizeof k, "genSqAuto%d", d); p.setB(k, S.sqAutoD[d]);
        snprintf(k, sizeof k, "genSqDb%d", d); p.setD(k, S.sqDbD[d]);
        snprintf(k, sizeof k, "genAgc%d", d); p.setB(k, S.agcD[d]);
        snprintf(k, sizeof k, "genGain%d", d); p.setD(k, S.gainD[d]);
    }
    p.setB("genSnap", S.snapOn); p.setB("genAutoDemod", S.autoDemod);
    p.setI("genDeNfm", S.deNfm); p.setI("genDeWfm", S.deWfm); p.setD("genPitch", S.pitch); p.setD("genOffset", S.chanOff);
    p.setI("genAmDet", S.amDet); p.setB("genNb", S.nb); p.setD("genNbDb", S.nbDb);
    p.setB("genAmAuto", S.amAuto); p.setI("genAmNr", S.amNr); p.setB("genAmEnh", S.amEnh);
}

double listenHz(const App& a) { return a.freqMhz * 1e6 + S.chanOff; }
bool channelIq(int demod) { return S.txIq < 0 ? (demod == dect2::GenUsb || demod == dect2::GenLsb || demod == dect2::GenRaw) : S.txIq > 0; }
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
double snap(double hz) { return S.snapOn ? genlayout::snapHz(hz, stepHz()) : hz; }   // clicks and drags; the step arrows always step from a step

// another demodulator: the squelch, AGC and gain the old one had are kept for it, the new one's come back (its width and step are per demodulator too)
void setDemod(int d) {
    if (d == S.demod || d < 0 || d >= dect2::GenDemodCount) return;
    S.sqOnD[S.demod] = S.sqOn; S.sqAutoD[S.demod] = S.sqAuto; S.sqDbD[S.demod] = S.sqDb; S.agcD[S.demod] = S.agc; S.gainD[S.demod] = S.gainDb;
    S.demod = d;
    S.sqOn = S.sqOnD[d]; S.sqAuto = S.sqAutoD[d]; S.sqDb = S.sqDbD[d]; S.agc = S.agcD[d]; S.gainDb = S.gainD[d];
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
    const std::string dir = recordingsSub("audio-recordings");
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
    nxt(8);
    if (on) {
        if (t.dataValid) ImGui::Text("%.1f dBFS  SNR %.1f dB", t.levelDb, t.snrDb);
        else ImGui::Text("%.1f dBFS", t.levelDb);
        nxt(8);
        ImGui::TextColored(t.squelchOpen ? ImVec4(0.45f, 0.85f, 0.55f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "%s", t.squelchOpen ? "open" : "closed");
    } else ImGui::TextDisabled("%s", a.engine.running() ? "no level yet" : "stopped");
}

// the squelch: Off / Level / Auto in one combo, with the live level and the threshold; the same block in the controls, the receiver tab and the
// "squelch closed" popup of the Listen list, so it is the same for every mode (NFM, WFM, AM, USB, LSB, CW)
void squelchUi(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Squelch"));
    nxt(6);
    int sm = !S.sqOn ? 0 : S.sqAuto ? 2 : 1;
    wid(90);
    if (comboTr("##gsqm", &sm, {N_("Off"), N_("Level"), N_("Auto")})) { S.sqOn = sm != 0; S.sqAuto = sm == 2; save(); }
    tip("Off: everything is heard, no muting. Level: opens at the threshold in dBFS (drag the slider, or Set). Auto: USB and LSB open when the voice passband is 6 dB over its own noise (speech, no carrier needed). NFM, WFM, AM and CW open 6 dB over the noise beside the channel. CW holds the squelch open for a second so keyed signals and beacons do not chatter.");
    if (S.sqOn && !S.sqAuto) {
        nxt(6);
        wid(140);
        ImGui::SliderFloat("##gsql", &S.sqDb, -130, 0, "%.0f dBFS");
        if (ImGui::IsItemDeactivatedAfterEdit()) save();
        nxt(4);
        if (ImGui::SmallButton("Set") && live(a) && T(a).levelDb > -199) { S.sqDb = std::round(T(a).levelDb + 3); save(); }
        tip("Threshold 3 dB over the level now (set it while no one transmits)");
    }
    sMeter(a);
}

void extraChannelsUi(App& a);
bool amBroadcast(const App& a);
void autoDemodTick(App& a);
void demodGrid(int cols);
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
        wid(380);
        int sel = S.hvRegion;
        if (ImGui::Combo(tr("Region##hvreg"), &sel, kReg, 4)) { S.hvRegion = sel; save(); }
        bool known = false; const int auto_ = stationRegion(&known);
        ImGui::TextDisabled("%s", S.hvRegion == 0 ? (known ? (auto_ == 1 ? "Region 1 found from your locator" : auto_ == 2 ? "Region 2 found from your locator" : "Region 3 found from your locator") : "No locator set (Ham setup or the setup wizard): Region 1 until you choose") : "Chosen by hand");
    }
    flowEnd();
}

// "What is this signal?": bandwidth, shape and a ranked guess from the spectrum around the listening frequency (sig_guess.h)
void guessUi(App& a) {
    static std::vector<dect2::sigguess::Guess> guess;
    static std::string guessNote;
    const bool have = a.engine.running() && !a.spec.dbfs.empty();
    ImGui::BeginDisabled(!have);
    if (ImGui::SmallButton("What is this signal?")) {
        guess.clear();
        const double fs = a.engine.sampleRate();
        const size_t n = a.spec.dbfs.size();
        const double binHz = fs / (double)n;
        const double offHz = listenHz(a) - a.engine.captureCentreHz();
        const double halfHz = std::min(fs * 0.45, 100e3);
        const long c = (long)std::lround(offHz / binHz + (double)n / 2.0), hb = (long)(halfHz / binHz);
        const long lo = std::max<long>(0, c - hb), hi = std::min<long>((long)n - 1, c + hb);
        if (hi - lo >= 64) {
            std::vector<float> win(a.spec.dbfs.begin() + lo, a.spec.dbfs.begin() + hi + 1);
            const dect2::sigguess::Features f = dect2::sigguess::measure(win, binHz);
            guess = dect2::sigguess::guess(f);
            for (auto& g : guess) g.offsetHz += ((double)(lo + hi) / 2.0 - (double)n / 2.0) * binHz + a.engine.captureCentreHz() - 0;   // against the capture centre
            char b[120]; std::snprintf(b, sizeof b, f.ok ? "Occupied bandwidth %.1f kHz, %.0f dB over the noise." : "No signal found near the listening frequency (%.1f kHz of span read).", f.ok ? f.obwHz / 1e3 : 2 * halfHz / 1e3, f.snrDb);
            guessNote = b;
        } else guessNote = "The capture is too narrow to look at the signal.";
    }
    ImGui::EndDisabled();
    tip("Measures the occupied bandwidth and the shape of the spectrum around the listening frequency and suggests the mode (AM, NFM, SSB, CW, FSK, wide FM, OFDM) and where to tune.");
    if (!guessNote.empty()) { nxt(-1); ImGui::TextDisabled("%s", guessNote.c_str()); }
    for (size_t gi = 0; gi < guess.size() && gi < 3; gi++) {
        const auto& g = guess[gi];
        ImGui::PushID((int)gi);
        ImGui::Text("%s  (%.0f%%)  %s", g.what.c_str(), g.confidence * 100, g.why.c_str());
        const int d = g.mode == "AM" ? dect2::GenAm : g.mode == "USB" ? dect2::GenUsb : g.mode == "LSB" ? dect2::GenLsb : g.mode == "CW" ? dect2::GenCw : g.mode == "FM" ? dect2::GenWfm : g.mode == "NFM" ? dect2::GenNfm : -1;
        if (d >= 0) { ImGui::SameLine(); if (ImGui::SmallButton("Use")) { setDemod(d); setListen(a, g.offsetHz, false); } }
        ImGui::PopID();
    }
}

// medium wave and the shortwave broadcast bands (kHz: the band centre, or a common frequency): the AM broadcast entry's tune list
void broadcastPresetsUi(App& a) {
    static const struct { const char* name; double khz; } kPre[] = {{"MW 531", 531}, {"MW 999", 999}, {"MW 1368", 1368}, {"MW 1566", 1566}};
    ImGui::TextDisabled("presets");
    for (const auto& pr : kPre) {
        nxt(4);
        if (ImGui::SmallButton(pr.name)) { S.chanOff = 0; tuneFreq(a, pr.khz / 1e3); S.lastDial = a.freqMhz; save(); }
    }
    size_t nsw = 0;
    const dect2::SwBand* sw = dect2::swBroadcastBands(nsw);
    for (size_t i = 0; i < nsw; i++) {   // each band: centre, AM with the synchronous detector, 9 kHz filter, 5 kHz step
        nxt(4);
        ImGui::PushID((int)i);
        if (ImGui::SmallButton(sw[i].name)) {
            setDemod(dect2::GenAm); S.chanOff = 0;
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
    if (gSide) flowEnd();
    ImGui::TextDisabled("MW steps are 9 kHz (Europe, Asia) or 10 kHz (Americas); the shortwave buttons use 5 kHz. Needs a radio that reaches medium wave and shortwave (HF / direct sampling).");
}

// AGC or the gain, de-emphasis, CW pitch, the AM processor and the noise blanker: what the demodulator in use offers
void demodOptionsUi(App& a) {
    const bool fm = S.demod == dect2::GenNfm || S.demod == dect2::GenWfm;
    if (fm) {
        ImGui::TextDisabled("de-emphasis"); flowNext(4 * gUi);
        int& de = S.demod == dect2::GenNfm ? S.deNfm : S.deWfm;
        wid(85);
        if (ImGui::Combo("##gde", &de, kDeemphNames, 4)) save();
        tip("FM de-emphasis: 50 us in Europe, 75 us in the Americas for broadcast FM; 750 us for pre-emphasised narrow FM");
    } else {
        if (ImGui::Checkbox("AGC", &S.agc)) save();
        tip("Automatic gain: AM follows the carrier, SSB and CW the envelope (fast attack, slow release)");
        if (!S.agc) {
            flowNext(4 * gUi);
            wid(130);
            ImGui::SliderFloat("##ggain", &S.gainDb, 0, 80, "gain %.0f dB");
            if (ImGui::IsItemDeactivatedAfterEdit()) save();
        }
        if (dect2::generalIsCw(S.demod)) {
            flowNext(10 * gUi);
            wid(130);
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
                wid(120);
                int nrSel = S.amNr < 0 ? 0 : S.amNr == 0 ? 1 : S.amNr <= 40 ? 2 : 3;
                if (comboTr("##gamnr", &nrSel, {N_("Noise: auto"), N_("Noise: off"), N_("Noise: gentle"), N_("Noise: strong")})) { S.amNr = nrSel == 0 ? -1 : nrSel == 1 ? 0 : nrSel == 2 ? 30 : 90; save(); }
                tip("Noise reduction strength. Auto follows the signal to noise ratio (off on a clean station).");
                flowNext(6 * gUi);
                if (ImGui::Checkbox(tr("Enhanced treble"), &S.amEnh)) save();
                tip("Experimental: adds synthetic treble above what the station sends (made from the audio below it). Not faithful; off by default.");
                if (live(a) && T(a).amActive && !T(a).amStatus.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", T(a).amStatus.c_str()); ImGui::PopTextWrapPos(); }
            }
            flowNext(10 * gUi);
            wid(130);
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
            wid(110);
            ImGui::SliderFloat("##gnbdb", &S.nbDb, 6, 30, "above %.0f dB");
            if (ImGui::IsItemDeactivatedAfterEdit()) save();
            tip("How far above the average a pulse has to be to be cut out. Lower catches weaker pulses but can clip strong speech.");
        }
    }
    if (gSide) flowEnd();   // (the old row goes on with the volume)
}

// recording: one file, or one per transmission (with pre-roll, hang and the channel IQ), WAV, FLAC or MP3
void recordingUi(App& a) {
    const bool rec = live(a) ? T(a).recording : grx(a).recording();
    if (!rec) {
        ImGui::BeginDisabled(!a.engine.running());
        if (ImGui::SmallButton(S.mp3 ? "Record MP3" : S.flac ? "Record FLAC" : "Record WAV")) startRecording(a);
        ImGui::EndDisabled();
        tip("Record what you hear (after the squelch, before the volume) to a 16-bit 48 kHz WAV file in the audio-recordings folder");
    } else if (ImGui::SmallButton("Stop recording")) grx(a).stopRecording();
    nxt(-1); kit::recordingsButton("gen", "audio-recordings");
    nxt(6);
    if (ImGui::Checkbox("FLAC", &S.flac)) { plat::prefs().setB("genFlac", S.flac); grx(a).setRecordFormat(S.flac, recTags(a, listenHz(a))); grx(a).setRecordMp3(S.mp3); }
    tip("Save recordings as FLAC (lossless, about half the size of WAV, with the frequency and date as tags) instead of WAV");
    nxt(6);
    ImGui::BeginDisabled(!dect2::GeneralReceiver::mp3Available());
    if (ImGui::Checkbox("MP3", &S.mp3)) { plat::prefs().setB("genMp3", S.mp3); grx(a).setRecordMp3(S.mp3); }
    ImGui::EndDisabled();
    tip(dect2::GeneralReceiver::mp3Available() ? "Save recordings as MP3 (64 kbit/s mono, small files); wins over FLAC" : "MP3 needs the LAME library (libmp3lame): install it (brew install lame, apt install libmp3lame0, or put libmp3lame.dll in the OnAir codecs folder, %LOCALAPPDATA%\\OnAir\\codecs, which updates do not touch). OnAir does not ship it.");
    {   // one file per transmission, for the voice modes
        nxt(8);
        if (ImGui::Checkbox("One file per transmission", &S.txRec)) {
            std::error_code ec;
            const std::string dir = recordingsSub("audio-recordings");
            std::filesystem::create_directories(dir, ec);
            auto& r = grx(a);
            r.setRecordFormat(S.flac, recTags(a, listenHz(a))); r.setRecordMp3(S.mp3);
            r.setListenHz(listenHz(a));
            r.setTransmissionGate(S.preRoll, S.txHang, channelIq(S.demod), true);
            r.setTransmissionRecording(S.txRec, dir, "");
        }
        tip("One file each time the squelch opens. It starts with the pre-roll, so the first syllable is kept, and it stays open through the hang so a pause between words is not a new file. The name has the time, the frequency and the mode, and a line is added to transmissions.csv. Clips under half a second are dropped. Needs the squelch on.");
        if (S.txRec) { nxt(6); ImGui::TextDisabled("%d saved", grx(a).transmissionFiles()); }
    }
    if (live(a) && (T(a).recording || !T(a).recordPath.empty())) {
        nxt(8);
        if (T(a).recording) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "REC %.1f s", T(a).recordSec);
        else ImGui::TextDisabled("saved");
        nxt(6);
        if (gSide) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", T(a).recordPath.c_str()); ImGui::PopTextWrapPos(); }
        else ImGui::TextDisabled("%s", T(a).recordPath.c_str());
    }
    const std::string& err = S.recErr.empty() && live(a) ? T(a).recordError : S.recErr;
    if (!err.empty()) { nxt(8); ImGui::TextColored(pal::warnAmber(), "%s", err.c_str()); }
    wid(155);
    ImGui::SliderFloat("##gpr", &S.preRoll, 0.f, 3.f, "pre-roll %.1f s");
    if (ImGui::IsItemDeactivatedAfterEdit()) plat::prefs().setD("genPreRoll", S.preRoll);
    tip("Seconds of signal kept from before the squelch opens, written at the start of each transmission file (0 to 3).");
    nxt(8);
    wid(140);
    ImGui::SliderFloat("##ghang", &S.txHang, 0.f, 3.f, "hang %.1f s");
    if (ImGui::IsItemDeactivatedAfterEdit()) plat::prefs().setD("genTxHang", S.txHang);
    tip("How long the file stays open after the squelch shuts. About 1.5 s keeps one SSB transmission together across a gap between words. The mute itself still waits 150 ms.");
    nxt(8);
    bool iq = channelIq(S.demod);
    if (ImGui::Checkbox("Channel IQ", &iq)) { S.txIq = iq ? 1 : 0; plat::prefs().setI("genTxIq", S.txIq); }
    tip("Save the channel's complex baseband next to the audio (48 kHz float IQ, about 23 MB a minute), so the carrier can be measured afterwards. On for USB, LSB and RAW unless you change it; off for NFM, where those files would be large.");
}

void controls(App& a) {
    load();
    if (a.family == 55 && a.newUi && !a.touchUi && !plat::prefs().getB("gen40Screen", true)) {   // the 3.9 layout chosen: the way back
        if (ImGui::SmallButton(tr("Use the 4.0 layout"))) plat::prefs().setB("gen40Screen", true);
        tip("The spectrum and the waterfall over the whole window, every control in one sidebar (docs/GENERAL-4.0.md). Needs View > Layout: Sidebar or Scope.");
    }
    // the listening frequency and the step
    ImGui::PushFont(a.mono, 0);
    ImGui::Text("%.4f MHz", listenHz(a) / 1e6);
    ImGui::PopFont();
    tip("The frequency listened to: the dial plus where the channel marker sits.\nClick the spectrum or the waterfall to move it; drag the edges of the light band to change the filter.");
    flowNext(10 * gUi);
    if (freqStepArrow("##gdn", ImGuiDir_Left)) setListen(a, genlayout::snapHz(listenHz(a), stepHz()) - stepHz(), a.centreTuning);   // target button: the radio follows (#301)
    tip("Down one step. Hold for a continuous change.");
    ImGui::SameLine(0, 2 * gUi);
    if (freqStepArrow("##gup", ImGuiDir_Right)) setListen(a, genlayout::snapHz(listenHz(a), stepHz()) + stepHz(), a.centreTuning);
    tip("Up one step. Hold for a continuous change.");
    ImGui::SameLine(0, 4 * gUi);
    ImGui::SetNextItemWidth(95 * gUi);
    if (ImGui::Combo("##gstep", &S.step[S.demod], kStepNames, kNumSteps)) save();
    tip("Frequency step: the arrows move by it and a click on the spectrum snaps to it");
    flowNext(6 * gUi);
    if (ImGui::SmallButton("Centre")) { S.chanOff = 0; tuneFreq(a, snap(listenHz(a)) / 1e6); S.lastDial = a.freqMhz; save(); }
    tip("Tune the radio so the channel sits on the dial (the marker back in the middle)");
    flowEnd();

    // the demodulator (4.0: the same grid as the sidebar, one row when it fits)
    demodGrid(ImGui::GetContentRegionAvail().x >= 10 * 54 * gUi ? 10 : 0);
    guessUi(a);
    if (amBroadcast(a)) broadcastPresetsUi(a);
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
    squelchUi(a);
    flowNext(14 * gUi);
    demodOptionsUi(a);
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
    recordingUi(a);
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
        nxt(8);
        ImGui::TextDisabled("%s", lab);
        nxt(3);
        wid(62);
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
        nxt(8);
        wid(110);
        if (ImGui::SliderFloat("##gennrs", &S.nrStr[d], 0.f, 100.f, "%.0f%%")) ch = true;
        tip(tr("Mix of cleaned and original audio: right removes more noise, left keeps more of the original. Adds about 20 ms of delay."));
    }
    nxt(8);
    ImGui::TextDisabled("%s", tr("Speech only; do not use on music or data modes."));
    if (ch) { auto& p = plat::prefs(); char k[24]; snprintf(k, sizeof k, "genNrOn%d", d); p.setB(k, S.nrOn[d]); snprintf(k, sizeof k, "genNrStr%d", d); p.setD(k, S.nrStr[d]); }
}

// ---- more channels from the same capture
void extraChannelsUi(App& a) {
    if (isSw(a)) return;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Channels");
    nxt(-1);
    ImGui::BeginDisabled(S.nExtra <= 0);
    if (ImGui::SmallButton("-##exn")) { S.nExtra--; }
    ImGui::EndDisabled();
    nxt(-1);
    ImGui::Text("%d", S.nExtra + 1);
    nxt(-1);
    ImGui::BeginDisabled(S.nExtra >= dect2::Engine::kGeneralExtra);
    if (ImGui::SmallButton("+##exn")) {
        S.exDemod[S.nExtra] = S.demod;
        S.exOff[S.nExtra] = S.chanOff + (S.nExtra % 2 ? -1 : 1) * 100e3 * (1 + S.nExtra / 2);
        S.nExtra++;
    }
    ImGui::EndDisabled();
    tip("Listen to more than one channel of the capture at once. Each extra channel is another demodulator and costs CPU time; drag the amber lines on the spectrum onto the signals. You hear the first channel; the others show their level and can record each transmission to a file.");
    if (S.nExtra > 0) {
        nxt(-1);
        ImGui::TextColored(pal::warnAmber(), "Each channel adds CPU load: use a lower sample rate when the computer struggles.");
    }
    for (int i = 0; i < S.nExtra && i < dect2::Engine::kGeneralExtra; i++) {
        ImGui::PushID(i);
        const auto& t = S.exTel[i];
        ImGui::Text("Ch %d  %.4f MHz", i + 2, (a.freqMhz * 1e6 + S.exOff[i]) / 1e6);
        nxt(8);
        wid(70);
        if (ImGui::BeginCombo("##exd", dect2::generalDemodName(S.exDemod[i]))) {
            for (int d = 0; d < dect2::GenDemodCount; d++) if (ImGui::Selectable(dect2::generalDemodName(d), d == S.exDemod[i])) S.exDemod[i] = d;
            ImGui::EndCombo();
        }
        nxt(8);
        if (live(a) && t.levelDb > -199) ImGui::TextColored(t.squelchOpen ? ImVec4(0.45f, 0.85f, 0.55f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "%.1f dBFS %s", t.levelDb, t.squelchOpen ? "open" : "closed");
        else ImGui::TextDisabled("no level yet");
        nxt(8);
        if (ImGui::SmallButton("Listen")) {   // swap with the channel that is heard
            std::swap(S.exOff[i], S.chanOff);
            { const int nd = S.exDemod[i]; S.exDemod[i] = S.demod; setDemod(nd); }   // its own squelch and AGC come with the demodulator
            save();
        }
        tip("Hear this channel (it swaps places with the first one)");
        nxt(8);
        if (ImGui::Checkbox("Rec", &S.exRec[i])) {
            std::error_code ec;
            const std::string dir = recordingsSub("audio-recordings");
            std::filesystem::create_directories(dir, ec);
            auto& x = a.engine.generalExtra(i);
            const double hz = a.freqMhz * 1e6 + S.exOff[i];
            x.setRecordFormat(S.flac, recTags(a, hz)); x.setRecordMp3(S.mp3);
            x.setListenHz(hz);
            x.setTransmissionGate(S.preRoll, S.txHang, channelIq(S.exDemod[i]), true);
            x.setTransmissionRecording(S.exRec[i], dir, "");
        }
        tip("Record each transmission on this channel to its own file (time, frequency and mode in the name; audio-recordings folder)");
        if (S.exRec[i]) { nxt(6); ImGui::TextDisabled("%d saved", a.engine.generalExtra(i).transmissionFiles()); }
        ImGui::PopID();
    }
}

bool amBroadcast(const App& a) { const dect2::SubMode* sm = curSub(a); return sm && !strcmp(sm->id, "am"); }

void tick(App& a) {
    load();
    {   // the AM broadcast entry opens in AM (once per visit; the demodulator can be changed afterwards)
        static bool wasAm = false;
        const bool am = amBroadcast(a);
        if (am && !wasAm && S.demod != dect2::GenAm) setDemod(dect2::GenAm);
        wasAm = am;
    }
    if (S.lastDial >= 0 && std::fabs(a.freqMhz - S.lastDial) > 1e-9) { S.chanOff = 0; save(); }   // the dial was moved elsewhere
    S.lastDial = a.freqMhz;
    autoDemodTick(a);   // 4.0 Auto (off by default): the demodulator and filter for a new band
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
    squelchUi(a);
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
    if (live(a)) {   // clickable: opens the squelch control (also in the receiver controls)
        const char* txt = !S.sqOn ? "squelch off" : T(a).squelchOpen ? "squelch open" : "squelch closed";
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Selectable(txt, false, 0, ImVec2(ImGui::CalcTextSize(txt).x, 0))) ImGui::OpenPopup("##gsqpop");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to change the squelch");
    }
    if (ImGui::BeginPopup("##gsqpop")) { squelchUi(a); ImGui::EndPopup(); }
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
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("one station on the dial frequency, modulated by a 1 kHz tone (CW, CW-L, RAW: a steady carrier; DSB: no carrier), over noise"); ImGui::PopTextWrapPos(); }
    int m = std::clamp(sc.modeOpt[0], 0, (int)dect2::GenDemodCount - 1);
    ImGui::SetNextItemWidth(120 * gUi);
    if (ImGui::Combo("Modulation##gsyn", &m, "NFM\0WFM\0AM\0USB\0LSB\0CW\0DSB\0RAW\0CW-L\0")) { sc.modeOpt[0] = m; changed = true; }
    bool quiet = sc.modeOpt[1] == 1;
    if (ImGui::Checkbox("Noise only##gsyn", &quiet)) { sc.modeOpt[1] = quiet ? 1 : 0; changed = true; }
    tip("No station: the squelch must stay closed");
}

// ---- 4.0: the demodulator grid, the shortcuts, Auto, the bar and the VFO menu (docs/GENERAL-4.0.md)
// SAM is the AM demodulator with the synchronous detector (the AM controls offer the same choice); the others are demodulators of their own
struct DemodBtn { const char* label; int demod; int amDet; ImGuiKey key; const char* keyName; const char* tip; };
const DemodBtn kDemodBtns[] = {
    {"NFM", dect2::GenNfm, -1, ImGuiKey_N, "Alt+N", "Narrow FM: two-way radio, PMR, ham repeaters, marine VHF"},
    {"WFM", dect2::GenWfm, -1, ImGuiKey_W, "Alt+W", "Wide FM: broadcast FM, mono (the FM broadcast mode has stereo and RDS)"},
    {"AM", dect2::GenAm, 0, ImGuiKey_A, "Alt+A", "AM: broadcast and airband"},
    {"SAM", dect2::GenAm, 1, ImGuiKey_S, "Alt+S", "Synchronous AM: locks onto the carrier, no distortion when it fades on shortwave"},
    {"DSB", dect2::GenDsb, -1, ImGuiKey_D, "Alt+D", "Both sidebands, no carrier: tune exactly onto the missing carrier (snap off, a fine step)"},
    {"USB", dect2::GenUsb, -1, ImGuiKey_U, "Alt+U", "Upper sideband: ham voice above 10 MHz, utility stations, data modes"},
    {"LSB", dect2::GenLsb, -1, ImGuiKey_L, "Alt+L", "Lower sideband: ham voice below 10 MHz"},
    {"CW", dect2::GenCw, -1, ImGuiKey_C, "Alt+C", "Morse: the beat note rises with the signal (pitch under the options)"},
    {"CW-L", dect2::GenCwl, -1, ImGuiKey_X, "Alt+X", "Morse with the beat note on the other side: it falls as the signal rises (to move a neighbour out of the way)"},
    {"RAW", dect2::GenRaw, -1, ImGuiKey_R, "Alt+R", "No demodulation: the channel's in-phase part is the audio; recordings keep the channel IQ"},
};
constexpr int kNumDemodBtns = (int)(sizeof kDemodBtns / sizeof *kDemodBtns);

bool demodBtnOn(const DemodBtn& b) { return b.demod == S.demod && (b.amDet < 0 || (b.amDet == 1) == (S.amDet == 1)); }
void pickDemod(const DemodBtn& b) {
    setDemod(b.demod);
    if (b.demod == dect2::GenAm) { if (b.amDet == 1) S.amDet = 1; else if (S.amDet == 1) S.amDet = 0; }
    save();
}
const char* demodLabel() { for (const DemodBtn& b : kDemodBtns) if (demodBtnOn(b)) return b.label; return dect2::generalDemodName(S.demod); }

// cols buttons a row (0: as many as fit the width, at least 3); the one in use is filled with the accent
void demodGrid(int cols) {
    const float sp = 3 * gUi, avail = ImGui::GetContentRegionAvail().x;
    if (cols <= 0) cols = std::clamp((int)((avail + sp) / (50 * gUi + sp)), 3, 5);
    const float w = std::floor((avail - (cols - 1) * sp) / cols);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp, sp));
    for (int i = 0; i < kNumDemodBtns; i++) {
        const DemodBtn& b = kDemodBtns[i];
        if (i % cols) ImGui::SameLine();
        const bool on = demodBtnOn(b);
        if (on) { ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.75f)); ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1)); }
        ImGui::PushID(i);
        if (ImGui::Button(b.label, ImVec2(w, 0)) && !on) pickDemod(b);
        ImGui::PopID();
        if (on) ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s  (%s)", b.tip, b.keyName);
    }
    ImGui::PopStyleVar();
}

// the filter presets of the demodulator in use, as buttons (the one in use filled)
void widthPresets() {
    int np = 0;
    const double* pr = dect2::generalWidthPresets(S.demod, np);
    for (int i = 0; i < np; i++) {
        if (i) flowNext(3 * gUi);
        char b[32];
        snprintf(b, sizeof b, "%.4g k##gwp%d", pr[i] / 1e3, i);
        const bool sel = std::fabs(S.width[S.demod] - pr[i]) < 1;
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::SmallButton(b)) { S.width[S.demod] = pr[i]; save(); }
        if (sel) ImGui::PopStyleColor();
    }
    flowEnd();
}

void widthSlider() {
    double lo, hi;
    dect2::generalWidthRange(S.demod, lo, hi);
    float k = (float)(S.width[S.demod] / 1e3);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderFloat("##gbw", &k, (float)(lo / 1e3), (float)(hi / 1e3), "bandwidth %.2f kHz", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
        S.width[S.demod] = std::round(dect2::generalClampWidth(S.demod, k * 1e3) / 10) * 10;
    if (ImGui::IsItemDeactivatedAfterEdit()) save();
    tip("The channel filter (each demodulator keeps its own). Also: drag the edges of the band on the spectrum, or the presets above. Ctrl+click to type a value.");
}

// the choices of the demodulator chip and of the right click on the VFO: the grid and the filter presets
void demodMenuBody() {
    ImGui::TextDisabled("%s", tr("Demodulator"));
    ImGui::Dummy(ImVec2(260 * gUi, 0));
    demodGrid(5);
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("Bandwidth"));
    widthPresets();
    widthSlider();
    if (ImGui::Checkbox(tr("Auto from the band plan##gautom"), &S.autoDemod)) { S.autoLastHz = -1; S.autoBand.clear(); save(); }
}

// Auto (off by default): on a new band (the plan's, else a built-in range), the demodulator and the filter the band is used with. Inside one
// band nothing is changed again, so a demodulator picked by hand stays until the frequency moves to another band.
void autoDemodTick(App& a) {
    if (!S.autoDemod) return;
    const double hz = listenHz(a);
    if (S.autoLastHz >= 0 && std::fabs(hz - S.autoLastHz) < 1) return;
    S.autoLastHz = hz;
    std::string service, band;
    double bw = 0;
    if (!bandPlanServiceAt(a, hz, service, bw, band)) { service = genlayout::builtInService(hz); band = service; bw = 0; }
    if (service.empty()) {   // the shortwave broadcast bands: AM
        size_t n = 0;
        const dect2::SwBand* sw = dect2::swBroadcastBands(n);
        for (size_t i = 0; i < n; i++) { double s0, e0; if (dect2::swBandInRegion(sw[i], stationRegion(), s0, e0) && hz >= s0 * 1e3 && hz <= e0 * 1e3) { service = "am"; band = sw[i].name; bw = dect2::kSwAmWidthHz; } }
    }
    char key[96];
    snprintf(key, sizeof key, "%s|%s", service.c_str(), band.c_str());
    const genlayout::AutoPick pk = genlayout::autoPick(service, hz, bw);
    // ham: the segment matters (CW, SSB, FM parts of one band); the others: one pick per band
    std::string id = key;
    if (service == "ham") id += std::string("|") + pk.why + std::to_string(pk.demod);
    if (id == S.autoBand) return;
    S.autoBand = id;
    if (!pk.ok) { S.autoWhy = band.empty() ? "no band plan entry here: unchanged" : band + ": no rule, unchanged"; return; }
    setDemod(pk.demod);
    S.width[pk.demod] = dect2::generalClampWidth(pk.demod, pk.widthHz);
    char w[96];
    snprintf(w, sizeof w, "%s, %s %.4g kHz", pk.why, dect2::generalDemodName(pk.demod), S.width[pk.demod] / 1e3);
    S.autoWhy = (band.empty() ? std::string() : band + ": ") + w;
    save();
}

const char* shortcutHelp() {
    return "Alt+N NFM, Alt+W WFM, Alt+A AM, Alt+S SAM, Alt+D DSB, Alt+U USB, Alt+L LSB, Alt+C CW, Alt+X CW-L, Alt+R RAW (Option on a Mac).\n"
           "Ctrl+B (Cmd+B): show or hide the sidebar. The wheel over the VFO band steps it; elsewhere it zooms around the mouse.";
}

void shortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || !io.KeyAlt || io.KeyCtrl || io.KeySuper || io.KeyShift) return;
    for (const DemodBtn& b : kDemodBtns) if (ImGui::IsKeyPressed(b.key, false)) { if (!demodBtnOn(b)) pickDemod(b); return; }
}

// the S-meter, compact: a bar of w pixels, the SNR and the squelch state after it
void levelCompact(const App& a, float w) {
    const auto& t = T(a);
    const bool on = live(a) && t.levelDb > -199;
    const float h = ImGui::GetFrameHeight() * 0.55f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p(p0.x, p0.y + (ImGui::GetFrameHeight() - h) * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(26, 30, 36, 255), 2.f);
    auto xOf = [&](float db) { return p.x + w * std::clamp((db + 130.f) / 130.f, 0.f, 1.f); };
    if (on) {
        dl->AddRectFilled(p, ImVec2(xOf(t.levelDb), p.y + h), t.squelchOpen ? IM_COL32(90, 200, 120, 255) : IM_COL32(110, 130, 150, 255), 2.f);
        if (t.noiseDb > -199) dl->AddLine(ImVec2(xOf(t.noiseDb), p.y), ImVec2(xOf(t.noiseDb), p.y + h), IM_COL32(200, 200, 200, 140), 1.f);
        if (t.squelchDb > -199) dl->AddLine(ImVec2(xOf(t.squelchDb), p.y - 2), ImVec2(xOf(t.squelchDb), p.y + h + 2), IM_COL32(240, 170, 60, 255), 2.f);
    }
    ImGui::Dummy(ImVec2(w, ImGui::GetFrameHeight()));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Channel level, -130 to 0 dBFS (after the channel filter).\nGrey line: the noise in the channel; amber: the squelch threshold.");
    ImGui::SameLine(0, 6 * gUi);
    ImGui::AlignTextToFramePadding();
    if (on) {
        if (t.dataValid) ImGui::Text("SNR %.0f dB", t.snrDb); else ImGui::Text("%.0f dBFS", t.levelDb);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.1f dBFS in the channel, %.1f dB over its noise", t.levelDb, t.snrDb);
        ImGui::SameLine(0, 6 * gUi);
        ImGui::TextColored(!S.sqOn ? ImVec4(0.6f, 0.6f, 0.6f, 1) : t.squelchOpen ? ImVec4(0.45f, 0.85f, 0.55f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "%s", !S.sqOn ? "sq off" : t.squelchOpen ? "open" : "closed");
    } else ImGui::TextDisabled("%s", a.engine.running() ? "no level yet" : "stopped");
}

// the VFO menu (the right click on the VFO band): opened by generalPlotMouse, drawn in the same ID scope each frame
int gVfoMenuFrame = -1000;

}  // namespace (4.0 pieces; the sidebar sections and the bar follow, outside it)

// ---- the sidebar sections of the 4.0 screen (general/general_panels.h)
void generalSideRadio(App& a) {
    load();
    gSide = true;
    demodGrid(0);
    if (ImGui::Checkbox(tr("Auto from the band plan"), &S.autoDemod)) { S.autoLastHz = -1; S.autoBand.clear(); save(); }
    tip("Off by default. On: when the frequency moves to another band, the demodulator and the filter are set for it from the band plan (FM broadcast: WFM; AM and airband: AM; PMR and marine: NFM; amateur bands: CW, LSB, USB or FM by the segment). Inside one band a demodulator you pick yourself stays.");
    if (S.autoDemod && !S.autoWhy.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", S.autoWhy.c_str()); ImGui::PopTextWrapPos(); }
    ImGui::Spacing();
    widthPresets();
    widthSlider();
    squelchUi(a);
    demodOptionsUi(a);
    ImGui::Spacing();
    ImGui::TextDisabled("(?) %s", tr("shortcuts"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", shortcutHelp());
    gSide = false;
}

void generalSideAudio(App& a) {
    load();
    gSide = true;
    audioFiltersUi(a);
    noiseRemoverUi(a);
    gSide = false;
}

void generalSideRecording(App& a) {
    load();
    gSide = true;
    recordingUi(a);
    flowEnd();
    gSide = false;
}

void generalSideBookmarks(App& a) {
    load();
    gSide = true;
    if (ImGui::SmallButton(tr("Memory..."))) a.memoryOpen = true;
    tip("The frequencies you keep, with the demodulator, filter and squelch (Ctrl+M)");
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Frequencies..."))) a.freqOpen = true;
    tip("Known frequencies of every mode (Ctrl+Shift+F)");
    hamVoiceUi(a);
    if (ImGui::TreeNodeEx(trw("Broadcast bands (AM)"), amBroadcast(a) ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        ImGui::PushTextWrapPos(0);
        broadcastPresetsUi(a);
        ImGui::PopTextWrapPos();
        ImGui::TreePop();
    }
    gSide = false;
}

void generalSideSignals(App& a) {
    load();
    gSide = true;
    ImGui::PushTextWrapPos(0);
    guessUi(a);
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    generalAutoListBody(a);
    gSide = false;
}

void generalSideChannels(App& a) {
    load();
    gSide = true;
    ImGui::PushTextWrapPos(0);
    extraChannelsUi(a);
    ImGui::PopTextWrapPos();
    gSide = false;
}

float generalDemodChip(App& a) {
    load();
    (void)a;
    char b[48];
    snprintf(b, sizeof b, "%s %.4g k##gdchip", demodLabel(), S.width[S.demod] / 1e3);
    ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.35f));
    if (ImGui::Button(b)) ImGui::OpenPopup("##gdchippop");
    ImGui::PopStyleColor();
    const float right = ImGui::GetItemRectMax().x;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", tr("The demodulator and the filter: click to change them (also in the sidebar, and a right click on the VFO band)"), shortcutHelp());
    if (ImGui::BeginPopup("##gdchippop")) { demodMenuBody(); ImGui::EndPopup(); }
    return right;
}

void generalShortcuts(App& a) { (void)a; load(); shortcuts(); }
const char* generalShortcutHelp() { return shortcutHelp(); }

void generalBar(App& a) {
    load();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("step"));
    ImGui::SameLine(0, 4 * gUi);
    if (freqStepArrow("##gbdn", ImGuiDir_Left)) setListen(a, genlayout::snapHz(listenHz(a), stepHz()) - stepHz(), a.centreTuning);
    tip("Down one step (hold: continuous; also the wheel over the VFO band)");
    ImGui::SameLine(0, 2 * gUi);
    ImGui::SetNextItemWidth(86 * gUi);
    if (ImGui::Combo("##gbstep", &S.step[S.demod], kStepNames, kNumSteps)) save();
    tip("The tuning step of this demodulator: the arrows and the wheel over the VFO move by it; with Snap on, clicks and drags land on it");
    ImGui::SameLine(0, 2 * gUi);
    if (freqStepArrow("##gbup", ImGuiDir_Right)) setListen(a, genlayout::snapHz(listenHz(a), stepHz()) + stepHz(), a.centreTuning);
    tip("Up one step");
    ImGui::SameLine(0, 8 * gUi);
    if (ImGui::Checkbox(tr("Snap"), &S.snapOn)) save();
    tip("Clicks and drags of the VFO land on a multiple of the step. Off: exactly where the mouse is (DSB, a station off the raster).");
    ImGui::SameLine(0, 8 * gUi);
    if (ImGui::SmallButton(tr("Centre"))) { S.chanOff = 0; tuneFreq(a, snap(listenHz(a)) / 1e6); S.lastDial = a.freqMhz; save(); }
    tip("Tune the radio so the VFO sits in the middle of the captured band");
    const float meterW = std::min(170 * gUi, std::max(0.f, ImGui::GetContentRegionAvail().x - 190 * gUi));
    if (meterW > 50 * gUi) {
        ImGui::SameLine(0, 14 * gUi);
        levelCompact(a, meterW);
    }
}

bool generalVfoMenuTaken() { return gVfoMenuFrame >= ImGui::GetFrameCount() - 1; }

namespace {

} // namespace


// ---- the Memory panel (app/memory): the listening frequency and the options a memory keeps for this mode
double generalListenHz(const App& a) { load(); return listenHz(a); }
// the big readout: the VFO inside the band, the radio only past its edge; with the target button on the radio centres on it, as in the other modes (#301)
void generalSetListenHz(App& a, double hz) { load(); setListen(a, hz, a.centreTuning); }
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
                        [](App&, const std::string& v) { load(); setDemod(std::clamp((int)numV(v, S.demod), 0, (int)dect2::GenDemodCount - 1)); save(); }});
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
    ImGui::Dummy(ImVec2(280 * gUi, 0));
    demodGrid(5);
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

bool generalDragging(int plotId) { return (S.drag >= 0 && S.dragPlot == plotId) || S.vfoGrab == plotId; }

// Drag to tune (Settings > Radio > Drag on spectrum, gfx/drag_tune.h): the press puts the channel there, the held drag keeps moving it
static struct { int plot = -1; bool moved = false; double last = NAN, press = NAN; ImVec2 p0; } gGDT;

void generalPlotMouse(App& a, int plotId) {
    load();
    if (gGDT.plot == plotId) {
        ImGuiIO& io = ImGui::GetIO();
        if (!ImGui::IsMouseDown(0)) {   // let go: the target style centres the radio on a press that did not move (#301)
            if (!gGDT.moved && a.centreTuning && !std::isnan(gGDT.press)) setListen(a, gGDT.press, true);
            gGDT.plot = -1;
            return;
        }
        if (!gGDT.moved && dragtune::moved(io.MousePos.x - gGDT.p0.x, io.MousePos.y - gGDT.p0.y)) gGDT.moved = true;
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (!gGDT.moved) return;
        const plt::Rect lim = plt::GetPlotLimits();
        const double hzPerPx = (lim.X.Max - lim.X.Min) * 1e6 / std::max(1.f, plt::GetPlotSize().x);
        const double step = dragtune::dragStepHz(stepHz(), stepHz(), hzPerPx, io.KeyShift || io.KeyCtrl);
        const double x = std::min(lim.X.Max, std::max(lim.X.Min, plt::GetPlotMousePos().x));
        const double to = dragtune::snapHz(x * 1e6, step);
        if (dragtune::changed(gGDT.last, to)) { setListen(a, to, false); gGDT.last = to; }
        ImGui::SetTooltip("%.4f MHz", to / 1e6);
        return;
    }
    const bool hov = plt::IsPlotHovered();
    if (!hov && !(S.drag >= 0 && S.dragPlot == plotId) && S.exDrag < 0 && S.vfoGrab != plotId) return;
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
    const float grab = 5 * gUi;
    // 4.0: the VFO body held: it moves with the mouse and keeps where it was grabbed (snapped to the step when Snap is on)
    if (S.vfoGrab == plotId) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsMouseDown(0)) {
            const double to = genlayout::dragCentreHz(mhz * 1e6, S.vfoGrabOff, stepHz(), S.snapOn);
            if (std::fabs(to - listenHz(a)) > 0.5) setListen(a, to, false);
            ImGui::SetTooltip("%.4f MHz", listenHz(a) / 1e6);
        } else { S.vfoGrab = -1; S.dragged = true; save(); }
        return;
    }
    const genlayout::Hit hit = hov ? genlayout::hitTest((float)mx, px(el), px(eh), px(f), grab, canLo, canHi) : genlayout::Hit::None;
    const int edgeNear = hit == genlayout::Hit::EdgeLo ? 0 : hit == genlayout::Hit::EdgeHi ? 1 : -1;
    if (S.drag >= 0 && S.dragPlot == plotId) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsMouseDown(0)) {
            const double d = (mhz - f) * 1e6;
            const double w = genlayout::edgeWidthHz(S.demod == dect2::GenUsb ? 1 : S.demod == dect2::GenLsb ? 2 : 0, d, dect2::kGeneralSsbLowCutHz);
            S.width[S.demod] = std::round(dect2::generalClampWidth(S.demod, w) / 10) * 10;
            ImGui::SetTooltip("filter %.4g kHz", S.width[S.demod] / 1e3);
        } else { S.drag = -1; S.dragPlot = -1; S.dragged = true; save(); }
        return;
    }
    if (!hov) return;
    // a right click on the VFO band: its menu (the demodulators and the filter presets), instead of the spectrum's own
    if (hit != genlayout::Hit::None && dragtune::menuOpens(specDragTunes(a) ? dragtune::Tune : dragtune::Pan, ImGui::IsMouseClicked(ImGuiMouseButton_Right),
                                                           ImGui::IsMouseReleased(ImGuiMouseButton_Right), std::sqrt(ImGui::GetIO().MouseDragMaxDistanceSqr[ImGuiMouseButton_Right]))) {
        gVfoMenuFrame = ImGui::GetFrameCount();
        return;
    }
    if (edgeNear >= 0) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsMouseClicked(0)) { S.drag = edgeNear; S.dragPlot = plotId; return; }
        ImGui::SetTooltip("drag to change the filter (%.4g kHz)", S.width[S.demod] / 1e3);
        return;
    }
    if (hit == genlayout::Hit::Body && !a.identifyTool && !ImGui::GetIO().KeyShift && !plt::PressOnScale() && !touch::on()) {   // grab the VFO
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsMouseClicked(0)) { S.vfoGrab = plotId; S.vfoGrabOff = mhz * 1e6 - listenHz(a); S.dragged = false; return; }
        ImGui::SetTooltip("%s %.4g kHz: drag to tune, wheel: %s steps, right click: demodulator and filter", demodLabel(), S.width[S.demod] / 1e3, kStepNames[S.step[S.demod]]);
        return;
    }
    if (ImGui::IsMouseClicked(0)) S.dragged = false;
    const double to = snap(mhz * 1e6);
    if (specDragTunes(a) && !touch::on()) {   // drag to tune: the press tunes (the target style waits for the release), a drag goes on tuning
        if (ImGui::IsMouseClicked(0) && !plt::PressOnScale() && !ImGui::GetIO().KeyShift && !a.identifyTool) {
            gGDT.plot = plotId; gGDT.moved = false; gGDT.p0 = ImGui::GetIO().MousePos; gGDT.press = to; gGDT.last = to;
            if (!a.centreTuning) setListen(a, to, false);
            return;
        }
        if (ImGui::IsMouseReleased(0)) return;   // a release here is the end of a pan of the scale, not a click
    }
    const ImVec2 dd = ImGui::GetMouseDragDelta(0, 0);
    if (ImGui::IsMouseReleased(0) && !S.dragged && std::fabs(dd.x) < 4 && std::fabs(dd.y) < 4) { setListen(a, to, a.centreTuning); return; }
    if (std::fabs(to - listenHz(a)) > 0.5) {
        const double lx[2] = {to / 1e6, to / 1e6}, ly[2] = {lim.Y.Min, lim.Y.Max};
        plt::Spec ls; ls.LineColor = ImVec4(0.95f, 0.25f, 0.25f, 0.5f); ls.LineWeight = 1.f;
        plt::PlotLine("##gtunehere", lx, ly, 2, ls);
        ImGui::SetTooltip("click to listen on %.4f MHz", to / 1e6);
    }
}

// The mouse wheel (View > Controls: Tune by step) in the general receiver: moves the listening channel by n steps (#409)
void generalWheelTune(App& a, int n, double stepHzOverride) {
    load();
    const double st = stepHzOverride > 0 ? stepHzOverride : stepHz();
    setListen(a, std::round(listenHz(a) / st) * st + n * st, a.centreTuning);
}

// 4.0: the wheel over the VFO band steps the VFO by the step (elsewhere the wheel zooms, as View > Controls says). Before BeginPlot, so the
// plot does not zoom with the same notch. A band wider than 40 % of the plot leaves the wheel to the zoom (a WFM band zoomed in).
void generalVfoWheel(App& a, ImVec2 p0, ImVec2 sz, double loMhz, double hiMhz) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseWheel == 0 || io.KeyCtrl || io.KeyShift || io.KeyAlt || io.KeySuper || !(hiMhz > loMhz) || sz.x < 10) return;
    if (!ImGui::IsMouseHoveringRect(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), false) || !ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) return;
    if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) || ImGui::IsAnyItemActive()) return;
    load();
    double lo, hi;
    edges(lo, hi);
    const double f = listenHz(a) / 1e6;
    auto px = [&](double mhz) { return p0.x + (float)((mhz - loMhz) / (hiMhz - loMhz) * sz.x); };
    const float xl = px(f + lo / 1e6), xh = px(f + hi / 1e6);
    if (xh - xl > 0.4f * sz.x) return;
    if (genlayout::hitTest(io.MousePos.x, xl, xh, px(f), 5 * gUi, true, true) != genlayout::Hit::Body &&
        !(io.MousePos.x > xl - 5 * gUi && io.MousePos.x < xh + 5 * gUi)) return;
    static float acc = 0;
    acc += io.MouseWheel;
    const int n = (int)acc;
    acc -= (float)n;
    io.MouseWheel = 0;
    if (n) setListen(a, genlayout::wheelTuneHz(listenHz(a), n, stepHz()), a.centreTuning);
}

// the VFO menu, once a frame after the plots (spectrumWaterfall): opened by a right click on the VFO band (generalPlotMouse)
void generalVfoMenu(App& a) {
    (void)a;
    if (gVfoMenuFrame == ImGui::GetFrameCount()) ImGui::OpenPopup("##gvfomenu");
    if (ImGui::BeginPopup("##gvfomenu")) { load(); demodMenuBody(); ImGui::EndPopup(); }
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
