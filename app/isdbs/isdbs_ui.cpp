// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ISDB-S (BS satellite television) screens: status bar, the analysis row (constellation, Es/N0), the Receiver tab with the TMCC stream table, the
// tuner (BS channel, stream to decode) and the test signal options. The TV tab, services and the player are the shared ones (the decoded
// transport stream goes there like DVB-S's).
#include "app.h"
#include "dect2/isdbs/isdbs_rx.h"
#include <cmath>
#include <deque>

namespace {

struct State {
    uint64_t lastSeq = 0;
    bool wasRunning = false;
    int sel = -1;                 // relative stream to decode, -1 the biggest
    int pushedSel = -2;
    std::deque<float> snr;
};
State S;

bool live(const App& a) { return a.rx.standard == 155; }

const ImVec4 kDim(0.6f, 0.64f, 0.68f, 1), kGood(0.35f, 0.90f, 0.45f, 1), kWarn(0.95f, 0.60f, 0.25f, 1), kBad(0.95f, 0.45f, 0.30f, 1), kText(0.93f, 0.95f, 0.97f, 1);
const char* kNote = "ISDB-S is the BS satellite TV of Japan (12 GHz, through an LNB). It needs a radio that samples 39 Msample/s or more (a HackRF or RTL-SDR is too slow) "
                    "and a dish aimed at 110 degrees east. Only streams sent in the clear are decoded: scrambled (conditional access) streams stay scrambled. "
                    "Written from public descriptions and tried only on a generated test signal.";

void tick(App& a) {
    const bool run = a.engine.running();
    if (run && (!S.wasRunning || S.pushedSel != S.sel)) { a.engine.isdbs().selectStream(S.sel); S.pushedSel = S.sel; }
    S.wasRunning = run;
    if (!live(a) || a.rx.seq == S.lastSeq) return;
    S.lastSeq = a.rx.seq;
    if (a.rx.isdbs.locked) { S.snr.push_back(a.rx.isdbs.snrDb); if (S.snr.size() > 600) S.snr.pop_front(); }
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const IsdbsTelemetry& t = a.rx.isdbs;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Frame", !on ? 0 : t.locked ? 1 : 0); flowNext(12 * gUi);
    lamp("TMCC", !on ? 0 : t.tmcc.valid ? 1 : t.locked ? 2 : 0); flowNext(12 * gUi);
    lamp("RS", !on ? 0 : t.packets == 0 ? 0 : t.rsFailed * 10 > t.packets ? 3 : t.rsFailed ? 2 : 1); flowNext(12 * gUi);
    lamp("TS", !run ? 0 : a.ts.services.empty() ? 0 : 1, (int)Ic::Layers); flowNext(12 * gUi);
    const PlayerStats ps = a.engine.player().stats();
    const bool pl = run && ps.active;
    lamp("Video", !pl || !ps.hasVideo ? 0 : (ps.shown > 0 && ps.videoQueue > 2) ? 1 : 2, (int)Ic::Tv); flowNext(12 * gUi);
    lamp("Audio", !pl || !ps.hasAudio ? 0 : (ps.audioBufferMs > 150) ? 1 : 2, (int)Ic::Speaker); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val, ImVec4 col = kText) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextColored(col, "%s", val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[96];
    if (!run) ro("State", "stopped", kDim);
    else if (a.engine.radioLost()) ro("State", "radio disconnected", kBad);
    else if (!on) ro("State", "starting", kDim);
    else if (!a.rx.rateOk) ro("State", "sample rate too low", kBad);
    else if (t.state >= 4) ro("State", "Locked", kGood);
    else if (t.tmcc.valid) ro("State", "TMCC decoded", ImVec4(0.95f, 0.85f, 0.35f, 1));
    else if (t.locked) ro("State", "Frame sync", ImVec4(0.95f, 0.85f, 0.35f, 1));
    else ro("State", "Searching", kDim);
    if (on && t.locked) {
        snprintf(b, sizeof b, "%.1f dB", t.snrDb); ro("Es/N0", b);
        snprintf(b, sizeof b, "%+.0f Hz", t.cfoHz); ro("CFO", b);
        snprintf(b, sizeof b, "%llu / %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); ro("RS ok/bad", b, t.blocksBad ? kWarn : kText);
    }
    if (run) { snprintf(b, sizeof b, "%.3f Msps", a.engine.sampleRate() / 1e6); ro("fs", b); }
    if (run) { const SampleLoss l = a.engine.sampleLoss(); ro("dropped", lossText(l), lossColour(l)); lossTooltip(l); }
    flowBreak();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    const IsdbsTelemetry& t = a.rx.isdbs;
    l1 = "ISDB-S";
    if (!live(a)) return;
    l2 = dect2::isdbsSummary(t);
}

void history(const char* id, const std::deque<float>& d, ImVec2 sz) {
    if (!plt::BeginPlot(id, sz, plt::Flags_NoLegend | plt::Flags_NoTitle)) return;
    plt::SetupAxes("reports (~4/s)", "dB", 0, plt::AxisFlags_AutoFit);
    plt::SetupAxisLimits(plt::X1, 0, 600, plt::Cond_Always);
    if (!d.empty()) { std::vector<float> v(d.begin(), d.end()); plt::Spec sp; sp.LineColor = pal::accent(); sp.LineWeight = 1.4f; plt::PlotLine("a", v.data(), (int)v.size(), sp); }
    plt::EndPlot();
}

void panels(App& a) {
    const IsdbsTelemetry& t = a.rx.isdbs;
    const bool on = live(a);
    const float availW = ImGui::GetContentRegionAvail().x, availH = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi;
    const float plotH = std::max(90.f, availH - 26.f - ImGui::GetFrameHeight());
    const float sqW = std::min(plotH, std::max(120.f, availW * 0.30f));
    const float colW = std::max(120.f, availW - 3 * gap - sqW);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap);
    ImGui::BeginGroup();
    captionFit(sqW, "Constellation (%zu symbols)", on ? t.constellation.size() : (size_t)0);
    scatter("##isdbscon", on ? t.constellation : std::vector<cf32>(), ImVec2(sqW, sqW), 1.5, pal::accent(0.40f));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "Es/N0 (dB)");
    history("##isdbssnr", S.snr, ImVec2(colW, plotH));
    ImGui::EndGroup();
}

void receiver(App& a) {
    const IsdbsTelemetry& t = a.rx.isdbs;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("ISDB-S (ARIB STD-B20, BS satellite television): 28.86 Msymbol/s, roll-off 0.35, frames of 48 slots, a BPSK header with the TMCC, BPSK / QPSK / "
                        "trellis coded 8PSK slots, Reed-Solomon (204,188). Matched filter and timing loop, coarse carrier from the 8th power, frame sync, carrier from the "
                        "known burst symbols, then each slot of the chosen stream is decoded and the packets go to the player.");
    ImGui::TextColored(kWarn, "%s", kNote);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "Start the receiver with ISDB-S selected."); return; }
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(190 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("frame sync", "%s", t.locked ? "yes" : "searching");
    kv("carrier offset", "%+.1f Hz", t.cfoHz);
    kv("Es/N0", "%.1f dB", t.snrDb);
    kv("frames", "%llu (%llu super-frames), sync lost %llu times", (unsigned long long)t.frames, (unsigned long long)t.superFrames, (unsigned long long)t.syncLosses);
    kv("TMCC", "%llu good, %llu bad super-frames, change counter %d", (unsigned long long)t.tmccGood, (unsigned long long)t.tmccBad, t.tmcc.change);
    ImGui::Spacing();
    if (t.streams.empty()) kv("streams", "not known yet");
    for (const auto& s : t.streams)
        kv(s.rel == t.selected ? "stream (decoded)" : "stream", "%d: TS id %u, %d slots, %s, up to %.2f Mbit/s%s", s.rel, (unsigned)s.tsId, s.slots, dect2::isdbs::modName(s.mod), s.bitrate / 1e6, s.mixed ? ", slots of mixed modulation" : "");
    ImGui::Spacing();
    kv("Reed-Solomon", "%llu clean, %llu corrected, %llu failed", (unsigned long long)t.rsClean, (unsigned long long)t.rsCorrected, (unsigned long long)t.rsFailed);
    kv("transport stream", "%llu packets, %.2f Mbit/s measured", (unsigned long long)t.packets, t.tsBitrate / 1e6);
    kv("input rate", "%s", a.rx.rateOk ? "ok" : "too low for ISDB-S (39 Msample/s or more)");
}

template <size_t N> bool combo(const char* id, float w, int& v, const char* const (&names)[N]) {
    bool ch = false;
    ImGui::SetNextItemWidth(std::min(w * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo(id, names[(size_t)v < N ? (size_t)v : 0])) {
        for (size_t i = 0; i < N; i++) if (ImGui::Selectable(names[i], (size_t)v == i)) { v = (int)i; ch = true; }
        ImGui::EndCombo();
    }
    return ch;
}

void tuner(App& a, bool& retune) {
    (void)retune;
    // BS-1 to BS-23 (odd numbers): 11727.48 MHz + 38.36 MHz a step, here as the L-band IF after the usual 10.678 GHz LNB oscillator
    ImGui::TextDisabled("BS channel");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Transponders BS-1 to BS-23 (odd numbers) as the L-band IF after an LNB with a 10.678 GHz oscillator.\n%s", kNote);
    sameLineIf(90 * gUi, 5 * gUi);
    char cur[48] = "custom";
    for (int k = 0; k < 12; k++) if (std::fabs(a.freqMhz - (1049.48 + 38.36 * k)) < 1.0) snprintf(cur, sizeof cur, "BS-%d  %.2f MHz", 1 + 2 * k, 1049.48 + 38.36 * k);
    ImGui::SetNextItemWidth(std::min(160 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##isdbsch", cur)) {
        for (int k = 0; k < 12; k++) {
            const double f = 1049.48 + 38.36 * k;
            char n[48]; snprintf(n, sizeof n, "BS-%d  %.2f MHz", 1 + 2 * k, f);
            if (ImGui::Selectable(n, std::fabs(a.freqMhz - f) < 1.0)) tuneFreq(a, f);
        }
        ImGui::EndCombo();
    }
    flowNext(8 * gUi);
    ImGui::TextDisabled("Stream");
    sameLineIf(50 * gUi, 5 * gUi);
    std::string cs = S.sel < 0 ? "biggest" : "TS " + std::to_string(S.sel);
    ImGui::SetNextItemWidth(std::min(110 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##isdbssel", cs.c_str())) {
        if (ImGui::Selectable("biggest", S.sel < 0)) S.sel = -1;
        if (live(a)) for (const auto& s : a.rx.isdbs.streams) {
            char n[64]; snprintf(n, sizeof n, "%d: id %u, %d slots", s.rel, (unsigned)s.tsId, s.slots);
            if (ImGui::Selectable(n, S.sel == s.rel)) S.sel = s.rel;
        }
        ImGui::EndCombo();
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(kWarn, "%s", kNote);
    ImGui::PopTextWrapPos();
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    static const char* const layout[] = {"3 streams", "TC8PSK 48 slots", "QPSK 1/2 48 slots"};
    if (combo("##isl", 120, sc.modeOpt[0], layout)) changed = true;
    flowNext(12 * gUi); ImGui::TextDisabled("Es/N0"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(90 * gUi);
    float snr = (float)sc.snrDb; if (ImGui::SliderFloat("##issnr", &snr, 4, 40, "%.0f dB")) { sc.snrDb = snr; changed = true; }
    flowNext(8 * gUi); ImGui::TextDisabled("CFO"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(110 * gUi);
    float cfo = (float)(sc.cfoHz / 1e3); if (ImGui::SliderFloat("##iscfo", &cfo, -1500, 1500, "%.0f kHz")) { sc.cfoHz = cfo * 1e3; changed = true; }
    flowNext(8 * gUi); ImGui::TextDisabled("clock"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(90 * gUi);
    float ppm = (float)sc.modeVal[1]; if (ImGui::SliderFloat("##isppm", &ppm, -100, 100, "%.0f ppm")) { sc.modeVal[1] = ppm; changed = true; }
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const bool on = live(a);
    const IsdbsTelemetry& t = a.rx.isdbs;
    out.push_back({"Es/N0  dB", "%.1f", on && t.locked ? t.snrDb : 0, 0, 30, !on || !t.locked ? 0 : t.snrDb >= 14 ? 1 : t.snrDb >= 9 ? 2 : 3});
    out.push_back({"RS FAILED  packets", "%.0f", on ? (double)t.rsFailed : 0, 0, 1000, !on || t.packets == 0 ? 0 : t.rsFailed == 0 ? 1 : 3});
    out.push_back({"CFO  Hz", "%+.0f", on ? t.cfoHz : 0, -2000000, 2000000, 0});
    out.push_back({"PAYLOAD  Mbit/s", "%.2f", on ? t.tsBitrate / 1e6 : 0, 0, 60, 0});
}

} // namespace

extern const ModeUi kIsdbsUi;
const ModeUi kIsdbsUi = {
    .sideTitle = "SERVICES",
    .receiver = receiver,
    .stream = true,
    .panels = panels,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .synth = synth,
    .tick = tick,
    .meters = meters,
};
