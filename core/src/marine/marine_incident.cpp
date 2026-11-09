#include "dect2/marine/marine_incident.h"
#include "dect2/ais/ais_alerts.h"
#include "dect2/ais/ais_mid.h"
#include "dect2/marine/marine_navtex_geo.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dect2 {
namespace marine {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr size_t kAisCap = 30000;

std::string upperOf(const std::string& s) {
    std::string o = s;
    for (auto& c : o) c = (char)std::toupper((unsigned char)c);
    return o;
}
bool hasWord(const std::string& hay, const std::string& needle) {   // both upper case; the needle stands as a word
    if (needle.size() < 3) return false;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) {
        const bool l = p == 0 || !std::isalnum((unsigned char)hay[p - 1]);
        const bool r = p + needle.size() >= hay.size() || !std::isalnum((unsigned char)hay[p + needle.size()]);
        if (l && r) return true;
    }
    return false;
}
std::string mmsiText(uint32_t m) { char b[16]; snprintf(b, sizeof b, "%09u", (unsigned)m); return b; }
std::string posText(double lat, double lon) {
    char b[48];
    snprintf(b, sizeof b, "%.3f%c %.3f%c", std::fabs(lat), lat >= 0 ? 'N' : 'S', std::fabs(lon), lon >= 0 ? 'E' : 'W');
    return b;
}
bool isDeviceKind(int k) { return k == AIS_MMSI_SART || k == AIS_MMSI_MOB || k == AIS_MMSI_EPIRB; }
const char* deviceKindName(int k) { return k == AIS_MMSI_SART ? "AIS-SART" : k == AIS_MMSI_MOB ? "Man overboard" : "EPIRB-AIS"; }
const char* deviceSource(int k) { return k == AIS_MMSI_SART ? "AIS-SART" : k == AIS_MMSI_MOB ? "MOB" : "EPIRB"; }
bool isDevice(uint32_t mmsi) { return isDeviceKind(aisMmsiInfo(mmsi).kind); }
// "TEST" in the text of the device, or a navigation status other than 14 ("AIS-SART active"): a test transmission (IEC 61097-14: a test sends status 15)
bool deviceIsTest(const std::string& name, const std::string& dest, int navStatus) {
    const std::string u = upperOf(name + " " + dest);
    if (hasWord(u, "TEST")) return true;
    if (hasWord(u, "ACTIVE")) return false;
    return navStatus >= 0 && navStatus != 14;
}
std::string sub(const std::string& s, size_t n) { return s.size() <= n ? s : s.substr(0, n) + "..."; }

} // namespace

double incidentDistanceNm(double a, double b, double c, double d) { return aisDistanceNm(a, b, c, d); }
double incidentBearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double r = kPi / 180;
    const double dl = (lon2 - lon1) * r;
    const double y = std::sin(dl) * std::cos(lat2 * r);
    const double x = std::cos(lat1 * r) * std::sin(lat2 * r) - std::sin(lat1 * r) * std::cos(lat2 * r) * std::cos(dl);
    return std::fmod(std::atan2(y, x) / r + 360.0, 360.0);
}

IncidentBoard& IncidentBoard::global() { static IncidentBoard b; return b; }

void IncidentBoard::configure(const IncidentConfig& c) { std::lock_guard<std::mutex> lk(mu_); cfg_ = c; }
IncidentConfig IncidentBoard::config() const { std::lock_guard<std::mutex> lk(mu_); return cfg_; }
void IncidentBoard::setOwn(bool valid, double lat, double lon) {
    std::lock_guard<std::mutex> lk(mu_);
    ownValid_ = valid; ownLat_ = lat; ownLon_ = lon;
}
size_t IncidentBoard::openCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (const auto& i : inc_) if (!i.closed && !i.test && !i.mergedInto) n++;
    return n;
}
void IncidentBoard::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    inc_.clear(); alerts_.clear(); ais_.clear(); nav_.clear(); lastNear_.clear();
}
std::vector<IncidentAlert> IncidentBoard::takeAlerts() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<IncidentAlert> o;
    o.swap(alerts_);
    return o;
}
std::vector<Incident> IncidentBoard::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Incident> o;
    for (const auto& i : inc_) if (!i.mergedInto) o.push_back(i);
    std::stable_sort(o.begin(), o.end(), [](const Incident& a, const Incident& b) {
        if (a.closed != b.closed) return !a.closed;
        if (a.test != b.test) return !a.test;
        return a.lastUtc > b.lastUtc;
    });
    return o;
}

Incident* IncidentBoard::findOpen(const std::string& mmsi) {
    for (auto& i : inc_) if (!i.closed && !i.mergedInto && i.mmsi == mmsi) return &i;
    for (auto& i : inc_) if (!i.closed && !i.mergedInto && std::find(i.devices.begin(), i.devices.end(), mmsi) != i.devices.end()) return &i;
    return nullptr;
}

Incident& IncidentBoard::open(const std::string& kind, const std::string& mmsi, double now) {
    if (inc_.size() >= cfg_.maxIncidents) {   // the oldest closed (or merged) one goes
        size_t k = inc_.size();
        for (size_t j = 0; j < inc_.size(); j++) if ((inc_[j].closed || inc_[j].mergedInto) && (k == inc_.size() || inc_[j].lastUtc < inc_[k].lastUtc)) k = j;
        if (k == inc_.size()) k = 0;
        inc_.erase(inc_.begin() + (long)k);
    }
    Incident i;
    i.id = nextId_++;
    i.kind = kind; i.mmsi = mmsi; i.openedUtc = i.lastUtc = now;
    inc_.push_back(i);
    return inc_.back();
}

void IncidentBoard::addEvent(Incident& i, double utc, const std::string& src, const std::string& text, bool mergeRepeat) {
    if (mergeRepeat && !i.timeline.empty()) {
        IncidentEvent& l = i.timeline.back();
        if (l.source == src && l.text == text && utc - l.utc <= cfg_.repeatSec) { l.count++; i.lastUtc = std::max(i.lastUtc, utc); return; }
    }
    IncidentEvent e;
    e.utc = utc; e.source = src; e.text = text;
    i.timeline.push_back(e);
    if (i.timeline.size() > 200) i.timeline.erase(i.timeline.begin());
    i.lastUtc = std::max(i.lastUtc, utc);
}

void IncidentBoard::alert(Incident& i, double now, const std::string& what, const std::string& text) {
    if (i.test) return;
    IncidentAlert a;
    a.id = i.id; a.utc = now; a.what = what; a.text = text; a.kind = i.kind; a.mmsi = i.mmsi;
    a.hasPos = i.hasPos; a.lat = i.lat; a.lon = i.lon; a.distNm = i.hasOwn ? i.distNm : -1;
    alerts_.push_back(a);
    if (alerts_.size() > 100) alerts_.erase(alerts_.begin());
}

void IncidentBoard::pickPosition(Incident& i) {
    if (i.hasDscPos) { i.hasPos = true; i.lat = i.dscLat; i.lon = i.dscLon; i.posSource = "DSC"; }
    else if (i.hasAisPos) {
        i.hasPos = true; i.lat = i.aisLat; i.lon = i.aisLon;
        if (i.posSource.empty() || i.posSource == "DSC") i.posSource = "AIS";
    }
    if (i.hasDscPos && i.hasAisPos) {
        i.posGapNm = aisDistanceNm(i.dscLat, i.dscLon, i.aisLat, i.aisLon);
        i.posGapFlag = i.posGapNm > cfg_.gapFlagNm;
    }
}

void IncidentBoard::applyAis(Incident& i, const AisPoint& p, double now) {
    if (now - p.utc > cfg_.aisMaxAgeSec) return;
    const bool first = !i.hasAisPos;
    const double moved = first ? 1e9 : aisDistanceNm(i.aisLat, i.aisLon, p.lat, p.lon);
    i.hasAisPos = true; i.aisLat = p.lat; i.aisLon = p.lon; i.aisAgeSec = std::max(0.0, now - p.utc);
    if (i.name.empty() && !p.name.empty()) i.name = p.name;
    if (first || moved > 1.0) {
        char b[160];
        snprintf(b, sizeof b, "AIS position %s, %s%.0f min old%s", posText(p.lat, p.lon).c_str(), "", i.aisAgeSec / 60.0,
                 p.sog >= 0 && p.sog < 102 ? (std::string(", ") + std::to_string((int)std::lround(p.sog)) + " kn").c_str() : "");
        addEvent(i, now, "AIS", b, false);
    }
}

void IncidentBoard::refresh(Incident& i, double now, bool full) {
    if (!i.hasPos) return;
    if (ownValid_) {
        i.hasOwn = true;
        i.distNm = aisDistanceNm(ownLat_, ownLon_, i.lat, i.lon);
        i.bearingDeg = incidentBearingDeg(ownLat_, ownLon_, i.lat, i.lon);
    }
    auto last = lastNear_.find(i.id);
    if (!full && last != lastNear_.end() && now - last->second < 30) return;
    lastNear_[i.id] = now;
    const uint32_t self = (uint32_t)std::strtoul(i.mmsi.c_str(), nullptr, 10);
    std::vector<IncidentNear> v;
    for (const auto& kv : ais_) {
        const AisPoint& p = kv.second;
        if (kv.first == self || isDevice(kv.first)) continue;
        if (p.cls == AIS_CLASS_ATON || p.cls == AIS_CLASS_BASE) continue;
        const double age = now - p.utc;
        if (age > cfg_.nearbyMaxAgeSec || age < -60) continue;
        const double d = aisDistanceNm(i.lat, i.lon, p.lat, p.lon);
        if (d > cfg_.nearbyNm) continue;
        IncidentNear n;
        n.mmsi = kv.first; n.name = p.name; n.distNm = d; n.bearingDeg = incidentBearingDeg(i.lat, i.lon, p.lat, p.lon);
        n.sogKn = p.sog; n.ageSec = age;
        const AisCpa c = aisCpa(i.lat, i.lon, 0, 0, p.lat, p.lon, p.sog, p.cog);
        n.dcpaNm = c.valid ? c.dcpaNm : d; n.tcpaMin = c.valid ? c.tcpaMin : 0;
        v.push_back(n);
    }
    std::sort(v.begin(), v.end(), [](const IncidentNear& a, const IncidentNear& b) { return a.distNm < b.distNm; });
    if (v.size() > cfg_.maxNearby) v.resize(cfg_.maxNearby);
    const bool wasEmpty = i.nearby.empty();
    i.nearby = v;
    if (wasEmpty && !v.empty()) {
        char b[200];
        snprintf(b, sizeof b, "%zu vessel%s within %.0f nm, nearest %s %.1f nm", v.size(), v.size() == 1 ? "" : "s", cfg_.nearbyNm,
                 v[0].name.empty() ? mmsiText(v[0].mmsi).c_str() : v[0].name.c_str(), v[0].distNm);
        addEvent(i, now, "Board", b, false);
    }
}

void IncidentBoard::joinNavtex(Incident& i, const NavRec& n, double now) {
    if (i.test || n.utc < i.openedUtc - cfg_.navtexBeforeSec || n.utc > i.lastUtc + cfg_.navtexAfterSec) return;
    for (const auto& x : i.navtex) if (x.header == n.m.header && std::fabs(x.utc - n.utc) < 1) return;
    if (i.navtex.size() >= 6) return;
    std::string why;
    if (!i.mmsi.empty() && n.upper.find(i.mmsi) != std::string::npos) why = "names the MMSI " + i.mmsi;
    else if (i.name.size() >= 4 && hasWord(n.upper, upperOf(i.name))) why = "names the vessel " + i.name;
    if (why.empty() && i.hasPos) {
        for (size_t k = 0; k < n.lat.size(); k++) {
            const double d = aisDistanceNm(i.lat, i.lon, n.lat[k], n.lon[k]);
            if (d <= cfg_.navtexNm) { char b[120]; snprintf(b, sizeof b, "position %s is %.0f nm from the scene", n.posText[k].c_str(), d); why = b; break; }
        }
    }
    if (why.empty() && i.hasPos && (n.m.subject == 'A' || n.m.subject == 'D' || n.m.subject == 'L')) {
        const auto& tab = navtexAreaTable();
        for (int a : n.areas) if (tab[(size_t)a].contains(i.lat, i.lon)) { why = "covers the " + tab[(size_t)a].name; break; }
    }
    if (why.empty()) return;
    IncidentNavtex x;
    x.header = n.m.header; x.subject = n.m.subjectName; x.reason = why; x.utc = n.utc;
    std::string t = n.m.text;
    for (auto& c : t) if (c == '\n' || c == '\r') c = ' ';
    x.excerpt = sub(t, 160);
    i.navtex.push_back(x);
    addEvent(i, now, "NAVTEX", n.m.header + " (" + n.m.subjectName + "): " + why, false);
    alert(i, now, "navtex", "NAVTEX " + n.m.header + " " + why);
}

void IncidentBoard::joinDevices(Incident& i, double now) {
    if (!i.hasPos || i.test || i.kind != "DSC distress") return;
    for (const auto& kv : ais_) {
        const int k = aisMmsiInfo(kv.first).kind;
        if (!isDeviceKind(k)) continue;
        const AisPoint& p = kv.second;
        if (now - p.utc > 3600 || deviceIsTest(p.name, p.dest, p.navStatus)) continue;
        const std::string dm = mmsiText(kv.first);
        if (std::find(i.devices.begin(), i.devices.end(), dm) != i.devices.end()) continue;
        const double d = aisDistanceNm(i.lat, i.lon, p.lat, p.lon);
        if (d > cfg_.joinNm) continue;
        i.devices.push_back(dm);
        // a device-only incident of this device folds into this one
        for (auto& o : inc_) if (&o != &i && !o.mergedInto && !o.closed && o.mmsi == dm && o.kind != "DSC distress") {
            o.mergedInto = i.id;
            for (const auto& e : o.timeline) i.timeline.push_back(e);
            std::stable_sort(i.timeline.begin(), i.timeline.end(), [](const IncidentEvent& a, const IncidentEvent& b) { return a.utc < b.utc; });
        }
        char b[160];
        snprintf(b, sizeof b, "%s %s heard %.1f nm from the DSC position", deviceKindName(k), dm.c_str(), d);
        addEvent(i, now, deviceSource(k), b, false);
        alert(i, now, "device", b);
    }
}

void IncidentBoard::onDsc(const DscCall& c, double now) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!cfg_.enabled || !c.eccOk || !c.distress) return;
    std::string station = !c.distressMmsi.empty() ? c.distressMmsi : c.fromMmsi;
    if (station.empty() || station.find('~') != std::string::npos) return;
    const bool ack = c.kind.find("cknowledg") != std::string::npos;
    const bool relay = c.kind.find("relay") != std::string::npos;
    Incident* ip = findOpen(station);
    const bool fresh = !ip;
    if (fresh) ip = &open("DSC distress", station, now);
    Incident& i = *ip;
    if (i.kind != "DSC distress") { i.kind = "DSC distress"; i.test = false; }   // a device-only incident the DSC call now explains
    if (!ack && !relay) i.dscCalls++;
    if (relay) i.relays++;
    if (c.nature >= 0 && !c.natureName.empty()) i.nature = c.natureName;
    if (i.country.empty()) i.country = c.distressMmsi.empty() ? c.fromCountry : (c.distressCountry.empty() ? c.fromCountry : c.distressCountry);
    if (c.hasPos) {
        const bool changed = !i.hasDscPos || aisDistanceNm(i.dscLat, i.dscLon, c.lat, c.lon) > 0.5;
        i.hasDscPos = true; i.dscLat = c.lat; i.dscLon = c.lon;
        if (changed && !fresh) { char b[100]; snprintf(b, sizeof b, "New DSC position %s", posText(c.lat, c.lon).c_str()); addEvent(i, now, "DSC", b, false); }
    }
    if (ack) {
        i.acked = true; i.ackedBy = c.fromMmsi;
        addEvent(i, now, "DSC", "Acknowledged by " + c.fromMmsi + (c.fromCountry.empty() ? "" : " (" + c.fromCountry + ")"), true);
        alert(i, now, "ack", "Distress of " + station + " acknowledged by " + c.fromMmsi);
    } else {
        std::string t = c.kind.empty() ? c.formatName : c.kind;
        if (!i.nature.empty()) t += ", " + i.nature;
        if (c.hasPos) t += ", " + posText(c.lat, c.lon);
        if (c.hasTime) { char b[16]; snprintf(b, sizeof b, ", %02d%02d UTC", c.utcHour, c.utcMin); t += b; }
        if (relay) t += " (relayed by " + c.fromMmsi + ")";
        addEvent(i, now, "DSC", t, true);
    }
    const uint32_t sm = (uint32_t)std::strtoul(station.c_str(), nullptr, 10);
    auto a = ais_.find(sm);
    if (a != ais_.end()) { applyAis(i, a->second, now); }
    else if (fresh) addEvent(i, now, "Board", "No AIS position of " + station + " heard", false);
    pickPosition(i);
    joinDevices(i, now);
    for (const auto& n : nav_) joinNavtex(i, n, now);
    refresh(i, now, true);
    if (fresh) {
        i.pinned = true;
        char b[220];
        std::string where = i.hasPos ? posText(i.lat, i.lon) + " (" + i.posSource + ")" : "no position";
        if (i.hasOwn) { char d[64]; snprintf(d, sizeof d, ", %.0f nm at %03.0f deg from you", i.distNm, i.bearingDeg); where += d; }
        snprintf(b, sizeof b, "DSC distress %s%s: %s", station.c_str(), i.nature.empty() ? "" : (" " + i.nature).c_str(), where.c_str());
        alert(i, now, "opened", b);
    } else if (i.posGapFlag) {
        // the gap alert is raised once
        bool had = false;
        for (const auto& e : i.timeline) if (e.source == "Board" && e.text.rfind("DSC and AIS positions", 0) == 0) { had = true; break; }
        if (!had) {
            char b[160];
            snprintf(b, sizeof b, "DSC and AIS positions are %.1f nm apart (AIS %.0f min old)", i.posGapNm, i.aisAgeSec / 60.0);
            addEvent(i, now, "Board", b, false);
            alert(i, now, "position", b);
        }
    }
    if (fresh && i.posGapFlag) {
        char b[160];
        snprintf(b, sizeof b, "DSC and AIS positions are %.1f nm apart (AIS %.0f min old)", i.posGapNm, i.aisAgeSec / 60.0);
        addEvent(i, now, "Board", b, false);
    }
}

void IncidentBoard::deviceSeen(uint32_t mmsi, const AisVessel& v, double now) {
    const int k = aisMmsiInfo(mmsi).kind;
    const std::string dm = mmsiText(mmsi);
    const bool test = deviceIsTest(v.name, v.destination, v.navStatus);
    Incident* ip = nullptr;
    for (auto& o : inc_) if (!o.mergedInto && !o.closed && (o.mmsi == dm || std::find(o.devices.begin(), o.devices.end(), dm) != o.devices.end())) { ip = &o; break; }
    if (ip) {
        Incident& i = *ip;
        i.lastUtc = std::max(i.lastUtc, now);
        if (i.mmsi == dm) {   // a device-only incident
            const bool wasTest = i.test;
            i.test = test;
            const double moved = i.hasAisPos ? aisDistanceNm(i.aisLat, i.aisLon, v.lat, v.lon) : 1e9;
            i.hasAisPos = true; i.aisLat = v.lat; i.aisLon = v.lon; i.aisAgeSec = 0;
            i.hasPos = true; i.lat = v.lat; i.lon = v.lon; i.posSource = deviceSource(k);
            if (wasTest && !test) {
                i.pinned = true;
                addEvent(i, now, deviceSource(k), std::string(deviceKindName(k)) + " is now ACTIVE", false);
                alert(i, now, "opened", std::string(deviceKindName(k)) + " " + dm + " ACTIVE at " + posText(v.lat, v.lon));
            } else if (moved > 0.5) {
                addEvent(i, now, deviceSource(k), "Position " + posText(v.lat, v.lon), true);
            }
            refresh(i, now, false);
            for (const auto& n : nav_) joinNavtex(i, n, now);
            // a DSC incident may already be waiting near it
            if (!test) for (auto& d : inc_) if (!d.closed && !d.mergedInto && d.kind == "DSC distress" && d.hasPos) {
                if (aisDistanceNm(d.lat, d.lon, v.lat, v.lon) <= cfg_.joinNm) { joinDevices(d, now); break; }
            }
        } else if (!test) {
            refresh(i, now, false);
        }
        return;
    }
    // a DSC incident near it takes it in
    if (!test) for (auto& d : inc_) if (!d.closed && !d.mergedInto && d.kind == "DSC distress" && d.hasPos) {
        if (aisDistanceNm(d.lat, d.lon, v.lat, v.lon) <= cfg_.joinNm) {
            d.devices.push_back(dm);
            char b[160];
            snprintf(b, sizeof b, "%s %s heard %.1f nm from the DSC position", deviceKindName(k), dm.c_str(), aisDistanceNm(d.lat, d.lon, v.lat, v.lon));
            addEvent(d, now, deviceSource(k), b, false);
            alert(d, now, "device", b);
            return;
        }
    }
    Incident& i = open(deviceKindName(k), dm, now);
    i.test = test;
    i.hasAisPos = true; i.aisLat = v.lat; i.aisLon = v.lon; i.hasPos = true; i.lat = v.lat; i.lon = v.lon; i.posSource = deviceSource(k);
    addEvent(i, now, deviceSource(k), std::string(deviceKindName(k)) + (test ? " test transmission " : " ACTIVE ") + posText(v.lat, v.lon), false);
    for (const auto& n : nav_) joinNavtex(i, n, now);
    refresh(i, now, true);
    if (!test) {
        i.pinned = true;
        char b[200];
        std::string where = posText(v.lat, v.lon);
        if (i.hasOwn) { char d[64]; snprintf(d, sizeof d, ", %.0f nm at %03.0f deg from you", i.distNm, i.bearingDeg); where += d; }
        snprintf(b, sizeof b, "%s %s ACTIVE: %s", deviceKindName(k), dm.c_str(), where.c_str());
        alert(i, now, "opened", b);
    }
}

void IncidentBoard::onAis(const AisVessel& v, double now) {
    if (!v.hasPos || v.mmsi == 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (!cfg_.enabled) return;
    AisPoint& p = ais_[v.mmsi];
    p.lat = v.lat; p.lon = v.lon; p.utc = now; p.sog = v.sog; p.cog = v.cog; p.navStatus = v.navStatus; p.cls = v.cls;
    if (!v.name.empty()) p.name = v.name;
    if (!v.destination.empty()) p.dest = v.destination;
    if (ais_.size() > kAisCap) {   // forget the stations that have been silent longest
        std::vector<std::pair<double, uint32_t>> a;
        a.reserve(ais_.size());
        for (const auto& kv : ais_) a.push_back({kv.second.utc, kv.first});
        std::nth_element(a.begin(), a.begin() + (long)a.size() / 4, a.end());
        for (long k = 0; k < (long)a.size() / 4; k++) ais_.erase(a[(size_t)k].second);
    }
    if (isDevice(v.mmsi)) { deviceSeen(v.mmsi, v, now); return; }
    if (inc_.empty()) return;
    const std::string ms = mmsiText(v.mmsi);
    for (auto& i : inc_) {
        if (i.closed || i.mergedInto || i.mmsi != ms) continue;
        const bool hadPos = i.hasPos;
        const double oldLat = i.lat, oldLon = i.lon;
        applyAis(i, p, now);
        pickPosition(i);
        i.lastUtc = std::max(i.lastUtc, now);
        if (hadPos && !i.hasDscPos && aisDistanceNm(oldLat, oldLon, i.lat, i.lon) > 2.0) alert(i, now, "position", "Vessel in distress moved to " + posText(i.lat, i.lon));
        refresh(i, now, false);
    }
    for (auto& i : inc_) if (!i.closed && !i.mergedInto && !i.test && i.hasPos && i.mmsi != ms) {
        // a ship that comes into the 20 nm circle shows up in the list within 30 s
        refresh(i, now, false);
    }
}

void IncidentBoard::onNavtex(const NavtexMessage& m, double now) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!cfg_.enabled || m.text.empty()) return;
    NavRec n;
    n.m = m; n.utc = now; n.upper = upperOf(m.text);
    if (n.m.text.size() > 4000) n.m.text.resize(4000);
    for (const auto& g : navtexPositions(m.text)) { n.lat.push_back(g.lat); n.lon.push_back(g.lon); n.posText.push_back(g.text); }
    const auto& tab = navtexAreaTable();
    for (const auto& a : navtexAreas(m.text)) for (size_t k = 0; k < tab.size(); k++) if (tab[k].name == a.name) n.areas.push_back((int)k);
    for (const auto& o : nav_) if (o.m.header == n.m.header && o.m.text == n.m.text && now - o.utc < 3600) return;   // a repeat of the same message
    nav_.push_back(n);
    while (nav_.size() > 200 || (!nav_.empty() && now - nav_.front().utc > 48 * 3600.0)) nav_.erase(nav_.begin());
    for (auto& i : inc_) if (!i.closed && !i.mergedInto) joinNavtex(i, nav_.back(), now);
}

void IncidentBoard::tick(double now) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& i : inc_) {
        if (i.closed || i.mergedInto) continue;
        if (now - i.lastUtc > cfg_.closeAfterSec) { i.closed = true; addEvent(i, now, "Board", "Closed: no news for a long time", false); }
        else if (i.hasPos && !i.test) refresh(i, now, false);
    }
    inc_.erase(std::remove_if(inc_.begin(), inc_.end(), [&](const Incident& i) { return (i.test || i.mergedInto) && now - i.lastUtc > cfg_.testKeepSec; }), inc_.end());
    for (auto it = lastNear_.begin(); it != lastNear_.end();) {
        bool live = false;
        for (const auto& i : inc_) if (i.id == it->first) { live = true; break; }
        it = live ? std::next(it) : lastNear_.erase(it);
    }
}

} // namespace marine
} // namespace dect2
