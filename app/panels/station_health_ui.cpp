// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Station health (View > Station health): one button measures the noise floor, ADC level and reference stations on a few fixed bands, adds the ADS-B range
// plot and a frequency error from a known carrier when there is one, and lists plain advice. The analysis is in core/src/sweep/station_health.cpp
// (docs/STATION_HEALTH.md); this file is the window, the worker thread and the report text.
#include "dect2/common/thread_name.h"
#include "../app.h"
#include "dect2/sweep/station_health.h"
#include <atomic>
#include <cmath>
#include <fstream>
#include <mutex>
#include <thread>

using namespace dect2;
using namespace dect2::sweep;

namespace {

struct State {
    std::thread th;
    std::atomic<bool> cancel{false}, running{false};
    std::mutex mu;
    std::vector<BandResult> bands;
    PpmResult ppm;
    bool havePpm = false, have = false;
    double gainDb = 0, gainMaxDb = 0;
    std::string msg, progress;
    float carrierMhz = 0;
};
State S;

const char* bandName(const std::string& id) {
    if (id == "fm") return N_("FM broadcast");
    if (id == "air") return N_("Airband");
    if (id == "dab") return N_("DAB / VHF band III");
    if (id == "uhf") return N_("UHF TV");
    if (id == "ism868") return N_("868 MHz ISM");
    if (id == "adsb") return N_("ADS-B 1090 MHz");
    return "";
}

// the radio's own gain number: a generic radio's dB, the HackRF's three stages as one total (as the gain optimiser does)
void gainOf(const App& a, double& g, double& gmax) {
    const DeviceInfo& d = a.devices[(size_t)a.devIdx];
    if (d.isGeneric()) { g = a.tune.gainDb; gmax = d.gainMaxDb; }
    else { g = a.tune.lnaDb + a.tune.vgaDb + (a.tune.ampOn ? 14 : 0); gmax = 104; }
}

void worker(DeviceInfo dev, TuneSettings t, HealthConfig c, double carrierHz) {
    std::string err;
    IqSweepSource src(dev, t);
    std::vector<BandResult> out;
    PpmResult pp;
    if (!src.open(err)) { std::lock_guard<std::mutex> l(S.mu); S.msg = err; S.running = false; return; }
    const bool ok = runHealthCheck(src, c, out, err, [](int i, int n, const std::string& id) {
        std::lock_guard<std::mutex> l(S.mu);
        char b[160];
        snprintf(b, sizeof b, tr("Measuring %s (%d/%d)"), tr(bandName(id)), i + 1, n);
        S.progress = b;
    }, &S.cancel);
    if (ok && carrierHz > 0 && !S.cancel.load()) {
        SweepConfig sc;
        sc.startHz = carrierHz - 100e3; sc.stopHz = carrierHz + 100e3; sc.binHz = 1e3; sc.dwellSec = 0.3; sc.settleSec = c.settleSec;
        Sweep s;
        if (runSweep(src, sc, s, err, &S.cancel)) pp = frequencyError(s, carrierHz, 50e3);
    }
    src.close();
    std::lock_guard<std::mutex> l(S.mu);
    if (!err.empty() && err != "cancelled") S.msg = err;
    S.bands = out;
    S.ppm = pp; S.havePpm = carrierHz > 0;
    S.have = !out.empty();
    S.progress.clear();
    S.running = false;
}

void stopRun() {
    S.cancel = true;
    if (S.th.joinable()) S.th.join();
    S.running = false;
}

void startRun(App& a) {
    stopRun();
    if (a.engine.running()) a.engine.stop();
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size() || !a.devices[(size_t)a.devIdx].isRadio()) { S.msg = tr("Pick a radio first (the test signal and files cannot be measured)"); return; }
    DeviceInfo dev = a.devices[(size_t)a.devIdx];
    TuneSettings t = a.tune;
    t.sampleRate = 10e6;
    if (dev.maxRateHz > 0) t.sampleRate = std::min(t.sampleRate, dev.maxRateHz);
    t.bandwidthMhz = t.sampleRate / 1e6;
    HealthConfig c;
    c.bands = healthBands(dev.minFreqHz, dev.maxFreqHz);
    if (c.bands.empty()) { S.msg = tr("None of the measuring bands is inside this radio's range"); return; }
    t.centerHz = c.bands.front().startHz;
    S.cancel = false; S.running = true; S.msg.clear();
    gainOf(a, S.gainDb, S.gainMaxDb);
    S.th = dect2::namedThread("onair-health", worker, dev, t, c, (double)S.carrierMhz * 1e6);
}

std::string findingText(const Finding& f) {
    char b[700];
    const std::string where = bandName(f.where)[0] ? std::string(tr(bandName(f.where))) : f.where;
    const char* w = where.c_str();
    switch (f.code) {
    case FindCode::Overload: snprintf(b, sizeof b, tr("%s: the ADC is overloaded (%.2f%% of the samples clip). Lower the gain or add an attenuator."), w, f.a); break;
    case FindCode::StrongOutOfBand: snprintf(b, sizeof b, tr("%s: a strong signal (%.0f dB over the floor) overloads the ADC while other bands are starved at this gain. Add a band-stop filter for that band (an FM notch for the FM band) instead of lowering the gain."), w, f.a); break;
    case FindCode::HotHeadroom: snprintf(b, sizeof b, tr("%s: the ADC level is high (%.1f dBFS rms), little headroom is left for strong signals. Lower the gain a little."), w, f.a); break;
    case FindCode::AdcLimited: snprintf(b, sizeof b, tr("%s: the receiver noise is only %.1f dB above the ADC's own, so weak signals are lost. Raise the gain."), w, f.a); break;
    case FindCode::AdcLimitedMax: snprintf(b, sizeof b, tr("%s: the receiver noise is only %.1f dB above the ADC's own and the gain is already at its maximum. Add a low-noise amplifier (LNA) next to the antenna."), w, f.a); break;
    case FindCode::WastedGain: snprintf(b, sizeof b, tr("%s: the gain could be about %.0f dB lower without losing sensitivity, which gives more headroom against strong signals."), w, f.a); break;
    case FindCode::NoiseExcess: snprintf(b, sizeof b, tr("%s: the noise floor is %.0f dB higher than in the quietest band. Look for a local noise source (LED lighting, switching power supplies, computers), move the antenna away from it, or add a ferrite choke on the cable."), w, f.a); break;
    case FindCode::RefAbsent: snprintf(b, sizeof b, tr("No %s stations were found, though there are normally some. Check the antenna and the cable; the band may also be empty in your area."), w); break;
    case FindCode::RefWeak: snprintf(b, sizeof b, tr("The strongest %s station is only %.0f dB over the noise (a healthy antenna gives at least %.0f dB). Move the antenna higher or outdoors, or add a band filter and an LNA."), w, f.a, f.b); break;
    case FindCode::PpmHigh: snprintf(b, sizeof b, tr("The radio's clock is %+.1f ppm off. Enter %+.1f as the frequency correction (ppm) in the radio's settings."), f.a, f.a); break;
    case FindCode::PpmOk: snprintf(b, sizeof b, tr("The radio's clock is %+.1f ppm off: fine."), f.a); break;
    case FindCode::RangeShort: snprintf(b, sizeof b, tr("ADS-B range is short: the farthest aircraft was %.0f nm away (a clear outdoor antenna reaches 150 nm or more); the weakest direction is around %.0f degrees. Move the antenna higher, with a clear view of the sky."), f.a, f.b); break;
    case FindCode::RangeBlind: snprintf(b, sizeof b, tr("No aircraft at all in %.0f sectors of 10 degrees starting at %.0f degrees, while the neighbouring directions reach far: something blocks the view there (a wall, a hill, a building)."), f.a, f.b); break;
    case FindCode::RangeNone: snprintf(b, sizeof b, "%s", tr("ADS-B range: not enough positions yet. Run the ADS-B mode with the antenna position set for a few minutes, then run the report again.")); break;
    case FindCode::AllGood: snprintf(b, sizeof b, "%s", tr("Nothing wrong found. The gain, noise floor and reference stations look healthy.")); break;
    }
    return b;
}

HealthReport buildReport(App& a) {
    HealthInput in;
    {
        std::lock_guard<std::mutex> l(S.mu);
        in.bands = S.bands; in.ppm = S.ppm; in.havePpm = S.havePpm; in.gainDb = S.gainDb; in.gainMaxDb = S.gainMaxDb;
    }
    const AdsbRangeStats& st = a.rx.adsb.stats;
    in.haveRange = a.rx.adsb.refValid || st.positions > 0;
    if (in.haveRange) in.range = assessRange(st.sectorMaxNm, kAdsbSectors, (unsigned long long)st.positions);
    return assessHealth(in);
}

std::string reportText(const HealthReport& r) {
    std::string t = std::string(tr("Station health report")) + "\n\n";
    char b[400];
    snprintf(b, sizeof b, tr("Gain: %.0f dB"), r.in.gainDb);
    t += std::string(b) + "\n\n";
    for (const auto& x : r.in.bands) {
        if (!x.measured) continue;
        snprintf(b, sizeof b, tr("%s: noise floor %.1f dBFS per bin, ADC margin %.1f dB, strongest signal %.0f dB over the floor, ADC level %.1f dBFS, clipping %.2f%%"),
                 tr(bandName(x.band.id)), x.floorDb, x.adcMarginDb, x.peakSnrDb, x.adc.rmsDbfs, x.adc.clipFraction * 100);
        t += std::string(b) + "\n";
        if (x.ref.kind != HealthRef::None) {
            snprintf(b, sizeof b, tr("   %s stations found: %d, strongest %.0f dB over the floor"), refName(x.ref.kind), x.ref.count, x.ref.strongestSnrDb);
            t += std::string(b) + "\n";
        }
    }
    t += "\n";
    for (const auto& f : r.findings) t += "- " + findingText(f) + "\n";
    t += "\n" + std::string(tr("The numbers are relative to the receiver itself (not calibrated to dBm).")) + "\n";
    return t;
}

void polar(const AdsbRangeStats& st) {
    const float side = std::min(ImGui::GetContentRegionAvail().x, 220.f * gUi);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##shpolar", ImVec2(side, side));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p0.x + side / 2, p0.y + side / 2);
    const float R = side / 2 - 4;
    float mx = 10;
    for (int i = 0; i < kAdsbSectors; i++) mx = std::max(mx, st.sectorMaxNm[i]);
    dl->AddCircle(c, R, IM_COL32(120, 120, 120, 160), 48);
    dl->AddCircle(c, R / 2, IM_COL32(120, 120, 120, 90), 48);
    for (int i = 0; i < kAdsbSectors; i++) {
        const float r = R * st.sectorMaxNm[i] / mx;
        const float a0 = (i * 10 - 90) * 3.14159265f / 180, a1 = (i * 10 + 10 - 90) * 3.14159265f / 180;
        dl->AddTriangleFilled(c, ImVec2(c.x + r * std::cos(a0), c.y + r * std::sin(a0)), ImVec2(c.x + r * std::cos(a1), c.y + r * std::sin(a1)), IM_COL32(90, 190, 255, 170));
    }
    dl->AddText(ImVec2(c.x - 4, p0.y - 2), IM_COL32(200, 200, 200, 255), "N");
    char b[40];
    snprintf(b, sizeof b, "%.0f nm", mx);
    dl->AddText(ImVec2(p0.x + side - 50, p0.y + side - 16), IM_COL32(200, 200, 200, 255), b);
}

} // namespace

void stationHealthWindow(App& a) {
    if (!a.stationHealthOpen) { if (S.th.joinable()) stopRun(); return; }
    firstWindowSize(760 * gUi, 640 * gUi);
    if (!ImGui::Begin(trw("Station health"), &a.stationHealthOpen)) { ImGui::End(); return; }
    const bool run = S.running.load();
    if (S.th.joinable() && !run) S.th.join();
    ImGui::TextWrapped("%s", tr("Measures the noise floor and the ADC level on a few bands, looks for FM, DAB and DVB-T stations, and tells you what to change at the antenna. The receiver stops while it measures (about a minute)."));
    ImGui::BeginDisabled(run);
    ImGui::SetNextItemWidth(110 * gUi);
    ImGui::InputFloat(tr("known carrier MHz (optional)"), &S.carrierMhz, 0, 0, "%.4f");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tr("A carrier whose frequency you trust (an ATSC pilot, a cell tower's beacon, a time signal). The report gives the radio's clock error in ppm from it. Leave at 0 to skip."));
    ImGui::EndDisabled();
    if (ImGui::Button(tr(run ? "Stop" : "Check station"))) { if (run) stopRun(); else startRun(a); }
    {
        std::lock_guard<std::mutex> l(S.mu);
        if (!S.progress.empty()) { ImGui::SameLine(); ImGui::TextColored(pal::heading(), "%s", S.progress.c_str()); }
        if (!S.msg.empty()) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, .7f, .3f, 1), "%s", S.msg.c_str()); }
    }
    bool have;
    { std::lock_guard<std::mutex> l(S.mu); have = S.have; }
    if (have && !run) {
        const HealthReport rep = buildReport(a);
        ImGui::SameLine();
        if (ImGui::Button(tr("Save report..."))) {
            const std::string p = saveFileDialog("station-health.txt");
            if (!p.empty()) { std::ofstream o(p, std::ios::binary); o << reportText(rep); }
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Save as JSON..."))) {
            const std::string p = saveFileDialog("station-health.json");
            if (!p.empty()) { std::ofstream o(p, std::ios::binary); o << healthJson(rep); }
        }
        ImGui::Separator();
        ImGui::TextColored(pal::heading(), tr("Gain: %.0f dB"), rep.in.gainDb);
        if (ImGui::BeginTable("##shbands", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
            ImGui::TableSetupColumn(tr("Band"));
            ImGui::TableSetupColumn(tr("Noise floor"));
            ImGui::TableSetupColumn(tr("ADC margin"));
            ImGui::TableSetupColumn(tr("Strongest"));
            ImGui::TableSetupColumn(tr("ADC level"));
            ImGui::TableSetupColumn(tr("Clipping"));
            ImGui::TableSetupColumn(tr("Stations"));
            ImGui::TableHeadersRow();
            for (const auto& x : rep.in.bands) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(tr(bandName(x.band.id)));
                if (!x.measured) continue;
                ImGui::TableNextColumn(); ImGui::Text(tr("%.1f dBFS"), x.floorDb);
                ImGui::TableNextColumn(); ImGui::Text(tr("%.1f dB"), x.adcMarginDb);
                ImGui::TableNextColumn(); ImGui::Text(tr("%.0f dB"), x.peakSnrDb);
                ImGui::TableNextColumn(); ImGui::Text(tr("%.1f dBFS"), x.adc.rmsDbfs);
                ImGui::TableNextColumn();
                if (x.adc.clipFraction > 0.001) ImGui::TextColored(ImVec4(1, .4f, .3f, 1), "%.2f%%", x.adc.clipFraction * 100); else ImGui::Text("%.2f%%", x.adc.clipFraction * 100);
                ImGui::TableNextColumn();
                if (x.ref.kind == HealthRef::None) ImGui::TextDisabled("-");
                else if (x.ref.level == RefLevel::Absent) ImGui::TextColored(ImVec4(.95f, .70f, .15f, 1), "%s: %s", refName(x.ref.kind), tr("none found"));
                else ImGui::Text(tr("%s: %d, best %.0f dB"), refName(x.ref.kind), x.ref.count, x.ref.strongestSnrDb);
            }
            ImGui::EndTable();
        }
        if (rep.in.havePpm) {
            if (rep.in.ppm.valid) ImGui::Text(tr("Clock error: %+.2f ppm (%+.0f Hz at %.4f MHz)"), rep.in.ppm.ppm, rep.in.ppm.errorHz, rep.in.ppm.expectedHz / 1e6);
            else ImGui::TextDisabled("%s", tr("No carrier found at the frequency given"));
        }
        ImGui::Spacing();
        ImGui::TextColored(pal::heading(), "%s", tr("What to do"));
        for (const auto& f : rep.findings) {
            const ImVec4 col = f.severity == Severity::Bad ? ImVec4(1, .45f, .35f, 1) : f.severity == Severity::Warn ? ImVec4(.95f, .70f, .15f, 1) : pal::okGreen();
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::Bullet();
            ImGui::TextWrapped("%s", findingText(f).c_str());
            ImGui::PopStyleColor();
        }
        const AdsbRangeStats& st = a.rx.adsb.stats;
        if (st.positions >= 50) {
            ImGui::Spacing();
            ImGui::TextColored(pal::heading(), "%s", tr("ADS-B range by direction"));
            polar(st);
        }
        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr("The numbers are relative to the receiver itself (not calibrated to dBm)."));
    }
    ImGui::End();
}
