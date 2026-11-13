// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Round-5 smart engines for GNSS and science in the mode screens (docs/SMART5-gnss-3.9.md items 21-23, docs/SMART5-science-3.9.md
// items 24-26, docs/SMART5-UI-3.9.md): the radio's crystal error from the GNSS clock drift, the scintillation index S4 and the
// multipath indicator per satellite, the Sun drift-scan calibration of a radio telescope, the day-night terminator marks of a
// VLF (SuperSID) path, and the meteor-scatter hourly rate against the shower calendar.
#include "smart5_ui.h"
#include "dect2/selfcal/selfcal.h"
#include "dect2/smart5/drift_scan.h"
#include "dect2/smart5/gnss_clock.h"
#include "dect2/smart5/gnss_multipath.h"
#include "dect2/smart5/gnss_s4.h"
#include "dect2/smart5/meteor_rate.h"
#include "dect2/smart5/vlf_terminator.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

namespace s5 = dect2::smart5;

namespace {

// ------------------------------------------------------------------------------------------------ GNSS
struct Gn {
    s5::GnssClockCal clock;
    s5::GnssS4 s4{1.0, 60};          // fed with the receiver's C/N0 once a second (no prompt correlator power is published)
    s5::GnssMultipath mp;
    std::map<int, double> lastSeen;  // sat key -> last time tracked with an elevation
    std::map<int, s5::MultipathResult> mpDone;
    double last = 0, lastMp = 0;
    bool settledSaid = false;
    std::map<int, bool> scintSaid, mpSaid;
} N;

int satKey(const GnssChannel& c) { return c.sys * 1000 + c.prn; }
std::string satName(int k) { char b[16]; const char* s = "GRECJS"; snprintf(b, sizeof b, "%c%02d", k / 1000 >= 0 && k / 1000 < 6 ? s[k / 1000] : '?', k % 1000); return b; }

void gnssTick(App& a) {
    if (a.family != 12 || !a.engine.running()) return;
    if (!s5ui::every(N.last, 1.0)) return;
    const double now = s5ui::wall();
    const auto& g = a.rx.gnss;
    N.clock.fix(now, g.fix.clockDriftMps / 299792458.0, g.fix.valid, g.fix.nSats);
    if (N.clock.settled()) {
        char b[120];
        snprintf(b, sizeof b, "GNSS: the radio's crystal is %+.2f ppm off (%+.0f Hz at %.3f MHz); settled", N.clock.ppm(), N.clock.correctionHz(a.freqMhz * 1e6), a.freqMhz);
        s5ui::edge(N.settledSaid, true, "gnss", "radio_clock", 0, "ppm", b);
    }
    for (const auto& c : g.channels) {
        if (c.prn <= 0 || c.cn0 <= 0) continue;
        const int k = satKey(c);
        N.s4.sample(k, std::pow(10.0, c.cn0 / 10.0), c.cn0);
        const auto r = N.s4.result(k);
        if (r.valid) s5ui::edge(N.scintSaid[k], r.level >= s5::Scint::Moderate, "gnss", "scintillation", 1, satName(k),
                                satName(k) + ": " + s5::GnssS4::levelName(r.level) + " scintillation, S4 " + std::to_string(r.s4).substr(0, 4));
        if (c.hasAzEl && c.elDeg > 0) {
            if (now - N.lastMp >= 10) N.mp.sample(k, c.elDeg, c.azDeg, c.cn0);
            N.lastSeen[k] = now;
        }
    }
    if (now - N.lastMp >= 10) N.lastMp = now;
    for (auto it = N.lastSeen.begin(); it != N.lastSeen.end();) {   // a pass ended (not tracked for 5 minutes): judge its multipath
        if (now - it->second > 300) {
            const auto r = N.mp.analyse(it->first);
            if (r.valid) {
                N.mpDone[it->first] = r;
                if (r.multipath) {
                    char b[160];
                    snprintf(b, sizeof b, "%s: multipath, %.1f dB fringe, a reflector about %.1f m below the antenna towards %.0f deg", satName(it->first).c_str(), r.depthDb, r.heightM, r.azDeg);
                    s5ui::post("gnss", "multipath", 1, satName(it->first), b);
                }
            }
            N.mp.clear(it->first);
            it = N.lastSeen.erase(it);
        } else ++it;
    }
}

// ------------------------------------------------------------------------------------------------ radio astronomy: Sun drift scan
struct Ds {
    bool running = false;
    std::vector<double> t, p;
    uint64_t seen = 0;
    s5::DriftScanResult res;
    bool have = false;
} DS;

void astroTick(App& a) {
    if (a.family != 48 || !DS.running || !a.engine.running()) return;
    const auto& t = a.rx.astro;
    if (t.completed == DS.seen || t.powerDb <= -199) return;
    DS.seen = t.completed;
    DS.t.push_back(s5ui::wall());
    DS.p.push_back(std::pow(10.0, t.powerDb / 10.0));
}

void driftFinish() {
    DS.running = false;
    double la = 0, lo = 0;
    receiverLocation(la, lo);
    DS.res = s5::driftScan(DS.t, DS.p, la, lo, plat::prefs().getD("smart5.dishAz", 0), plat::prefs().getD("smart5.dishEl", 0));
    DS.have = true;
    if (DS.res.valid) s5ui::post("science", "drift_scan", 0, s5ui::utc(DS.res.peakTime, true), s5::driftScanText(DS.res));
}

// ------------------------------------------------------------------------------------------------ SuperSID terminator
struct Vl { std::map<std::string, std::vector<s5::TermEvent>> ev; double day = -1, last = 0; std::map<std::string, double> lastMark; } VL;

const dect2::hamsci::SidStation* sidFind(const std::string& call) {
    int n = 0;
    const dect2::hamsci::SidStation* s = dect2::hamsci::sidStations(n);
    for (int i = 0; i < n; i++) if (call == s[i].call) return &s[i];
    return nullptr;
}

void vlfTick(App& a) {
    if (a.family != 500 || !receiverLocationSet()) return;
    if (!s5ui::every(VL.last, 5.0)) return;
    const double now = s5ui::wall();
    const double day = std::floor(now / 86400) * 86400;
    double la, lo; receiverLocation(la, lo);
    if (day != VL.day) { VL.day = day; VL.ev.clear(); }
    for (const auto& st : a.rx.hamsci.sid.stations) {
        if (!st.inSpan) continue;
        const auto* s = sidFind(st.call);
        if (!s) continue;
        auto& v = VL.ev[st.call];
        if (v.empty()) v = s5::vlfTerminator({s->lat, s->lon, la, lo}, day);
        double& m = VL.lastMark[st.call];
        for (const auto& e : v)
            if (e.t > m && e.t <= now && now - e.t < 600) { m = e.t; s5ui::post("science", "vlf_terminator", 0, st.call + " " + std::to_string((long long)e.t), st.call + ": " + s5::termText(e)); }
    }
}

// ------------------------------------------------------------------------------------------------ meteor scatter rate
struct Mr { std::unique_ptr<s5::MeteorRate> rate; int64_t hour = 0; } MR;

void meteorTick(App& a) {
    if (a.family != 104 || !receiverLocationSet()) return;
    const auto& t = a.rx.mscatter;
    if (t.hourly.size() < 2 || t.hourStart == 0) return;
    if (!MR.rate) { double la, lo; receiverLocation(la, lo); MR.rate = std::make_unique<s5::MeteorRate>(la, lo); }
    if (MR.hour == 0) { MR.hour = t.hourStart; return; }
    if (t.hourStart == MR.hour) return;
    MR.hour = t.hourStart;
    MR.rate->hour((double)t.hourStart - 3600, t.hourly[t.hourly.size() - 2]);   // the hour that just ended
    const auto& h = MR.rate->hours();
    if (!h.empty() && h.back().outburst) s5ui::post("science", "meteor_outburst", 1, std::to_string((long long)h.back().t), MR.rate->text());
}

}  // namespace

void smart5SciTick(App& a) {
    gnssTick(a);
    astroTick(a);
    vlfTick(a);
    meteorTick(a);
}

// ------------------------------------------------------------------------------------------------ cards
void smart5GnssCard(App& a) {
    if (a.family != 12 || !s5ui::card("s5gnss")) return;
    if (N.clock.ready()) s5ui::line(0, N.clock.text());
    else ImGui::TextDisabled("%s", tr("Radio clock: measuring the crystal error from the fixes (10 fixes)"));
    int scint = 0;
    for (const auto& c : a.rx.gnss.channels) {
        if (c.prn <= 0) continue;
        const auto r = N.s4.result(satKey(c));
        if (r.valid && r.level != s5::Scint::None) { char b[96]; snprintf(b, sizeof b, tr("%s: %s scintillation, S4 %.2f"), satName(satKey(c)).c_str(), s5::GnssS4::levelName(r.level), r.s4); s5ui::line(r.level >= s5::Scint::Moderate ? 1 : 0, b); scint++; }
    }
    if (!scint) ImGui::TextDisabled("%s", tr("No ionospheric scintillation seen"));
    int mp = 0;
    for (const auto& kv : N.mpDone) if (kv.second.multipath) mp++;
    if (mp) { char b[96]; snprintf(b, sizeof b, tr("Multipath on %d of %zu judged passes"), mp, N.mpDone.size()); s5ui::line(1, b); }
    if (s5ui::details("s5gnssd")) {
        if (N.clock.ready()) {
            ImGui::TextDisabled(tr("Crystal %+.3f ppm +/- %.3f, trend %+.4f ppm a minute"), N.clock.ppm(), N.clock.sigmaPpm(), N.clock.trendPpmPerMin());
            if (N.clock.settled() && ImGui::SmallButton(tr("Use as the radio's frequency correction"))) {
                // the error measured is what is left on top of the radio's present setting (as in Tools > Calibrate frequency)
                auto it = a.tune.radio.find("ppm");
                const double base = it != a.tune.radio.end() ? atof(it->second.c_str()) : 0;
                dect2::selfcal::applyPpm(a.tune.radio, base + N.clock.ppm());
                saveRadioSettings(a);
            }
        }
        for (const auto& kv : N.mpDone)
            ImGui::TextDisabled(tr("%s  fringe %.1f dB, reflector %.1f m, azimuth %.0f deg%s"), satName(kv.first).c_str(), kv.second.depthDb, kv.second.heightM, kv.second.azDeg, kv.second.multipath ? tr(", multipath") : "");
        ImGui::TextDisabled("%s", tr("S4 comes from the receiver's 1 Hz C/N0: only slow, strong scintillation shows. Multipath is judged at the end of each pass."));
        ImGui::TreePop();
    }
}

void smart5AstroCard(App& a) {
    if (a.family != 48 || !s5ui::card("s5astro")) return;
    if (!DS.running) {
        if (ImGui::Button(tr("Sun drift scan"))) { DS = Ds(); DS.running = true; DS.seen = a.rx.astro.completed; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Point the dish ahead of the Sun and leave it fixed; press Finish after the Sun has drifted through. Gives the beamwidth, the pointing error and the Sun-to-cold-sky ratio."));
    } else {
        ImGui::Text(tr("Recording the drift: %zu points"), DS.t.size());
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Finish"))) driftFinish();
    }
    if (DS.have) s5ui::line(DS.res.valid ? 0 : 1, DS.res.valid ? s5::driftScanText(DS.res) : tr("No Sun transit found in the recording."));
    if (s5ui::details("s5astrod")) {
        float az = (float)plat::prefs().getD("smart5.dishAz", 0), el = (float)plat::prefs().getD("smart5.dishEl", 0);
        ImGui::SetNextItemWidth(100 * gUi);
        if (ImGui::InputFloat(tr("Dish azimuth (deg)"), &az, 0, 0, "%.1f")) plat::prefs().setD("smart5.dishAz", az);
        ImGui::SetNextItemWidth(100 * gUi);
        if (ImGui::InputFloat(tr("Dish elevation (deg)"), &el, 0, 0, "%.1f")) plat::prefs().setD("smart5.dishEl", el);
        if (DS.have && DS.res.valid) ImGui::TextDisabled(tr("Beam %.2f deg (FWHM %.0f s); correct the pointing by az %+.2f, el %+.2f deg; Y factor %.1f dB"), DS.res.beamDeg, DS.res.fwhmSec, DS.res.azCorrDeg, DS.res.elCorrDeg, DS.res.yFactorDb);
        ImGui::TreePop();
    }
}

void smart5SidCard(App& a) {
    if (a.family != 500 || !s5ui::card("s5sid")) return;
    if (!receiverLocationSet()) { ImGui::TextDisabled("%s", tr("Sunrise and sunset marks: set your position first.")); return; }
    const double now = s5ui::wall();
    int shown = 0;
    for (const auto& kv : VL.ev) {
        const s5::TermEvent* next = nullptr;
        for (const auto& e : kv.second) if (e.t > now) { next = &e; break; }
        double la, lo; receiverLocation(la, lo);
        const auto* s = sidFind(kv.first);
        const double lit = s ? s5::vlfSunlitFraction({s->lat, s->lon, la, lo}, now) : 0;
        char b[200];
        snprintf(b, sizeof b, tr("%s: path %.0f %% in daylight%s%s"), kv.first.c_str(), lit * 100, next ? tr("; next: ") : "", next ? s5::termText(*next).c_str() : "");
        s5ui::line(0, b);
        shown++;
    }
    if (!shown) ImGui::TextDisabled("%s", tr("Sunrise and sunset marks appear for the stations in the span."));
    if (s5ui::details("s5sidd")) {
        for (const auto& kv : VL.ev) for (const auto& e : kv.second) ImGui::TextDisabled("%s  %s", kv.first.c_str(), s5::termText(e).c_str());
        ImGui::TreePop();
    }
}

void smart5MeteorCard(App& a) {
    if (a.family != 104 || !s5ui::card("s5ms")) return;
    if (!MR.rate) { ImGui::TextDisabled("%s", tr("Rate against the showers: set your position first.")); return; }
    double la, lo; receiverLocation(la, lo);
    std::string sh;
    for (const auto& s : s5::showersAt(s5ui::wall(), la, lo)) { char b[96]; snprintf(b, sizeof b, "%s%s ZHR %.0f, radiant %.0f deg", sh.empty() ? "" : "; ", s.name.c_str(), s.zhrNow, s.radiantElDeg); sh += b; }
    if (!sh.empty()) s5ui::line(0, std::string(tr("Active: ")) + sh);
    if (MR.rate->hours().empty()) ImGui::TextDisabled("%s", tr("Hourly rate: the first full hour is still being counted"));
    else s5ui::line(MR.rate->hours().back().outburst ? 1 : 0, MR.rate->text() + (MR.rate->ready() ? "" : tr(" (learning the station's curve: 3 days)")));
}

void smart5InsightSci(App& a) {
    (void)a;
    ImGui::SeparatorText(tr("GNSS"));
    if (N.clock.ready()) ImGui::TextUnformatted(N.clock.text().c_str()); else ImGui::TextDisabled("-");
    ImGui::SeparatorText(tr("Science"));
    if (DS.have && DS.res.valid) ImGui::TextUnformatted(s5::driftScanText(DS.res).c_str());
    if (MR.rate && !MR.rate->hours().empty()) ImGui::TextUnformatted(MR.rate->text().c_str());
    ImGui::Text(tr("%zu VLF paths with sunrise and sunset marks today"), VL.ev.size());
}
