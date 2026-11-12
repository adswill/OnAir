// "Audio" (core/include/dect2/audio/audio_control.h, docs/AUDIO.md): one compact pop-up, reachable from every mode, with everything that shapes the
// sound you hear: volume (remembered per mode), squelch (level / SNR / CTCSS / DCS, hysteresis, hang, tail), high-pass / low-pass, 5-band EQ with
// presets, notch (manual and automatic), AGC, de-emphasis, stereo, clarifier / CW pitch, output device, latency, recorder, mute / solo of the
// receivers, a level meter with a clip light and the audio spectrum + waterfall strip (click it to put the notch there).
// The settings of a mode are saved in the preferences as one string, "acl.<family>"; the decoders always get the untouched sound.
#include "../app.h"
#include "state/presets_app.h"
#include "i18n/i18n.h"
#include "platform/platform.h"
#include "dect2/audio/audio_control.h"
#include "dect2/audio/audio_rec.h"
#include "dect2/audio/audioout.h"
#include "dect2/source/iq_record.h"
#include "ui/icons.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using dect2::AudioControlConfig;
using namespace dect2::actl;

namespace {

constexpr int kRows = 36, kCols = AudioControlConfig::kAfBins / 2;

struct State {
    int curFamily = -1;
    AudioControlSettings s;
    bool loaded = false;
    float savedVol = -1; bool savedMute = false; double lastSave = 0;
    std::vector<std::string> devs;
    double clipSeenAt = -100; unsigned lastClips = 0;
    float wf[kRows][kCols] = {};
    int wfRow = 0; double wfT = 0;
    bool recGateOpenOnly = false;
};
State S;

std::string keyFor(int family) { return "acl." + std::to_string(family); }

void push(App& a) {   // volume and mute reach the engines the way the main slider does it
    a.engine.player().setVolume(a.volume); a.engine.dabAudio().setVolume(a.volume); a.engine.setWideVolume(a.volume);
    a.engine.player().setMuted(a.muted); a.engine.dabAudio().setMuted(a.muted);
}

void applyConfig() {
    AudioControlConfig::global().set(S.s);
    if (S.devs.empty()) S.devs = dect2::AudioOut::devices();
    // a mode without its own device uses Settings > Audio > Output device (empty: the system's)
    dect2::AudioOut::setDefaultDevice(audioPickDevice(S.devs, S.s.device.empty() ? plat::prefs().getS("audioDevDefault", "") : S.s.device));
}

// Settings > Audio: what a mode starts with the first time it is used (AGC, squelch); each mode keeps its own after that
void newModeDefaults(AudioControlSettings& s) {
    const long agc = plat::prefs().getI("aclDefAgc", 0), sq = plat::prefs().getI("aclDefSq", 0);
    if (agc >= 1 && agc <= 2) { s.agc = (dect2::actl::AgcMode)agc; s.on = true; }
    if (sq >= 1 && sq <= 2) { s.sqKind = (dect2::actl::SquelchKind)sq; s.sqOpenDb = (float)plat::prefs().getD("aclDefSqDb", -50); s.on = true; }
}

void savePrefs(int family) { plat::prefs().setS(keyFor(family).c_str(), audioControlToString(S.s)); }

void loadFamily(App& a) {
    S.curFamily = a.family;
    const AudioControlSettings d = audioControlDefaults(audioCleanupProfileOf(a));
    const std::string str = plat::prefs().getS(keyFor(a.family).c_str(), "");
    const bool had = !str.empty();
    AudioControlSettings s = had ? audioControlFromString(str, d) : d;
    if (!had) { s.volume = a.volume; s.muted = a.muted; newModeDefaults(s); }   // a mode never visited starts at the volume you have now
    S.s = s;
    if (had) { a.volume = s.volume; a.muted = s.muted; push(a); }   // this mode's own volume
    S.savedVol = a.volume; S.savedMute = a.muted;
    S.devs = dect2::AudioOut::devices();
    applyConfig();
}

bool slider(const char* id, float& v, float lo, float hi, const char* fmt, float w = 130) {
    ImGui::SetNextItemWidth(w * gUi);
    return ImGui::SliderFloat(id, &v, lo, hi, fmt);
}
bool sw(const char* label, bool& v, const char* tip = nullptr) {
    const bool c = ImGui::Checkbox(label, &v);
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    return c;
}
bool combo(const char* id, int& v, const char* const* names, int n, float w = 110) {
    bool c = false;
    ImGui::SetNextItemWidth(w * gUi);
    if (ImGui::BeginCombo(id, tr(names[v]))) {
        for (int i = 0; i < n; i++) if (ImGui::Selectable(tr(names[i]), i == v)) { v = i; c = true; }
        ImGui::EndCombo();
    }
    return c;
}

// the AF strip: spectrum line on top, waterfall under it; a click puts the notch on that frequency
bool afStrip(float w, AudioControlSettings& s) {
    AudioControlConfig& cfg = AudioControlConfig::global();
    cfg.afWanted.store(true);
    const int rate = std::max(8000, cfg.afRate.load());
    const float nyq = rate * 0.5f, maxHz = std::min(nyq, 8000.f);
    const int cols = std::min(AudioControlConfig::kAfBins, (int)(AudioControlConfig::kAfBins * maxHz / nyq));
    const double now = ImGui::GetTime();
    if (now - S.wfT > 0.08) {   // a new waterfall row, 12 per second
        S.wfT = now;
        for (int c = 0; c < kCols; c++) S.wf[S.wfRow][c] = std::max(cfg.afDb[2 * c].load(), cfg.afDb[2 * c + 1].load());
        S.wfRow = (S.wfRow + 1) % kRows;
    }
    const float hSpec = 54 * gUi, hWf = 44 * gUi;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##afstrip", ImVec2(w, hSpec + hWf));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + hSpec + hWf), IM_COL32(14, 18, 24, 255), 3 * gUi);
    auto xOf = [&](float hz) { return p.x + w * hz / maxHz; };
    auto yOf = [&](float db) { return p.y + hSpec * (1.f - std::min(1.f, std::max(0.f, (db + 100.f) / 100.f))); };
    ImVec2 prev(p.x, p.y + hSpec);
    for (int b = 0; b < cols; b++) {
        const ImVec2 q(p.x + w * (b + 0.5f) / cols, yOf(cfg.afDb[b].load()));
        if (b) dl->AddLine(prev, q, IM_COL32(120, 200, 150, 255), 1.2f);
        prev = q;
    }
    for (int row = 0; row < kRows; row++) {
        const int r = (S.wfRow + row) % kRows;   // oldest first, newest at the bottom
        const float y0 = p.y + hSpec + hWf * row / kRows, y1 = p.y + hSpec + hWf * (row + 1) / kRows;
        for (int c = 0; c < kCols && c * 2 < cols; c++) {
            const float t = std::min(1.f, std::max(0.f, (S.wf[r][c] + 95.f) / 75.f));
            const int g = (int)(t * 255);
            dl->AddRectFilled(ImVec2(p.x + w * c * 2 / (float)cols, y0), ImVec2(p.x + w * (c * 2 + 2) / (float)cols, y1), IM_COL32(g / 3, g, g / 2 + 40 * (t > 0.05f), 255));
        }
    }
    for (int k = 1; k < 8; k++) { dl->AddLine(ImVec2(xOf(k * 1000.f), p.y + hSpec - 4 * gUi), ImVec2(xOf(k * 1000.f), p.y + hSpec), IM_COL32(150, 160, 170, 255)); }
    if (s.notch) dl->AddLine(ImVec2(xOf(s.notchHz), p.y), ImVec2(xOf(s.notchHz), p.y + hSpec + hWf), IM_COL32(255, 170, 60, 220), 1.5f);
    const float an = cfg.statNotchHz.load();
    if (s.autoNotch && an > 0) dl->AddLine(ImVec2(xOf(an), p.y), ImVec2(xOf(an), p.y + hSpec + hWf), IM_COL32(120, 200, 255, 220), 1.5f);
    bool changed = false;
    if (ImGui::IsItemHovered()) {
        const float hz = std::min(maxHz, std::max(0.f, (ImGui::GetIO().MousePos.x - p.x) / w * maxHz));
        ImGui::SetTooltip(tr("%.0f Hz  (click: put the notch here)"), hz);
        if (ImGui::IsItemClicked()) { s.notchHz = std::max(50.f, hz); s.notch = true; changed = true; }
    }
    return changed;
}

void meter(float w) {
    AudioControlConfig& cfg = AudioControlConfig::global();
    const float pk = cfg.statPeakDb.load(), rms = cfg.statRmsDb.load();
    const unsigned clips = cfg.statClips.load();
    const double now = ImGui::GetTime();
    if (clips != S.lastClips) { S.lastClips = clips; S.clipSeenAt = now; }
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = 9 * gUi;
    ImGui::Dummy(ImVec2(w, h + 2 * gUi));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float lw = w - 34 * gUi;
    dl->AddRectFilled(p, ImVec2(p.x + lw, p.y + h), IM_COL32(30, 34, 40, 255), 2 * gUi);
    auto fx = [&](float db) { return p.x + lw * std::min(1.f, std::max(0.f, (db + 60.f) / 60.f)); };
    dl->AddRectFilled(p, ImVec2(fx(rms), p.y + h), IM_COL32(90, 170, 120, 255), 2 * gUi);
    dl->AddLine(ImVec2(fx(pk), p.y), ImVec2(fx(pk), p.y + h), IM_COL32(255, 255, 255, 230), 2.f);
    const bool clipping = now - S.clipSeenAt < 2.0;
    dl->AddRectFilled(ImVec2(p.x + lw + 4 * gUi, p.y), ImVec2(p.x + w, p.y + h), clipping ? IM_COL32(220, 50, 40, 255) : IM_COL32(60, 40, 40, 255), 2 * gUi);
    dl->AddText(ImVec2(p.x + lw + 8 * gUi, p.y - 3 * gUi), IM_COL32(255, 255, 255, 200), "CLIP");
}

bool recordButton(App& a, AudioControlSettings& s) {
    dect2::AudioRecorder& rec = dect2::AudioRecorder::instance();
    const dect2::AudioRecStats rs = rec.stats();
    bool changed = false;
    if (rs.active) {
        char b[96];
        snprintf(b, sizeof b, "%s  %.1f s  %.1f MB###acrecstop", tr("Stop recording"), rs.seconds, rs.bytes / 1e6);
        ImGui::PushStyleColor(ImGuiCol_Text, pal::badRed());
        if (ImGui::SmallButton(b)) rec.stop();
        ImGui::PopStyleColor();
    } else {
        ImGui::BeginDisabled(!a.engine.running());
        kit::recordingsButton("ac"); ImGui::SameLine();
        if (ImGui::SmallButton(tr("Record audio (WAV)"))) {
            dect2::AudioRecOptions o;
            o.format = dect2::AudioRecFormat::Wav;
            const int m = a.engine.standardMode();
            char tag[96]; snprintf(tag, sizeof tag, "OnAir %s %.3f MHz", dect2::recordingModeId(m).c_str(), a.freqMhz);
            o.tag = tag;
            const std::string path = recordingsDir() + "/" + dect2::AudioRecorder::fileName(dect2::recordingModeId(m), a.freqMhz * 1e6, o.format);
            std::string err;
            if (!rec.start(path, o, err)) a.engine.log("audio recording failed: " + err);
        }
        ImGui::EndDisabled();
    }
    ImGui::SameLine(0, 8 * gUi);
    changed |= sw(tr("only while the squelch is open"), s.recordOnlyOpen, tr("Nothing is written to the recording while the squelch holds the sound off."));
    return changed;
}

const char* const kStereoN[] = {N_("Stereo"), N_("Mono"), N_("Swap L/R"), N_("Left only"), N_("Right only")};
const char* const kDeempN[] = {N_("Off"), N_("50 us"), N_("75 us")};
const char* const kAgcN[] = {N_("Off"), N_("Fast"), N_("Slow")};
const char* const kSqN[] = {N_("Off"), N_("Level"), N_("SNR (noise)"), N_("CTCSS tone"), N_("DCS code")};
const char* const kEqN[] = {N_("Flat"), N_("Voice"), N_("Music"), N_("CW"), N_("Custom")};

bool gDevOpen = false;

}  // namespace

namespace {
// The per-mode presets (app/state/presets_app.h) save and apply the audio settings of the mode on screen: the "audio.*" keys of
// dect2::presets::settingSpecs(). A filter switched off reads as 0 Hz; a preset with 0 Hz switches it off.
void registerPresetProviders() {
    using dect2::presets::Value;
    auto ready = [](App& a) { if (S.curFamily != a.family || !S.loaded) { S.loaded = true; loadFamily(a); } };
    auto changed = [](App& a) { a.volume = S.s.volume; a.muted = S.s.muted; push(a); applyConfig(); savePrefs(a.family); S.savedVol = a.volume; S.savedMute = a.muted; };
    auto num = [&](const char* key, float AudioControlSettings::*f) {
        presetsapp::registerProvider({key, [=](App& a, Value& v) { ready(a); v = Value::n(S.s.*f); return true; },
                                      [=](App& a, const Value& v) { ready(a); S.s.*f = (float)v.num; changed(a); }});
    };
    num("audio.volume", &AudioControlSettings::volume);
    num("audio.sqOpenDb", &AudioControlSettings::sqOpenDb);
    num("audio.sqHystDb", &AudioControlSettings::sqHystDb);
    num("audio.sqHangMs", &AudioControlSettings::sqHangMs);
    num("audio.shiftHz", &AudioControlSettings::shiftHz);
    num("audio.ctcssHz", &AudioControlSettings::ctcssHz);
    presetsapp::registerProvider({"audio.muted", [=](App& a, Value& v) { ready(a); v = Value::f(S.s.muted); return true; },
                                  [=](App& a, const Value& v) { ready(a); S.s.muted = v.flag; changed(a); }});
    auto filt = [&](const char* key, bool AudioControlSettings::*on, float AudioControlSettings::*hz) {
        presetsapp::registerProvider({key, [=](App& a, Value& v) { ready(a); v = Value::n(S.s.*on ? S.s.*hz : 0.f); return true; },
                                      [=](App& a, const Value& v) { ready(a); S.s.*on = v.num > 0; if (v.num > 0) S.s.*hz = (float)v.num; changed(a); }});
    };
    filt("audio.hpHz", &AudioControlSettings::hp, &AudioControlSettings::hpHz);
    filt("audio.lpHz", &AudioControlSettings::lp, &AudioControlSettings::lpHz);
}
}  // namespace

void audioControlTick(App& a) {
    static const bool registered = (registerPresetProviders(), true);
    (void)registered;
    if (&a != receiverHeard()) return;   // the heard receiver drives the chain
    if (S.curFamily != a.family || !S.loaded) { S.loaded = true; loadFamily(a); }
    // the volume and mute of this mode are remembered (written at most twice a second)
    const double now = ImGui::GetTime();
    if ((a.volume != S.savedVol || a.muted != S.savedMute) && now - S.lastSave > 0.5) {
        S.lastSave = now; S.savedVol = a.volume; S.savedMute = a.muted;
        S.s.volume = a.volume; S.s.muted = a.muted;
        savePrefs(a.family);
        AudioControlConfig::global().set(S.s);
    }
    // keyboard: Alt+Up / Alt+Down volume, Alt+M mute (not while typing)
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput && io.KeyAlt && !io.KeyCtrl && !io.KeySuper) {
        bool ch = false;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) { a.volume = std::min(1.f, a.volume + 0.05f); ch = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) { a.volume = std::max(0.f, a.volume - 0.05f); ch = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_M, false)) { a.muted = !a.muted; ch = true; }
        if (ch) push(a);
    }
}


bool audioControlButton(App& a) {
    if (!S.loaded || S.curFamily != a.family) { if (&a == receiverHeard()) audioControlTick(a); }
    AudioControlSettings& s = S.s;
    bool open = false;
    ImGui::PushID("audioctl");
    if (iconFlat(Ic::Wave, tr("Audio: squelch, filters, EQ, notch, AGC, output ...  (Alt+Up / Alt+Down: volume, Alt+M: mute)"), s.on)) ImGui::OpenPopup("##acpop2");
    if (gDevOpen && ImGui::GetFrameCount() > 40) { gDevOpen = false; ImGui::OpenPopup("##acpop2"); }
    const ImVec2 bmax = ImGui::GetItemRectMax();
    bool changed = false;
    ImGui::SetNextWindowPos(ImVec2(bmax.x, bmax.y + 2 * gUi), ImGuiCond_Appearing, ImVec2(1.f, 0.f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(430 * gUi, 0), ImVec2(560 * gUi, 760 * gUi));
    if (ImGui::BeginPopup("##acpop2")) {
        open = true;
        bool c = false;
        ImGui::TextUnformatted(tr("Audio"));
        ImGui::SameLine(0, 10 * gUi);
        ImGui::TextDisabled("%s", tr("settings are kept for this mode"));
        // volume, mute, meter, spectrum
        if (iconFlat(a.muted ? Ic::Mute : Ic::Speaker, tr(a.muted ? "Unmute" : "Mute"))) { a.muted = !a.muted; push(a); }
        ImGui::SameLine(0, 4 * gUi);
        float vol = a.volume * 100.f;
        if (slider("##acvol", vol, 0, 100, "%.0f %%", 200)) { a.volume = vol / 100.f; push(a); }
        ImGui::SameLine(0, 8 * gUi);
        c |= sw(tr("Audio control on"), s.on, tr("Switches the whole chain below. Off: the sound is exactly as the mode makes it."));
        meter(ImGui::GetContentRegionAvail().x);
        c |= afStrip(ImGui::GetContentRegionAvail().x, s);
        c |= recordButton(a, s);
        ImGui::Separator();
        if (ImGui::BeginTabBar("##acpt")) {
            if (ImGui::BeginTabItem(trw("Squelch"))) {
                int k = (int)s.sqKind;
                ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(tr("Type")); ImGui::SameLine();
                if (combo("##sqk", k, kSqN, 5, 140)) {
                    s.sqKind = (SquelchKind)k;
                    if (k == (int)SquelchKind::Level) s.sqOpenDb = -50.f; else if (k == (int)SquelchKind::Snr) s.sqOpenDb = 8.f;
                    if (k == (int)SquelchKind::Ctcss && s.ctcssHz <= 0) s.ctcssHz = 88.5f;
                    c = true;
                }
                if (s.sqKind == SquelchKind::Level) c |= slider("##sqo", s.sqOpenDb, -90, -10, tr("opens at %.0f dBFS"), 190);
                if (s.sqKind == SquelchKind::Snr) c |= slider("##sqo", s.sqOpenDb, 0, 30, tr("opens at %.0f dB over the noise"), 220);
                if (s.sqKind == SquelchKind::Level || s.sqKind == SquelchKind::Snr) {
                    c |= slider("##sqh", s.sqHystDb, 0, 15, tr("closes %.0f dB lower (hysteresis)"), 220);
                }
                if (s.sqKind == SquelchKind::Ctcss) {
                    ImGui::SetNextItemWidth(120 * gUi);
                    char lbl[32]; snprintf(lbl, sizeof lbl, "%.1f Hz", s.ctcssHz);
                    if (ImGui::BeginCombo("##ctcss", lbl)) {
                        for (float f : kCtcssHz) { char b[32]; snprintf(b, sizeof b, "%.1f Hz", f); if (ImGui::Selectable(b, f == s.ctcssHz)) { s.ctcssHz = f; c = true; } }
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine(); ImGui::TextDisabled("%s", tr("opens only while this sub-audible tone is present (about 0.2 s)"));
                }
                if (s.sqKind == SquelchKind::Dcs) {
                    int code = s.dcsCode; ImGui::SetNextItemWidth(90 * gUi);
                    if (ImGui::InputInt("##dcs", &code, 0, 0)) { s.dcsCode = std::max(0, std::min(777, code)); c = true; }
                    ImGui::SameLine(); ImGui::TextDisabled("%s", tr("octal code, for example 023"));
                }
                if (s.sqKind != SquelchKind::Off) {
                    c |= slider("##sqg", s.sqHangMs, 0, 2000, tr("hang %.0f ms"), 150);
                    c |= slider("##sqt", s.sqTailMs, 0, 300, tr("tail elimination %.0f ms"), 190);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Delays the sound a little so the burst of noise at the end of a transmission is cut off before it is heard."));
                    const AudioControlConfig& g = AudioControlConfig::global();
                    ImGui::TextDisabled(g.statSqOpen.load() ? tr("open  (measure %.0f)") : tr("closed  (measure %.0f)"), g.statSqMeasure.load());
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(trw("Tone"))) {
                c |= sw(tr("High-pass"), s.hp); ImGui::SameLine();
                c |= slider("##hpf", s.hpHz, 20, 2000, "%.0f Hz", 130); ImGui::SameLine();
                { int sl = s.hpSlope == 24 ? 1 : 0; const char* const n[] = {"12 dB", "24 dB"}; if (combo("##hps", sl, n, 2, 70)) { s.hpSlope = sl ? 24 : 12; c = true; } }
                c |= sw(tr("Low-pass"), s.lp); ImGui::SameLine();
                c |= slider("##lpf", s.lpHz, 500, 16000, "%.0f Hz", 130); ImGui::SameLine();
                { int sl = s.lpSlope == 24 ? 1 : 0; const char* const n[] = {"12 dB", "24 dB"}; if (combo("##lps", sl, n, 2, 70)) { s.lpSlope = sl ? 24 : 12; c = true; } }
                ImGui::TextDisabled("%s", tr("Audio bandwidth: AM 100-5000, NFM 300-3000, SSB 300-2700 Hz (already set for each kind of mode)"));
                ImGui::Separator();
                c |= sw(tr("Equalizer"), s.eq); ImGui::SameLine();
                { int pr = (int)s.eqPreset; if (combo("##eqp", pr, kEqN, 5, 100)) { s.eqPreset = (EqPreset)pr; if (pr < 4) eqPresetGains(s.eqPreset, s.eqDb); c = true; } }
                {
                    static const char* const bn[] = {"120", "400", "1k", "2.5k", "6k"};
                    for (int b = 0; b < kEqBands; b++) {
                        ImGui::PushID(b);
                        if (b) ImGui::SameLine();
                        if (ImGui::VSliderFloat("##eqb", ImVec2(30 * gUi, 70 * gUi), &s.eqDb[b], -15.f, 15.f, "")) { s.eqPreset = EqPreset::Custom; c = true; }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s Hz: %+.1f dB", bn[b], s.eqDb[b]);
                        ImGui::PopID();
                    }
                }
                ImGui::Separator();
                c |= sw(tr("Notch"), s.notch); ImGui::SameLine();
                c |= slider("##ntf", s.notchHz, 100, 6000, "%.0f Hz", 120); ImGui::SameLine();
                c |= slider("##ntq", s.notchQ, 2, 40, "Q %.0f", 80);
                c |= sw(tr("Auto notch"), s.autoNotch, tr("Finds a steady whistle and cuts it, following it as it drifts (an adaptive line enhancer)."));
                { const float hz = AudioControlConfig::global().statNotchHz.load(); if (s.autoNotch && hz > 0) { ImGui::SameLine(); ImGui::TextDisabled(tr("cutting about %.0f Hz"), hz); } }
                ImGui::Separator();
                { int d = (int)s.deemph; ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(tr("De-emphasis")); ImGui::SameLine(); if (combo("##de", d, kDeempN, 3, 80)) { s.deemph = (Deemph)d; c = true; } }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Leave off unless the mode does not de-emphasise by itself (FM broadcast: 50 us Europe, 75 us Americas)."));
                { int st = (int)s.stereo; ImGui::SameLine(0, 14 * gUi); if (combo("##st", st, kStereoN, 5, 100)) { s.stereo = (StereoMode)st; c = true; } }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(trw("Level"))) {
                int g = (int)s.agc; ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("AGC"); ImGui::SameLine();
                if (combo("##agc", g, kAgcN, 3, 80)) { s.agc = (AgcMode)g; c = true; }
                if (s.agc != AgcMode::Off) {
                    ImGui::SameLine(); c |= slider("##agt", s.agcTargetDb, -36, -4, tr("target %.0f dB"), 130);
                    ImGui::TextDisabled(tr("gain now %+.1f dB (no boost of the noise between transmissions)"), AudioControlConfig::global().statAgcDb.load());
                }
                ImGui::Separator();
                c |= slider("##shf", s.shiftHz, -1000, 1000, tr("clarifier %+.0f Hz"), 200);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Shifts the whole sound up or down, for a single-sideband station that is a little off tune."));
                float cw = s.cwToneHz > 0 ? s.cwToneHz : kCwNominalHz;
                if (slider("##cwt", cw, 300, 1200, tr("CW tone %.0f Hz"), 200)) { s.cwToneHz = std::fabs(cw - kCwNominalHz) < 1.f ? 0.f : cw; c = true; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The pitch of the CW tone you hear. The receiver makes 700 Hz; this moves it."));
                ImGui::Separator();
                ImGui::TextDisabled("%s", tr("Noise reduction (spectral gating) is in Audio cleanup, the button next to this one."));
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(trw("Output"))) {
                ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(tr("Device")); ImGui::SameLine();
                ImGui::SetNextItemWidth(250 * gUi);
                const bool gone = !s.device.empty() && audioPickDevice(S.devs, s.device).empty();
                std::string cur = s.device.empty() ? tr("System default") : s.device + (gone ? std::string(" (") + tr("not found: using the default") + ")" : "");
                if (ImGui::BeginCombo("##acdev", cur.c_str())) {
                    if (ImGui::IsWindowAppearing()) S.devs = dect2::AudioOut::devices();
                    if (ImGui::Selectable(tr("System default"), s.device.empty())) { s.device.clear(); c = true; }
                    for (const auto& d : S.devs) if (!d.empty() && ImGui::Selectable(d.c_str(), d == s.device)) { s.device = d; c = true; }
                    ImGui::EndCombo();
                }
                ImGui::TextDisabled("%s", tr("Remembered for this mode; used the next time the mode's sound starts. A device that is gone falls back to the default."));
                float lat = (float)s.latencyMs;
                if (slider("##aclat", lat, 50, 1000, tr("latency %.0f ms"), 220)) { s.latencyMs = (int)lat; c = true; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Sound queued before playback starts. Lower: less delay, but more risk of dropouts. Applies to modes that do not set their own."));
                ImGui::TextDisabled("%s", tr("Output rate: the mode's own (48 kHz); the system converts it."));
                const auto rx = receiversAll();
                if (rx.size() > 1) {
                    ImGui::Separator();
                    ImGui::TextUnformatted(tr("Receivers"));
                    for (App* r : rx) {
                        ImGui::PushID(r->rxId);
                        const bool heard = r == receiverHeard();
                        char b[48]; snprintf(b, sizeof b, "RX %d", r->rxId);
                        if (pillButton(heard ? (std::string(b) + "  " + tr("solo")).c_str() : b, heard)) receiverHear(r->rxId);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Solo: only this receiver's sound plays, the others are muted."));
                        if (heard) { ImGui::SameLine(0, 6 * gUi); if (sw(tr("mute"), a.muted)) push(a); }
                        ImGui::PopID();
                    }
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        if (ImGui::SmallButton(tr("Reset audio to defaults"))) {
            const float v = s.volume; const bool m = s.muted; const std::string dev = s.device;
            s = audioControlDefaults(audioCleanupProfileOf(a)); s.volume = v; s.muted = m; s.device = dev;
            c = true;
        }
        if (c) { S.s.volume = a.volume; S.s.muted = a.muted; savePrefs(a.family); applyConfig(); changed = true; }
        ImGui::EndPopup();
    }
    AudioControlConfig::global().afWanted.store(open);
    ImGui::PopID();
    return changed;
}

// Settings > Audio > Output device changed: the sound goes there now (a mode with its own device keeps it)
void audioDefaultDeviceChanged() { S.devs = dect2::AudioOut::devices(); applyConfig(); }
