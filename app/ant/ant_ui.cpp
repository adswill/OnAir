// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ANT+ screens: the sensors heard (heart rate straps, bike sensors, power meters, footpods...), the selected sensor's value over time (the
// heart rate with the beat count and R-R interval, power, speed, temperature), its pages (manufacturer, serial, battery) and the frame log.
// Receive only: only the open broadcasts a watch hears, nothing is sent, paired or acknowledged.
#include "app.h"
#include "dect2/ant/ant_proto.h"
#include "dect2/ant/ant_rx.h"
#include "dect2/ant/ant_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

using dect2::AntDevice;

struct State {
    bool loaded = false;
    int view = 0;                           // 0 sensor, 1 frames
    uint32_t selKey = 0;                    // the selected sensor (device number | type << 16 | trans << 24); 0: the newest heart rate strap, else the newest
    bool anyNet = true; int pushedAny = -1;
    int wheelMm = 2096; int pushedWheel = -1;
    float plotMin = 40, plotMax = 200;
    bool autoRange = true;
};
State S;

bool live(const App& a) { return a.rx.standard == 78; }   // the engine reports its standard code minus one

void load() {
    if (S.loaded) return;
    S.loaded = true;
    S.anyNet = plat::prefs().getB("antAnyNet", true);
    S.wheelMm = (int)plat::prefs().getI("antWheelMm", 2096);
}

uint32_t keyOf(const AntDevice& d) { return (uint32_t)d.deviceNumber | (uint32_t)d.type << 16 | (uint32_t)d.transType << 24; }

const AntDevice* selected(const dect2::AntTelemetry& t) {
    if (t.devices.empty()) return nullptr;
    if (S.selKey) for (const AntDevice& d : t.devices) if (keyOf(d) == S.selKey) return &d;
    for (const AntDevice& d : t.devices) if (d.type == dect2::ant::HeartRate) return &d;
    return &t.devices.front();
}

std::string ago(double now, double sec) {
    const double d = now - sec;
    char b[32];
    if (d < 1.5) return "now";
    if (d < 90) { snprintf(b, sizeof b, "%.0f s ago", d); return b; }
    snprintf(b, sizeof b, "%.0f min ago", d / 60);
    return b;
}

std::string clockText(int64_t wall) {
    char b[32];
    const time_t t = (time_t)wall;
    struct tm g {};
    dect2::gmTime(t, &g);
    snprintf(b, sizeof b, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

const char* mainValueName(int type) {
    switch (type) {
    case dect2::ant::HeartRate: return "Heart rate";
    case dect2::ant::BikePower: return "Power";
    case dect2::ant::BikeCadence: return "Cadence";
    case dect2::ant::BikeSpeed: case dect2::ant::BikeSpeedCadence: case dect2::ant::StrideSdm: return "Speed";
    case dect2::ant::Environment: return "Temperature";
    default: return "Value";
    }
}

void valuePlot(const App& a, const AntDevice& d, float h) {
    const dect2::AntTelemetry& t = a.rx.ant;
    const double now = t.timeSec, span = 300;   // the last five minutes
    std::vector<double> xs, ys;
    float lo = 1e9, hi = -1e9;
    for (const dect2::AntSample& s : d.history) {
        if (s.sec < now - span) continue;
        xs.push_back(s.sec - now); ys.push_back(s.value);
        lo = std::min(lo, s.value); hi = std::max(hi, s.value);
    }
    char ylabel[40];
    snprintf(ylabel, sizeof ylabel, "%s (%s)", mainValueName(d.type), d.historyUnit.c_str());
    if (plt::BeginPlot("##antplot", ImVec2(-1, h), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("seconds", ylabel, 0, 0);
        plt::SetupAxisLimits(plt::X1, -span, 0, plt::Cond_Always);
        double ymin = S.plotMin, ymax = S.plotMax;
        if (S.autoRange && hi > lo) { const float pad = std::max(5.f, (hi - lo) * 0.15f); ymin = lo - pad; ymax = hi + pad; }
        else if (S.autoRange && d.type != dect2::ant::HeartRate) { ymin = 0; ymax = d.type == dect2::ant::BikePower ? 400 : 60; }
        plt::SetupAxisLimits(plt::Y1, ymin, ymax, plt::Cond_Always);
        if (!xs.empty()) {
            plt::Spec sp; sp.LineColor = d.type == dect2::ant::HeartRate ? ImVec4(0.95f, 0.35f, 0.35f, 1.f) : pal::accent(); sp.LineWeight = 1.6f;
            plt::PlotLine("v", xs.data(), ys.data(), (int)xs.size(), sp);
        }
        plt::EndPlot();
    }
}

void sensorView(App& a) {
    const dect2::AntTelemetry& t = a.rx.ant;
    const AntDevice* d = selected(t);
    if (!d) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("No sensor heard yet. A heart rate strap or a watch broadcasting its heart rate sends four frames a second on 2457 MHz; bike sensors and footpods the same. Hold the radio's antenna within a few metres: the transmitters are weak (about 0 dBm).");
        ImGui::PopTextWrapPos();
        return;
    }
    // the headline
    ImGui::PushFont(a.mono, 0);
    if (d->type == dect2::ant::HeartRate) {
        if (d->heartRate > 0) ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.f), "%d bpm", d->heartRate); else ImGui::TextDisabled("no heart rate (the strap is not on the skin)");
        ImGui::SameLine(0, 16 * gUi);
        ImGui::Text("beat %d", d->beatCount);
        if (d->rrSec > 0) { ImGui::SameLine(0, 16 * gUi); ImGui::Text("R-R %.0f ms", d->rrSec * 1000); }
    } else if (d->type == dect2::ant::BikePower) {
        ImGui::Text("%d W", std::max(0, d->powerW));
        if (d->cadenceRpm >= 0) { ImGui::SameLine(0, 16 * gUi); ImGui::Text("%d rpm", d->cadenceRpm); }
    } else if (d->type == dect2::ant::Environment) ImGui::Text("%.2f C", d->temperatureC);
    else {
        if (d->speedKmh >= 0) ImGui::Text("%.1f km/h", d->speedKmh); else ImGui::TextDisabled("waiting for a second frame");
        if (d->cadenceRpm >= 0) { ImGui::SameLine(0, 16 * gUi); ImGui::Text("%d %s", d->cadenceRpm, d->type == dect2::ant::StrideSdm ? "strides/min" : "rpm"); }
        if (d->distanceM >= 0) { ImGui::SameLine(0, 16 * gUi); ImGui::Text("%.0f m", d->distanceM); }
    }
    ImGui::PopFont();
    ImGui::TextDisabled("%s %u, transmission type %u%s, %s, %.1f dBFS, %llu frames%s", d->typeName.c_str(), d->deviceNumber, d->transType,
                        d->marker == dect2::ant::kAntPlusMarker ? "" : " (another ANT network)", ago(t.timeSec, d->lastSec).c_str(), d->rssi,
                        (unsigned long long)d->frames, d->pairing ? ", pairing requested" : "");
    const float H = ImGui::GetContentRegionAvail().y;
    valuePlot(a, *d, std::max(120 * gUi, H * 0.45f));
    ImGui::Checkbox("Auto range##antar", &S.autoRange);
    if (!S.autoRange) { ImGui::SameLine(0, 8 * gUi); ImGui::SetNextItemWidth(180 * gUi); ImGui::DragFloatRange2("##antrange", &S.plotMin, &S.plotMax, 1, -50, 2000, "%.0f", "%.0f"); }
    ImGui::Spacing();
    const auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("last page", "%s: %s", d->lastPage.c_str(), d->lastText.c_str());
    if (!d->manufacturer.empty() || d->serial >= 0) kv("manufacturer", "%s%s%s", d->manufacturer.c_str(), d->serial >= 0 ? ", serial " : "", d->serial >= 0 ? std::to_string(d->serial).c_str() : "");
    if (d->model >= 0 || d->hwRev >= 0 || d->swRev >= 0) kv("product", "model %d, hardware %d, software %d", d->model, d->hwRev, d->swRev);
    if (d->batteryStatus >= 0) {
        if (d->batteryPct >= 0 && d->batteryV >= 0) kv("battery", "%d %%, %.2f V (%s)", d->batteryPct, d->batteryV, dect2::ant::batteryStatusName(d->batteryStatus));
        else if (d->batteryV >= 0) kv("battery", "%.2f V (%s)", d->batteryV, dect2::ant::batteryStatusName(d->batteryStatus));
        else kv("battery", "%s", dect2::ant::batteryStatusName(d->batteryStatus));
    }
    if (d->operatingTimeSec >= 0) kv("operating time", "%.1f h", d->operatingTimeSec / 3600);
    if (d->periodSec > 0) kv("message period", "%.0f ms (%.2f a second)", d->periodSec * 1000, 1 / d->periodSec);
    {
        std::string pages;
        for (int p = 0; p < 32; p++) if (d->pagesSeen >> p & 1) { if (!pages.empty()) pages += ", "; pages += std::to_string(p); }
        if (!pages.empty()) kv("pages seen", "%s", pages.c_str());
    }
    kv("first heard", "%s%s", clockText(d->firstWall).c_str(), dect2::timefmt::sfx());
}

void framesView(App& a) {
    const dect2::AntTelemetry& t = a.rx.ant;
    if (!ImGui::BeginTable("##antlog", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn("Sensor"); ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("dBFS");
    ImGui::TableSetupColumn("Payload"); ImGui::TableSetupColumn("Decoded", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    for (const dect2::AntFrameLog& f : t.log) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clockText(f.wall).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%u", f.deviceNumber);
        ImGui::TableNextColumn(); ImGui::Text("%d", f.type);
        ImGui::TableNextColumn(); ImGui::Text("%.0f", f.rssi);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::ant::hex(f.payload, 8).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(f.text.c_str());
    }
    ImGui::PopFont();
    ImGui::EndTable();
}

void tab(App& a) {
    load();
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver on 2457 MHz to hear ANT+ sensors"); ImGui::PopTextWrapPos(); return; }
    ImGui::RadioButton("Sensor##antv", &S.view, 0); ImGui::SameLine();
    ImGui::RadioButton("Frames##antv", &S.view, 1);
    ImGui::Separator();
    if (S.view == 1) framesView(a); else sensorView(a);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Receive only: the open broadcasts every ANT+ sensor sends, as a watch hears them. Nothing is sent, paired or acknowledged.");
    ImGui::PopTextWrapPos();
}

void list(App& a) {
    load();
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see sensors"); ImGui::PopTextWrapPos(); return; }
    const dect2::AntTelemetry& t = a.rx.ant;
    if (resultsClearButton("ant")) a.engine.ant().clearResults();
    ImGui::TextDisabled("%zu sensors, %llu frames", t.devices.size(), (unsigned long long)t.blocksOk);
    if (!ImGui::BeginTable("##ant_l", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Sensor", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Value"); ImGui::TableSetupColumn("Heard");
    ImGui::TableHeadersRow();
    const AntDevice* sel = selected(t);
    for (const AntDevice& d : t.devices) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID((int)keyOf(d));
        if (ImGui::Selectable("##r", &d == sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.selKey = keyOf(d);
        ImGui::PopID();
        ImGui::SameLine(0, 0);
        ImGui::Text("%s %u", d.typeName.c_str(), d.deviceNumber);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", d.lastPage.c_str(), d.lastText.c_str());
        ImGui::TableNextColumn();
        ImGui::PushFont(a.mono, 0);
        if (d.type == dect2::ant::HeartRate) { if (d.heartRate > 0) ImGui::Text("%d bpm", d.heartRate); else ImGui::TextDisabled("-"); }
        else if (d.type == dect2::ant::BikePower) ImGui::Text("%d W", std::max(0, d.powerW));
        else if (d.type == dect2::ant::Environment) ImGui::Text("%.1f C", d.temperatureC);
        else if (d.speedKmh >= 0) ImGui::Text("%.1f km/h", d.speedKmh);
        else if (d.cadenceRpm >= 0) ImGui::Text("%d rpm", d.cadenceRpm);
        else ImGui::TextDisabled("-");
        ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(t.timeSec, d.lastSec).c_str());
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::AntTelemetry& t = a.rx.ant;
    const auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("state", "%s", t.state ? "frames heard" : "nothing heard lately");
    kv("channel", "%s", t.channelMhz > 0 ? (std::to_string((int)std::lround(t.channelMhz)) + " MHz" + (std::fabs(t.channelMhz - dect2::ant::kChannelMhz) < 0.5 ? " (ANT+)" : "")).c_str() : "a file");
    kv("frames", "%llu good, %llu bad CRC, %llu preamble hits", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.preambleHits);
    kv("signal", "eye %.1f dB, carrier error %+.0f Hz, deviation %.0f kHz", t.snrDb, t.cfoHz, t.deviationHz / 1e3);
    kv("input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    ImGui::Spacing();
    if (ImGui::Checkbox("Also other ANT networks (not only ANT+)", &S.anyNet)) plat::prefs().setB("antAnyNet", S.anyNet);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("ANT+ devices share one network marker; private ANT networks use their own, found by the frame's checksum");
    ImGui::SetNextItemWidth(120 * gUi);
    if (ImGui::InputInt("Wheel circumference, mm", &S.wheelMm)) { S.wheelMm = std::max(500, std::min(3000, S.wheelMm)); plat::prefs().setI("antWheelMm", S.wheelMm); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("For the bike speed sensors: 2096 mm is a 700x23c road wheel");
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const dect2::AntTelemetry& t = a.rx.ant;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Frames", !on ? 0 : t.state ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Sensors"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0);
    if (on) ImGui::Text("%zu", t.devices.size()); else ImGui::TextUnformatted(run ? "starting" : "stopped");
    ImGui::PopFont();
    const AntDevice* d = on ? selected(t) : nullptr;
    if (d && d->type == dect2::ant::HeartRate && d->heartRate > 0) { flowNext(10 * gUi); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Heart rate"); ImGui::SameLine(0, 5 * gUi); ImGui::PushFont(a.mono, 0); ImGui::Text("%d bpm", d->heartRate); ImGui::PopFont(); }
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "ANT+ sensors";
    if (live(a)) l2 = dect2::antSummary(a.rx.ant);
}

void tuner(App& a, bool& retune) {
    load();
    ImGui::TextDisabled("Channel");
    ImGui::SameLine(0, 5 * gUi);
    if (ImGui::SmallButton("ANT+ 2457 MHz")) { tuneFreq(a, dect2::ant::kChannelMhz); retune = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every ANT+ device (heart rate, bike, footpod...) is on 2457 MHz; other ANT networks use any 1 MHz channel from 2400 to 2524 MHz");
}

void synth(App& a, bool& changed) {
    static const char* names[] = {"A heart rate strap, a power meter and a speed / cadence sensor", "A heart rate strap alone", "Noise only"};
    int& o = a.tune.synth.modeOpt[0];
    if (o < 0 || o > 2) o = 0;
    ImGui::SetNextItemWidth(std::min(330 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##antsynth", names[o])) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(names[i], i == o)) { o = i; changed = true; }
        ImGui::EndCombo();
    }
}

void tick(App& a) {
    load();
    if (S.pushedAny != (int)S.anyNet) { a.engine.ant().setAnyNetwork(S.anyNet); S.pushedAny = (int)S.anyNet; }
    if (S.pushedWheel != S.wheelMm) { a.engine.ant().setWheelMm(S.wheelMm); S.pushedWheel = S.wheelMm; }
}

} // namespace

extern const ModeUi kAntUi;
const ModeUi kAntUi = {
    .sideTitle = "SENSORS",
    .tabName = "Sensors",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .synth = synth,
    .tick = tick,
};
