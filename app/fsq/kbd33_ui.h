// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The screens of FSQ and IFKP (one template, two traits): the dial-frequency presets, the speed (automatic or one of them), Listen, the waterfall of the
// audio passband with the band of the 33 tones marked on it, how often each tone position was the strongest, and the text in big letters beside
// the chat log (IFKP) or the table of messages with sender, addressee and command (FSQ). Callsigns are highlighted.
#pragma once
#include "app.h"
#include "gfx/gfx.h"
#include "i18n/i18n.h"
#include "dect2/cw/cw_morse.h"
#include "dect2/fsq/kbd33.h"
#include "dect2/fsq/kbd33_station.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../ui/font_size.h"

namespace kbd33ui {

const ImVec4 kCallCol(1.0f, 0.82f, 0.36f, 1);

// words in a row, callsigns in colour, wrapped at the right edge; a line end starts a new row
inline void richText(const std::string& text, float maxX) {
    const float spaceW = ImGui::CalcTextSize(" ").x;
    const float x0 = ImGui::GetCursorScreenPos().x;
    float x = x0;
    bool first = true, newLine = false;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\n')) { if (text[i] == '\n') newLine = true; i++; }
        size_t e = i;
        while (e < text.size() && text[e] != ' ' && text[e] != '\n') e++;
        if (e == i) break;
        const std::string w = text.substr(i, e - i);
        const float ww = ImGui::CalcTextSize(w.c_str()).x;
        if (!first) {
            if (newLine || (x + spaceW + ww > maxX && x > x0)) x = x0;
            else { ImGui::SameLine(0, spaceW); x += spaceW; }
        }
        newLine = false;
        if (dect2::cw::isCallsign(w)) ImGui::TextColored(kCallCol, "%s", w.c_str());
        else ImGui::TextUnformatted(w.c_str());
        x += ww;
        first = false;
        i = e;
    }
    if (first) ImGui::TextUnformatted("");
}

// Tr: name(), key (prefs prefix), standard (the engine reports it minus one: live()), tel(a), setSpeed(a, s), setCall(a, c), setListen..., presets
template <class Tr>
struct Ui {
    struct State {
        int speed = -1;                          // -1 automatic
        bool wasRunning = false;
        int pushedSpeed = -2;
        bool listen = false; int pushedListen = -1; float pushedVol = -1; bool pushedMute = false;
        char call[24] = {};
        bool callPushed = false;
        bool loaded = false;
        static constexpr int kRows = 160;
        int bins = 0;
        std::vector<uint32_t> px;
        uint64_t specSeq = 0;
        float lo = -120, hi = -60;
        gfx::Image* wf = nullptr;
        int wfW = 0;
        bool dirty = false;
    };
    inline static State S;

    static bool live(const App& a) { return a.rx.standard == Tr::standard() - 1; }
    static std::string key(const char* k) { return std::string(Tr::key()) + k; }

    static void load() {
        if (S.loaded) return;
        S.loaded = true;
        S.speed = (int)plat::prefs().getI(key("Speed").c_str(), -1);
        if (S.speed < -1 || S.speed >= dect2::kbd33::speedCount(Tr::ifkp())) S.speed = -1;
        S.listen = plat::prefs().getB(key("Listen").c_str(), false);
        snprintf(S.call, sizeof S.call, "%s", plat::prefs().getS(key("Call").c_str(), "").c_str());
    }

    static double absKhz(const App& a, double offHz) { return a.freqMhz * 1000.0 + offHz / 1000.0; }

    static void controls(App& a) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", tr("Band")); flowNext(6 * gUi);
        int n = 0;
        const typename Tr::Preset* p = Tr::presets(n);
        for (int i = 0; i < n; i++) {
            const bool here = std::fabs(a.freqMhz - p[i].mhz) < 0.002;
            if (here) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
            char b[48]; snprintf(b, sizeof b, "%s##%sp%d", p[i].name, Tr::key(), i);
            if (ImGui::Button(b)) tuneFreq(a, p[i].mhz);
            if (here) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.4f MHz", p[i].mhz);
            flowNext(4 * gUi);
        }
        ImGui::TextDisabled("|"); flowNext(8 * gUi);
        ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Speed")); ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(90 * gUi);
        char cur[24];
        if (S.speed >= 0) snprintf(cur, sizeof cur, "%s", dect2::kbd33::speedName(Tr::ifkp(), S.speed)); else snprintf(cur, sizeof cur, "%s", tr("Auto"));
        char id[24]; snprintf(id, sizeof id, "##%ssp", Tr::key());
        if (ImGui::BeginCombo(id, cur)) {
            for (int v = -1; v < dect2::kbd33::speedCount(Tr::ifkp()); v++) {
                char l[24];
                if (v >= 0) snprintf(l, sizeof l, "%s", dect2::kbd33::speedName(Tr::ifkp(), v)); else snprintf(l, sizeof l, "%s", tr("Auto"));
                if (ImGui::Selectable(l, v == S.speed)) { S.speed = v; plat::prefs().setI(key("Speed").c_str(), v); }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr::speedTip());
        flowNext(8 * gUi);
        if (!Tr::ifkp()) {
            ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", tr("Callsign")); ImGui::SameLine(0, 5 * gUi);
            ImGui::SetNextItemWidth(90 * gUi);
            char cid[24]; snprintf(cid, sizeof cid, "##%scall", Tr::key());
            if (ImGui::InputText(cid, S.call, sizeof S.call, ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank)) { plat::prefs().setS(key("Call").c_str(), S.call); S.callPushed = false; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Your callsign: lines addressed to it, to allcall or to cqcqcq are marked"));
            flowNext(8 * gUi);
        }
        listenControls(a, S.listen, key("Listen").c_str(), Tr::key(), tr("Play the sideband (dial + 200 Hz to 3.8 kHz) on the sound card; decoding is the same either way"));
        flowNext(8 * gUi);
        char cl[48]; snprintf(cl, sizeof cl, "%s##%s", tr("Clear"), Tr::key());
        if (ImGui::Button(cl)) Tr::clear(a);
        flowEnd();
    }

    static void follow(const dect2::Kbd33Telemetry& t) {
        if (t.spectrum.empty() || t.specSeq == S.specSeq) return;
        S.specSeq = t.specSeq;
        const int W = (int)t.spectrum.size();
        if (W != S.bins) { S.bins = W; S.px.assign((size_t)W * State::kRows, 0xFF000000u); }
        std::vector<float> srt(t.spectrum);
        std::nth_element(srt.begin(), srt.begin() + W / 2, srt.end());
        const float med = srt[(size_t)W / 2];
        S.lo += (med - 3 - S.lo) * 0.2f;
        S.hi += (med + 36 - S.hi) * 0.2f;
        std::copy_backward(S.px.begin(), S.px.end() - W, S.px.end());
        for (int k = 0; k < W; k++) S.px[(size_t)k] = Waterfall::jet((t.spectrum[(size_t)k] - S.lo) / std::max(1.f, S.hi - S.lo));
        S.dirty = true;
    }

    static void waterfall(App& a, float h) {
        const dect2::Kbd33Telemetry& t = Tr::tel(a);
        const bool on = live(a);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float l = p0.x, r = p0.x + w, top = p0.y, bot = p0.y + h - ImGui::GetTextLineHeight() - 4 * gUi;
        dl->AddRectFilled(p0, ImVec2(r, p0.y + h), IM_COL32(8, 9, 10, 255));
        const double lo = t.hiHz > t.loHz ? t.loHz : 100, hi = t.hiHz > t.loHz ? t.hiHz : 3900;
        auto X = [&](double hz) { return (float)(l + (hz - lo) / (hi - lo) * (r - l)); };
        if (gGfx && S.bins > 0 && (!S.wf || S.wfW != S.bins)) { delete S.wf; S.wf = gGfx->createImage(S.bins, State::kRows, 0xFF000000u); S.wfW = S.bins; }
        if (S.wf && S.dirty) { S.wf->update(0, 0, S.bins, State::kRows, S.px.data()); S.dirty = false; }
        if (S.wf && on) dl->AddImage(S.wf->texture(), ImVec2(l, top), ImVec2(r, bot));
        const ImU32 txt = IM_COL32(120, 124, 128, 255);
        char b[48];
        for (int k = 500; k <= 3500; k += 500) {   // audio Hz above the dial
            const float x = X(k);
            dl->AddLine(ImVec2(x, bot), ImVec2(x, bot + 4 * gUi), txt);
            snprintf(b, sizeof b, "%d", k);
            dl->AddText(ImVec2(x - ImGui::CalcTextSize(b).x / 2, bot + 3 * gUi), txt, b);
        }
        if (on && t.speed >= 0 && t.state >= 1) {   // the band the receiver is on: the tones heard, or the grid's middle 33 until enough are
            double f0 = t.occLoHz, f1 = t.occHiHz + t.spacingHz;
            if (f1 <= f0 + t.spacingHz * 1.5) { f0 = t.gridHz + 20 * t.spacingHz; f1 = f0 + t.bandHz; }
            const float x0 = X(f0), x1 = X(f1);
            const ImU32 c = IM_COL32(120, 230, 140, 230);
            dl->AddRect(ImVec2(x0, top + 1), ImVec2(x1, bot - 1), c, 0, 0, 1.5f);
            snprintf(b, sizeof b, "%s %s", Tr::name(), dect2::kbd33::speedName(Tr::ifkp(), t.speed));
            dl->AddText(ImVec2(x0 + 4, top + 3), c, b);
        }
        if (!on) dl->AddText(ImVec2(l + 10, top + 10), txt, a.engine.running() ? tr("starting") : tr("start the receiver to read the band"));
        char bid[32]; snprintf(bid, sizeof bid, "##%swf", Tr::key());
        ImGui::InvisibleButton(bid, ImVec2(w, h));
        if (on && ImGui::IsItemHovered()) {
            const double hz = lo + (ImGui::GetIO().MousePos.x - l) / std::max(1.f, r - l) * (hi - lo);
            ImGui::SetTooltip(tr("%.0f Hz above the dial (%.3f kHz)"), hz, absKhz(a, hz));
        }
    }

    // the tone positions: one bar each
    static void tones(App& a, float h) {
        const dect2::Kbd33Telemetry& t = Tr::tel(a);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(12, 14, 16, 255));
        const int n = (int)t.toneUse.size();
        if (live(a) && n > 0) {
            float mx = 1e-6f;
            for (float v : t.toneUse) mx = std::max(mx, v);
            const float bw = w / n;
            const double sp = std::max(1.0, t.spacingHz);
            for (int i = 0; i < n; i++) {
                const float v = std::max(0.f, std::min(1.f, t.toneUse[(size_t)i] / mx));
                const double f = t.gridHz + i * sp;
                const bool inBand = t.occHiHz > 0 && f >= t.occLoHz - 0.5 * sp && f <= t.occHiHz + 0.5 * sp;
                dl->AddRectFilled(ImVec2(p0.x + i * bw + 1, p0.y + h - 2 - v * (h - 4)), ImVec2(p0.x + (i + 1) * bw - 1, p0.y + h - 2), inBand ? IM_COL32(110, 200, 130, 230) : IM_COL32(120, 130, 140, 200));
            }
        }
        char bid[32]; snprintf(bid, sizeof bid, "##%stones", Tr::key());
        ImGui::InvisibleButton(bid, ImVec2(w, h));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The 73 tone positions watched: how often each was the strongest tone. The 33 tones of the band are the ones that light up."));
    }

    static void messages(App& a, const dect2::Kbd33Telemetry& t, float w) {
        char id[32]; snprintf(id, sizeof id, "##%smsg", Tr::key());
        if (ImGui::BeginTable(id, 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersOuter, ImVec2(w, 0))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(tr("Time")); ImGui::TableSetupColumn(tr("From")); ImGui::TableSetupColumn(tr("To"));
            ImGui::TableSetupColumn(tr("Cmd")); ImGui::TableSetupColumn(tr("Text"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            ImGui::PushFont(a.mono, 0);
            for (const auto& m : t.msgs) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const int s = (int)m.timeSec;
                ImGui::TextDisabled("%d:%02d", s / 60, s % 60);
                ImGui::TableNextColumn();
                if (m.from.empty()) ImGui::TextDisabled("-"); else ImGui::TextColored(m.crcOk ? kCallCol : ImVec4(0.7f, 0.7f, 0.7f, 1), "%s", m.from.c_str());
                ImGui::TableNextColumn();
                if (m.to.empty()) ImGui::TextDisabled("-"); else if (m.toMe) ImGui::TextColored(pal::okGreen(), "%s", m.to.c_str()); else ImGui::TextUnformatted(m.to.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%c", m.trigger);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n\n%s", dect2::fsqTriggerMeaning(m.trigger), tr("The command character after the addressee: ? asks for the signal report, * for an acknowledgement, $ for the heard list, @ for the location, & for the message of the day, ^ for the software version, ; relays the message, ! and ~ repeat it, # stores it, + asks for a file, | is an alert, > and < change the speed"));
                ImGui::TableNextColumn();
                ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(m.text.c_str()); ImGui::PopTextWrapPos();
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
            ImGui::PopFont();
            ImGui::EndTable();
        }
    }

    static void textView(App& a) {
        const dect2::Kbd33Telemetry& t = Tr::tel(a);
        ImGui::PushFont(a.mono, 0);
        if (t.state >= 1 && t.speed >= 0) {
            ImGui::TextColored(pal::okGreen(), "%s %s", Tr::name(), dect2::kbd33::speedName(Tr::ifkp(), t.speed));
            ImGui::SameLine(0, 14 * gUi); ImGui::Text("%.2g Bd  %.0f Hz", t.baud, t.bandHz);
            ImGui::SameLine(0, 14 * gUi); ImGui::Text("SNR %.0f dB", t.snrDb);
            ImGui::SameLine(0, 14 * gUi); ImGui::Text("AFC %+.1f Hz", t.cfoHz);
            for (const auto& c : t.calls) { ImGui::SameLine(0, 10 * gUi); ImGui::TextColored(kCallCol, "%s", c.c_str()); }
        } else {
            ImGui::TextDisabled("%s", tr("searching"));
        }
        ImGui::PopFont();
        const float avW = ImGui::GetContentRegionAvail().x;
        const float logW = avW > 700 * gUi ? avW * 0.46f : 0;
        if (logW > 0) {
            if (!Tr::ifkp()) {
                messages(a, t, logW);
            } else {   // the chat log: one row per line
                char id[32]; snprintf(id, sizeof id, "##%slog", Tr::key());
                if (ImGui::BeginTable(id, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersOuter, ImVec2(logW, 0))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn(tr("Time")); ImGui::TableSetupColumn(tr("Line"), ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();
                    ImGui::PushFont(a.mono, 0);
                    for (const auto& l : t.lines) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        const int s = (int)l.timeSec;
                        ImGui::TextDisabled("%d:%02d", s / 60, s % 60);
                        ImGui::TableNextColumn();
                        ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(l.text.c_str()); ImGui::PopTextWrapPos();
                    }
                    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
                    ImGui::PopFont();
                    ImGui::EndTable();
                }
            }
            ImGui::SameLine();
        }
        char cid[32]; snprintf(cid, sizeof cid, "##%sbig", Tr::key());
        if (ImGui::BeginChild(cid, ImVec2(0, 0), ImGuiChildFlags_Borders)) {
            ImGui::PushFont(a.mono, fontBase() * 1.6f);
            richText(t.text, ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x);
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
            ImGui::PopFont();
        }
        ImGui::EndChild();
    }

    // ---------------------------------------------------------------- hooks

    static void tick(App& a) {
        load();
        if (!a.engine.running()) { S.wasRunning = false; S.callPushed = false; return; }
        if (live(a)) follow(Tr::tel(a));
        if (!S.wasRunning || S.speed != S.pushedSpeed) { Tr::setSpeed(a, S.speed); S.pushedSpeed = S.speed; }
        if (!S.wasRunning || !S.callPushed) { Tr::setCall(a, S.call); S.callPushed = true; }
        if (!S.wasRunning || (int)S.listen != S.pushedListen) { Tr::setListen(a, S.listen); S.pushedListen = S.listen; }
        if (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute) {   // the sound controls are shared with the other modes
            Tr::setSound(a, a.volume, a.muted);
            S.pushedVol = a.volume; S.pushedMute = a.muted;
        }
        S.wasRunning = true;
    }

    static void tab(App& a) {
        load();
        controls(a);
        ImGui::Spacing();
        const float av = ImGui::GetContentRegionAvail().y;
        waterfall(a, std::max(90.f, av * 0.34f));
        ImGui::Spacing();
        if (!live(a)) return;
        tones(a, std::max(36.f * gUi, av * 0.1f));
        ImGui::Spacing();
        textView(a);
    }

    static void list(App& a) {
        if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
        const auto& calls = Tr::tel(a).calls;
        if (calls.empty()) { ImGui::TextDisabled("%s", tr("no callsigns yet")); return; }
        ImGui::PushFont(a.mono, 0);
        for (const auto& c : calls) ImGui::TextColored(kCallCol, "%s", c.c_str());
        ImGui::PopFont();
    }

    static void receiver(App& a) {
        if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
        const dect2::Kbd33Telemetry& t = Tr::tel(a);
        auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
        kv(tr("state"), "%s", dect2::kbd33Summary(t, Tr::name()).c_str());
        if (t.speed >= 0) {
            kv(tr("speed"), "%s (%.2g baud)", dect2::kbd33::speedName(Tr::ifkp(), t.speed), t.baud);
            kv(tr("bandwidth"), "%.0f Hz", t.bandHz);
            kv(tr("tone spacing"), "%.3f Hz", t.spacingHz);
            kv(tr("tones heard"), "%.1f .. %.1f Hz (%.3f kHz), AFC %+.1f Hz", t.occLoHz, t.occHiHz + t.spacingHz, absKhz(a, t.occLoHz), t.cfoHz);
            kv(tr("tone quality"), "%.2f", t.quality);
        }
        kv(tr("characters"), "%llu, %llu %s", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, tr("unclear"));
        kv(tr("input"), "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
        kv(tr("signal time"), "%.1f s", t.timeSec);
    }

    static void status(App& a) {
        const bool run = a.engine.running(), on = live(a);
        const SignalStats& st = a.spec.stats;
        const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
        lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
        lamp(tr("Signal"), !on ? 0 : Tr::tel(a).state >= 1 ? 1 : 0); flowNext(10 * gUi);
        lamp(tr("Text"), !on ? 0 : Tr::tel(a).dataValid ? 1 : 0); flowNext(10 * gUi);
        ImGui::TextDisabled("|"); flowNext(10 * gUi);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::kbd33Summary(Tr::tel(a), Tr::name()).c_str()); ImGui::PopFont();
    }

    static void summary(const App& a, std::string& l1, std::string& l2) {
        l1 = Tr::name();
        if (live(a)) l2 = dect2::kbd33Summary(Tr::tel(a), Tr::name());
    }

    static ModeUi make() {
        ModeUi u;
        u.sideTitle = "CALLSIGNS";
        u.tabName = Tr::name();
        u.tabIcon = Ic::Radio;
        u.tab = tab;
        u.receiver = receiver;
        u.list = list;
        u.status = status;
        u.summary = summary;
        u.tick = tick;
        return u;
    }
};

} // namespace kbd33ui
