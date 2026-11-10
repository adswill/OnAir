// Field insights (3.9, docs/SMART5-field-3.9.md): the aviation, marine and IoT analysers that work on one decoder's own output, joined to
// the insight hub (hub.h). ADS-B, AIS and NAVTEX come in through the automation events ("adsb.aircraft", "ais.vessel", "msg.navtex");
// FLARM, HFDL, LoRa mesh, LoRaWAN and the user's own water meter are handed over by the interface from the receivers' telemetry. Findings become
// Insight rows (and automation events "insight.<kind>"). Offline; positions are only what the transmitters broadcast to everyone, and only
// meters the user marked as their own are followed.
#pragma once
#include "dect2/adsb/adsb_manoeuvre.h"
#include "dect2/ais/ais_ducting.h"
#include "dect2/automation/event.h"
#include "dect2/flarm/flarm_thermals.h"
#include "dect2/hfdl/hfdl_prop.h"
#include "dect2/insight/insight.h"
#include "dect2/lorawan/lorawan_health.h"
#include "dect2/marine/marine_navtex_force.h"
#include "dect2/mesh/mesh_census.h"
#include "dect2/wmbus/wmbus_leak.h"
#include <map>
#include <string>
#include <vector>

namespace dect2::insight {

class Field {
public:
    void setHome(double lat, double lon) { home_ = true; hLat_ = lat; hLon_ = lon; duct_.setHome(lat, lon); }
    void setTzOffset(int sec) { tz_ = sec; for (auto& [k, m] : leak_) m.tzOffsetSec = sec; }

    // automation events (the hub hands every one over); the ones it does not use are ignored
    void onEvent(const automation::Event& e, std::vector<Insight>& out) {
        auto num = [&](const char* k, double& v) { auto it = e.f.find(k); if (it == e.f.end() || it->second.t != automation::Value::Num) return false; v = it->second.n; return true; };
        auto str = [&](const char* k) { auto it = e.f.find(k); return it == e.f.end() ? std::string() : it->second.text(); };
        if (e.type == "adsb.aircraft") {
            AdsbManState s;
            s.hex = str("hex"); s.callsign = str("callsign"); s.t = e.time;
            s.hasAlt = num("alt_ft", s.altFt); s.hasVrate = num("vrate_fpm", s.vrateFpm); s.hasHeading = num("heading_deg", s.headingDeg);
            s.hasSpeed = num("speed_kt", s.speedKt);
            s.hasPos = num("lat", s.lat) && num("lon", s.lon);
            auto g = e.f.find("ground"); s.ground = g != e.f.end() && g->second.truthy();
            double sq; if (num("squawk", sq)) s.squawk = (int)sq;
            double em; if (num("emergency", em)) s.emergency = (int)em;
            std::vector<AdsbManAlert> v;
            man_.add(s, v);
            for (const auto& a : v) {
                Insight i; i.time = a.t; i.area = "aviation"; i.kind = "adsb_" + a.kind; i.severity = a.severity; i.key = a.hex; i.text = a.text;
                i.hasPos = a.hasPos; i.lat = a.lat; i.lon = a.lon;
                out.push_back(i);
            }
        } else if (e.type == "msg.navtex") {
            const std::string src = str("source"), hdr = str("header");
            if (src.size() != 1 || hdr.size() != 4) return;
            navtex(src[0], hdr[1], hdr, str("text"), (int64_t)e.time, out);
        } else if (e.type == "ais.vessel") {
            double mmsi, lat, lon;
            if (!num("mmsi", mmsi) || !num("lat", lat) || !num("lon", lon)) return;
            std::vector<AisDuctEvent> v;
            duct_.add(e.time, (uint32_t)mmsi, lat, lon, str("class"), e.freqHz, v);
            ducts(v, out);
        }
    }

    void tick(double now, std::vector<Insight>& out) {
        man_.prune(now);
        therm_.prune(now);
        std::vector<AisDuctEvent> v;
        duct_.tick(now, v);
        ducts(v, out);
        std::vector<marine::NavtexChange> c;
        navtex_.tick((int64_t)now, c);
        navtexChanges(c, out);
        for (auto& [key, m] : leak_) { std::vector<WaterLeakCheck::Finding> f; m.tick((int64_t)now, f); leaks(key, now, f, out); }
    }

    // ---- the interface's feeds
    void flarmFix(const TrafficFix& f, std::vector<Insight>& out) {
        Thermal th;
        if (therm_.add(f, &th) && th.climbMs >= therm_.strongMs) {
            Insight i; i.time = f.t; i.area = "aviation"; i.kind = "thermal"; i.key = "thermal " + std::to_string(th.id);
            i.text = "Strong " + ThermalMap::describe(th); i.hasPos = true; i.lat = th.lat; i.lon = th.lon;
            out.push_back(i);
        }
    }
    void hfdlListened(double t, const std::vector<int>& khz, double dtSec) { hfdl_.listened(t, khz, dtSec); }
    void hfdlHeard(double t, int gsId, int khz, float snr, bool squitter, const std::string& name, double gsLat, double gsLon, bool gsPos, std::vector<Insight>& out) {
        std::vector<HfdlPropagation::Up> ups;
        hfdl_.heard(t, gsId, khz, snr, squitter, &ups);
        for (const auto& u : ups) {
            Insight i; i.time = t; i.area = "aviation"; i.kind = "hfdl_station_up"; i.key = "HFDL " + std::to_string(gsId);
            char b[200];
            const std::string nm = name.empty() ? "ground station " + std::to_string(gsId) : name;
            if (home_ && gsPos) snprintf(b, sizeof b, "HFDL %s heard again on %d kHz after %.0f h (%.0f km %s)", nm.c_str(), u.khz, u.silentSec / 3600,
                                         distanceKm(hLat_, hLon_, gsLat, gsLon), compass8(bearingDeg(hLat_, hLon_, gsLat, gsLon)));
            else snprintf(b, sizeof b, "HFDL %s heard again on %d kHz after %.0f h", nm.c_str(), u.khz, u.silentSec / 3600);
            i.text = b;
            if (gsPos) { i.hasPos = true; i.lat = gsLat; i.lon = gsLon; }
            out.push_back(i);
        }
    }
    void navtex(char station, char subject, const std::string& header, const std::string& text, int64_t rx, std::vector<Insight>& out) {
        std::vector<marine::NavtexChange> c;
        navtex_.add(station, subject, header, text, rx, c);
        navtexChanges(c, out);
    }
    void mesh(const MeshCensusFrame& f, std::vector<Insight>& out) {
        std::vector<std::pair<std::string, std::string>> v;
        mesh_.add(f, &v);
        for (const auto& [k, t] : v) { Insight i; i.time = f.t; i.area = "iot"; i.kind = k; i.key = k == "mesh_busy" ? "mesh" : f.from; i.severity = k == "mesh_busy" ? 1 : 0; i.text = t; out.push_back(i); }
    }
    void lorawan(const LwHealthFrame& f, std::vector<Insight>& out) {
        std::vector<LorawanHealth::Finding> v;
        lw_.add(f, v);
        for (const auto& x : v) { Insight i; i.time = f.t; i.area = "iot"; i.kind = x.kind; i.key = x.key; i.severity = x.severity; i.text = x.text; out.push_back(i); }
    }
    // a reading of a meter the user marked as theirs (litres)
    void meter(const std::string& key, int64_t utc, double litres, std::vector<Insight>& out) {
        auto& m = leakFor(key);
        std::vector<WaterLeakCheck::Finding> f;
        m.add(utc, litres, f);
        leaks(key, (double)utc, f, out);
    }

    AdsbManoeuvreWatch& manoeuvres() { return man_; }
    ThermalMap& thermals() { return therm_; }
    HfdlPropagation& hfdl() { return hfdl_; }
    AisDucting& ducting() { return duct_; }
    marine::NavtexInForce& navtexInForce() { return navtex_; }
    MeshCensus& meshCensus() { return mesh_; }
    LorawanHealth& lorawanHealth() { return lw_; }
    WaterLeakCheck& leakFor(const std::string& key) { auto it = leak_.find(key); if (it == leak_.end()) { it = leak_.emplace(key, WaterLeakCheck()).first; it->second.tzOffsetSec = tz_; } return it->second; }
    const std::map<std::string, WaterLeakCheck>& leaks() const { return leak_; }
    bool home(double& lat, double& lon) const { lat = hLat_; lon = hLon_; return home_; }

private:
    AdsbManoeuvreWatch man_;
    ThermalMap therm_;
    HfdlPropagation hfdl_;
    AisDucting duct_;
    marine::NavtexInForce navtex_;
    MeshCensus mesh_;
    LorawanHealth lw_;
    std::map<std::string, WaterLeakCheck> leak_;
    bool home_ = false; double hLat_ = 0, hLon_ = 0; int tz_ = 0;

    void ducts(const std::vector<AisDuctEvent>& v, std::vector<Insight>& out) {
        for (const auto& d : v) {
            Insight i; i.time = d.t; i.area = "marine"; i.kind = d.start ? "ais_ducting" : "ais_ducting_end"; i.key = "AIS"; i.severity = 0; i.text = d.text;
            if (d.start) { i.hasPos = true; i.lat = d.farLat; i.lon = d.farLon; }
            out.push_back(i);
        }
    }
    void navtexChanges(const std::vector<marine::NavtexChange>& c, std::vector<Insight>& out) {
        for (const auto& x : c) {
            if (x.kind == "replaced") continue;
            Insight i; i.time = (double)x.t; i.area = "marine"; i.key = x.w.header;
            const std::string what = std::string("NAVTEX ") + x.w.header + " (" + x.w.title + ")";
            if (x.kind == "new") { i.kind = "navtex_in_force"; i.text = what + " in force"; i.severity = x.w.subject == 'D' || x.w.subject == 'B' ? 1 : 0; }
            else if (x.kind == "cancelled") { i.kind = "navtex_cancelled"; i.text = what + " cancelled by " + x.by; }
            else if (x.kind == "expired") { i.kind = "navtex_expired"; i.text = what + " expired"; }
            else if (x.kind == "dropped") { i.kind = "navtex_cancelled"; i.text = what + " no longer in the station's in-force list"; }
            else { i.kind = "navtex_expired"; i.text = what + " dropped after its usual lifetime (assumed)"; }
            out.push_back(i);
        }
    }
    void leaks(const std::string& key, double t, const std::vector<WaterLeakCheck::Finding>& f, std::vector<Insight>& out) {
        for (const auto& x : f) { Insight i; i.time = t; i.area = "iot"; i.kind = x.kind; i.key = key; i.severity = x.severity; i.text = "Meter " + key + ": " + x.text; out.push_back(i); }
    }
};

}  // namespace dect2::insight
