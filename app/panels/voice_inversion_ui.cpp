// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Voice inversion" (core/include/dect2/audio/voice_inversion.h): a small button next to the audio cleanup button opens a pop-up with the
// descrambler for analog voice (airband, NFM, WFM, SSB, generic receivers): simple inversion with a carrier, split-band inversion, and a
// search for the carrier / split frequency. Off by default; the settings are kept per mode ("vi.<mode>.<field>") in the preferences.
// A notice has to be accepted once before first use ("vi.accepted"). Digital voice and data modes never get the button.
#include "../app.h"
#include "i18n/i18n.h"
#include "platform/platform.h"
#include "dect2/audio/voice_inversion.h"
#include "ui/icons.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using dect2::VoiceInversionConfig;
using dect2::VoiceInversionSettings;
using dect2::VoiceInvMode;

namespace {

struct Searcher {
    std::atomic<int> state{0};   // 0 idle, 1 listening, 2 searching, 3 done
    std::atomic<float> progress{0.f};
    std::atomic<bool> cancel{false};
    dect2::VoiceInvSearchResult result;
    VoiceInvMode mode = VoiceInvMode::Simple;
    double t0 = 0;
    std::thread th;
    ~Searcher() { cancel = true; if (th.joinable()) th.join(); VoiceInversionConfig::global().capture = false; }
};

struct State {
    VoiceInversionSettings s;
    bool loaded = false;
    int family = -1;
    bool analog = false;
    Searcher sr;
    char msg[160] = "";
};
State S;

std::string key(int fam, const char* f) { return "vi." + std::to_string(fam) + "." + f; }

void load(int fam) {
    auto& pr = plat::prefs();
    VoiceInversionSettings d, s = d;
    s.on = pr.getB(key(fam, "on").c_str(), false);
    s.mode = pr.getI(key(fam, "mode").c_str(), 0) == 1 ? VoiceInvMode::Split : VoiceInvMode::Simple;
    s.carrierHz = (float)pr.getD(key(fam, "car").c_str(), d.carrierHz); s.splitHz = (float)pr.getD(key(fam, "spl").c_str(), d.splitHz);
    s.loHz = (float)pr.getD(key(fam, "lo").c_str(), d.loHz); s.hiHz = (float)pr.getD(key(fam, "hi").c_str(), d.hiHz);
    S.s = s;
}
void save(int fam) {
    auto& pr = plat::prefs();
    const VoiceInversionSettings& s = S.s;
    pr.setB(key(fam, "on").c_str(), s.on); pr.setI(key(fam, "mode").c_str(), s.mode == VoiceInvMode::Split ? 1 : 0);
    pr.setD(key(fam, "car").c_str(), s.carrierHz); pr.setD(key(fam, "spl").c_str(), s.splitHz);
    pr.setD(key(fam, "lo").c_str(), s.loHz); pr.setD(key(fam, "hi").c_str(), s.hiHz);
}
bool accepted() { return plat::prefs().getB("vi.accepted", false); }
void apply() {
    VoiceInversionSettings s = S.s;
    s.on = s.on && S.analog && accepted();   // never without the notice accepted, never on a data or digital voice mode
    VoiceInversionConfig::global().set(s);
}

bool gDevOpen = false;

bool numField(const char* id, float& v, float lo, float hi, float w = 90) {
    ImGui::SetNextItemWidth(w * gUi);
    float t = v;
    if (ImGui::InputFloat(id, &t, 0.f, 0.f, "%.0f Hz", ImGuiInputTextFlags_EnterReturnsTrue)) { v = std::clamp(t, lo, hi); return true; }
    return false;
}

void startSearch(VoiceInvMode mode) {
    Searcher& sr = S.sr;
    sr.cancel = true; if (sr.th.joinable()) sr.th.join();
    sr.cancel = false; sr.mode = mode; sr.progress = 0.f; sr.state = 1; sr.t0 = ImGui::GetTime();
    VoiceInversionConfig::global().capture = true;   // the audio output keeps the sound going in, from now on
}
// called every frame while the pop-up is open
void pollSearch() {
    Searcher& sr = S.sr;
    if (sr.state == 1 && ImGui::GetTime() - sr.t0 >= 5.0) {
        std::vector<float> snap;
        const int rate = VoiceInversionConfig::global().snapshot(snap, 5.0);
        if (rate == 0 || (int)snap.size() < rate * 3) { sr.state = 0; VoiceInversionConfig::global().capture = false; snprintf(S.msg, sizeof S.msg, "%s", tr("No sound reached the descrambler: tune a voice signal first.")); return; }
        VoiceInversionConfig::global().capture = S.s.on;
        sr.state = 2;
        const VoiceInvMode mode = sr.mode; const VoiceInversionSettings base = S.s;
        sr.th = std::thread([&sr, snap = std::move(snap), rate, mode, base]() mutable {
            sr.result = dect2::voiceInversionSearch(snap.data(), (int)snap.size(), rate, mode, base, &sr.progress, &sr.cancel);
            sr.state = 3;
        });
    }
}

}  // namespace

void voiceInversionDevOpen() { gDevOpen = true; }

// 1..5: the audio profiles of the cleanup UI that are analog voice (generic, AM / airband, NFM, WFM, SSB)
void voiceInversionTick(App& a) {
    const int prof = audioCleanupProfileOf(a);
    const bool analog = prof >= 1 && prof <= 5;
    if (!S.loaded || a.family != S.family || analog != S.analog) {
        S.loaded = true; S.family = a.family; S.analog = analog;
        load(a.family);
        apply();
    }
}

bool voiceInversionButton(App& a) {
    voiceInversionTick(a);
    if (!S.analog) return false;
    ImGui::PushID("voiceinv");
    if (iconFlat(Ic::Wave, tr("Voice inversion: undo a simple voice scrambler"), S.s.on && accepted())) ImGui::OpenPopup("##vipop");
    if (gDevOpen && ImGui::GetFrameCount() > 40) { gDevOpen = false; ImGui::OpenPopup("##vipop"); }
    const ImVec2 bmax = ImGui::GetItemRectMax();
    bool changed = false;
    ImGui::SetNextWindowPos(ImVec2(bmax.x, bmax.y + 2 * gUi), ImGuiCond_Appearing, ImVec2(1.f, 0.f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(380 * gUi, 0), ImVec2(480 * gUi, 700 * gUi));
    if (ImGui::BeginPopup("##vipop")) {
        VoiceInversionSettings& s = S.s;
        ImGui::TextUnformatted(tr("Voice inversion"));
        ImGui::Separator();
        if (!accepted()) {
            ImGui::PushTextWrapPos(360 * gUi);
            ImGui::TextWrapped("%s", tr("Frequency inversion only mirrors the sound of a voice; it is not encryption. In many countries it may be illegal to listen to private communications. You are responsible for what you listen to and for following the law where you are. Encrypted traffic is never touched."));
            ImGui::PopTextWrapPos();
            if (ImGui::Button(tr("I understand and accept"))) { plat::prefs().setB("vi.accepted", true); apply(); changed = true; }
            ImGui::EndPopup(); ImGui::PopID();
            return changed;
        }
        bool c = false;
        c |= ImGui::Checkbox(tr("On"), &s.on);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Descramble the sound you hear. The decoders always get the untouched sound."));
        ImGui::SameLine(0, 14 * gUi);
        if (pillButton(tr("Simple"), s.mode == VoiceInvMode::Simple)) { s.mode = VoiceInvMode::Simple; c = true; }
        ImGui::SameLine(0, 4 * gUi);
        if (pillButton(tr("Split-band"), s.mode == VoiceInvMode::Split)) { s.mode = VoiceInvMode::Split; c = true; }
        ImGui::BeginDisabled(!s.on);
        if (s.mode == VoiceInvMode::Simple) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", tr("Carrier"));
            for (int i = 0; i < 5; i++) {
                ImGui::SameLine(0, 4 * gUi);
                char l[24]; snprintf(l, sizeof l, "%.2f", dect2::kVoiceInvPresets[i] / 1000.f);
                if (pillButton(l, std::fabs(s.carrierHz - dect2::kVoiceInvPresets[i]) < 1.f, 8)) { s.carrierHz = dect2::kVoiceInvPresets[i]; c = true; }
            }
            ImGui::SameLine(0, 4 * gUi); ImGui::TextDisabled("kHz");
            ImGui::SetNextItemWidth(250 * gUi);
            c |= ImGui::SliderFloat("##vicar", &s.carrierHz, 2000.f, 3800.f, "%.0f Hz");
            ImGui::SameLine(0, 6 * gUi);
            c |= numField("##vicarn", s.carrierHz, 600.f, 4500.f, 80);
        } else {
            ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Split at"));
            ImGui::SameLine(0, 6 * gUi);
            ImGui::SetNextItemWidth(200 * gUi);
            c |= ImGui::SliderFloat("##visp", &s.splitHz, s.loHz + 200.f, s.hiHz - 200.f, "%.0f Hz");
            ImGui::SameLine(0, 6 * gUi);
            c |= numField("##vispn", s.splitHz, 300.f, 3800.f, 80);
            ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Band from"));
            ImGui::SameLine(0, 6 * gUi);
            c |= numField("##vilo", s.loHz, 100.f, 1000.f, 80);
            ImGui::SameLine(0, 6 * gUi); ImGui::TextDisabled("%s", tr("to"));
            ImGui::SameLine(0, 6 * gUi);
            c |= numField("##vihi", s.hiHz, 1500.f, 4000.f, 80);
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        // search
        Searcher& sr = S.sr;
        pollSearch();
        const int st = sr.state.load();
        if (st == 0 || st == 3) {
            if (ImGui::Button(tr("Search"))) { S.msg[0] = 0; startSearch(s.mode); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Listens for 5 seconds, then tries the carrier (or split) frequencies and picks the one that sounds most like speech. Works best while someone is talking."));
        } else {
            if (ImGui::Button(tr("Cancel"))) { sr.cancel = true; if (sr.th.joinable()) sr.th.join(); sr.state = 0; VoiceInversionConfig::global().capture = s.on; }
        }
        ImGui::SameLine(0, 8 * gUi);
        if (st == 1) { ImGui::ProgressBar((float)std::min(1.0, (ImGui::GetTime() - sr.t0) / 5.0), ImVec2(150 * gUi, 0), tr("listening")); }
        else if (st == 2) { ImGui::ProgressBar(sr.progress.load(), ImVec2(150 * gUi, 0), tr("searching")); }
        else if (st == 3) {
            if (sr.th.joinable()) sr.th.join();
            const auto& r = sr.result;
            if (r.found) {
                if (sr.mode == VoiceInvMode::Simple) s.carrierHz = r.value; else s.splitHz = r.value;
                s.on = true; c = true;
                snprintf(S.msg, sizeof S.msg, tr("Found %.0f Hz (match %.0f %%)"), r.value, 100.0 * std::min(1.0, r.score));
            } else snprintf(S.msg, sizeof S.msg, "%s", tr("Nothing speech-like found. Try again while someone is talking."));
            sr.state = 0;
        }
        ImGui::PushTextWrapPos(360 * gUi);
        if (S.msg[0]) ImGui::TextDisabled("%s", S.msg);
        ImGui::TextDisabled("%s", tr("Not encryption: it only mirrors the sound. Encrypted traffic is never touched."));
        ImGui::PopTextWrapPos();
        if (c) { save(a.family); apply(); changed = true; }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}
