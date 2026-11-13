// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DECT screens: the base stations heard on a carrier (RFPI, manufacturer code, level, slot, how often), the system information they send and the
// burst log. Activity and signalling only: the voice is encrypted and its codec is not decoded, and nothing is sent.
#include "app.h"
#include "dect2/dect/dect_proto.h"
#include "dect2/dect/dect_rx.h"
#include "dect2/dect/dect_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>

namespace {

using dect2::DectBase;

struct State {
    int view = 0;               // 0 base stations, 1 burst log
    uint64_t sel = 0;           // the selected base station (RFPI), 0: the newest
    int band = 0;               // the regional band the carrier buttons and the scan use (dect::band())
    bool scan = false;          // stepping through the band's carriers, a second and a half each, staying on one that is heard
    double nextStep = 0;
    int scanAt = 0;
};
State S;

const char* bandName(int b) {
    switch (b) {
    case 1: return tr("US / Canada 1920-1930 MHz (DECT 6.0)");
    case 2: return tr("Brazil 1910-1920 MHz");
    case 3: return tr("Latin America 1910-1930 MHz");
    case 4: return tr("US ISM 902-928 MHz (DECT in ISM)");
    default: return tr("Europe 1880-1900 MHz");
    }
}
const char* bandWhere(int b) {
    switch (b) {
    case 1: return tr("USA, Canada: the UPCS band, DECT 6.0 phones");
    case 2: return tr("Brazil");
    case 3: return tr("Several Latin American countries");
    case 4: return tr("USA, standard DECT in the 900 MHz ISM band: rare, most 900 MHz cordless phones are not DECT");
    default: return tr("Europe, most of Asia, Africa, Australia: the common DECT band");
    }
}

// empty when the radio reaches the band; else the message that it cannot (also through a converter: radioRangeWarning applies it)
std::string reachMessage(const App& a, int band) {
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) return "";
    const DeviceInfo& d = a.devices[(size_t)a.devIdx];
    if (!d.isRadio()) return "";
    const double lo = dect::bandLowMhz(band), hi = dect::bandHighMhz(band);
    if (radioRangeWarning(d, lo, &a.tune.radio).empty() && radioRangeWarning(d, hi, &a.tune.radio).empty()) return "";
    char b[400];
    snprintf(b, sizeof b, tr("Your radio cannot tune %.3f - %.3f MHz (the %s band); its range is %.6g - %.6g MHz. Use a radio that reaches it (HackRF, PlutoSDR, LimeSDR, BladeRF) or a converter set under the radio's settings."),
             lo, hi, bandName(band), d.minFreqHz / 1e6, d.maxFreqHz / 1e6);
    return b;
}

bool live(const App& a) { return a.rx.standard == 79; }   // the engine reports its standard code minus one

std::string clockText(int64_t wall) {
    char b[32];
    const time_t t = (time_t)wall;
    struct tm g {};
    dect2::gmTime(t, &g);
    snprintf(b, sizeof b, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

std::string ago(double now, double sec) {
    const double d = now - sec;
    char b[32];
    if (d < 1.5) return "now";
    if (d < 90) { snprintf(b, sizeof b, "%.0f s ago", d); return b; }
    snprintf(b, sizeof b, "%.0f min ago", d / 60);
    return b;
}

const DectBase* selected(const dect2::DectTelemetry& t) {
    if (t.bases.empty()) return nullptr;
    if (S.sel) for (const DectBase& b : t.bases) if (b.rfpi == S.sel) return &b;
    return &t.bases.front();
}

const char* taName(int ta) {
    switch (ta) {
    case 0: case 1: return "control (Ct)";
    case 2: return "identities (Nt)";
    case 3: return "identities (Nt, connectionless)";
    case 4: return "system information (Qt)";
    case 5: return "escape";
    case 6: return "multiframe sync (Mt)";
    case 7: return "paging (Pt)";
    default: return "?";
    }
}

void basesView(App& a) {
    const dect2::DectTelemetry& t = a.rx.dect;
    const DectBase* b = selected(t);
    if (!b) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("No base station heard yet. A DECT base (a cordless phone's charging stand, a router with a phone, a DECT repeater) sends a beacon every 10 ms on one of ten carriers, 1881.792 - 1897.344 MHz in Europe (1921.536 - 1928.448 MHz in the US and Canada). Pick the band and tune to a carrier, or scan the band (the Receiver tab).");
        ImGui::PopTextWrapPos();
        return;
    }
    const auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    ImGui::PushFont(a.mono, 0);
    ImGui::Text("RFPI %s", dect::hex40(b->rfpi).c_str());
    ImGui::PopFont();
    ImGui::Spacing();
    if (b->emc >= 0) kv("manufacturer code", "%d (0x%04X)", b->emc, b->emc); else kv("manufacturer code", "%s", "not a standard public RFPI");
    if (b->fpn >= 0) kv("number in the system", "%d", b->fpn);
    kv("access rights class", "%d", b->arc);
    kv("beacons", "%llu good (%d identity, %d system information)", (unsigned long long)b->frames, b->ntSeen, b->qtSeen);
    kv("level", "%.0f dBFS", b->rssi);
    {
        std::string slots;
        for (int s = 0; s < dect::kSlots; s++) if (b->slotsMask >> s & 1) { if (!slots.empty()) slots += ", "; slots += std::to_string(s); }
        kv("slots used", "%s%s", slots.c_str(), __builtin_popcount(b->slotsMask) > 2 ? " (several: a call or data link is up)" : "");
    }
    if (b->qtSeen) kv("system information", "%s", dect::hex40(b->qtLast).c_str());
    kv("first heard", "%s%s", clockText(b->firstWall).c_str(), dect2::timefmt::sfx());
    kv("last heard", "%s", ago(t.timeSec, b->lastSec).c_str());
}

void logView(App& a) {
    const dect2::DectTelemetry& t = a.rx.dect;
    if (!ImGui::BeginTable("##dectlog", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn("From"); ImGui::TableSetupColumn("dBFS"); ImGui::TableSetupColumn("Tail", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Content");
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    for (const dect2::DectLog& l : t.log) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clockText(l.wall).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(l.fromBase ? "base" : "handset");
        ImGui::TableNextColumn(); ImGui::Text("%.0f", l.rssi);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(taName(l.ta));
        ImGui::TableNextColumn(); ImGui::TextUnformatted(dect::hex40(l.tail).c_str());
    }
    ImGui::PopFont();
    ImGui::EndTable();
}

void tab(App& a) {
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver on a DECT carrier (Europe 1881.792 - 1897.344 MHz, US/Canada 1921.536 - 1928.448 MHz) to hear base stations"); ImGui::PopTextWrapPos(); return; }
    ImGui::RadioButton("Base stations##dectv", &S.view, 0); ImGui::SameLine();
    ImGui::RadioButton("Bursts##dectv", &S.view, 1);
    ImGui::Separator();
    if (S.view == 1) logView(a); else basesView(a);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Activity and signalling only: calls are encrypted and the voice is not decoded. Nothing is sent.");
    ImGui::PopTextWrapPos();
}

void list(App& a) {
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see base stations"); ImGui::PopTextWrapPos(); return; }
    const dect2::DectTelemetry& t = a.rx.dect;
    if (resultsClearButton("dect")) a.engine.dect().clearResults();
    ImGui::TextDisabled("%zu base stations, %llu beacons", t.bases.size(), (unsigned long long)t.blocksOk);
    if (!ImGui::BeginTable("##dect_l", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("RFPI", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("dBFS"); ImGui::TableSetupColumn("Heard");
    ImGui::TableHeadersRow();
    const DectBase* sel = selected(t);
    for (const DectBase& b : t.bases) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID((int)(b.rfpi & 0x7FFFFFFF));
        if (ImGui::Selectable("##r", &b == sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.sel = b.rfpi;
        ImGui::PopID();
        ImGui::SameLine(0, 0);
        ImGui::PushFont(a.mono, 0);
        ImGui::TextUnformatted(dect::hex40(b.rfpi).c_str());
        ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::Text("%.0f", b.rssi);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(t.timeSec, b.lastSec).c_str());
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::DectTelemetry& t = a.rx.dect;
    const auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("state", "%s", t.state ? "bursts heard" : "nothing heard lately");
    if (t.carrier >= 0) kv("carrier", "%d (%.3f MHz), %s", t.carrier, t.channelMhz, bandName(t.band));
    else kv("channel", "%s", t.channelMhz > 0 ? (std::to_string(t.channelMhz) + " MHz, not a DECT carrier").c_str() : "a file");
    kv("bursts", "%llu from bases, %llu from handsets", (unsigned long long)t.baseBursts, (unsigned long long)t.handsetBursts);
    kv("A-fields", "%llu good, %llu bad CRC", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kv("CRC form", "%llu plain, %llu complemented", (unsigned long long)t.crcPlain, (unsigned long long)t.crcInverted);
    kv("signal", "eye %.1f dB, carrier error %+.0f Hz", t.snrDb, t.cfoHz);
    kv("input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const dect2::DectTelemetry& t = a.rx.dect;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Bursts", !on ? 0 : t.state ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Bases"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0);
    if (on) ImGui::Text("%zu", t.bases.size()); else ImGui::TextUnformatted(run ? "starting" : "stopped");
    ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "DECT cordless phones";
    if (live(a)) l2 = dect2::dectSummary(a.rx.dect);
}

void tuner(App& a, bool& retune) {
    if (S.band < 0 || S.band >= dect::kBands) S.band = 0;
    ImGui::TextDisabled("%s", tr("Band"));
    ImGui::SameLine(0, 4 * gUi);
    ImGui::SetNextItemWidth(std::min(300 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##dectband", bandName(S.band))) {
        for (int b = 0; b < dect::kBands; b++) {
            if (ImGui::Selectable(bandName(b), b == S.band)) { S.band = b; S.scan = false; tuneFreq(a, dect::carrierMhz(b, 0)); retune = true; }   // a new band retunes to its first carrier
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", bandWhere(b));
        }
        ImGui::EndCombo();
    }
    const dect::Band& bd = dect::band(S.band);
    ImGui::TextDisabled("%s", tr("Carrier"));
    for (int n = 0; n < bd.count; n++) {
        ImGui::SameLine(0, 4 * gUi);
        char l[16]; snprintf(l, sizeof l, "%d##dc%d", n, n);
        if (ImGui::SmallButton(l)) { tuneFreq(a, dect::carrierMhz(S.band, n)); retune = true; S.scan = false; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("DECT carrier %d, %.3f MHz"), n, dect::carrierMhz(S.band, n));
    }
    ImGui::SameLine(0, 8 * gUi);
    if (ImGui::SmallButton(S.scan ? tr("Stop scan##dsc") : tr("Scan band##dsc"))) { S.scan = !S.scan; S.scanAt = 0; S.nextStep = 0; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Steps through the carriers of the band, 1.5 s each, and stays on one while bursts are heard."));
    if (S.scan && a.engine.running()) {
        double plan[32];
        const int k = dect::scanPlan(S.band, plan, 32);
        const double now = ImGui::GetTime();
        const bool heard = live(a) && a.rx.dect.state;
        if (now >= S.nextStep && !heard && k > 0) {
            tuneFreq(a, plan[S.scanAt % k]); retune = true;
            S.scanAt++;
        }
        if (now >= S.nextStep) S.nextStep = now + 1.5;
    }
    const std::string why = reachMessage(a, S.band);
    if (!why.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.2f, 1), "%s", why.c_str()); ImGui::PopTextWrapPos(); }
}

void synth(App& a, bool& changed) {
    static const char* names[] = {"Two base stations with a handset each", "One base station", "Noise only"};
    int& o = a.tune.synth.modeOpt[0];
    if (o < 0 || o > 2) o = 0;
    ImGui::SetNextItemWidth(std::min(330 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##dectsynth", names[o])) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(names[i], i == o)) { o = i; changed = true; }
        ImGui::EndCombo();
    }
}

} // namespace

extern const ModeUi kDectUi;
const ModeUi kDectUi = {
    .sideTitle = "BASES",
    .tabName = "Bases",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .synth = synth,
};
