// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Round-5 smart engines for satellites in the mode screens (docs/SMART5-sat-3.9.md items 16-20, docs/SMART5-UI-3.9.md):
// the learned horizon and the next good pass (Meteor LRPT, HRPT, APT and the other weather downlinks), the Sun outage of a
// geostationary downlink and the product cadence watchdog (GOES, GK-2A, Elektro, FY-4), the L-band carrier quality log
// (Inmarsat, BGAN) and the CubeSat beacon interval learner.
#include "smart5_ui.h"
#include "../panels/gallery.h"
#include "dect2/geo/passes.h"
#include "dect2/smart5/beacon_learn.h"
#include "dect2/smart5/carrier_log.h"
#include "dect2/smart5/horizon_learn.h"
#include "dect2/smart5/product_cadence.h"
#include "dect2/smart5/sun_outage.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace s5 = dect2::smart5;

namespace {

dect2::Observer site() {
    dect2::Observer o;
    receiverLocation(o.latDeg, o.lonDeg);
    o.altKm = receiverAltitudeM() / 1000.0;
    return o;
}

// ------------------------------------------------------------------------------------------------ learned horizon
struct Hz {
    s5::HorizonLearn learn;
    bool loaded = false;
    double last = 0, lastPlan = 0, lastSave = 0;
    std::string family;        // the satellite name filter of the mode being planned
    struct Plan { std::string sat; double aos = 0, maxEl = 0, minutes = 0; };
    std::vector<Plan> plan;    // the next day's passes, best first
} HZ;

// the name filter of a weather-satellite mode, "" for the others
std::string wxFilter(const App& a) {
    if (a.family == 59) return "METEOR-M";
    if (a.family == 64) return "NOAA";
    if (a.family >= 260 && a.family <= 271) return a.rx.wxsat.satName;
    return "";
}
bool wxDecoding(const App& a) {
    if (a.family == 59) return a.rx.meteor.state >= 3;
    if (a.family == 64) return a.rx.hrpt.state >= 3;
    return a.rx.wxsat.state >= 3;
}

void horizonTick(App& a) {
    const std::string f = wxFilter(a);
    if (f.empty() || !receiverLocationSet()) return;
    if (!HZ.loaded) { HZ.loaded = true; HZ.learn.load(s5ui::readData("horizon_learn.txt")); }
    const double now = s5ui::wall();
    const dect2::Observer o = site();
    if (a.engine.running() && s5ui::every(HZ.last, 1.0)) {
        std::string name;
        if (const dect2::Sgp4* s = passesFindSat(f, now, &name)) {
            dect2::Look l;
            if (dect2::lookAt(*s, o, now, l) && l.elDeg > 0) HZ.learn.sample(l.azDeg, l.elDeg, wxDecoding(a));
        }
        if (now - HZ.lastSave > 600) { HZ.lastSave = now; s5ui::writeData("horizon_learn.txt", HZ.learn.save()); }
    }
    if (f != HZ.family || now - HZ.lastPlan > 600) {   // the next day's passes of the satellites this mode receives, scored by the learned horizon
        HZ.family = f; HZ.lastPlan = now; HZ.plan.clear();
        std::set<std::string> seen;
        for (double t = now; t < now + 86400; t += 1800) {
            std::string name;
            const dect2::Sgp4* s = passesFindSat(f, t, &name);
            if (!s || !seen.insert(name).second) continue;
            for (const auto& p : dect2::findPasses(*s, o, now, now + 86400, 5)) {
                std::vector<s5::TrackPoint> tr;
                for (double x = p.aos; x <= p.los; x += 20) { dect2::Look l; if (dect2::lookAt(*s, o, x, l)) tr.push_back({x, l.azDeg, l.elDeg}); }
                HZ.plan.push_back({name, p.aos, p.maxElDeg, HZ.learn.predictMinutes(tr)});
            }
        }
        std::sort(HZ.plan.begin(), HZ.plan.end(), [](const Hz::Plan& x, const Hz::Plan& y) { return x.aos < y.aos; });
    }
}

// ------------------------------------------------------------------------------------------------ geostationary
struct GeoSat { int famLo, famHi; const char* name; double lonE; };
// default sub-satellite longitudes (from memory; the user can correct them in Details): the satellite named by the mode
const GeoSat kGeo[] = {{60, 60, "GOES-East", -75.2}, {230, 231, "GK-2A", 128.2}, {80, 80, "Elektro-L", 76.0}, {232, 234, "FY-4", 105.0}};
const GeoSat* geoOf(int fam) { for (const auto& g : kGeo) if (fam >= g.famLo && fam <= g.famHi) return &g; return nullptr; }

struct Geo {
    s5::ProductCadence cad;
    std::set<uint64_t> seenIds;
    int fam = -1;
    double last = 0, lastOut = 0;
    std::vector<s5::SunOutage> outages;
    std::map<std::string, int> missSaid;
    bool inOutageSaid = false;
} G;

double geoLon(const GeoSat& g) { return plat::prefs().getD((std::string("smart5.geoLon.") + g.name).c_str(), g.lonE); }
double dishM() { return plat::prefs().getD("smart5.dishM", 1.2); }

const GoesTelemetry* goesTel(const App& a) {
    if (a.family == 60) return &a.rx.goes;
    if (a.family == 230 || a.family == 231) return &a.rx.gk2a;
    if (a.family == 80) return &a.rx.elektro.hrit;
    if (a.family >= 232 && a.family <= 234) return &a.rx.fygeo;
    return nullptr;
}

// a product's kind without its time stamps: "OR_ABI-L2-CMIPF-M6C13_G16_s2026..." -> "OR_ABI-L#-CMIPF-M#C#_G#_s#"
std::string productKey(const std::string& n) {
    std::string k;
    for (char c : n) { if (std::isdigit((unsigned char)c)) { if (k.empty() || k.back() != '#') k += '#'; } else k += c; }
    return k;
}

void geoTick(App& a) {
    const GeoSat* g = geoOf(a.family);
    const GoesTelemetry* t = goesTel(a);
    if (!g || !t) return;
    if (a.family != G.fam) { G = Geo(); G.fam = a.family; }
    const double now = s5ui::wall();
    if (receiverLocationSet() && (G.outages.empty() || now - G.lastOut > 3600)) {
        G.lastOut = now;
        double la, lo; receiverLocation(la, lo);
        G.outages = s5::sunOutages(now - 86400, now + 200 * 86400, la, lo, geoLon(*g), s5::dishBeamwidthDeg(dishM(), a.freqMhz * 1e6));
    }
    if (!a.engine.running() || !s5ui::every(G.last, 2.0)) return;
    G.cad.link(now, t->state >= 2, t->state >= 3);
    for (const auto& p : t->products) {
        if (!p || !p->complete || !G.seenIds.insert(p->id).second) continue;
        G.cad.product(productKey(p->name), now);
    }
    G.cad.tick(now);
    for (const auto& s : G.cad.states()) {
        int& said = G.missSaid[s.name];
        if (s.missing != s5::MissCause::None && s.missedCount > said) {
            said = s.missedCount;
            s5ui::post("satellite", "product_overdue", s.missing == s5::MissCause::NotSent ? 0 : 1, std::string(g->name) + " " + s.name, std::string(g->name) + ": " + G.cad.text(s.name));
        } else if (s.missing == s5::MissCause::None) said = 0;
    }
    if (receiverLocationSet()) {
        double la, lo; receiverLocation(la, lo);
        const bool in = s5::inSunOutage(now, la, lo, geoLon(*g), s5::dishBeamwidthDeg(dishM(), a.freqMhz * 1e6));
        s5ui::edge(G.inOutageSaid, in, "satellite", "sun_outage", 0, g->name, std::string(g->name) + ": the Sun is in the dish beam now (a sun outage, not a fault)");
    }
}

// ------------------------------------------------------------------------------------------------ L-band carriers
struct Lb { s5::CarrierLog log; std::map<long long, uint64_t> frames; double last = 0; int fam = -1; std::set<std::pair<long long, long long>> said; } L;

void lbandTick(App& a) {
    if ((a.family != 208 && a.family != 245) || !a.engine.running()) return;
    if (a.family != L.fam) { L = Lb(); L.fam = a.family; }
    if (!s5ui::every(L.last, 10.0)) return;
    const double now = s5ui::wall();
    auto feed = [&](double f, float snr, uint64_t fr) {
        if (f <= 0) return;
        uint64_t& w = L.frames[(long long)std::lround(f)];
        s5::CarrierSample s;
        s.snrDb = snr; s.frames = fr >= w ? (int)(fr - w) : 0;
        w = fr;
        L.log.sample(f, now, s);
    };
    if (a.family == 208) for (const auto& c : a.rx.inmsat.carriers) feed(c.freqHz, c.snrDb, c.frames);
    else for (const auto& c : a.rx.bgan.rows) feed(c.freqHz, c.snrDb, c.frames);
    for (double f : L.log.carriers())
        for (const auto& o : L.log.outages(f))
            if (o.end > 0 && L.said.insert({(long long)std::lround(f), (long long)o.start}).second) {
                char b[160];
                snprintf(b, sizeof b, "%.4f MHz: outage %s - %s UTC (%s)", f / 1e6, s5ui::utc(o.start).c_str(), s5ui::utc(o.end).c_str(), s5::CarrierLog::reasonName(o.reason));
                s5ui::post("satellite", "carrier_outage", o.reason == s5::OutageReason::Unexplained ? 1 : 0, b, b);
            }
}

// ------------------------------------------------------------------------------------------------ CubeSat beacons
struct Cb { s5::BeaconLearn learn; int64_t lastWall = 0; std::map<int, std::string> names; double last = 0; std::map<int, int> missSaid; } B;

void cubeTick(App& a) {
    if (a.family != 61) return;
    const CubeTelemetry& t = a.rx.cube;
    int64_t newest = B.lastWall;
    for (const auto& f : t.frames) {   // newest first
        if (f.wallTime <= B.lastWall || f.norad <= 0) continue;
        B.learn.beacon(f.norad, (double)f.wallTime);
        if (!f.sat.empty()) B.names[f.norad] = f.sat;
        newest = std::max(newest, f.wallTime);
    }
    B.lastWall = newest;
    if (!s5ui::every(B.last, 10.0)) return;
    const double now = s5ui::wall();
    for (const auto& kv : B.names) {
        dect2::Look l;
        const bool up = a.engine.smartCubeDecoder().look(kv.first, now, l) && l.elDeg > 0;
        B.learn.tick(kv.first, now, up);
        const auto st = B.learn.state(kv.first);
        int& said = B.missSaid[kv.first];
        if (st.overdue && st.missedWhileUp > said) { said = st.missedWhileUp; s5ui::post("satellite", "beacon_missed", 1, std::to_string(kv.first), B.learn.text(kv.first, kv.second, now)); }
    }
}

}  // namespace

void smart5SatTick(App& a) {
    horizonTick(a);
    geoTick(a);
    lbandTick(a);
    cubeTick(a);
}

// ------------------------------------------------------------------------------------------------ cards
void smart5WxPassCard(App& a) {
    if (wxFilter(a).empty() || !s5ui::card("s5wx")) return;
    if (!receiverLocationSet()) { ImGui::TextDisabled("%s", tr("Next good pass: set your position (Settings or the ADS-B map) first.")); return; }
    const double now = s5ui::wall();
    const Hz::Plan* best = nullptr;
    for (const auto& p : HZ.plan) if (p.aos > now && (!best || p.minutes > best->minutes)) best = &p;
    if (!best) ImGui::TextDisabled("%s", tr("Next good pass: no pass in the next day (or no orbit data: Passes > Update)."));
    else {
        char b[200];
        snprintf(b, sizeof b, tr("Best pass in the next day: %s at %s UTC, max %.0f deg, about %.0f decoded minutes"), best->sat.c_str(), s5ui::utc(best->aos).c_str(), best->maxEl, best->minutes);
        s5ui::line(0, b);
    }
    if (s5ui::details("s5wxd")) {
        for (const auto& p : HZ.plan) if (p.aos > now) ImGui::TextDisabled(tr("%s  %s  max %.0f deg  %.1f min"), s5ui::utc(p.aos, true).c_str(), p.sat.c_str(), p.maxEl, p.minutes);
        std::string h;
        for (int s = 0; s < 12; s++) { char x[32]; snprintf(x, sizeof x, "%s%d:%.0f%s", h.empty() ? "" : "  ", s * 30, HZ.learn.horizon(s * 30 + 15), HZ.learn.learned(s * 30 + 15) ? "" : "?"); h += x; }
        ImGui::TextDisabled(tr("Learned horizon (azimuth: elevation; ? = default 10 deg until learned): %s"), h.c_str());
        ImGui::TreePop();
    }
}

void smart5GeoCard(App& a) {
    const GeoSat* g = geoOf(a.family);
    if (!g || !s5ui::card("s5geo")) return;
    const double now = s5ui::wall();
    const s5::SunOutage* next = nullptr;
    for (const auto& o : G.outages) if (o.end > now) { next = &o; break; }
    if (!receiverLocationSet()) ImGui::TextDisabled("%s", tr("Sun outages: set your position first."));
    else if (next) s5ui::line(next->start <= now ? 1 : 0, std::string(next->start <= now ? tr("Sun outage now: ") : tr("Next sun outage: ")) + s5::sunOutageText(*next));
    else ImGui::TextDisabled("%s", tr("No sun outage in the next 200 days for this dish."));
    int miss = 0;
    for (const auto& s : G.cad.states()) if (s.missing != s5::MissCause::None) { s5ui::line(s.missing == s5::MissCause::LostLock ? 1 : 0, G.cad.text(s.name)); miss++; }
    if (!miss) ImGui::TextDisabled(tr("Products: %zu kinds followed, none overdue"), G.cad.states().size());
    if (s5ui::details("s5geod")) {
        for (const auto& s : G.cad.states()) ImGui::TextDisabled(tr("%s  every %.0f min, last %s"), s.name.c_str(), s.periodSec / 60, s.last > 0 ? s5ui::utc(s.last).c_str() : "-");
        for (const auto& o : G.outages) if (o.end > now) ImGui::TextDisabled("%s", s5::sunOutageText(o).c_str());
        float lon = (float)geoLon(*g), d = (float)dishM();
        ImGui::SetNextItemWidth(120 * gUi);
        if (ImGui::InputFloat(tr("Satellite longitude (deg E)"), &lon, 0, 0, "%.1f")) { plat::prefs().setD((std::string("smart5.geoLon.") + g->name).c_str(), lon); G.outages.clear(); }
        ImGui::SetNextItemWidth(120 * gUi);
        if (ImGui::InputFloat(tr("Dish diameter (m)"), &d, 0, 0, "%.2f") && d > 0.1f) { plat::prefs().setD("smart5.dishM", d); G.outages.clear(); }
        ImGui::TreePop();
    }
}

void smart5LbandCard(App& a) {
    if ((a.family != 208 && a.family != 245) || !s5ui::card("s5lb")) return;
    const auto cs = L.log.carriers();
    if (cs.empty()) { ImGui::TextDisabled("%s", tr("Carrier log: waiting for carriers")); return; }
    double worst = 100; double wf = 0;
    for (double f : cs) { const double v = L.log.availabilityPct(f); if (v < worst) { worst = v; wf = f; } }
    char b[160];
    snprintf(b, sizeof b, tr("%zu carriers logged; lowest availability %.0f %% (%.4f MHz)"), cs.size(), worst, wf / 1e6);
    s5ui::line(worst < 90 ? 1 : 0, b);
    if (s5ui::details("s5lbd")) {
        for (double f : cs) {
            const auto m = L.log.minutes(f);
            ImGui::TextDisabled(tr("%.4f MHz  %.0f %% available, %zu outages, SNR %.1f dB"), f / 1e6, L.log.availabilityPct(f), L.log.outages(f).size(), m.empty() ? 0.0 : m.back().snrDb);
        }
        ImGui::TreePop();
    }
}

void smart5CubeCard(App& a) {
    if (a.family != 61 || !s5ui::card("s5cube")) return;
    if (B.names.empty()) { ImGui::TextDisabled("%s", tr("Beacon intervals: learned from the decoded beacons over several passes")); return; }
    const double now = s5ui::wall();
    for (const auto& kv : B.names) {
        const auto st = B.learn.state(kv.first);
        s5ui::line(st.overdue ? 1 : 0, B.learn.text(kv.first, kv.second, now));
    }
}

void smart5InsightSat(App& a) {
    (void)a;
    ImGui::SeparatorText(tr("Weather satellites"));
    const double now = s5ui::wall();
    const Hz::Plan* best = nullptr;
    for (const auto& p : HZ.plan) if (p.aos > now && (!best || p.minutes > best->minutes)) best = &p;
    if (best) ImGui::Text(tr("Best pass: %s at %s UTC, about %.0f decoded minutes"), best->sat.c_str(), s5ui::utc(best->aos).c_str(), best->minutes);
    else ImGui::TextDisabled("-");
    ImGui::SeparatorText(tr("Geostationary"));
    for (const auto& o : G.outages) if (o.end > now) { ImGui::Text("%s %s", tr("Next sun outage:"), s5::sunOutageText(o).c_str()); break; }
    for (const auto& s : G.cad.states()) if (s.missing != s5::MissCause::None) ImGui::BulletText("%s", G.cad.text(s.name).c_str());
    ImGui::SeparatorText(tr("L-band and CubeSats"));
    ImGui::Text(tr("%zu carriers logged, %zu CubeSats with learned beacons"), L.log.carriers().size(), B.names.size());
}
