// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The keyboard-mode half of the Ham view: see ham_kbd_ui.h.
#include "ham_kbd_ui.h"
#include "ham_station_ui.h"
#include "dect2/cw/cw_tel.h"
#include "../platform/platform.h"
#include "ham_ops_ui.h"
#include "dect2/cw/cw_tel.h"
#include "dect2/hfdig/hfdig_rtty.h"
#include "dect2/hfdig/hfdig_tel.h"
#include "dect2/psk/psk_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

using namespace dect2;

namespace {
struct KbdUi {
    bool loaded = false, followBottom = true;
    char name[24] = {}, qth[40] = {}, theirCall[16] = {}, theirName[24] = {}, rst[8] = "599", rstRcvd[8] = "599", buf[ham::kKbdMaxChars + 8] = {};
    float hz = 1000, wpm = 20;
    int preset = -1;
    std::string sending, info;
    double pickedAt = 0;
    ham::TxDuty duty;
    ham::KbdStation station() const;
};
KbdUi& K() { static KbdUi k; return k; }

void load(KbdUi& k) {
    if (k.loaded) return;
    k.loaded = true;
    plat::Prefs& p = plat::basePrefs();
    snprintf(k.name, sizeof k.name, "%s", p.getS("ham.name", "").c_str());
    snprintf(k.qth, sizeof k.qth, "%s", p.getS("ham.qth", "").c_str());
    k.wpm = (float)p.getD("ham.cwWpm", 20);
}

std::string rxText(App& a, const ham::KbdPreset& p) {
    switch (p.kind) {
    case ham::KbdKind::Psk: { const PskSignal* s = pskFocus(a.rx.psk); return s ? s->text : std::string(); }
    case ham::KbdKind::Cw: { const CwSignal* s = cwFocus(a.rx.cw); return s ? s->text : std::string(); }
    case ham::KbdKind::Rtty: { HfdigRttyTelemetry t; a.engine.hfdig().rtty().telemetry(t); return t.text; }
    default: { HfdigKbdTelemetry t; a.engine.hfdig().kbd().telemetry(t); return t.text; }
    }
}
}  // namespace

ham::KbdStation KbdUi::station() const {
    ham::KbdStation s;
    s.myName = name; s.myQth = qth; s.call = theirCall; s.name = theirName; s.rst = rst;
    return s;
}

void hamKbdSelect(App& a, int preset) {
    const auto& ps = ham::kbdPresets();
    if (preset < 0 || preset >= (int)ps.size()) return;
    const ham::KbdPreset& p = ps[(size_t)preset];
    selectMode(a, p.rxFamily, p.rxSub);
    switch (p.kind) {
    case ham::KbdKind::Psk: a.engine.psk().setMode(p.pskMode); break;
    case ham::KbdKind::Hfdig: {
        KbdConfig k;
        k.family = p.family; k.tones = p.tones ? p.tones : 32; k.bandwidth = p.bw ? p.bw : 1000; k.variant = p.variant;
        k.centreHz = p.defaultHz; k.autoCentre = false;
        a.engine.hfdig().kbd().setConfig(k);
        break;
    }
    default: break;
    }
    KbdUi& k = K();
    k.preset = preset;
    k.hz = (float)p.defaultHz;
}

void hamKbdPanel(HamKbdCtx& c, int preset) {
    KbdUi& k = K();
    load(k);
    const auto& ps = ham::kbdPresets();
    if (preset < 0 || preset >= (int)ps.size()) return;
    const ham::KbdPreset& p = ps[(size_t)preset];
    App& a = c.a;
    const double now = (double)time(nullptr);
    auto savePrefs = [&] { plat::Prefs& pr = plat::basePrefs(); pr.setS("ham.name", k.name); pr.setS("ham.qth", k.qth); pr.setD("ham.cwWpm", k.wpm); pr.flush(); };

    // a finished burst
    if (c.txState == 2 || c.txState == 3) {
        if (c.worker.joinable()) c.worker.join();
        std::lock_guard<std::mutex> l(c.mu);
        if (c.txState == 2) k.info = "Sent " + std::to_string(k.sending.size()) + " characters";
        else {   // not sent: the text goes back to the front of the buffer
            k.info = "Not sent (" + c.txErr + ")";
            std::string t = k.sending + std::string(k.buf);
            snprintf(k.buf, sizeof k.buf, "%s", t.substr(0, ham::kKbdMaxChars).c_str());
        }
        k.sending.clear();
        c.txState = 0;
    }
    if (!c.armed && c.tx.busy()) c.tx.abort();   // disarm and Halt stop a running burst and release PTT at once

    ImGui::Separator();
    // station for the macros
    ImGui::SetNextItemWidth(110 * gUi);
    bool ch = ImGui::InputText(tr("My name"), k.name, sizeof k.name);
    ImGui::SameLine(); ImGui::SetNextItemWidth(140 * gUi);
    ch |= ImGui::InputText(tr("My QTH"), k.qth, sizeof k.qth);
    if (ch) savePrefs();
    ImGui::SetNextItemWidth(90 * gUi);
    ImGui::SliderFloat(tr("Audio Hz"), &k.hz, 300, 3000, "%.0f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The middle of your signal in the audio passband. Choose a clear spot on the waterfall."));
    if (p.kind == ham::KbdKind::Cw) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); if (ImGui::SliderFloat(tr("WPM"), &k.wpm, 5, 40, "%.0f")) savePrefs();
        // 4.0: follow the speed of the station in focus (the CW decoder's reading), so you answer as fast as you are called
        HamStation& st = hamStation();
        ImGui::SameLine();
        if (ImGui::Checkbox(tr("Match speed"), &st.cwMatch)) { st.cwm.reset(); hamStationSave(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Your keyer follows the speed of the CW station in focus (the decoder's WPM), rounded, 5 to 45 WPM."));
        if (st.cwMatch) {
            const dect2::CwSignal* f = dect2::cwFocus(a.rx.cw);
            if (f && f->id != st.cwFocusId) { st.cwm.reset(); st.cwFocusId = f->id; }
            if (f && f->wpm > 0) st.cwm.heard(f->wpm);
            const int w = st.cwm.wpm((int)std::lround(k.wpm));
            if (w != (int)std::lround(k.wpm)) k.wpm = (float)w;
        }
    }
    else if (p.kind == ham::KbdKind::Psk) { ImGui::SameLine(); if (ImGui::SmallButton(tr("Focus RX here"))) a.engine.psk().setFocusHz(k.hz); }
    else if (p.kind == ham::KbdKind::Hfdig) {
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Set RX here"))) {
            KbdConfig kc = a.engine.hfdig().kbd().config();
            kc.centreHz = k.hz; kc.autoCentre = false;
            a.engine.hfdig().kbd().setConfig(kc);
        }
    }
    ImGui::SameLine(); ImGui::TextDisabled("%.4f MHz   %s", a.freqMhz, p.name);

    c.rigWidgets();

    // Arm / Halt: the same switch as the slot modes
    ImGui::PushStyleColor(ImGuiCol_Button, c.armed ? ImVec4(0.75f, 0.1f, 0.1f, 1) : ImVec4(0.3f, 0.3f, 0.3f, 1));
    if (ImGui::Button(c.armed ? tr("ARMED: click to disarm") : tr("Arm TX"))) c.armed = !c.armed;
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Nothing is sent unless this is armed. Without a licence or outside the amateur bands OnAir warns you and still sends: you are responsible."));
    ImGui::SameLine();
    if (ImGui::Button(tr("Halt TX"))) { c.armed = false; c.tx.abort(); c.status = "Halted"; }
    ImGui::SameLine(); ImGui::TextDisabled("%s: %.0f s / 10 min", tr("On air"), k.duty.used(now));
    if (!c.status.empty()) ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "%s", c.status.c_str());

    // received text and the callsigns in it
    ImGui::Separator();
    const std::string rx = rxText(a, p);
    const float h = std::max(80.0f, ImGui::GetContentRegionAvail().y * 0.4f);
    ImGui::BeginChild("##kbdrx", ImVec2(0, h), true);
    if (p.family == KbdHell && p.kind == ham::KbdKind::Hfdig) ImGui::TextDisabled("%s", tr("Hellschreiber is a picture: see the Hellschreiber view of the receiver."));
    else ImGui::TextWrapped("%s", rx.c_str());
    static size_t lastLen = 0;
    if (rx.size() != lastLen) { lastLen = rx.size(); ImGui::SetScrollHereY(1.0f); }
    ImGui::EndChild();
    const std::string tail = rx.size() > 400 ? rx.substr(rx.size() - 400) : rx;
    const auto calls = ham::kbdCallsInText(tail, c.call);
    ImGui::TextDisabled("%s", tr("Callsigns heard (click to work):"));
    for (size_t i = 0; i < calls.size() && i < 8; i++) {
        ImGui::SameLine();
        const bool w = c.worked.worked(calls[i]);
        const std::string tag = hamCountryInMessages() ? hamCountryTag(calls[i]) : std::string();
        if (ImGui::SmallButton((calls[i] + (w ? " *" : "") + (tag.empty() ? "" : " (" + tag + ")")).c_str())) { snprintf(k.theirCall, sizeof k.theirCall, "%s", calls[i].c_str()); k.pickedAt = now; }
    }

    // the other station
    ImGui::SetNextItemWidth(100 * gUi);
    if (ImGui::InputText(tr("Call"), k.theirCall, sizeof k.theirCall, ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank) && k.pickedAt == 0) k.pickedAt = now;
    ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi); ImGui::InputText(tr("Name"), k.theirName, sizeof k.theirName);
    ImGui::SameLine(); ImGui::SetNextItemWidth(50 * gUi); ImGui::InputText(tr("RST sent"), k.rst, sizeof k.rst);
    ImGui::SameLine(); ImGui::SetNextItemWidth(50 * gUi); ImGui::InputText(tr("RST rcvd"), k.rstRcvd, sizeof k.rstRcvd);

    // macros: they add to the buffer, nothing is sent until Send
    ImGui::TextDisabled("%s", tr("Macros:"));
    for (const auto& m : ham::kbdDefaultMacros()) {
        ImGui::SameLine();
        if (ImGui::SmallButton(m.name)) {
            ham::KbdStation s = k.station();
            s.myCall = c.call; s.myGrid = c.grid;
            std::string out, miss;
            if (!ham::kbdExpand(m.text, s, out, &miss)) c.status = std::string("Fill in first: ") + miss;
            else {
                c.status.clear();
                std::string t = std::string(k.buf) + out;
                snprintf(k.buf, sizeof k.buf, "%s", t.substr(0, ham::kKbdMaxChars).c_str());
            }
        }
    }

    // the type-ahead buffer: type while a burst is playing, Send when it is done
    ImGui::InputTextMultiline("##kbdtx", k.buf, sizeof k.buf, ImVec2(-1, 3.2f * ImGui::GetTextLineHeightWithSpacing()));
    ImGui::TextDisabled("%zu / %zu", strlen(k.buf), ham::kKbdMaxChars);
    const bool running = c.txState == 1;
    ImGui::SameLine();
    ImGui::BeginDisabled(running || !c.stationOk || !c.armed || k.buf[0] == 0);
    if (ImGui::Button(tr("Send"))) {
        std::string text = k.buf;
        k.buf[0] = 0;
        k.sending = text;
        ham::HamTxCheck g;
        g.licenceConfirmed = c.licence; g.armed = c.armed; g.call = c.call; g.grid = c.grid; g.dialHz = a.freqMhz * 1e6;
        c.txState = 1;
        const double hz = k.hz, lv = c.level, wpm = k.wpm;
        // the context lives one frame; the transmitter, the duty counter and the flags live as long as the program
        c.worker = std::thread([&tx = c.tx, &duty = k.duty, &mu = c.mu, &txErr = c.txErr, &txState = c.txState, g, p, text, hz, lv, wpm, now] {
            std::string err;
            const bool ok = ham::kbdSend(tx, duty, g, p, text, hz, lv, wpm, now, err);
            std::lock_guard<std::mutex> l(mu);
            txErr = err;
            txState = ok ? 2 : 3;
        });
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Sends the buffer as one transmission. Needs Arm; a missing licence or a frequency outside the amateur bands is a warning, not a lock."));
    ImGui::SameLine();
    if (ImGui::Button(tr("Clear"))) k.buf[0] = 0;
    ImGui::SameLine();
    ImGui::BeginDisabled(!c.stationOk || k.theirCall[0] == 0);
    if (ImGui::Button(tr("Log QSO"))) {
        ham::KbdStation s = k.station();
        s.myCall = c.call; s.myGrid = c.grid;
        c.logQso(ham::kbdLogRecord(p, s, a.freqMhz * 1e6, k.pickedAt > 0 ? k.pickedAt : now, now, k.rst, k.rstRcvd));
        k.theirCall[0] = 0; k.theirName[0] = 0; k.pickedAt = 0;
        c.status = "Logged";
    }
    ImGui::EndDisabled();
    if (running) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", tr("TRANSMITTING")); }
    if (!k.info.empty()) ImGui::TextDisabled("%s", k.info.c_str());
}
