// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Airport table (see airports.h): the built-in text, search, near, and the channels of an airport for the airband mode.
#include "dect2/airports/airports.h"
#include "dect2/packs/datasets.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
#include "airports_data.inc"

std::vector<std::string> splitTab(const std::string& s, size_t from, size_t to) {
    std::vector<std::string> v;
    size_t p = from;
    while (true) {
        const size_t e = s.find('\t', p);
        if (e == std::string::npos || e >= to) { v.push_back(s.substr(p, to - p)); break; }
        v.push_back(s.substr(p, e - p));
        p = e + 1;
    }
    return v;
}

std::string lower(const std::string& s) {
    std::string o = s;
    for (auto& c : o) c = (char)std::tolower((unsigned char)c);
    return o;
}

std::string clean(const std::string& s) {
    std::string o;
    bool sp = true;
    for (unsigned char c : s) {
        if (c == '\t' || c == '\r' || c == '\n' || c == ' ') { if (!sp) o += ' '; sp = true; }
        else { o += (char)c; sp = false; }
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

// One CSV record per call of row(); quotes, doubled quotes, commas and line breaks inside quotes, CRLF. Returns the number of records.
template <class F> size_t parseCsv(const std::string& t, F row) {
    std::vector<std::string> r;
    std::string f;
    bool q = false, any = false;
    size_t n = 0;
    auto endRow = [&] {
        if (any || !f.empty() || !r.empty()) { r.push_back(f); row(r); n++; }
        r.clear(); f.clear(); any = false;
    };
    for (size_t i = 0; i < t.size(); i++) {
        const char c = t[i];
        if (q) {
            if (c == '"') { if (i + 1 < t.size() && t[i + 1] == '"') { f += '"'; i++; } else q = false; }
            else f += c;
        } else if (c == '"') { q = true; any = true; }
        else if (c == ',') { r.push_back(f); f.clear(); any = true; }
        else if (c == '\n') endRow();
        else if (c != '\r') f += c;
    }
    endRow();
    return n;
}

int colOf(const std::vector<std::string>& h, const char* name) {
    for (size_t i = 0; i < h.size(); i++) if (h[i] == name) return (int)i;
    return -1;
}

double toD(const std::string& s, bool& ok) {
    char* e = nullptr;
    const double v = std::strtod(s.c_str(), &e);
    ok = e && e != s.c_str() && std::isfinite(v);
    return v;
}
}   // namespace

int airportKhz(double mhz) {
    if (!(mhz > 0) || !std::isfinite(mhz)) return 0;
    const long k = std::lround(mhz * 1000);
    if (k >= 118000 && k <= 137000) {
        const long r = k % 25;
        if (r == 0 || r == 5 || r == 10 || r == 15) return (int)k;
        const long base = k - r;
        if (r <= 5) return (int)base;
        if (r >= 20) return (int)(base + 25);
        return 0;
    }
    if (k < 100 || k > 3000000) return 0;   // under 100 kHz or over 3 GHz: a typing error
    return (int)k;
}

std::string airportTypeName(const std::string& t) {
    static const std::map<std::string, const char*> m = {
        {"TWR", "Tower"}, {"GND", "Ground"}, {"ATIS", "ATIS"}, {"APP", "Approach"}, {"DEP", "Departure"}, {"CTAF", "CTAF"}, {"UNICOM", "Unicom"},
        {"CLD", "Clearance"}, {"AFIS", "AFIS"}, {"FSS", "Flight service"}, {"AWOS", "AWOS"}, {"ASOS", "ASOS"}, {"OPS", "Operations"},
        {"ARR", "Arrival"}, {"DIR", "Director"}, {"A/D", "Arrival/Departure"}, {"CNTR", "Centre"}, {"ACC", "Area control"}, {"INFO", "Information"},
        {"RDO", "Radio"}, {"RADIO", "Radio"}, {"A/G", "Air/ground"}, {"RMP", "Ramp"}, {"RDR", "Radar"}, {"ATF", "ATF"}, {"GCA", "GCA"}, {"PMSV", "Met"},
        {"RCO", "Remote radio"}, {"TCA", "TMA"}, {"FIS", "Flight info"}, {"MF", "MF"}, {"TML", "Terminal"}, {"MISC", ""}};
    const auto it = m.find(t);
    return it == m.end() ? t : it->second;
}

double airportDistanceKm(double la1, double lo1, double la2, double lo2) {
    const double r = dect2::math::kPi / 180;
    const double dla = (la2 - la1) * r, dlo = (lo2 - lo1) * r;
    const double a = std::sin(dla / 2) * std::sin(dla / 2) + std::cos(la1 * r) * std::cos(la2 * r) * std::sin(dlo / 2) * std::sin(dlo / 2);
    return 6371.0 * 2 * std::atan2(std::sqrt(a), std::sqrt(std::max(0.0, 1 - a)));
}

// ---------------------------------------------------------------- table text

AirportReport AirportDb::parseTsv(const std::string& text) {
    AirportReport rep;
    v_.clear();
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        size_t le = e;
        if (le > p && text[le - 1] == '\r') le--;
        const size_t ls = p;
        p = e + 1;
        if (le == ls || text[ls] == '#') continue;
        const auto f = splitTab(text, ls, le);
        if (f[0] == "A") {
            bool o1, o2, o3;
            if (f.size() < 11) { rep.skipped++; continue; }
            Airport a;
            a.icao = f[1]; a.iata = f[2]; a.name = f[3]; a.city = f[4]; a.country = f[5];
            a.latDeg = toD(f[6], o1); a.lonDeg = toD(f[7], o2);
            a.elevFt = (int)toD(f[8], o3);
            a.kind = f[9].empty() ? 'S' : f[9][0];
            a.scheduled = f[10] == "1";
            if (a.icao.empty() || !o1 || !o2 || std::fabs(a.latDeg) > 90 || std::fabs(a.lonDeg) > 180) { rep.skipped++; v_.push_back(Airport()); v_.back().icao = "\x01"; continue; }
            v_.push_back(std::move(a));
            rep.airports++;
        } else if (f[0] == "F") {
            if (f.size() < 3 || v_.empty() || v_.back().icao == "\x01") { rep.skipped++; continue; }
            const long k = atol(f[2].c_str());
            if (k <= 0 || k > 3000000) { rep.skipped++; continue; }
            AirportFreq q;
            q.type = f[1]; q.khz = (int)k;
            if (f.size() > 3) q.desc = f[3];
            v_.back().freqs.push_back(std::move(q));
            rep.freqs++;
        } else rep.skipped++;
    }
    v_.erase(std::remove_if(v_.begin(), v_.end(), [](const Airport& a) { return a.icao == "\x01"; }), v_.end());
    return rep;
}

std::string AirportDb::toTsv() const {
    std::string o = "# Airports and their radio frequencies (see data/airports/airports.tsv for the format)\n";
    char b[96];
    for (const auto& a : v_) {
        o += "A\t" + a.icao + "\t" + a.iata + "\t" + a.name + "\t" + a.city + "\t" + a.country;
        snprintf(b, sizeof b, "\t%.4f\t%.4f\t%d\t%c\t%d\n", a.latDeg, a.lonDeg, a.elevFt, a.kind, a.scheduled ? 1 : 0);
        o += b;
        for (const auto& f : a.freqs) { snprintf(b, sizeof b, "%d", f.khz); o += "F\t" + f.type + "\t" + b + "\t" + f.desc + "\n"; }
    }
    return o;
}

const char* builtinAirportText() { return kAirportData; }

const AirportDb& AirportDb::builtin() {
    static const AirportDb db = [] {
        AirportDb d;
        const std::string over = dect2::datasets::userText("airports");   // the user's airports win over the bundled ones with the same code
        if (over.empty()) d.parseTsv(kAirportData);
        else d.parseTsv(dect2::datasets::layerGrouped(kAirportData, over, "A", 1));
        return d;
    }();
    return db;
}

// ---------------------------------------------------------------- OurAirports CSV

AirportReport AirportDb::importOurAirports(const std::string& airportsCsv, const std::string& frequenciesCsv) {
    AirportReport rep;
    v_.clear();
    // frequencies by airport id
    std::map<std::string, std::map<std::pair<int, std::string>, std::string>> byId;
    {
        std::vector<std::string> head;
        int cRef = -1, cType = -1, cDesc = -1, cMhz = -1;
        bool first = true;
        parseCsv(frequenciesCsv, [&](const std::vector<std::string>& r) {
            if (first) {
                first = false; head = r;
                cRef = colOf(head, "airport_ref"); cType = colOf(head, "type"); cDesc = colOf(head, "description"); cMhz = colOf(head, "frequency_mhz");
                return;
            }
            if (cRef < 0 || cMhz < 0 || (int)r.size() <= std::max(cRef, cMhz)) { rep.skipped++; return; }
            bool ok;
            const double mhz = toD(r[(size_t)cMhz], ok);
            const int k = ok ? airportKhz(mhz) : 0;
            if (!k) { rep.skipped++; return; }
            std::string t = cType >= 0 && (int)r.size() > cType ? clean(r[(size_t)cType]) : "";
            for (auto& c : t) c = (char)std::toupper((unsigned char)c);
            if (t == "UNIC") t = "UNICOM";
            if (t.empty()) t = "MISC";
            std::string d = cDesc >= 0 && (int)r.size() > cDesc ? clean(r[(size_t)cDesc]) : "";
            std::string du = d;
            for (auto& c : du) c = (char)std::toupper((unsigned char)c);
            if (du == t) d.clear();
            if (d.size() > 40) d.resize(40);
            byId[r[(size_t)cRef]][{k, t}] = d;
        });
    }
    std::vector<std::string> head;
    int cId = -1, cIdent = -1, cType = -1, cName = -1, cLat = -1, cLon = -1, cElev = -1, cCountry = -1, cCity = -1, cSched = -1, cIcao = -1, cIata = -1;
    bool first = true;
    parseCsv(airportsCsv, [&](const std::vector<std::string>& r) {
        if (first) {
            first = false; head = r;
            cId = colOf(head, "id"); cIdent = colOf(head, "ident"); cType = colOf(head, "type"); cName = colOf(head, "name");
            cLat = colOf(head, "latitude_deg"); cLon = colOf(head, "longitude_deg"); cElev = colOf(head, "elevation_ft");
            cCountry = colOf(head, "iso_country"); cCity = colOf(head, "municipality"); cSched = colOf(head, "scheduled_service");
            cIcao = colOf(head, "icao_code"); cIata = colOf(head, "iata_code");
            return;
        }
        if (cId < 0 || cType < 0 || cLat < 0 || cLon < 0 || cName < 0 || r.size() < head.size() / 2) { rep.skipped++; return; }
        auto at = [&](int c) -> std::string { return c >= 0 && (size_t)c < r.size() ? r[(size_t)c] : std::string(); };
        const std::string ty = at(cType);
        char kind = 0;
        if (ty == "large_airport") kind = 'L'; else if (ty == "medium_airport") kind = 'M'; else if (ty == "small_airport") kind = 'S'; else if (ty == "seaplane_base") kind = 'W';
        if (!kind) return;   // closed, heliports, balloon ports: left out on purpose, not counted as damage
        bool o1, o2;
        Airport a;
        a.latDeg = toD(at(cLat), o1); a.lonDeg = toD(at(cLon), o2);
        if (!o1 || !o2 || std::fabs(a.latDeg) > 90 || std::fabs(a.lonDeg) > 180) { rep.skipped++; return; }
        a.kind = kind;
        a.scheduled = at(cSched) == "yes";
        const auto it = byId.find(at(cId));
        if ((kind == 'S' || kind == 'W') && it == byId.end() && !a.scheduled) return;
        a.icao = clean(at(cIcao).empty() ? at(cIdent) : at(cIcao));
        if (a.icao.empty()) { rep.skipped++; return; }
        a.iata = clean(at(cIata)); a.name = clean(at(cName)); a.city = clean(at(cCity)); a.country = at(cCountry);
        a.elevFt = (int)atol(at(cElev).c_str());
        if (it != byId.end()) for (const auto& kv : it->second) { AirportFreq f; f.khz = kv.first.first; f.type = kv.first.second; f.desc = kv.second; a.freqs.push_back(f); rep.freqs++; }
        std::sort(a.freqs.begin(), a.freqs.end(), [](const AirportFreq& x, const AirportFreq& y) { return x.type != y.type ? x.type < y.type : x.khz < y.khz; });
        v_.push_back(std::move(a));
        rep.airports++;
    });
    std::sort(v_.begin(), v_.end(), [](const Airport& x, const Airport& y) { return x.icao < y.icao; });
    if (rep.airports == 0) rep.log.push_back("no airport found: not the OurAirports airports.csv?");
    return rep;
}

// ---------------------------------------------------------------- search

const Airport* AirportDb::find(const std::string& code) const {
    const std::string c = lower(code);
    if (c.empty()) return nullptr;
    for (const auto& a : v_) if (lower(a.icao) == c) return &a;
    for (const auto& a : v_) if (!a.iata.empty() && lower(a.iata) == c) return &a;
    return nullptr;
}

namespace {
// 0 exact code, 1 code prefix, 2 word prefix in name or city, 3 anywhere in name or city or the country code; 9 no match
int tokenScore(const Airport& a, const std::string& icao, const std::string& iata, const std::string& name, const std::string& city, const std::string& t) {
    if (icao == t || (!iata.empty() && iata == t)) return 0;
    if (icao.compare(0, t.size(), t) == 0 || (!iata.empty() && iata.compare(0, t.size(), t) == 0)) return 1;
    auto wordPrefix = [&](const std::string& s) {
        for (size_t i = 0; i < s.size(); i++)
            if ((i == 0 || !std::isalnum((unsigned char)s[i - 1])) && s.compare(i, t.size(), t) == 0) return true;
        return false;
    };
    if (wordPrefix(name) || wordPrefix(city)) return 2;
    if (name.find(t) != std::string::npos || city.find(t) != std::string::npos) return 3;
    if (lower(a.country) == t) return 3;
    return 9;
}
int kindRank(char k) { return k == 'L' ? 0 : k == 'M' ? 1 : k == 'S' ? 2 : 3; }
}   // namespace

std::vector<AirportHit> AirportDb::search(const std::string& text, size_t maxHits, const double* from, bool withFreqOnly) const {
    std::vector<std::string> tok;
    {
        std::string t;
        for (unsigned char c : text + " ") {
            if (c == ' ' || c == ',' || c == '\t') { if (!t.empty()) tok.push_back(lower(t)); t.clear(); }
            else t += (char)c;
        }
    }
    std::vector<AirportHit> out;
    if (tok.empty()) return out;
    struct Sc { size_t i; int score; double km; };
    std::vector<Sc> sc;
    for (size_t i = 0; i < v_.size(); i++) {
        const Airport& a = v_[i];
        if (withFreqOnly && !a.airbandCount()) continue;
        const std::string icao = lower(a.icao), iata = lower(a.iata), name = lower(a.name), city = lower(a.city);
        int worst = 0;
        for (const auto& t : tok) { worst = std::max(worst, tokenScore(a, icao, iata, name, city, t)); if (worst == 9) break; }
        if (worst == 9) continue;
        sc.push_back({i, worst, from ? airportDistanceKm(from[0], from[1], a.latDeg, a.lonDeg) : -1});
    }
    std::sort(sc.begin(), sc.end(), [&](const Sc& x, const Sc& y) {
        if (x.score != y.score) return x.score < y.score;
        const Airport& a = v_[x.i]; const Airport& b = v_[y.i];
        if (a.scheduled != b.scheduled) return a.scheduled;
        if (from) { if (std::fabs(x.km - y.km) > 0.5) return x.km < y.km; }
        else if (kindRank(a.kind) != kindRank(b.kind)) return kindRank(a.kind) < kindRank(b.kind);
        return a.icao < b.icao;
    });
    for (size_t k = 0; k < sc.size() && out.size() < maxHits; k++) out.push_back({sc[k].i, sc[k].km});
    return out;
}

std::vector<AirportHit> AirportDb::nearby(double lat, double lon, double radiusKm, size_t maxHits, bool airbandOnly) const {
    std::vector<AirportHit> out;
    const double dLat = radiusKm / 111.0 + 0.01;
    for (size_t i = 0; i < v_.size(); i++) {
        const Airport& a = v_[i];
        if (std::fabs(a.latDeg - lat) > dLat) continue;
        if (airbandOnly && !a.airbandCount()) continue;
        const double d = airportDistanceKm(lat, lon, a.latDeg, a.lonDeg);
        if (d <= radiusKm) out.push_back({i, d});
    }
    std::sort(out.begin(), out.end(), [](const AirportHit& x, const AirportHit& y) { return x.km < y.km; });
    if (out.size() > maxHits) out.resize(maxHits);
    return out;
}

// ---------------------------------------------------------------- channels for the airband mode

std::vector<AirbandChannel> airportAirbandChannels(const Airport& a, int* skipped) {
    if (skipped) *skipped = 0;
    struct One { double hz; bool n833; int khz; std::vector<std::string> names; };
    std::vector<One> v;
    for (const auto& f : a.freqs) {
        if (!f.airband()) { if (skipped) (*skipped)++; continue; }
        char b[24];
        snprintf(b, sizeof b, "%d.%03d", f.khz / 1000, f.khz % 1000);
        double hz; bool n833;
        if (!airbandParse(b, hz, n833)) { if (skipped) (*skipped)++; continue; }
        std::string nm = airportTypeName(f.type);
        if (nm.empty()) nm = f.desc.empty() ? "Misc" : f.desc;
        One* hit = nullptr;
        for (auto& o : v) if (o.khz == f.khz) hit = &o;
        if (!hit) { v.push_back({hz, n833, f.khz, {}}); hit = &v.back(); }
        if (std::find(hit->names.begin(), hit->names.end(), nm) == hit->names.end()) hit->names.push_back(nm);
    }
    std::sort(v.begin(), v.end(), [](const One& x, const One& y) { return x.khz < y.khz; });
    std::map<std::string, int> seen, total;
    std::vector<std::string> base;
    for (const auto& o : v) {
        std::string n;
        for (size_t i = 0; i < o.names.size(); i++) n += (i ? "/" : "") + o.names[i];
        base.push_back(a.icao + " " + n);
        total[base.back()]++;
    }
    std::vector<AirbandChannel> out;
    for (size_t i = 0; i < v.size(); i++) {
        AirbandChannel c;
        c.freqHz = v[i].hz; c.is833 = v[i].n833;
        c.label = base[i];
        if (total[base[i]] > 1) c.label += " " + std::to_string(++seen[base[i]]);
        out.push_back(std::move(c));
    }
    return out;
}

} // namespace dect2
