// "Audio cleanup" (core/include/dect2/audio/audio_cleanup.h): one small button next to the volume opens a pop-up with the cleanup chain for
// listening: noise reduction, automatic and manual notch, leveller, silence gate, voice emphasis and de-esser. Every kind of mode (AM / airband,
// NFM, WFM, SSB, CW, digital voice, generic; data modes have none) keeps its own settings, started from the preset in audioCleanupPreset() and
// saved in the preferences ("ac.<kind>.<field>"). Basic: a switch and one strength slider. Advanced: every stage.
#include "../app.h"
#include "i18n/i18n.h"
#include "platform/platform.h"
#include "dect2/audio/audio_cleanup.h"
#include "ui/icons.h"
#include <algorithm>
#include <cstdio>
#include <string>

using dect2::AudioCleanupConfig;
using dect2::AudioCleanupSettings;
using dect2::AudioProfile;

namespace {

constexpr int kProfiles = (int)AudioProfile::Count;
// Names shown (translated where drawn)
const char* const kProfileNames[kProfiles] = {N_("Data (off)"), N_("Generic"), N_("AM / airband"), N_("NFM / VHF / UHF"), N_("WFM broadcast"), N_("SSB / HF"), N_("CW"), N_("Digital voice")};

struct State {
    AudioCleanupSettings s[kProfiles];
    bool loaded = false, advanced = false;
    int cur = -1;                 // the profile the chain follows now
    std::string famKey;           // pref key of the override for the mode
};
State S;

std::string key(int p, const char* f) { return std::string("ac.") + std::to_string(p) + "." + f; }

AudioCleanupSettings loadProfile(int p) {
    AudioCleanupSettings d = dect2::audioCleanupPreset((AudioProfile)p), s = d;
    auto& pr = plat::prefs();
    s.master = pr.getB(key(p, "on").c_str(), d.master);
    s.nr = pr.getB(key(p, "nr").c_str(), d.nr); s.nrStrength = (float)pr.getD(key(p, "nrs").c_str(), d.nrStrength);
    s.neural = pr.getB(key(p, "nn").c_str(), d.neural); s.neuralStrength = (float)pr.getD(key(p, "nns").c_str(), d.neuralStrength);
    s.autoNotch = pr.getB(key(p, "an").c_str(), d.autoNotch);
    s.notch = pr.getB(key(p, "mn").c_str(), d.notch); s.notchHz = (float)pr.getD(key(p, "mnf").c_str(), d.notchHz); s.notchWidth = (float)pr.getD(key(p, "mnw").c_str(), d.notchWidth);
    s.level = pr.getB(key(p, "lv").c_str(), d.level); s.levelTargetDb = (float)pr.getD(key(p, "lvt").c_str(), d.levelTargetDb);
    s.levelAmount = (float)pr.getD(key(p, "lva").c_str(), d.levelAmount); s.levelHold = pr.getB(key(p, "lvh").c_str(), d.levelHold);
    s.gate = pr.getB(key(p, "gt").c_str(), d.gate); s.gateOffsetDb = (float)pr.getD(key(p, "gto").c_str(), d.gateOffsetDb);
    s.voice = pr.getB(key(p, "vo").c_str(), d.voice); s.voiceAmount = (float)pr.getD(key(p, "voa").c_str(), d.voiceAmount);
    s.deess = pr.getB(key(p, "ds").c_str(), d.deess); s.deessAmount = (float)pr.getD(key(p, "dsa").c_str(), d.deessAmount);
    s.recCleaned = pr.getB(key(p, "rc").c_str(), d.recCleaned); s.skipSilence = pr.getB(key(p, "ss").c_str(), d.skipSilence);
    return s;
}
void saveProfile(int p) {
    const AudioCleanupSettings& s = S.s[p];
    auto& pr = plat::prefs();
    pr.setB(key(p, "on").c_str(), s.master);
    pr.setB(key(p, "nr").c_str(), s.nr); pr.setD(key(p, "nrs").c_str(), s.nrStrength);
    pr.setB(key(p, "nn").c_str(), s.neural); pr.setD(key(p, "nns").c_str(), s.neuralStrength);
    pr.setB(key(p, "an").c_str(), s.autoNotch);
    pr.setB(key(p, "mn").c_str(), s.notch); pr.setD(key(p, "mnf").c_str(), s.notchHz); pr.setD(key(p, "mnw").c_str(), s.notchWidth);
    pr.setB(key(p, "lv").c_str(), s.level); pr.setD(key(p, "lvt").c_str(), s.levelTargetDb); pr.setD(key(p, "lva").c_str(), s.levelAmount); pr.setB(key(p, "lvh").c_str(), s.levelHold);
    pr.setB(key(p, "gt").c_str(), s.gate); pr.setD(key(p, "gto").c_str(), s.gateOffsetDb);
    pr.setB(key(p, "vo").c_str(), s.voice); pr.setD(key(p, "voa").c_str(), s.voiceAmount);
    pr.setB(key(p, "ds").c_str(), s.deess); pr.setD(key(p, "dsa").c_str(), s.deessAmount);
    pr.setB(key(p, "rc").c_str(), s.recCleaned); pr.setB(key(p, "ss").c_str(), s.skipSilence);
}

void load() {
    if (S.loaded) return;
    S.loaded = true;
    for (int p = 0; p < kProfiles; p++) S.s[p] = loadProfile(p);
    S.advanced = plat::prefs().getB("ac.adv", false);
}

// The kind of sound a mode makes
AudioProfile autoProfile(int family) {
    switch (family) {
        case 5: case 21: case 2: return AudioProfile::WFM;        // FM broadcast, HD Radio, DAB
        case 26: return AudioProfile::AM;                          // airband
        case 34: return AudioProfile::CW;
        case 55: return AudioProfile::Generic;                     // the General receiver (its own demodulators)
        case 9: case 31: case 33: case 38: case 45: case 46: case 568: case 19: return AudioProfile::DigitalVoice;   // DMR, M17, D-STAR, NXDN, dPMR, TETRA, P25, Iridium
        default: return AudioProfile::Off;                          // data modes, TV, spectrum: no cleanup
    }
}
std::string overrideKey(int family) { return "ac.ovr." + std::to_string(family); }
int profileFor(const App& a) {
    const long o = plat::prefs().getI(overrideKey(a.family).c_str(), -1);
    if (o >= 0 && o < kProfiles) return (int)o;
    return (int)autoProfile(a.family);
}

void apply() {
    if (S.cur >= 0) AudioCleanupConfig::global().set(S.s[S.cur]);
}

bool sw(const char* label, bool& v, const char* tip = nullptr) {
    const bool ch = ImGui::Checkbox(label, &v);
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    return ch;
}
bool slider(const char* id, float& v, float lo, float hi, const char* fmt, float w = 140) {
    ImGui::SetNextItemWidth(w * gUi);
    return ImGui::SliderFloat(id, &v, lo, hi, fmt);
}

bool gDevOpen = false;

}  // namespace

int audioCleanupProfileOf(const App& a) { load(); return profileFor(a); }

void audioCleanupDevOpen(bool advanced) { gDevOpen = true; load(); S.advanced = advanced; }

void audioCleanupTick(App& a) {
    load();
    const int p = profileFor(a);
    if (p != S.cur) { S.cur = p; apply(); }
}

bool audioCleanupButton(App& a) {
    load();
    if (S.cur < 0) audioCleanupTick(a);
    AudioCleanupSettings& s = S.s[S.cur];
    const bool active = s.master;
    ImGui::PushID("audioclean");
    if (iconFlat(Ic::Sliders, tr("Audio cleanup: noise reduction, notch, leveller ..."), active)) ImGui::OpenPopup("##acpop");
    if (gDevOpen && ImGui::GetFrameCount() > 40) { gDevOpen = false; ImGui::OpenPopup("##acpop"); }
    const ImVec2 bmax = ImGui::GetItemRectMax();
    bool changed = false;
    ImGui::SetNextWindowPos(ImVec2(bmax.x, bmax.y + 2 * gUi), ImGuiCond_Appearing, ImVec2(1.f, 0.f));   // under the button, right-aligned with it
    ImGui::SetNextWindowSizeConstraints(ImVec2(S.advanced ? 400 * gUi : 330 * gUi, 0), ImVec2(520 * gUi, 700 * gUi));
    if (ImGui::BeginPopup("##acpop")) {
        const int cur = S.cur;
        ImGui::TextUnformatted(tr("Audio cleanup"));
        ImGui::SameLine(0, 12 * gUi);
        if (pillButton(tr("Basic"), !S.advanced)) { S.advanced = false; plat::prefs().setB("ac.adv", false); }
        ImGui::SameLine(0, 4 * gUi);
        if (pillButton(tr("Advanced"), S.advanced)) { S.advanced = true; plat::prefs().setB("ac.adv", true); }
        ImGui::Separator();
        // the kind of mode: automatic from the mode, or chosen
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", tr("Settings for"));
            ImGui::SameLine(0, 6 * gUi);
            ImGui::SetNextItemWidth(230 * gUi);
            const bool isAuto = plat::prefs().getI(overrideKey(a.family).c_str(), -1) < 0;
            char lbl[96];
            if (isAuto) snprintf(lbl, sizeof lbl, tr("Automatic (%s)"), tr(kProfileNames[cur])); else snprintf(lbl, sizeof lbl, "%s", tr(kProfileNames[cur]));
            if (ImGui::BeginCombo("##acprof", lbl)) {
                if (ImGui::Selectable(tr("Automatic"), isAuto)) { plat::prefs().setI(overrideKey(a.family).c_str(), -1); S.cur = (int)autoProfile(a.family); apply(); }
                for (int p = 0; p < kProfiles; p++)
                    if (ImGui::Selectable(tr(kProfileNames[p]), !isAuto && p == cur)) { plat::prefs().setI(overrideKey(a.family).c_str(), p); S.cur = p; apply(); }
                ImGui::EndCombo();
            }
        }
        if (S.cur != cur) { ImGui::EndPopup(); ImGui::PopID(); return false; }
        if (cur == (int)AudioProfile::Off) {
            ImGui::TextWrapped("%s", tr("A data mode: its sound is left as it is. Pick another kind above to clean it up anyway."));
        }
        bool c = false;
        c |= sw(tr("On"), s.master, tr("Clean the sound you hear. The decoders always get the untouched sound."));
        if (!S.advanced) {
            ImGui::SameLine(0, 14 * gUi);
            ImGui::BeginDisabled(!s.master);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", tr("Strength"));
            ImGui::SameLine(0, 6 * gUi);
            float st = s.nrStrength * 100.f;
            if (slider("##acstr", st, 0, 100, "%.0f %%", 150)) { s.nrStrength = st / 100.f; c = true; }
            ImGui::EndDisabled();
            std::string on;
            auto add = [&](bool f, const char* t) { if (f) { if (!on.empty()) on += ", "; on += tr(t); } };
            add(s.nr, N_("noise reduction")); add(s.autoNotch, N_("auto notch")); add(s.level, N_("leveller")); add(s.gate, N_("silence gate")); add(s.voice, N_("voice emphasis")); add(s.neural, N_("voice noise remover"));
            ImGui::TextDisabled("%s", on.empty() ? tr("Nothing is on for this kind of mode. See Advanced.") : on.c_str());
            // one click for speech modes (analog and digital voice): the neural noise remover; music and data modes do not get the button
            if (cur == (int)AudioProfile::NFM || cur == (int)AudioProfile::AM || cur == (int)AudioProfile::SSB || cur == (int)AudioProfile::DigitalVoice || cur == (int)AudioProfile::Generic) {
                ImGui::BeginDisabled(!s.master);
                c |= sw(tr("Voice noise remover (neural)"), s.neural, tr("Removes steady background noise from speech with a small neural network (RNNoise). For voice only: it treats music and data tones as noise. Adds about 20 ms of delay."));
                ImGui::EndDisabled();
            }
        } else {
            ImGui::BeginDisabled(!s.master);
            // 1. noise reduction
            c |= sw(tr("Noise reduction"), s.nr, tr("Takes down the steady hiss and rumble between and under the speech. More strength cleans more and costs some naturalness."));
            if (s.nr) {
                ImGui::SameLine(0, 10 * gUi);
                float st = s.nrStrength * 100.f;
                if (slider("##acnrs", st, 0, 100, "%.0f %%")) { s.nrStrength = st / 100.f; c = true; }
                const float nd = AudioCleanupConfig::global().statNrDb.load();
                if (s.master && nd < -0.5f) { ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("%.0f dB", nd); }
            }
            // 1b. neural voice noise remover (RNNoise): every mode's audio goes through the same chain, so this covers every voice mode
            c |= sw(tr("Voice noise remover (neural)"), s.neural, tr("Removes steady background noise from speech with a small neural network (RNNoise). For voice only: it treats music and data tones as noise, so leave it off for those. Adds about 20 ms of delay."));
            if (s.neural) {
                ImGui::SameLine(0, 10 * gUi);
                float st = s.neuralStrength * 100.f;
                if (slider("##acnns", st, 0, 100, "%.0f %%")) { s.neuralStrength = st / 100.f; c = true; }
            }
            // 2. notches
            c |= sw(tr("Auto notch"), s.autoNotch, tr("Finds a steady whistle, carrier or heterodyne in the sound and cuts it out narrowly, following it as it drifts. Lets go again when it stops."));
            if (s.autoNotch) {
                const float hz = AudioCleanupConfig::global().statNotchHz.load();
                if (hz > 0) { ImGui::SameLine(0, 10 * gUi); ImGui::TextDisabled(tr("cutting %.0f Hz"), hz); }
            }
            c |= sw(tr("Manual notch"), s.notch, tr("Cuts one frequency out of the sound, for a tone you can see in the audio spectrum."));
            if (s.notch) {
                ImGui::SameLine(0, 10 * gUi);
                if (slider("##acmnf", s.notchHz, 100, 6000, "%.0f Hz", 130)) c = true;
                ImGui::SameLine(0, 6 * gUi);
                float wd = s.notchWidth * 100.f;
                if (slider("##acmnw", wd, 0, 100, tr("width %.0f %%"), 110)) { s.notchWidth = wd / 100.f; c = true; }
            }
            // 3. leveller
            c |= sw(tr("Leveller"), s.level, tr("Brings quiet and loud stations to the same loudness, with a soft limiter against peaks."));
            if (s.level) {
                ImGui::SameLine(0, 10 * gUi);
                float am = s.levelAmount * 100.f;
                if (slider("##aclva", am, 0, 100, tr("balance %.0f %%"), 130)) { s.levelAmount = am / 100.f; c = true; }
                ImGui::SameLine(0, 6 * gUi);
                if (slider("##aclvt", s.levelTargetDb, -30, -10, tr("target %.0f dB"), 120)) c = true;
                ImGui::Indent(18 * gUi);
                c |= sw(tr("Do not boost the noise between transmissions"), s.levelHold);
                ImGui::Unindent(18 * gUi);
            }
            // 4. silence gate
            c |= sw(tr("Silence gate"), s.gate, tr("Mutes the sound when only noise is left, like a squelch that follows the noise level by itself."));
            if (s.gate) {
                ImGui::SameLine(0, 10 * gUi);
                if (slider("##acgto", s.gateOffsetDb, 3, 24, tr("opens %.0f dB above noise"), 170)) c = true;
                if (s.master && !AudioCleanupConfig::global().statGateOpen.load()) { ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("%s", tr("muted")); }
            }
            // 5. voice
            c |= sw(tr("Voice emphasis"), s.voice, tr("Cuts the rumble below the voice and lifts the presence, so a weak voice is easier to understand."));
            if (s.voice) { ImGui::SameLine(0, 10 * gUi); float v = s.voiceAmount * 100.f; if (slider("##acvoa", v, 0, 100, "%.0f %%", 110)) { s.voiceAmount = v / 100.f; c = true; } }
            c |= sw(tr("De-esser"), s.deess, tr("Softens sharp S sounds."));
            if (s.deess) { ImGui::SameLine(0, 10 * gUi); float v = s.deessAmount * 100.f; if (slider("##acdsa", v, 0, 100, "%.0f %%", 110)) { s.deessAmount = v / 100.f; c = true; } }
            ImGui::EndDisabled();
            ImGui::Separator();
            c |= sw(tr("Record the cleaned sound"), s.recCleaned, tr("Record audio saves the sound as you hear it now, cleaned, instead of the raw sound."));
            if (s.recCleaned) { ImGui::Indent(18 * gUi); c |= sw(tr("Skip silence in the recording"), s.skipSilence, tr("Nothing is recorded while the silence gate holds the sound off.")); ImGui::Unindent(18 * gUi); }
            if (ImGui::SmallButton(tr("Reset to the preset"))) { const bool m = s.master; S.s[cur] = dect2::audioCleanupPreset((AudioProfile)cur); S.s[cur].master = m; c = true; }
        }
        if (c) { saveProfile(cur); apply(); changed = true; }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}
