// See ais_apps.h. Every "not available" code of the layouts leaves the value out of the record.
#include "dect2/ais/ais_apps.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace dect2 {
namespace ais {

void aisOffset(double lat, double lon, double bearingDeg, double distM, double& outLat, double& outLon) {
    const double R = 6371000.0, d = distM / R, b = bearingDeg * M_PI / 180, la = lat * M_PI / 180, lo = lon * M_PI / 180;
    const double la2 = std::asin(std::sin(la) * std::cos(d) + std::cos(la) * std::sin(d) * std::cos(b));
    const double lo2 = lo + std::atan2(std::sin(b) * std::sin(d) * std::cos(la), std::cos(d) - std::sin(la) * std::sin(la2));
    outLat = la2 * 180 / M_PI;
    outLon = std::remainder(lo2 * 180 / M_PI, 360.0);
}

namespace {

struct R {
    const Bits& b;
    AisAppRecord& o;
    uint32_t u(size_t p, size_t l) const { return getU(b, p, l); }
    int32_t s(size_t p, size_t l) const { return getS(b, p, l); }
    std::string t(size_t p, size_t l) const { return getText(b, p, l); }
    void f(const char* key, const char* fmt, ...) __attribute__((format(printf, 3, 4))) {
        char buf[200];
        va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
        o.fields.push_back({key, buf});
    }
    void fs(const char* key, const std::string& v) { if (!v.empty()) o.fields.push_back({key, v}); }
    void add(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char buf[200];
        va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
        if (!o.summary.empty()) o.summary += ", ";
        o.summary += buf;
    }
    void kind(const char* title, int cat) { o.title = title; o.category = cat; }
    void pos(double lat, double lon) { if (std::fabs(lat) > 90 || std::fabs(lon) > 180) return; o.hasPos = true; o.lat = lat; o.lon = lon; }
    // position fields in 1/1000 minute (latitude 24 bits, longitude 25)
    bool ll3(size_t laAt, size_t loAt) {
        const int la = s(laAt, 24), lo = s(loAt, 25);
        if (la == 91000 || lo == 181000 || std::abs(la) > 90 * 60000 || std::abs(lo) > 180 * 60000) return false;
        pos(la / 60000.0, lo / 60000.0);
        return true;
    }
    bool ll4(size_t laAt, size_t loAt) {
        const int la = s(laAt, 27), lo = s(loAt, 28);
        if (la == 91 * 600000 || lo == 181 * 600000 || std::abs(la) > 90 * 600000 || std::abs(lo) > 180 * 600000) return false;
        pos(la / 600000.0, lo / 600000.0);
        return true;
    }
    std::string ddhhmm(uint32_t d, uint32_t h, uint32_t m, uint32_t mo = 0) const {
        char x[40];
        if (h >= 24 || m >= 60) { if (!d && !mo) return ""; }
        if (mo) snprintf(x, sizeof x, "%02u-%02u %02u:%02u UTC", mo, d, h < 24 ? h : 0, m < 60 ? m : 0);
        else snprintf(x, sizeof x, "day %u %02u:%02u UTC", d, h < 24 ? h : 0, m < 60 ? m : 0);
        return x;
    }
};

const char* trendText(uint32_t v) { static const char* const n[4] = {"steady", "decreasing", "increasing", ""}; return n[v & 3]; }

// the fields IMO 236 FI 11 and IMO 289 FI 31 share in form: 'imo289' selects the coding
void metCommon(R& r, size_t wind, bool imo289) {
    const size_t off = wind;
    const uint32_t ws = r.u(off, 7), gu = r.u(off + 7, 7), wd = r.u(off + 14, 9), gd = r.u(off + 23, 9);
    const uint32_t naDir = imo289 ? 360 : 511;
    if (ws < 127) { r.f("Wind", "%u kn", ws); r.add("wind %u kn", ws); }
    if (gu < 127) r.f("Gust", "%u kn", gu);
    if (wd < naDir && wd < 360) { r.f("Wind direction", "%u deg", wd); r.add("from %u deg", wd); }
    if (gd < naDir && gd < 360) r.f("Gust direction", "%u deg", gd);
    size_t p = off + 32;
    if (imo289) {
        const int t = r.s(p, 11); if (t != -1024) { r.f("Air temperature", "%.1f C", t / 10.0); r.add("air %.1f C", t / 10.0); } p += 11;
        const uint32_t h = r.u(p, 7); if (h <= 100) r.f("Humidity", "%u %%", h); p += 7;
        const int dew = r.s(p, 10); if (dew != 501) r.f("Dew point", "%.1f C", dew / 10.0); p += 10;
        const uint32_t pr = r.u(p, 9);
        if (pr == 0) r.f("Pressure", "799 hPa or less"); else if (pr <= 401) { r.f("Pressure", "%u hPa", 799 + pr); r.add("%u hPa", 799 + pr); } else if (pr == 402) r.f("Pressure", "1201 hPa or more");
        p += 9;
        const uint32_t pt = r.u(p, 2); if (pt < 3) r.f("Pressure trend", "%s", trendText(pt)); p += 2;
        const uint32_t gt = r.u(p, 1), vi = r.u(p + 1, 7); if (vi < 127) r.f("Visibility", "%s%.1f nm", gt ? "over " : "", vi / 10.0); p += 8;
        const uint32_t wl = r.u(p, 12); if (wl != 4001 && wl <= 4000) { r.f("Water level", "%.2f m", wl / 100.0 - 10.0); r.add("water %.2f m", wl / 100.0 - 10.0); } p += 12;
    } else {
        const uint32_t t = r.u(p, 11); if (t != 2047) { r.f("Air temperature", "%.1f C", t / 10.0 - 60.0); r.add("air %.1f C", t / 10.0 - 60.0); } p += 11;
        const uint32_t h = r.u(p, 7); if (h <= 100) r.f("Humidity", "%u %%", h); p += 7;
        const uint32_t dew = r.u(p, 10); if (dew != 1023) r.f("Dew point", "%.1f C", dew / 10.0 - 20.0); p += 10;
        const uint32_t pr = r.u(p, 9); if (pr != 511) { r.f("Pressure", "%u hPa", pr + 800); r.add("%u hPa", pr + 800); } p += 9;
        const uint32_t pt = r.u(p, 2); if (pt < 3) r.f("Pressure trend", "%s", trendText(pt)); p += 2;
        const uint32_t vi = r.u(p, 8); if (vi != 255) r.f("Visibility", "%.1f nm", vi / 10.0); p += 8;
        const uint32_t wl = r.u(p, 9); if (wl != 511) { r.f("Water level", "%.1f m", wl / 10.0 - 10.0); r.add("water %.1f m", wl / 10.0 - 10.0); } p += 9;
    }
    const uint32_t lt = r.u(p, 2); if (lt < 3) r.f("Water level trend", "%s", trendText(lt)); p += 2;
    const uint32_t c1 = r.u(p, 8), d1 = r.u(p + 8, 9); p += 17;
    if (c1 <= 251) r.f("Surface current", "%.1f kn", c1 / 10.0);
    if (d1 < 360) r.f("Surface current direction", "%u deg", d1);
    for (int i = 2; i <= 3; i++) {
        const uint32_t c = r.u(p, 8), d = r.u(p + 8, 9), de = r.u(p + 17, 5); p += 22;
        if (c <= 251) r.f("Current", "%.1f kn at level %d", c / 10.0, i);
        if (d < 360) r.f("Current direction", "%u deg at level %d", d, i);
        if (de != 31) r.f("Current depth", "%u m at level %d", de, i);
    }
    const uint32_t wh = r.u(p, 8), wp = r.u(p + 8, 6), wa = r.u(p + 14, 9); p += 23;
    if (wh != 255 && wh <= 251) { r.f("Wave height", "%.1f m", wh / 10.0); r.add("waves %.1f m", wh / 10.0); }
    if (wp != 63 && wp <= 60) r.f("Wave period", "%u s", wp);
    if (wa < 360) r.f("Wave direction", "%u deg", wa);
    const uint32_t sh = r.u(p, 8), sp = r.u(p + 8, 6), sa = r.u(p + 14, 9); p += 23;
    if (sh != 255 && sh <= 251) r.f("Swell height", "%.1f m", sh / 10.0);
    if (sp != 63 && sp <= 60) r.f("Swell period", "%u s", sp);
    if (sa < 360) r.f("Swell direction", "%u deg", sa);
    const uint32_t ss = r.u(p, 4); if (ss <= 12) r.f("Sea state", "Beaufort %u", ss); p += 4;
    if (imo289) { const int wt = r.s(p, 10); if (wt != 501) { r.f("Water temperature", "%.1f C", wt / 10.0); r.add("sea %.1f C", wt / 10.0); } }
    else { const uint32_t wt = r.u(p, 10); if (wt != 1023) { r.f("Water temperature", "%.1f C", wt / 10.0 - 10.0); r.add("sea %.1f C", wt / 10.0 - 10.0); } }
    p += 10;
    static const char* const prec[8] = {"reserved", "rain", "thunderstorm", "freezing rain", "mixed or ice", "snow", "reserved", ""};
    const uint32_t pc = r.u(p, 3); if (pc > 0 && pc < 7 && pc != 6) r.f("Precipitation", "%s", prec[pc]); p += 3;
    const uint32_t sal = r.u(p, 9); if (sal <= 500) r.f("Salinity", "%.1f", sal / 10.0); p += 9;
    const uint32_t ic = r.u(p, 2); if (ic < 2) r.f("Ice", "%s", ic ? "yes" : "no");
}

const char* areaNotice(uint32_t n) {
    static const struct { uint8_t c; const char* t; } k[] = {
        {0,"Marine mammals habitat"},{1,"Marine mammals, reduce speed"},{2,"Marine mammals, stay clear"},{3,"Marine mammals, report sightings"},{4,"Protected habitat, reduce speed"},
        {5,"Protected habitat, stay clear"},{6,"Protected habitat, no fishing or anchoring"},{7,"Derelicts (drifting objects)"},{8,"Traffic congestion"},{9,"Marine event"},{10,"Divers down"},{11,"Swim area"},
        {12,"Dredge operations"},{13,"Survey operations"},{14,"Underwater operation"},{15,"Seaplane operations"},{16,"Fishery, nets in water"},{17,"Cluster of fishing vessels"},{18,"Fairway closed"},{19,"Harbour closed"},
        {20,"Risk"},{21,"Underwater vehicle operation"},{23,"Storm front"},{24,"Hazardous sea ice"},{25,"Storm warning"},{26,"High wind"},{27,"High waves"},{28,"Restricted visibility"},{29,"Strong currents"},{30,"Heavy icing"},
        {32,"Fishing prohibited"},{33,"No anchoring"},{34,"Entry approval required"},{35,"Entry prohibited"},{36,"Active military operations"},{37,"Firing, danger area"},{38,"Drifting mines"},
        {40,"Anchorage open"},{41,"Anchorage closed"},{42,"Anchorage prohibited"},{43,"Deep draft anchorage"},{44,"Shallow draft anchorage"},{45,"Vessel transfer operations"},
        {56,"Security alert level 1"},{57,"Security alert level 2"},{58,"Security alert level 3"},
        {64,"Distress: disabled and adrift"},{65,"Distress: sinking"},{66,"Distress: abandoning ship"},{67,"Distress: medical assistance"},{68,"Distress: flooding"},{69,"Distress: fire or explosion"},{70,"Distress: grounding"},{71,"Distress: collision"},{72,"Distress: listing or capsizing"},{73,"Distress: under assault"},{74,"Distress: person overboard"},{75,"Search and rescue area"},{76,"Pollution response area"},
        {80,"Contact VTS here"},{81,"Contact port administration here"},{82,"Do not proceed beyond here"},{83,"Await instructions before proceeding"},{84,"Proceed here, await instructions"},{85,"Clearance granted, proceed to berth"},
        {88,"Pilot boarding position"},{89,"Icebreaker waiting area"},{90,"Place of refuge"},{91,"Position of icebreakers"},{92,"Location of response units"},{93,"VTS active target"},{94,"Rogue or suspicious vessel"},{95,"Non-distress assistance requested"},
        {96,"Sunken vessel"},{97,"Submerged object"},{98,"Semi-submerged object"},{99,"Shoal area"},{100,"Shoal due north"},{101,"Shoal due east"},{102,"Shoal due south"},{103,"Shoal due west"},{104,"Channel obstruction"},{105,"Reduced vertical clearance"},{106,"Bridge closed"},{107,"Bridge partially open"},{108,"Bridge fully open"},
        {112,"Report from ship: icing"},{114,"Report from ship"},{120,"Recommended route"},{121,"Alternative route"},{122,"Recommended route through ice"},{125,"Other"},{126,"Cancellation"},{127,"Undefined"}};
    for (const auto& e : k) if (e.c == n) return e.t;
    return "Reserved";
}

// Area notice body: 'at' is the linkage ID (IMO 289 tables, 10 sub-areas of 87 bits at most)
void areaBody(R& r, size_t at) {
    r.kind("Area notice", AIS_APP_NOTICE);
    const uint32_t notice = r.u(at + 10, 7);
    r.fs("Notice", areaNotice(notice));
    r.add("%s", areaNotice(notice));
    r.f("Linkage ID", "%u", r.u(at, 10));
    const std::string when = r.ddhhmm(r.u(at + 21, 5), r.u(at + 26, 5), r.u(at + 31, 6), r.u(at + 17, 4));
    r.fs("Start", when);
    const uint32_t dur = r.u(at + 37, 18);
    if (dur != 262143 && dur) { r.f("Duration", "%u min", dur); r.add("%u min", dur); }
    size_t p = at + 55;
    std::string text;
    bool havePrev = false; double prevLa = 0, prevLo = 0;
    for (int i = 0; i < 10 && p + 87 <= r.b.size(); i++, p += 87) {
        const uint32_t kindv = r.u(p, 3);
        if (kindv > 5) break;
        if (kindv == 5) { text += r.t(p + 3, 84); continue; }
        const double scale = std::pow(10.0, (int)r.u(p + 3, 2));
        AisAreaShape sh;
        sh.kind = (int)kindv;
        if (kindv <= 2) {
            const int lo = r.s(p + 5, 25), la = r.s(p + 30, 24);
            if (lo == 181000 || la == 91000 || std::abs(lo) > 180 * 60000 || std::abs(la) > 90 * 60000) continue;
            sh.lat = la / 60000.0; sh.lon = lo / 60000.0;
            havePrev = true; prevLa = sh.lat; prevLo = sh.lon;
            if (kindv == 0) sh.radiusM = r.u(p + 57, 12) * scale;
            else if (kindv == 1) { sh.eastM = r.u(p + 57, 8) * scale; sh.northM = r.u(p + 65, 8) * scale; sh.orientDeg = r.u(p + 73, 9) % 360; }
            else { sh.radiusM = r.u(p + 57, 12) * scale; sh.leftDeg = r.u(p + 69, 9) % 360; sh.rightDeg = r.u(p + 78, 9) % 360; }
            if (!r.o.hasPos) r.pos(sh.lat, sh.lon);
        } else {
            if (!havePrev) continue;
            sh.lat = prevLa; sh.lon = prevLo;
            sh.pts.push_back({(float)prevLa, (float)prevLo});
            for (int k = 0; k < 4; k++) {
                const uint32_t brg = r.u(p + 5 + 20 * (size_t)k, 10), dist = r.u(p + 15 + 20 * (size_t)k, 10);
                if (brg == 720 || brg > 720 || dist == 0) continue;
                double la2, lo2; aisOffset(prevLa, prevLo, brg * 0.5, dist * scale, la2, lo2);
                sh.pts.push_back({(float)la2, (float)lo2});
                prevLa = la2; prevLo = lo2;
            }
        }
        r.o.shapes.push_back(std::move(sh));
    }
    r.fs("Text", text);
    if (!text.empty()) r.add("%s", text.c_str());
    r.f("Sub-areas", "%zu", r.o.shapes.size());
}

void routeBody(R& r, size_t at, size_t wpAt) {
    r.kind("Route information", AIS_APP_TRAFFIC);
    static const char* const snd[8] = {"ship", "authority", "reserved", "reserved", "reserved", "reserved", "reserved", "reserved"};
    static const char* const typ[32] = {"undefined", "mandatory", "recommended", "alternative", "recommended through ice", "ship route plan"};
    const uint32_t tv = r.u(at + 13, 5), count = r.u(wpAt - 5, 5);
    r.f("Sender", "%s", snd[r.u(at + 10, 3)]);
    r.f("Route type", "%s", tv <= 5 ? typ[tv] : "reserved");
    r.fs("Start", r.ddhhmm(r.u(at + 21, 5), r.u(at + 26, 5), r.u(at + 31, 6), r.u(at + 18, 4)));
    const uint32_t dur = r.u(at + 37, 18);
    if (dur && dur != 262143) r.f("Duration", "%u min", dur);
    AisAreaShape sh; sh.kind = 3;
    for (uint32_t i = 0; i < count && i < 16 && wpAt + 55 * i + 55 <= r.b.size(); i++) {
        const int lo = r.s(wpAt + 55 * i, 28), la = r.s(wpAt + 55 * i + 28, 27);
        if (lo == 181 * 600000 || la == 91 * 600000 || std::abs(lo) > 180 * 600000 || std::abs(la) > 90 * 600000) continue;
        sh.pts.push_back({(float)(la / 600000.0), (float)(lo / 600000.0)});
    }
    r.f("Waypoints", "%zu", sh.pts.size());
    r.add("%s, %zu waypoints", tv <= 5 ? typ[tv] : "reserved", sh.pts.size());
    if (!sh.pts.empty()) { r.pos(sh.pts[0].lat, sh.pts[0].lon); r.o.shapes.push_back(std::move(sh)); }
}

const char* ericType(uint32_t c) {   // the ERI classes most heard on rivers (CCNR Inland AIS, annex)
    switch (c) {
    case 8000: return "Vessel, type unknown"; case 8010: return "Motor freighter"; case 8020: return "Motor tanker"; case 8021: return "Motor tanker, liquid cargo type N"; case 8022: return "Motor tanker, liquid cargo type C";
    case 8023: return "Motor tanker, dry cargo as if liquid"; case 8030: return "Container vessel"; case 8040: return "Gas tanker"; case 8050: return "Motor freighter, tug"; case 8060: return "Motor tanker, tug";
    case 8070: return "Motor freighter with ships alongside"; case 8080: return "Motor freighter with tanker"; case 8090: return "Motor freighter pushing freighters"; case 8100: return "Motor freighter pushing a tank ship";
    case 8440: return "Passenger ship"; case 8441: return "Ferry"; case 8442: return "Red Cross ship"; case 8443: return "Cruise ship"; case 8444: return "Passenger ship without accommodation";
    case 8450: return "Service vessel, police, port service"; case 8460: return "Vessel, work maintenance craft"; case 8470: return "Object, towed"; case 8480: return "Fishing boat"; case 8490: return "Bunker ship"; case 8500: return "Barge, tanker, chemical"; case 8510: return "Object, not otherwise specified";
    default: return "";
    }
}

const char* sensorText(uint32_t v) { static const char* const n[8] = {"", "raw real time", "quality controlled", "predicted", "forecast", "nowcast", "", "sensor not available"}; return n[v & 7]; }

// Environmental message: 112-bit sensor reports from bit 'at' (IMO 289 FI 26, also used under DAC 367 FI 33)
void environmental(R& r, size_t at) {
    r.kind("Environmental report", AIS_APP_WATER);
    const size_t n = r.b.size();
    int reports = 0;
    for (size_t p = at; p + 112 <= n; p += 112) {
        const uint32_t ty = r.u(p, 4);
        const size_t q = p + 27;
        reports++;
        char head[16]; snprintf(head, sizeof head, "Site %u", r.u(p + 20, 7));
        const std::string when = r.ddhhmm(r.u(p + 4, 5), r.u(p + 9, 5), r.u(p + 14, 6));
        switch (ty) {
        case 0: {
            const int lo = r.s(q, 28), la = r.s(q + 28, 27);
            if (lo != 181 * 600000 && la != 91 * 600000 && std::abs(lo) <= 180 * 600000 && std::abs(la) <= 90 * 600000 && !r.o.hasPos) r.pos(la / 600000.0, lo / 600000.0);
            r.f(head, "site location, altitude %u", r.u(q + 55, 11));
            break; }
        case 1: r.f(head, "station %s", r.t(q, 84).c_str()); r.add("%s", r.t(q, 84).c_str()); break;
        case 2: {
            const uint32_t ws = r.u(q, 7), wg = r.u(q + 7, 7), wd = r.u(q + 14, 9);
            if (ws < 122) { r.f(head, "wind %u kn, gust %u kn, from %u deg (%s)", ws, wg < 122 ? wg : 0, wd < 360 ? wd : 0, sensorText(r.u(q + 32, 3))); r.add("wind %u kn", ws); }
            r.o.category = AIS_APP_WEATHER;
            break; }
        case 3: {
            const int lv = r.s(q + 1, 16);
            if (lv != -32768) { r.f(head, "water level %.2f m (%s) %s", lv / 100.0, r.u(q, 1) ? "absolute" : "relative to datum", when.c_str()); r.add("water %.2f m", lv / 100.0); }
            break; }
        case 4: case 5: case 6: { r.f(head, "current report, type %u", ty); break; }
        case 7: {
            const uint32_t sh = r.u(q, 8), wh = r.u(q + 50, 8), wt = r.u(q + 30, 10);
            if (wh < 251) r.add("waves %.1f m", wh / 10.0);
            r.f(head, "sea state: swell %.1f m, waves %.1f m, water %.1f C", sh < 251 ? sh / 10.0 : 0.0, wh < 251 ? wh / 10.0 : 0.0, wt != 1023 ? wt / 10.0 - 10.0 : 0.0);
            break; }
        case 8: r.f(head, "salinity %.1f, water %.1f C", r.u(q + 36, 9) / 10.0, r.u(q, 10) / 10.0 - 10.0); break;
        case 9: {
            const int t = r.s(q, 11); const uint32_t pr = r.u(q + 37, 9);
            r.f(head, "air %.1f C, pressure %u hPa", t / 10.0, pr + 799);
            r.add("air %.1f C", t / 10.0);
            r.o.category = AIS_APP_WEATHER;
            break; }
        case 10: { r.f(head, "air gap %.2f m, air draught %.2f m", r.u(q + 13, 13) / 100.0, r.u(q, 13) / 100.0); r.add("air gap %.2f m", r.u(q + 13, 13) / 100.0); break; }
        default: r.f(head, "report type %u", ty); break;
        }
    }
    r.f("Reports", "%d", reports);
}

const char* solasText(uint32_t v) { static const char* const n[4] = {"not available", "operational", "not operational", "no data"}; return n[v & 3]; }

bool dac1(R& r, bool broadcast, int fi) {
    const size_t n = r.b.size();
    AisAppRecord& o = r.o;
    if (!broadcast) {
        switch (fi) {
        case 0: if (n < 100) return false; r.kind("Text telegram", AIS_APP_TEXT); r.fs("Text", r.t(100, (n - 100) / 6 * 6)); r.add("%s", r.t(100, (n - 100) / 6 * 6).c_str()); r.f("Sequence", "%u", r.u(89, 11)); return true;
        case 2: if (n < 104) return false; r.kind("Interrogation for a function", AIS_APP_NETWORK); r.f("Requested DAC", "%u", r.u(88, 10)); r.f("Requested FI", "%u", r.u(98, 6)); r.add("DAC %u FI %u", r.u(88, 10), r.u(98, 6)); return true;
        case 5: if (n < 104) return false; r.kind("Application acknowledge", AIS_APP_NETWORK); r.f("For DAC", "%u", r.u(88, 10)); r.f("For FI", "%u", r.u(98, 6)); r.add("DAC %u FI %u", r.u(88, 10), r.u(98, 6)); return true;
        case 12:
            if (n < 144) return false;
            r.kind("Dangerous cargo", AIS_APP_SHIP); r.fs("Last port", r.t(88, 30));
            if (n >= 360) { r.fs("Next port", r.t(138, 30)); r.fs("Dangerous goods", r.t(188, 120)); r.fs("IMD class", r.t(308, 24)); r.f("UN number", "%u", r.u(332, 13)); r.add("%s", r.t(188, 120).c_str()); }
            return true;
        case 14: case 32: {
            if (n < 97 + (fi == 32 ? 88u : 93u)) return false;
            r.kind("Tidal window", AIS_APP_WATER);
            const size_t es = fi == 32 ? 88 : 93;
            int c = 0;
            for (size_t p = 97; p + es <= n; p += es) {
                double la, lo; uint32_t fh, fm, th, tm, cd, cs;
                if (fi == 32) { lo = r.s(p, 25) / 60000.0; la = r.s(p + 25, 24) / 60000.0; fh = r.u(p + 49, 5); fm = r.u(p + 54, 6); th = r.u(p + 60, 5); tm = r.u(p + 65, 6); cd = r.u(p + 71, 9); cs = r.u(p + 80, 8); }
                else { la = r.s(p, 27) / 600000.0; lo = r.s(p + 27, 28) / 600000.0; fh = r.u(p + 55, 5); fm = r.u(p + 60, 6); th = r.u(p + 66, 5); tm = r.u(p + 71, 6); cd = r.u(p + 77, 9); cs = r.u(p + 86, 7); }
                if (!o.hasPos) r.pos(la, lo);
                r.f("Window", "%02u:%02u to %02u:%02u, current %.1f kn from %u deg", fh, fm, th, tm, cs / 10.0, cd);
                c++;
            }
            r.add("%d windows", c);
            return true; }
        case 16: case 40: {
            if (n < 101) return false;
            const uint32_t p = r.u(88, 13);
            r.kind("Persons on board", AIS_APP_SHIP);
            if (p == 0) r.f("Persons", "not available"); else if (p == 8191) r.f("Persons", "8191 or more"); else r.f("Persons", "%u", p);
            r.add("%u persons", p);
            return true; }
        case 18: if (n < 168) return false; r.kind("Clearance time", AIS_APP_TRAFFIC); r.fs("Port", r.t(118, 120)); r.fs("Clearance", r.ddhhmm(r.u(102, 5), r.u(107, 5), r.u(112, 6), r.u(98, 4))); r.add("%s", r.t(118, 120).c_str()); if (n >= 318) { r.fs("Destination", r.t(238, 30)); if (r.s(268, 25) != 181000 && r.s(293, 24) != 91000) r.pos(r.s(293, 24) / 60000.0, r.s(268, 25) / 60000.0); } return true;
        case 20: {
            if (n < 360) return false;
            r.kind("Berthing data", AIS_APP_TRAFFIC); r.fs("Berth", r.t(191, 120)); r.add("%s", r.t(191, 120).c_str());
            r.f("Length", "%u m", r.u(98, 9)); r.f("Depth", "%.1f m", r.u(107, 8) / 10.0);
            r.fs("Time", r.ddhhmm(r.u(122, 5), r.u(127, 5), r.u(132, 6), r.u(118, 4)));
            if (r.s(311, 25) != 181000 && r.s(336, 24) != 91000) r.pos(r.s(336, 24) / 60000.0, r.s(311, 25) / 60000.0);
            return true; }
        case 23: if (n < 88 + 142) return false; areaBody(r, 88); return true;
        case 25: if (n < 117) return false; { r.kind("Dangerous cargo", AIS_APP_SHIP); static const char* const u[4] = {"", "kg", "tonnes", "1000 tonnes"}; r.f("Amount", "%u %s", r.u(90, 10), u[r.u(88, 2)]); int c = 0; for (size_t p = 100; p + 17 <= n; p += 17) { r.f("Cargo", "code %u, subtype %u", r.u(p, 4), r.u(p + 4, 13)); c++; } r.add("%d cargo entries", c); return true; }
        case 28: if (n < 149) return false; routeBody(r, 88, 149); return true;
        case 30: if (n < 98) return false; r.kind("Text description", AIS_APP_TEXT); r.fs("Text", r.t(98, (n - 98) / 6 * 6)); r.f("Linkage ID", "%u", r.u(88, 10)); r.add("%s", r.t(98, (n - 98) / 6 * 6).c_str()); return true;
        default: return false;
        }
    }
    switch (fi) {
    case 0: if (n < 68) return false; r.kind("Text telegram", AIS_APP_TEXT); r.fs("Text", r.t(68, (n - 68) / 6 * 6)); r.add("%s", r.t(68, (n - 68) / 6 * 6).c_str()); return true;
    case 11:
        if (n < 346 || n > 352) return false;
        r.kind("Weather and water", AIS_APP_WEATHER);
        r.ll3(56, 80);
        r.fs("Time", r.u(105, 5) != 31 ? r.ddhhmm(r.u(105, 5), r.u(110, 5), r.u(115, 6)) : "");
        metCommon(r, 121, false);
        return true;
    case 13:
        if (n < 468) return false;
        r.kind("Fairway closed", AIS_APP_NOTICE); r.fs("Reason", r.t(56, 120)); r.fs("From", r.t(176, 120)); r.fs("To", r.t(296, 120));
        r.fs("Start", r.ddhhmm(r.u(428, 5), r.u(437, 5), r.u(442, 6), r.u(433, 4))); r.fs("End", r.ddhhmm(r.u(448, 5), r.u(457, 5), r.u(462, 6), r.u(453, 4)));
        r.add("%s", r.t(56, 120).c_str());
        return true;
    case 15: if (n < 67) return false; r.kind("Air draught", AIS_APP_SHIP); r.f("Air draught", "%.1f m", r.u(56, 11) / 10.0); r.add("%.1f m", r.u(56, 11) / 10.0); return true;
    case 17: {
        r.kind("VTS targets", AIS_APP_TRAFFIC);
        int c = 0;
        AisAreaShape sh; sh.kind = 3;
        for (size_t p = 56; p + 122 <= n; p += 122) {
            const uint32_t idt = r.u(p, 2);
            const int la = r.s(p + 48, 24), lo = r.s(p + 72, 25);
            std::string id;
            char x[32];
            if (idt == 0 || idt == 1) { uint64_t v = (uint64_t)r.u(p + 2, 12) << 30 | r.u(p + 14, 30); snprintf(x, sizeof x, "%s %llu", idt ? "IMO" : "MMSI", (unsigned long long)v); id = x; }
            else id = r.t(p + 2, 42);
            const uint32_t sp = r.u(p + 112, 10), co = r.u(p + 97, 9);
            r.f("Target", "%s, %.4f %.4f, %u kn, %u deg", id.c_str(), la / 60000.0, lo / 60000.0, sp, co);
            if (c == 0 && la != 91000 && lo != 181000 && std::abs(la) <= 90 * 60000) r.pos(la / 60000.0, lo / 60000.0);
            c++;
        }
        r.add("%d targets", c);
        return c > 0; }
    case 19: {
        if (n < 258) return false;
        r.kind("Traffic signal", AIS_APP_TRAFFIC); r.fs("Station", r.t(66, 120));
        static const char* const sg[12] = {"not available", "serious emergency, all stop", "do not proceed", "one way traffic", "two way traffic", "proceed on order only", "do not proceed, except", "proceed on order only, except", "in-bound only", "out-bound only", "both directions", "shift to in-bound"};
        const uint32_t now = r.u(237, 5), nxt = r.u(253, 5);
        const std::string sn = now < 12 ? sg[now] : "other";
        r.fs("Signal", sn); r.fs("Next signal", nxt < 12 ? sg[nxt] : "other");
        r.add("%s: %s", r.t(66, 120).c_str(), sn.c_str());
        if (r.s(186, 25) != 181000 && r.s(211, 24) != 91000) r.pos(r.s(211, 24) / 60000.0, r.s(186, 25) / 60000.0);
        return true; }
    case 21:
        if (n < 301) return false;
        r.kind("Ship weather report", AIS_APP_WEATHER);
        if (r.u(56, 1) == 0) {
            if (n < 357) return false;
            r.fs("Location", r.t(57, 120));
            if (r.s(177, 25) != 181000 && r.s(202, 24) != 91000) r.pos(r.s(202, 24) / 60000.0, r.s(177, 25) / 60000.0);
            r.fs("Time", r.ddhhmm(r.u(226, 5), r.u(231, 5), r.u(236, 6)));
            const uint32_t ws = r.u(261, 7), wd = r.u(268, 9), pr = r.u(277, 9);
            if (ws < 127) { r.f("Wind", "%u kn", ws); r.add("wind %u kn", ws); }
            if (wd < 360) r.f("Wind direction", "%u deg", wd);
            if (pr <= 400) { r.f("Pressure", "%u hPa", pr + 800); r.add("%u hPa", pr + 800); }
            const int t = r.s(290, 11); if (t != -1024) { r.f("Air temperature", "%.1f C", t / 10.0); r.add("air %.1f C", t / 10.0); }
            const uint32_t wh = r.u(317, 8); if (wh < 251) r.f("Wave height", "%.1f m", wh / 10.0);
        } else {
            r.f("Coding", "WMO variant, not interpreted further");
            r.add("WMO coded report");
        }
        return true;
    case 22: if (n < 56 + 142) return false; areaBody(r, 56); return true;
    case 24: {
        if (n < 360) return false;
        r.kind("Extended voyage data", AIS_APP_SHIP);
        r.f("Air draught", "%.2f m", r.u(66, 13) / 100.0); r.fs("Last port", r.t(79, 30)); r.fs("Next port", r.t(109, 30)); r.fs("Second port", r.t(139, 30));
        r.f("AIS class A", "%s", solasText(r.u(169, 2)));
        static const struct { size_t bit; const char* name; } eq[] = {{171,"ATA"},{173,"BNWAS"},{175,"ECDIS backup"},{177,"Paper charts"},{179,"Echo sounder"},{181,"EPA"},{183,"Emergency steering"},{185,"GNSS"},{187,"Gyro compass"},{189,"LRIT"},{191,"Magnetic compass"},{193,"NAVTEX"},{195,"ARPA radar"},{197,"S-band radar"},{199,"X-band radar"},{201,"HF radio"},{203,"Inmarsat"},{205,"MF radio"},{207,"VHF radio"},{209,"Speed log, ground"},{211,"Speed log, water"},{213,"THD"},{215,"Track control"},{217,"VDR"}};
        for (const auto& e : eq) if (r.u(e.bit, 2) != 0) r.f(e.name, "%s", solasText(r.u(e.bit, 2)));
        const uint32_t ps = r.u(337, 13);
        if (ps) r.f("Persons", "%u", ps);
        r.add("%s to %s", r.t(79, 30).c_str(), r.t(109, 30).c_str());
        return true; }
    case 26: if (n < 56 + 112) return false; environmental(r, 56); return true;
    case 27: if (n < 117) return false; routeBody(r, 56, 117); return true;
    case 29: if (n < 66) return false; r.kind("Text description", AIS_APP_TEXT); r.fs("Text", r.t(66, (n - 66) / 6 * 6)); r.f("Linkage ID", "%u", r.u(56, 10)); r.add("%s", r.t(66, (n - 66) / 6 * 6).c_str()); return true;
    case 31:
        if (n < 340 || n > 368) return false;
        r.kind("Weather and water", AIS_APP_WEATHER);
        r.ll3(81, 56);
        r.fs("Time", r.u(106, 5) ? r.ddhhmm(r.u(106, 5), r.u(111, 5), r.u(116, 6)) : "");
        metCommon(r, 122, true);
        return true;
    default: return false;
    }
}

const char* hazardText(uint32_t v) { static const char* const n[6] = {"0 blue cones", "1 blue cone", "2 blue cones", "3 blue cones", "B-flag", ""}; return v < 6 ? n[v] : ""; }

bool dac200(R& r, bool broadcast, int fi) {
    const size_t n = r.b.size();
    if (!broadcast) {
        switch (fi) {
        case 21: if (n < 248) return false; r.kind("ETA at lock or bridge", AIS_APP_TRAFFIC); r.fs("Country", r.t(88, 12)); r.fs("UN locode", r.t(100, 18)); r.fs("Terminal", r.t(148, 30)); r.fs("ETA", r.ddhhmm(r.u(212, 5), r.u(217, 5), r.u(222, 6), r.u(208, 4))); r.add("%s %s", r.t(100, 18).c_str(), r.t(148, 30).c_str()); return true;
        case 22: if (n < 232) return false; { r.kind("RTA at lock or bridge", AIS_APP_TRAFFIC); r.fs("UN locode", r.t(100, 18)); r.fs("Terminal", r.t(148, 30)); r.fs("RTA", r.ddhhmm(r.u(212, 5), r.u(217, 5), r.u(222, 6), r.u(208, 4))); static const char* const s[4] = {"operational", "limited operation", "out of order", "not available"}; r.fs("Status", s[r.u(228, 2)]); r.add("%s, %s", r.t(148, 30).c_str(), s[r.u(228, 2)]); return true; }
        case 55: if (n < 168) return false; r.kind("Persons on board", AIS_APP_SHIP); r.f("Crew", "%u", r.u(88, 8)); r.f("Passengers", "%u", r.u(96, 13)); r.f("Shipboard personnel", "%u", r.u(109, 8)); r.add("%u crew, %u passengers", r.u(88, 8), r.u(96, 13)); return true;
        default: return false;
        }
    }
    switch (fi) {
    case 10: {
        if (n < 168 || n > 424) return false;
        r.kind("Inland ship data", AIS_APP_SHIP);
        r.fs("ENI", r.t(56, 48));
        const uint32_t hz = r.u(141, 3);
        if (hz > 5) return false;
        const uint32_t len = r.u(104, 13), beam = r.u(117, 10), sty = r.u(127, 14), dr = r.u(144, 11), ld = r.u(155, 2);
        if (len) r.f("Length", "%.1f m", len / 10.0);
        if (beam) r.f("Beam", "%.1f m", beam / 10.0);
        const char* tx = ericType(sty);
        r.f("Ship type", "%s (%u)", tx, sty);
        r.fs("Hazard", hazardText(hz));
        if (dr) r.f("Draught", "%.2f m", dr / 100.0);
        static const char* const lo[4] = {"", "unloaded", "loaded", ""};
        r.fs("Load", lo[ld]);
        r.add("ENI %s, %s", r.t(56, 48).c_str(), tx);
        return true; }
    case 23: {
        if (n < 250) return false;
        r.kind("EMMA warning", AIS_APP_NOTICE);
        static const char* const ty[10] = {"", "wind", "rain", "snow and ice", "thunderstorm", "fog", "low temperature", "high temperature", "flood", "forest fire"};
        static const char* const wd[9] = {"", "N", "NE", "E", "SE", "S", "SW", "W", "NW"};
        const uint32_t tv = r.u(222, 4), cl = r.u(244, 2), wv = r.u(246, 4);
        char st[32], en[32];
        snprintf(st, sizeof st, "20%02u-%02u-%02u %02u:%02u", r.u(56, 8) % 100, r.u(64, 4), r.u(68, 5), r.u(90, 5), r.u(95, 6));
        snprintf(en, sizeof en, "20%02u-%02u-%02u %02u:%02u", r.u(73, 8) % 100, r.u(81, 4), r.u(85, 5), r.u(101, 5), r.u(106, 6));
        r.fs("Start", st); r.fs("End", en);
        r.fs("Type", tv < 10 ? ty[tv] : "");
        r.f("Value", "%d to %d", r.s(226, 9), r.s(235, 9));
        static const char* const inten[4] = {"", "slight", "medium", "strong or heavy"};
        r.fs("Intensity", inten[cl]);
        if (wv < 9) r.fs("Wind", wd[wv]);
        r.add("%s, %s", tv < 10 ? ty[tv] : "", inten[cl]);
        AisAreaShape sh; sh.kind = 3;
        const int lo1 = r.s(112, 28), la1 = r.s(140, 27), lo2 = r.s(167, 28), la2 = r.s(195, 27);
        if (std::abs(la1) <= 90 * 600000 && std::abs(lo1) <= 180 * 600000) { r.pos(la1 / 600000.0, lo1 / 600000.0); sh.pts.push_back({(float)(la1 / 600000.0), (float)(lo1 / 600000.0)}); }
        if (std::abs(la2) <= 90 * 600000 && std::abs(lo2) <= 180 * 600000) sh.pts.push_back({(float)(la2 / 600000.0), (float)(lo2 / 600000.0)});
        if (sh.pts.size() == 2) r.o.shapes.push_back(std::move(sh));
        return true; }
    case 24: {
        if (n < 93) return false;
        r.kind("Water levels", AIS_APP_WATER); r.fs("Country", r.t(56, 12));
        int c = 0;
        for (size_t p = 68; p + 25 <= n && c < 4; p += 25) {
            const uint32_t id = r.u(p, 11); const int lv = r.s(p + 11, 14);
            if (!id && !lv) continue;
            r.f("Gauge", "%u: %d cm", id, lv);
            if (c == 0) r.add("gauge %u %d cm", id, lv);
            c++;
        }
        return true; }
    case 40: {
        if (n < 168) return false;
        r.kind("Signal status", AIS_APP_TRAFFIC);
        r.ll4(84, 56);
        static const char* const li[8] = {"unknown", "no light", "white", "yellow", "green", "red", "white flashing", "yellow flashing"};
        std::string lights;          // nine decimal digits, one per light of the signal form, first light in the highest digit
        uint32_t st = r.u(127, 30);
        for (uint32_t div = 100000000; div; div /= 10) {
            const uint32_t d = (st / div) % 10;
            if (d == 0 || d > 7) continue;
            if (!lights.empty()) lights += ", ";
            lights += li[d];
        }
        r.f("Form", "%u", r.u(111, 4));
        static const char* const imp[5] = {"unknown", "upstream", "downstream", "to left bank", "to right bank"};
        const uint32_t dv = r.u(124, 3);
        r.fs("Direction", dv < 5 ? imp[dv] : "");
        r.fs("Lights", lights);
        r.add("%s", lights.empty() ? "no light data" : lights.c_str());
        return true; }
    default: return false;
    }
}

} // namespace

bool interpretApp(const Bits& b, const AisMsg& m, AisAppRecord& o) {
    o = AisAppRecord();
    o.type = m.type; o.mmsi = m.mmsi; o.destMmsi = m.destMmsi;
    o.dac = m.dac; o.fi = m.fi;
    R r{b, o};
    const size_t n = b.size();
    switch (m.type) {
    case 6: case 8: {
        if (m.dac < 0 || m.fi < 0) return false;
        const bool br = m.type == 8;
        bool ok = false;
        if (m.dac == 1) ok = dac1(r, br, m.fi);
        else if (m.dac == 200) ok = dac200(r, br, m.fi);
        else if (m.dac == 367 && m.fi == 33 && br) { environmental(r, 56); ok = true; }
        else if ((m.dac == 235 || m.dac == 250) && m.fi == 10 && !br && n >= 136) {
            r.kind("AtoN monitoring", AIS_APP_TRAFFIC);
            static const char* const rc[4] = {"none", "not monitored", "operational", "error"};
            static const char* const lt[4] = {"none or not monitored", "on", "off", "failed"};
            r.f("Supply", "%.2f V", r.u(88, 10) * 0.05); r.fs("RACON", rc[r.u(118, 2)]); r.fs("Light", lt[r.u(120, 2)]);
            r.f("Alarm", "%s", r.u(122, 1) ? "yes" : "no"); r.f("Off position", "%s", r.u(131, 1) ? "yes" : "no");
            r.add("light %s, %s", lt[r.u(120, 2)], r.u(122, 1) ? "alarm" : "no alarm");
            ok = true;
        }
        if (ok) return true;
        o.fields.clear(); o.summary.clear(); o.shapes.clear(); o.hasPos = false;
        if (m.dac == 1 || m.dac == 200) { if ((m.dac == 1 && (m.fi == 11 || m.fi == 31 || m.fi == 22 || m.fi == 23)) || (m.dac == 200 && m.fi == 10)) return false; }
        char t[48]; snprintf(t, sizeof t, "Binary, DAC %d FI %d", m.dac, m.fi);
        o.title = t; o.category = AIS_APP_OTHER;
        r.add("%zu bits, not interpreted", n);
        return true;
    }
    case 7: case 13: {
        r.kind(m.type == 7 ? "Binary acknowledge" : "Safety acknowledge", AIS_APP_NETWORK);
        int c = 0;
        for (size_t p = 40; p + 30 <= n; p += 32) { const uint32_t d = r.u(p, 30); if (!d) continue; r.f("To", "%09u, sequence %u", d, r.u(p + 30, 2)); if (!c) o.destMmsi = d; c++; }
        r.add("%d stations", c);
        return true;
    }
    case 10: r.kind("UTC inquiry", AIS_APP_NETWORK); r.f("To", "%09u", r.u(40, 30)); r.add("to %09u", r.u(40, 30)); return true;
    case 12: r.kind("Addressed safety text", AIS_APP_TEXT); r.fs("Text", m.text); r.f("To", "%09u", r.u(40, 30)); r.add("%s", m.text.c_str()); return true;
    case 14: r.kind("Safety broadcast", AIS_APP_TEXT); r.fs("Text", m.text); r.add("%s", m.text.c_str()); return true;
    case 15: {
        r.kind("Interrogation", AIS_APP_NETWORK);
        r.f("Station 1", "%09u asked for message %u", r.u(40, 30), r.u(70, 6));
        if (n >= 110 && r.u(90, 6)) r.f("Station 1, second", "message %u", r.u(90, 6));
        if (n >= 158 && r.u(110, 30)) r.f("Station 2", "%09u asked for message %u", r.u(110, 30), r.u(140, 6));
        r.add("%09u: message %u", r.u(40, 30), r.u(70, 6));
        return true;
    }
    case 16: {
        r.kind("Assignment command", AIS_APP_NETWORK);
        r.f("Station 1", "%09u, offset %u, increment %u", r.u(40, 30), r.u(70, 12), r.u(82, 10));
        if (n >= 144 && r.u(92, 30)) r.f("Station 2", "%09u, offset %u, increment %u", r.u(92, 30), r.u(122, 12), r.u(134, 10));
        r.add("%09u", r.u(40, 30));
        return true;
    }
    case 17: {
        r.kind("DGNSS correction", AIS_APP_NETWORK);
        if (m.hasPos) r.pos(m.lat, m.lon);
        r.f("Correction data", "%zu bits", n >= 80 ? n - 80 : 0);
        r.add("%zu bits of corrections", n >= 80 ? n - 80 : 0);
        return true;
    }
    case 20: {
        r.kind("Slot reservation", AIS_APP_NETWORK);
        int c = 0;
        for (int i = 0; i < 4; i++) {
            const size_t p = 40 + 30 * (size_t)i;
            if (p + 30 > n) break;
            const uint32_t num = r.u(p + 12, 4);
            if (!num) continue;
            r.f("Reservation", "offset %u, %u slots, %u min, increment %u", r.u(p, 12), num, r.u(p + 16, 3), r.u(p + 19, 11));
            c++;
        }
        r.add("%d reservations", c);
        return true;
    }
    case 22: {
        r.kind("Channel management", AIS_APP_NETWORK);
        const uint32_t ca = r.u(40, 12), cb = r.u(52, 12);
        r.f("Channel A", "%u", ca); r.f("Channel B", "%u", cb);
        static const char* const tr[4] = {"Tx A+B, Rx A+B", "Tx A, Rx A+B", "Tx B, Rx A+B", "reserved"};
        r.fs("Mode", tr[r.u(64, 4) & 3]);
        r.f("Power", "%s", r.u(68, 1) ? "low" : "high");
        const bool addr = r.u(139, 1) != 0;
        if (addr) { r.f("Station 1", "%09u", r.u(69, 30)); r.f("Station 2", "%09u", r.u(104, 30)); }
        else {
            const int nelo = r.s(69, 18), nela = r.s(87, 17), swlo = r.s(104, 18), swla = r.s(122, 17);
            if (std::abs(nela) <= 54000 && std::abs(swla) <= 54000 && std::abs(nelo) <= 108000 && std::abs(swlo) <= 108000) {
                AisAreaShape sh; sh.kind = 4;
                const float a = (float)(nela / 600.0), b2 = (float)(nelo / 600.0), c = (float)(swla / 600.0), d = (float)(swlo / 600.0);
                sh.pts = {{a, b2}, {a, d}, {c, d}, {c, b2}, {a, b2}};
                o.shapes.push_back(sh);
                r.pos((nela + swla) / 1200.0, (nelo + swlo) / 1200.0);
            }
        }
        r.add("channels %u / %u", ca, cb);
        return true;
    }
    case 23: {
        r.kind("Group assignment", AIS_APP_NETWORK);
        static const char* const st[16] = {"all mobiles", "reserved", "all Class B", "SAR aircraft", "aid to navigation", "Class B shipborne", "regional", "regional", "regional", "regional", "base station area", "reserved", "reserved", "reserved", "reserved", "reserved"};
        static const char* const iv[11] = {"autonomous", "10 min", "6 min", "3 min", "1 min", "30 s", "15 s", "10 s", "5 s", "next shorter", "next longer"};
        r.fs("Station type", st[r.u(110, 4)]);
        r.f("Ship type", "%u", r.u(114, 8));
        { const uint32_t i = r.u(146, 4); r.f("Report interval", "%s", i < 11 ? iv[i] : "reserved"); }
        r.f("Quiet time", "%u min", r.u(150, 4));
        r.add("%s, %s", st[r.u(110, 4)], r.u(146, 4) < 11 ? iv[r.u(146, 4)] : "reserved");
        const int nelo = r.s(40, 18), nela = r.s(58, 17), swlo = r.s(75, 18), swla = r.s(93, 17);
        if ((nela || swla || nelo || swlo) && std::abs(nela) <= 54000 && std::abs(swla) <= 54000 && std::abs(nelo) <= 108000 && std::abs(swlo) <= 108000) {
            AisAreaShape sh; sh.kind = 4;
            const float a = (float)(nela / 600.0), b2 = (float)(nelo / 600.0), c = (float)(swla / 600.0), d = (float)(swlo / 600.0);
            sh.pts = {{a, b2}, {a, d}, {c, d}, {c, b2}, {a, b2}};
            o.shapes.push_back(sh);
            r.pos((nela + swla) / 1200.0, (nelo + swlo) / 1200.0);
        }
        return true;
    }
    case 25: case 26: {
        const bool addr = r.u(38, 1) != 0, str = r.u(39, 1) != 0;
        size_t hdr = 40 + (addr ? 32 : 0) + (str ? 16 : 0);
        if (m.type == 26 && n < hdr + 20) return false;
        if (n < hdr) return false;
        r.kind(m.type == 25 ? "Single slot binary" : "Multi slot binary", AIS_APP_OTHER);
        if (addr) { o.destMmsi = r.u(40, 30); r.f("To", "%09u", o.destMmsi); }
        if (str) { o.dac = (int)r.u(40 + (addr ? 32 : 0), 10); o.fi = (int)r.u(50 + (addr ? 32 : 0), 6); r.f("DAC", "%d", o.dac); r.f("FI", "%d", o.fi); r.add("DAC %d FI %d", o.dac, o.fi); }
        r.f("Data", "%zu bits", n - hdr - (m.type == 26 ? 20 : 0));
        if (!str) r.add("%zu bits", n - hdr - (m.type == 26 ? 20 : 0));
        return true;
    }
    default: return false;
    }
}

} // namespace ais
} // namespace dect2
