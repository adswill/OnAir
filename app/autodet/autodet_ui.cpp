// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "TV (auto)" and "Digital voice (auto)": one click, the standard is found from the signal (dect2/autodet/autodet.h, docs/AUTODETECT.md).
// Picking the row starts the radio in a mode of that kind (DVB for TV, DMR for voice), lets it settle, captures a stretch of what the
// receivers see and works the standard out on its own thread; then the matching receiver is switched on with the parameters found (the
// DVB-T2 / T width, the ISDB-T and DTMB width, the voice channel's frequency). Unsure answers are said as such and tried again; after three
// tries the status line says to pick the standard by hand. Voice keeps listening: every 20 s the span is looked at again and every system
// found is listed with a button to switch to it ("DMR on 446.006, P25 on 446.206").
#include "../app.h"
#include "dect2/autodet/autodet.h"
#include "dect2/general/general_rx.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace ad = dect2::autodet;

namespace {

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

enum Phase { PhaseOff = 0, PhaseSettle, PhaseCapture, PhaseIdle };

struct AutoState {
    int kind = 0;                 // ad::Capture::KindTv / KindVoice, 0 off
    int phase = PhaseOff;
    double at = 0;                // PhaseSettle: when to start the capture
    int tries = 0;
    uint64_t seq = 0;
    std::string msg;              // the status line
    int msgLevel = 0;             // 0 working, 1 found, 2 unsure
    ad::TvResult tv;
    ad::VoiceResult voice;
    int playing = ad::VoiceUnknown;   // the voice system switched to
    double playingRf = 0;
};

std::map<const App*, AutoState> gState;
bool gInternal = false;          // our own selectMode() calls do not end the automatic mode

bool tvFamily(int f) { return f == 0 || f == 1 || f == 3 || f == 4 || f == 6 || f == 7 || f == 8 || f == 40 || f == 741; }
bool voiceFamily(int f) { return f == 9 || f == 50 || f == 180 || f == 38 || f == 33 || f == 45 || f == 128 || f == 46 || f == 31; }

void switchTo(App& a, int fam) {
    gInternal = true;
    selectMode(a, fam, nullptr);
    gInternal = false;
}

// the radio onto freqMhz (the mode stays): a running receiver retunes, a stopped one starts
void tuneTo(App& a, double mhz) {
    a.freqMhz = mhz;
    if (a.engine.running()) { a.tune.centerHz = mhz * 1e6; a.engine.retune(a.tune); }
    else startReceiver(a);
}

void restart(App& a) {
    if (a.engine.running()) a.engine.stop();
    startReceiver(a);
}

void armSoon(AutoState& s, double sec) { s.phase = PhaseSettle; s.at = now() + sec; }

void applyTv(App& a, AutoState& s) {
    const ad::TvResult& r = s.tv;
    char b[320];
    if (!r.signal) {
        s.msgLevel = 2;
        if (++s.tries < 3) { s.msg = tr("No TV signal on this frequency: still listening..."); armSoon(s, 1.0); }
        else { s.msg = tr("No TV signal found here. Tune to a TV channel (or pick the standard by hand in the mode list)."); s.phase = PhaseIdle; }
        return;
    }
    if (r.std == ad::TvUnknown || r.verdict < ad::VerdictLikely) {
        s.msgLevel = 2;
        snprintf(b, sizeof b, tr("Unsure what this is (%s): looking again..."), r.headline.c_str());
        if (++s.tries < 3) { s.msg = b; armSoon(s, 1.0); }
        else { snprintf(b, sizeof b, tr("Unsure: %s. Pick the standard by hand in the mode list."), r.headline.c_str()); s.msg = b; s.phase = PhaseIdle; }
        return;
    }
    const int fam = ad::tvAppFamily(r.std);
    // the parameters the receiver takes from us; the rest (FFT size, guard, constellation, symbol rate) it finds itself
    bool changed = fam != a.family;
    if (r.std == ad::TvDvbt || r.std == ad::TvDvbt2) {
        const int want = r.verdict == ad::VerdictSure ? (r.std == ad::TvDvbt2 ? 1 : 2) : 0;
        if (a.stdMode != want) { a.stdMode = want; changed = true; }
        for (int i = 0; i < (int)(sizeof kBw / sizeof kBw[0]); i++)
            if (std::fabs(kBw[i].mhz - r.p.chanMhz) < 0.05 && (a.bwIdx != i || a.bwAuto)) { setDvbBandwidth(a, i); changed = true; }
    }
    if (r.std == ad::TvIsdbt && r.p.chanMhz > 0 && a.isdbtBwMhz != (int)r.p.chanMhz) { a.isdbtBwMhz = (int)r.p.chanMhz; changed = true; }
    if (r.std == ad::TvDtmb && r.p.chanMhz > 0 && a.dtmbBwMhz != (int)r.p.chanMhz) { a.dtmbBwMhz = (int)r.p.chanMhz; changed = true; }
    // a channel well off the centre: the radio goes onto it (a satellite transponder or a cable channel at any frequency)
    if (std::fabs(r.p.centreHz) > 0.4e6 && r.std != ad::TvAnalog) { a.freqMhz += std::round(r.p.centreHz / 1e3) / 1e3; changed = true; }
    if (fam != a.family) switchTo(a, fam);
    else if (changed) restart(a);
    snprintf(b, sizeof b, tr("Found %s. The %s receiver is locking..."), r.headline.c_str(), ad::tvStdName(r.std));
    s.msg = b; s.msgLevel = 1; s.phase = PhaseIdle;
    a.engine.log(std::string("TV (auto): ") + r.headline);
}

const ad::VoiceSignal* bestVoice(const ad::VoiceResult& r) {
    for (const ad::VoiceSignal& v : r.signals) if (v.sys != ad::VoiceUnknown && v.verdict >= ad::VerdictLikely) return &v;
    return nullptr;
}

void playVoice(App& a, AutoState& s, const ad::VoiceSignal& v) {
    const int fam = ad::voiceAppFamily(v.sys);
    if (fam < 0) return;
    s.playing = v.sys; s.playingRf = v.rfHz;
    if (fam != a.family) { a.freqMhz = v.rfHz / 1e6; switchTo(a, fam); if (std::fabs(a.freqMhz - v.rfHz / 1e6) > 1e-6) tuneTo(a, v.rfHz / 1e6); }
    else tuneTo(a, v.rfHz / 1e6);
    if (v.sys == ad::VoiceAnalogFm) generalApiSetDemod(dect2::GenNfm);
    a.engine.log(std::string("Digital voice (auto): ") + ad::voiceSysName(v.sys) + " - " + v.why);
}

void applyVoice(App& a, AutoState& s) {
    const ad::VoiceResult& r = s.voice;
    char b[400];
    const ad::VoiceSignal* v = bestVoice(r);
    if (!v) {
        s.msgLevel = r.signals.empty() ? 0 : 2;
        s.msg = r.signals.empty() ? tr("Listening for a digital voice signal...")
                                  : std::string(tr("Unsure: ")) + r.headline + tr(". Still listening...");
        armSoon(s, 2.0);
        return;
    }
    // the system played now is still on the air: stay with it, only the list is new
    bool still = false;
    for (const ad::VoiceSignal& w : r.signals) still |= w.sys == s.playing && std::fabs(w.rfHz - s.playingRf) < 3e3;
    if (!still) playVoice(a, s, *v);
    snprintf(b, sizeof b, tr("Found %s (%.0f %% sure): %s"), ad::voiceSysName(still ? s.playing : v->sys), v->confidence * 100, r.headline.c_str());
    s.msg = b; s.msgLevel = 1;
    armSoon(s, 20.0);   // keep an eye on the span: another system may start talking
}

} // namespace

bool autodetRow(const char* subId, int* kind) {
    if (!subId || strncmp(subId, "auto:", 5) != 0) return false;
    if (kind) *kind = !strcmp(subId + 5, "tv") ? ad::Capture::KindTv : ad::Capture::KindVoice;
    return true;
}

bool autodetActive(const App& a, int kind) {
    auto it = gState.find(&a);
    return it != gState.end() && it->second.phase != PhaseOff && it->second.kind == kind;
}

void autodetCancel(App& a) {
    if (gInternal) return;
    auto it = gState.find(&a);
    if (it == gState.end() || it->second.phase == PhaseOff) return;
    it->second = AutoState();
    a.engine.autodet().cancel();
}

void autodetSelect(App& a, int kind) {
    AutoState& s = gState[&a];
    s = AutoState();
    s.kind = kind;
    if (kind == ad::Capture::KindTv && !tvFamily(a.family)) switchTo(a, 0);
    if (kind == ad::Capture::KindVoice && !voiceFamily(a.family)) switchTo(a, 9);
    if (!a.engine.running()) startReceiver(a);
    s.msg = kind == ad::Capture::KindTv ? tr("Listening to the channel...") : tr("Listening for a digital voice signal...");
    armSoon(s, 1.0);   // the radio settles first (gain, filters)
}

void autodetTick(App& a) {
    auto it = gState.find(&a);
    if (it == gState.end() || it->second.phase == PhaseOff) return;
    AutoState& s = it->second;
    ad::Capture& cap = a.engine.autodet();
    if (s.phase == PhaseSettle && now() >= s.at) {
        if (!a.engine.running() || a.engine.sampleRate() <= 0) { s.at = now() + 0.5; return; }
        ad::TvOptions o;
        o.rfHz = a.engine.captureCentreHz();
        cap.start(s.kind, a.engine.sampleRate(), a.engine.captureCentreHz(), s.kind == ad::Capture::KindTv ? ad::tvCaptureSec() : ad::voiceCaptureSec(), o);
        s.phase = PhaseCapture;
        return;
    }
    if (s.phase != PhaseCapture) return;
    if (s.kind == ad::Capture::KindTv) { if (cap.tvResult(s.tv, s.seq)) applyTv(a, s); }
    else if (cap.voiceResult(s.voice, s.seq)) applyVoice(a, s);
    else if (!a.engine.running()) s.phase = PhaseOff;   // stopped by the user
}

// one line above the mode's own status: what was found, or what it is doing, and for voice a button per signal found
void autodetStatus(App& a) {
    auto it = gState.find(&a);
    if (it == gState.end() || it->second.phase == PhaseOff) return;
    AutoState& s = it->second;
    ad::Capture& cap = a.engine.autodet();
    const ImVec4 col = s.msgLevel == 1 ? ImVec4(0.55f, 0.88f, 0.55f, 1) : s.msgLevel == 2 ? ImVec4(0.95f, 0.75f, 0.35f, 1) : ImVec4(0.75f, 0.82f, 0.92f, 1);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", s.kind == ad::Capture::KindTv ? tr("TV (auto)") : tr("Digital voice (auto)"));
    ImGui::SameLine();
    std::string m = s.msg;
    if (s.phase == PhaseCapture && s.msgLevel == 0) m = cap.status();
    if (s.phase == PhaseCapture && cap.state() == ad::Capture::Collecting) { char p[32]; snprintf(p, sizeof p, " %.0f %%", cap.progress() * 100); m += p; }
    ImGui::TextColored(col, "%s", m.c_str());
    if (s.kind == ad::Capture::KindVoice && s.voice.signals.size() > 1) {
        ImGui::PushID("adv");
        for (size_t i = 0; i < s.voice.signals.size(); i++) {
            const ad::VoiceSignal& v = s.voice.signals[i];
            if (v.sys == ad::VoiceUnknown) continue;
            char lbl[96];
            snprintf(lbl, sizeof lbl, "%s %.4f%s##%zu", ad::voiceSysName(v.sys), v.rfHz / 1e6, v.verdict == ad::VerdictUnsure ? " ?" : "", i);
            ImGui::SameLine();
            const bool on = v.sys == s.playing && std::fabs(v.rfHz - s.playingRf) < 3e3;
            if (on) ImGui::BeginDisabled();
            if (ImGui::SmallButton(lbl)) playVoice(a, s, v);
            if (on) ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s (%s, %.0f %%)", v.why.c_str(), ad::verdictName(v.verdict), v.confidence * 100);
        }
        ImGui::PopID();
    }
    if (s.phase == PhaseIdle && s.msgLevel == 2) { ImGui::SameLine(); if (ImGui::SmallButton(tr("Try again"))) { s.tries = 0; armSoon(s, 0.2); } }
    if (s.kind == ad::Capture::KindTv && s.phase == PhaseIdle && s.msgLevel == 1) {
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Look again"))) { s.tries = 0; armSoon(s, 0.2); }
        if (ImGui::IsItemHovered() && !s.tv.evidence.empty()) {
            std::string t;
            for (const std::string& e : s.tv.evidence) t += e + "\n";
            ImGui::SetTooltip("%s", t.c_str());
        }
    }
}
