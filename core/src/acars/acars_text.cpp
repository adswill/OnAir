// See acars_text.h.
#include "dect2/acars/acars_text.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace dect2 {
namespace {

using Str = std::string;

std::vector<Str> split(const Str& s, char c) {
    std::vector<Str> v;
    size_t p = 0;
    for (;;) {
        const size_t q = s.find(c, p);
        if (q == Str::npos) { v.push_back(s.substr(p)); return v; }
        v.push_back(s.substr(p, q - p));
        p = q + 1;
    }
}
Str trim(const Str& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}
Str stripSpaces(Str s) { Str o; for (char c : s) if (c != ' ') o += c; return o; }
bool digits(const Str& s) { if (s.empty()) return false; for (char c : s) if (c < '0' || c > '9') return false; return true; }
bool numberText(const Str& s0) {
    const Str s = trim(s0);
    if (s.empty()) return false;
    char* e = nullptr;
    strtod(s.c_str(), &e);
    return e && *e == 0;
}
double num(const Str& s) { return strtod(trim(s).c_str(), nullptr); }
bool upper(const Str& s) { if (s.empty()) return false; for (char c : s) if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false; return true; }
bool airport(const Str& s) { return s.size() >= 3 && s.size() <= 4 && upper(s); }
Str sub(const Str& s, size_t a, size_t n = Str::npos) { return a < s.size() ? s.substr(a, n) : Str(); }
bool starts(const Str& s, const char* p) { return s.rfind(p, 0) == 0; }
double dirOf(char c) { return c == 'N' || c == 'E' ? 1 : c == 'S' || c == 'W' ? -1 : NAN; }

Str hhmm(const Str& t) {
    const Str s = trim(t);
    if (!digits(s) || (s.size() != 4 && s.size() != 6)) return s;
    return s.substr(0, 2) + ":" + s.substr(2, 2) + (s.size() == 6 ? ":" + s.substr(4, 2) : "");
}

struct W {
    AcarsReading& o;
    void kind(const char* k, int cat) { o.kind = k; o.category = cat; }
    void f(const char* k, const Str& v) { if (!v.empty()) o.fields.push_back({k, v}); }
    bool pos(double la, double lo) {
        if (!std::isfinite(la) || !std::isfinite(lo) || std::fabs(la) > 90 || std::fabs(lo) > 180 || (la == 0 && lo == 0)) return false;
        o.hasPos = true; o.lat = la; o.lon = lo;
        return true;
    }
    void alt(double ft) { if (std::isfinite(ft) && ft > -2000 && ft < 70000) { o.altFt = (int)std::lround(ft); char b[24]; snprintf(b, sizeof b, "%d ft", o.altFt); f("Altitude", b); } }
    void dep(const Str& s) { if (airport(trim(s))) { o.from = trim(s); f("From", o.from); } }
    void arr(const Str& s) { if (airport(trim(s))) { o.to = trim(s); f("To", o.to); } }
    void alternate(const Str& s) { if (airport(trim(s))) f("Alternate", trim(s)); }
    void flight(const Str& s) { const Str t = trim(s); if (!t.empty()) { o.flight = t; f("Flight", t); } }
    void time(const char* k, const Str& t) { f(k, hhmm(t)); }
    void fuel(const Str& s, const char* unit = "t") { if (numberText(s)) { f("Fuel", trim(s) + " " + unit); } }
    void heading(double h) { if (std::isfinite(h) && h >= 0 && h <= 360) { o.headingDeg = (int)h; char b[16]; snprintf(b, sizeof b, "%d deg", (int)h); f("Heading", b); } }
    void text(const Str& s) { f("Text", trim(s)); }
    void route(const Str& a, const Str& b, const Str& c = Str()) { Str r = a; if (!b.empty()) r += " > " + b; if (!c.empty()) r += " > " + c; f("Route", r); }
};

// N12345W123456 or "N12345 W123456", thousandths of a degree, or DDMMm minutes with tenths
bool coords(const Str& s, double& la, double& lo, bool minutes) {
    if (s.size() < 13) return false;
    char a = s[0], b = s[6]; size_t lonAt = 7;
    if (b == ' ') { b = s[7]; lonAt = 8; }
    if ((a != 'N' && a != 'S') || (b != 'E' && b != 'W')) return false;
    if (lonAt + 6 > s.size()) return false;
    Str las = s.substr(1, 5), los = s.substr(lonAt, 6);
    for (auto& c : los) if (c == ' ') c = '0';
    if (!digits(las) || !digits(los)) return false;
    const double l1 = atof(las.c_str()), l2 = atof(los.c_str());
    if (!minutes) { la = l1 / 1000 * dirOf(a); lo = l2 / 1000 * dirOf(b); return true; }
    la = (std::floor(l1 / 1000) + std::fmod(l1, 1000) / 10 / 60) * dirOf(a);
    lo = (std::floor(l2 / 1000) + std::fmod(l2, 1000) / 10 / 60) * dirOf(b);
    return true;
}

bool signedDecimal(W& w, const Str& lat, const Str& lon) {
    const Str a = stripSpaces(lat), b = stripSpaces(lon);
    if (a.empty() || b.empty()) return false;
    if ((a[0] == 'N' || a[0] == 'S') && (b[0] == 'E' || b[0] == 'W')) {
        if (!numberText(a.substr(1)) || !numberText(b.substr(1))) return false;
        return w.pos(dirOf(a[0]) * num(a.substr(1)), dirOf(b[0]) * num(b.substr(1)));
    }
    if (!numberText(a) || !numberText(b)) return false;
    return w.pos(num(a), num(b));
}

std::vector<Str> words(const Str& s) {
    std::vector<Str> v;
    Str cur;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { if (!cur.empty()) { v.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) v.push_back(cur);
    return v;
}

// ARINC 702 style reports: a type, then slash-separated fields each opened by a two-letter identifier
bool arinc702(const Str& t0, W& w) {
    Str t = t0;
    const size_t h = t.find('#');
    if (h != Str::npos && h + 4 <= t.size()) {
        if (h >= 8) w.flight(t.substr(4, h - 4));
        t = t.substr(h + (t.compare(h, 4, "#M1B") == 0 || t.compare(h, 4, "#T1B") == 0 || t.compare(h, 4, "#CFB") == 0 ? 4 : 3));
    }
    static const char* const types[] = {"POS", "FPN", "INI", "FTX", "PRG", "PRF", "REQ", "LDI", "SUM", "PER"};
    size_t at = Str::npos; const char* ty = nullptr;
    for (const char* ty2 : types) {
        const size_t q = t.find(ty2);
        if (q == Str::npos || q > 14) continue;
        const char after = q + 3 < t.size() ? t[q + 3] : 0;
        if (after != '/' && after != 'N' && after != 'S') continue;
        if (at == Str::npos || q < at) { at = q; ty = ty2; }
    }
    if (at == Str::npos) { w.o.flight.clear(); w.o.fields.clear(); return false; }
    const auto fields = split(t.substr(at), '/');
    Str kind = !strcmp(ty, "POS") ? "Position report" : !strcmp(ty, "FPN") ? "Flight plan" : !strcmp(ty, "INI") ? "Flight initialisation" : !strcmp(ty, "FTX") ? "Free text" :
               !strcmp(ty, "PRG") || !strcmp(ty, "PRF") ? "Performance data" : !strcmp(ty, "REQ") ? "Request" : !strcmp(ty, "LDI") ? "Landing data" : !strcmp(ty, "SUM") ? "Flight summary" : "Report";
    int cat = !strcmp(ty, "POS") ? ACARS_READ_POSITION : !strcmp(ty, "FPN") ? ACARS_READ_ROUTE : !strcmp(ty, "FTX") ? ACARS_READ_TEXT : ACARS_READ_OTHER;
    w.kind(kind.c_str(), cat);
    auto position = [&](const Str& c) {     // N43312W123174,EASON,215754,370 (waypoint optional)
        const auto q = split(c, ',');
        double la, lo;
        if (q[0].size() < 13 || !coords(q[0], la, lo, true) || !w.pos(la, lo)) return false;
        size_t i = 1;
        if (i < q.size() && !(q[i].size() == 6 && digits(q[i]))) { if (!trim(q[i]).empty()) w.f("Waypoint", trim(q[i])); i++; }
        if (i < q.size()) { w.time("Time", q[i]); i++; }
        if (i < q.size() && digits(q[i])) w.alt(atof(q[i].c_str()) * 100);
        return true;
    };
    if (!strcmp(ty, "POS") && fields[0].size() > 3) { if (!position(fields[0].substr(3))) return false; }
    for (size_t i = 1; i < fields.size(); i++) {
        const Str f = fields[i];
        if (f.size() < 2) continue;
        const Str iei = f.substr(0, 2), d = f.substr(2);
        if (iei == "ID") { const auto q = split(d, ','); if (q.size() > 1) w.flight(q[1]); if (!q.empty()) w.f("Tail", q[0]); }
        else if (iei == "FN") w.flight(d);
        else if (iei == "PS") { if (!position(d)) return false; }
        else if (iei == "AF") { const auto q = split(d, ','); if (q.size() > 1) { w.dep(q[0]); w.arr(q[1]); } }
        else if (iei == "DT") { for (const auto& a : split(d, ',')) if (airport(a) && a.size() == 4) { w.arr(a); break; } }
        else if (iei == "FX") w.text(d);
        else if (iei == "ET") w.time("ETA", sub(d, d.size() > 4 ? d.size() - 4 : 0));
        else if (iei == "RP" || iei == "RI" || iei == "RM" || iei == "RF" || iei == "RS") {
            const auto r = split(f, ':');
            for (size_t k = 1; k + 1 < r.size(); k++) { if (r[k] == "DA") w.dep(r[k + 1]); else if (r[k] == "AA") w.arr(r[k + 1]); }
        }
    }
    return true;
}

bool label10(const Str& t, W& w) {
    if (starts(t, "POS")) {
        const auto p = split(t, ',');
        if (p.size() != 12) return false;
        const Str a = trim(p[1]), b = trim(p[2]);
        if (a.size() < 2 || b.size() < 2 || !numberText(a.substr(1)) || !numberText(b.substr(1))) return false;
        w.kind("Position report", ACARS_READ_POSITION);
        if (!w.pos(dirOf(a[0]) * num(a.substr(1)) / 100, dirOf(b[0]) * num(b.substr(1)) / 100)) return false;
        w.alt(num(p[7]));
        return true;
    }
    if (starts(t, "/")) {
        const auto p = split(t, '/');
        if (p.size() < 17) return false;
        if (p[1].size() < 2 || p[2].size() < 2 || !numberText(p[1].substr(1)) || !numberText(p[2].substr(1))) return false;
        w.kind("Position report", ACARS_READ_POSITION);
        if (!w.pos(dirOf(p[1][0]) * num(p[1].substr(1)), dirOf(p[2][0]) * num(p[2].substr(1)))) return false;
        w.heading(num(p[5])); w.alt(100 * num(p[6])); w.arr(p[7]); w.time("ETA", p[8]);
        w.route(trim(p[11]) + " " + hhmm(p[13]), trim(p[12]) + " " + hhmm(p[15]), trim(p[14]));
        if (!trim(p[16]).empty()) w.dep(p[16]);
        return true;
    }
    if (starts(t, "LRW01,")) {
        const auto p = split(t, ',');
        if (p.size() < 8 || trim(p[7]).empty()) return false;
        w.kind("Landing runway request", ACARS_READ_ETA);
        w.dep(p[6]); w.arr(p[7]);
        return true;
    }
    return false;
}

bool label11(const Str& t, W& w) {
    if (!starts(t, "LRW01,")) return false;
    const size_t c = t.find(" LANDING RUNWAYS,");
    if (c == Str::npos || c != 10) return false;
    w.kind("Landing runways", ACARS_READ_ETA);
    w.f("Airport", t.substr(6, 4));
    w.text(sub(t, c + 17));
    return true;
}

bool nSpace(const Str& t, W& w, bool twelve) {
    if (!(starts(t, "N ") || starts(t, "S "))) return false;
    if (!twelve && t.find('/') != Str::npos && t.find(',') == Str::npos) {
        const auto p = split(t, '/');
        if (p.size() != 2) return false;
        w.kind("Position report", ACARS_READ_POSITION);
        return signedDecimal(w, p[0], p[1]);
    }
    const auto p = split(t, ',');
    if (p.size() < 5) return false;
    w.kind("Position report", ACARS_READ_POSITION);
    if (!signedDecimal(w, p[0], p[1])) return false;
    if (numberText(p[2])) w.alt(num(p[2]));
    else if (trim(p[2]) == "GRD" || trim(p[2]) == "***") w.f("Altitude", "ground");
    return true;
}

bool label12Pos(const Str& t, W& w) {
    if (!starts(t, "POS")) return false;
    const auto d = split(t.substr(3), ',');
    if (d.size() != 12 || d[0].size() < 16) return false;
    double la = 0, lo = 0;
    const Str a = d[0].substr(0, 8), b = d[0].substr(8);
    if (!digits(a.substr(1, 2)) && !digits(a.substr(1, 3))) return false;
    la = dirOf(a[0]) * (atof(a.substr(1, 3).c_str()) + atof(a.substr(4, 2).c_str()) / 60 + atof(a.substr(6, 2).c_str()) / 3600);
    lo = dirOf(b[0]) * (atof(b.substr(1, 3).c_str()) + atof(b.substr(4, 2).c_str()) / 60 + atof(b.substr(6, 2).c_str()) / 3600);
    w.kind("Position report", ACARS_READ_POSITION);
    if (!w.pos(la, lo)) return false;
    w.time("Time", d[2]); w.alt(10 * num(d[3]));
    w.fuel(sub(d[7], 3)); w.time("ETA", sub(d[8], 3)); w.dep(d[9]); w.arr(d[10]);
    return true;
}

bool label13to18(const Str& label, const Str& t, W& w) {
    if (!starts(t, "/")) return false;
    const auto lines = split(t, '\n');
    const auto p = split(trim(lines[0]), '/');
    const int n = atoi(label.c_str());
    if (p.size() < 3 || !digits(sub(p[1], 0, 2))) return false;
    if ((n != 18 && p.size() != 4) || (n == 18 && p.size() != 7)) return false;
    const auto d = split(p[2], ' ');
    if (d.size() < 5) return false;
    static const char* const names[] = {"OOOI: gate out", "OOOI: wheels off", "OOOI: wheels on", "OOOI: gate in"};
    if (n >= 13 && n <= 16) { w.kind(names[n - 13], ACARS_READ_OOOI); w.time(n == 13 ? "Out" : n == 14 ? "Off" : n == 15 ? "On" : "In", d[4]); }
    else { w.kind("Flight status", ACARS_READ_OOOI); w.time("Time", d[4]); }
    w.dep(d[1]); w.arr(d[2]);
    w.f("Day", trim(d[3]));
    for (size_t i = 1; i < lines.size(); i++) {
        const Str l = trim(lines[i]);
        if (!starts(l, "/LOC")) continue;
        const auto c = split(sub(l, 5), ',');
        if (c.size() != 2) continue;
        if (!c[0].empty() && (c[0][0] == '+' || c[0][0] == '-')) w.pos(num(c[0]), num(c[1]));
        else if (c[0].size() >= 7 && c[1].size() >= 8)
            w.pos(dirOf(c[0][0]) * (atof(c[0].substr(1, 2).c_str()) + atof(c[0].substr(3, 2).c_str()) / 60 + atof(c[0].substr(5, 2).c_str()) / 3600),
                  dirOf(c[1][0]) * (atof(c[1].substr(1, 3).c_str()) + atof(c[1].substr(4, 2).c_str()) / 60 + atof(c[1].substr(6, 2).c_str()) / 3600));
    }
    return true;
}

bool paren2(const Str& t, W& w, bool sixteen) {
    if (!starts(t, "(2")) return false;
    if (sixteen ? t.size() < 29 : (t.size() < 4 || t.compare(t.size() - 2, 2, "(Z") != 0)) return false;
    const Str b = t.substr(2, t.size() - 4);
    w.kind(sixteen ? "Position and route" : "Position report", ACARS_READ_POSITION);
    double la, lo;
    if (sixteen) {
        if (b.size() < 25 || !coords(b.substr(4, 13), la, lo, true)) return false;
        if (!w.pos(la, lo)) return false;
        const Str wp1 = trim(sub(b, 18, 5)), wp2 = trim(sub(b, 23, 5));
        if (b.size() >= 28 && airport(sub(b, 17, 4)) && airport(sub(b, 21, 4))) { w.dep(sub(b, 17, 4)); w.arr(sub(b, 21, 4)); }
        else if (!wp1.empty()) w.route(wp1, wp2);
        return true;
    }
    if (b.size() < 19 || !coords(b.substr(0, 13), la, lo, true) || !w.pos(la, lo)) return false;
    if (b.size() == 25) { w.alt(100 * num(sub(b, 19, 3))); Str t2 = sub(b, 22); for (auto& c : t2) if (c == ' ') c = '0'; w.f("Temperature", t2 + " C"); }
    else if (sub(b, 13, 3) == "OFF") { w.kind("OOOI: wheels off", ACARS_READ_OOOI); w.time("Off", sub(b, 22, 4)); }
    return true;
}

bool fst(const Str& t, W& w) {
    if (!starts(t, "FST01")) return false;
    const auto p = split(t, ' ');
    const Str h = p[0];
    if (h.size() < 28) return false;
    const Str c = h.substr(13);
    const Str a = sub(c, 1, 6), b = sub(c, 8, 7);
    if (!digits(a) || !digits(b)) return false;
    w.kind("Flight status", ACARS_READ_POSITION);
    if (!w.pos(dirOf(c[0]) * atof(a.c_str()) / 10000, dirOf(c[7]) * atof(b.c_str()) / 10000)) return false;
    w.dep(h.substr(5, 4)); w.arr(h.substr(9, 4));
    w.alt(100 * num(sub(c, 15)));
    return true;
}

bool autpos(const Str& t, W& w) {
    size_t a = t.find("/AUTPOS/LLD ");
    if (a != 6) return false;
    const auto lines = split(t, '\n');
    if (lines.size() < 5) return false;
    const Str l0 = trim(lines[0]);
    if (l0.size() < 34) return false;
    const Str la = l0.substr(18, 7), lo = l0.substr(26, 8);
    if (!digits(la.substr(1)) || !digits(lo.substr(1))) return false;
    w.kind("Position report", ACARS_READ_POSITION);
    if (!w.pos(dirOf(la[0]) * (atof(la.substr(1, 2).c_str()) + atof(la.substr(3, 2).c_str()) / 60 + atof(la.substr(5, 2).c_str()) / 3600),
               dirOf(lo[0]) * (atof(lo.substr(1, 3).c_str()) + atof(lo.substr(4, 2).c_str()) / 60 + atof(lo.substr(6, 2).c_str()) / 3600))) return false;
    w.flight(l0.substr(2, 4));
    for (const auto& ln : lines) {
        const Str l = trim(ln);
        const size_t at = l.find("/ALT "); if (at != Str::npos) w.alt(num(sub(l, at + 5, 6)));
        const size_t fb = l.find("/FOB "); if (fb != Str::npos) w.fuel(sub(l, fb + 5, 6), "kg");
    }
    return true;
}

bool posa1(const Str& t, W& w) {
    if (!starts(t, "POSA1")) return false;
    const auto f = split(t, ',');
    if (f.size() != 11) return false;
    double la, lo;
    if (!coords(f[0].substr(5), la, lo, false)) return false;
    w.kind("Position report", ACARS_READ_POSITION);
    if (!w.pos(la, lo)) return false;
    w.alt(num(f[3]) * 100);
    w.route(trim(f[1]) + " " + hhmm(f[2]), trim(f[4]) + " " + hhmm(f[5]));
    return true;
}

bool tod16(const Str& t, W& w) {
    const auto f = split(t, ',');
    if (f.size() != 5 || !digits(trim(f[0])) || trim(f[0]).size() != 6) return false;
    const auto tmp = split(f[4], '/');
    const auto pos = split(trim(tmp[0]), ' ');
    w.kind("Position report", ACARS_READ_POSITION);
    w.time("Time", f[0]);
    if (!f[1].empty()) w.alt(num(f[1]));
    w.time("ETA", f[2]);
    if (pos.size() == 4 && (pos[0] == "N" || pos[0] == "S")) { if (!w.pos(dirOf(pos[0][0]) * num(pos[1]), dirOf(pos[2][0]) * num(pos[3]))) return false; }
    else if (pos.size() == 2) { if (!w.pos(num(sub(pos[0], 1)) * dirOf(pos[0][0]) / 1000, num(sub(pos[1], 1)) * dirOf(pos[1][0]) / 1000)) return false; }
    else return false;
    if (tmp.size() > 1) w.flight(tmp[1]);
    return true;
}

bool label1L(const Str& t, W& w) {
    if (starts(t, "000000070")) {
        const auto p = split(t.substr(9), ',');
        if (p.size() != 7 || p[4].empty() || p[5].empty() || !numberText(p[4].substr(1)) || !numberText(p[5].substr(1))) return false;
        w.kind("Position report", ACARS_READ_POSITION);
        if (!w.pos(dirOf(p[4][0]) * num(p[4].substr(1)), dirOf(p[5][0]) * num(p[5].substr(1)))) return false;
        w.dep(p[0]); w.arr(p[1]); w.time("Time", p[2]); w.time("ETA", p[3]);
        return true;
    }
    if (starts(t, "000000660")) {
        const auto p = split(t.substr(9), ',');
        double la, lo;
        if (p.size() != 5 || !coords(p[0], la, lo, true)) return false;
        w.kind("Position report", ACARS_READ_POSITION);
        if (!w.pos(la, lo)) return false;
        w.time("Time", sub(p[1], 0, 6)); w.alt(atof(sub(p[1], 6, 3).c_str()) * 100); w.route(trim(sub(p[1], 9)), "");
        return true;
    }
    if (t[0] == '+' || t[0] == '-') {
        const auto p = split(t, '/');
        if (p.size() != 7 || !numberText(stripSpaces(p[0])) || !numberText(stripSpaces(p[1]))) return false;
        w.kind("Position report", ACARS_READ_POSITION);
        if (!w.pos(num(stripSpaces(p[0])), num(stripSpaces(p[1])))) return false;
        for (size_t i = 2; i < p.size(); i++) {
            const auto kv = words(p[i]);
            if (kv.size() < 2) continue;
            const Str v = kv[1];
            if (kv[0] == "UTC") w.time("Time", v); else if (kv[0] == "ALT") w.alt(num(v)); else if (kv[0] == "FOB") w.fuel(v, "kg"); else if (kv[0] == "ETA") w.time("ETA", v);
        }
        return true;
    }
    {
        Str q; for (char c : t) if (c == '\r') continue; else q += c;
        const auto ls = split(q, '\n');
        if (ls.size() == 3) {
            Str a; for (char c : q) a += c == '\n' ? '/' : c;
            const auto p = split(a, '/');
            Str dep, des, eta, alt, fn, lat, lon, utc;
            for (const auto& f : p) { const auto kv = split(f, ' '); if (kv.size() < 2) continue; const Str v = kv[1];
                if (kv[0] == "DEP") dep = v; else if (kv[0] == "DES") des = v; else if (kv[0] == "ETA") eta = v; else if (kv[0] == "ALT") alt = v; else if (kv[0] == "FN") fn = v; else if (kv[0] == "LAT") lat = f.substr(4); else if (kv[0] == "LON") lon = f.substr(4); else if (kv[0] == "UTC") utc = v; }
            if (dep.empty() && des.empty()) return false;
            w.kind("Position report", ACARS_READ_POSITION);
            w.dep(dep); w.arr(des); w.time("ETA", eta); if (numberText(alt)) w.alt(num(alt)); w.flight(fn); w.time("Time", utc);
            if (!lat.empty() && !lon.empty()) signedDecimal(w, lat, lon);
            return true;
        }
    }
    return false;
}

bool oooi44(const Str& t, W& w, const char* key, int minFields, const char* kind, const char* timeKey) {
    const auto d = split(t, ',');
    Str pre = t.substr(0, t.find(','));
    if (starts(pre, "00")) pre = pre.substr(2);
    if (!starts(pre, key)) return false;
    if ((int)d.size() < minFields) return false;
    double la, lo;
    if (!coords(d[1], la, lo, true)) return false;
    w.kind(kind, ACARS_READ_OOOI);
    if (!w.pos(la, lo)) return false;
    w.dep(d[2]); w.arr(d[3]);
    w.f("Date", sub(d[4], 0, 2) + "-" + sub(d[4], 2, 2));
    w.time(timeKey, d[5]);
    if (strcmp(key, "OFF") == 0 && d.size() >= 8) { w.time("ETA", d[6]); w.fuel(d[7]); }
    else if (d.size() >= 7) w.fuel(d[6]);
    return true;
}

bool label44(const Str& t, W& w) {
    Str pre = t.substr(0, t.find(','));
    if (starts(pre, "00")) pre = pre.substr(2);
    if (starts(pre, "ETA")) {
        const auto d = split(t, ',');
        if (d.size() < 9) return false;
        double la, lo;
        if (!coords(d[1], la, lo, true)) return false;
        w.kind("ETA report", ACARS_READ_ETA);
        if (!w.pos(la, lo)) return false;
        w.alt(100 * num(d[2])); w.dep(d[3]); w.arr(d[4]);
        w.f("Date", sub(d[5], 0, 2) + "-" + sub(d[5], 2, 2));
        w.time("Time", d[6]); w.time("ETA", d[7]); w.fuel(d[8]);
        return true;
    }
    if (starts(pre, "POS")) {
        const auto d = split(t, ',');
        if (d.size() < 9) return false;
        const size_t n = d.size();
        double la, lo;
        if (!coords(d[n - 8], la, lo, true)) return false;
        w.kind("Position report", ACARS_READ_POSITION);
        if (!w.pos(la, lo)) return false;
        const Str fl = trim(d[n - 7]);
        if (digits(fl)) w.alt(atof(fl.c_str()) * 100); else if (!fl.empty()) w.f("Altitude", "ground");
        w.dep(d[n - 6]); w.arr(d[n - 5]);
        w.f("Date", sub(d[n - 4], 0, 2) + "-" + sub(d[n - 4], 2, 2));
        w.time("Time", d[n - 3]); w.time("ETA", d[n - 2]); w.fuel(d[n - 1]);
        return true;
    }
    if (starts(pre, "IN0")) return oooi44(t, w, "IN", 7, "OOOI: gate in", "In");
    if (starts(pre, "ON0")) return oooi44(t, w, "ON", 7, "OOOI: wheels on", "On");
    if (starts(pre, "OFF")) return oooi44(t, w, "OFF", 8, "OOOI: wheels off", "Off");
    if (starts(t, " /FB") || starts(t, "/FB")) {
        const auto f = split(t, '/');
        if (f.size() != 4) return false;
        const auto d = split(f[3], ',');
        if (d.size() < 6 || d[0].empty() || d[1].empty()) return false;
        w.kind("Position report", ACARS_READ_POSITION);
        const double sgnA = d[0][0] == 'S' ? -1 : 1, sgnB = d[1][0] == 'W' ? -1 : 1;
        if (!w.pos(sgnA * num(sub(d[0], 1)), sgnB * num(sub(d[1], 1)))) return false;
        w.flight(d[2]); w.arr(d[4]); w.time("ETA", d[5]);
        return true;
    }
    return false;
}

bool label4A(const Str& t, W& w) {
    if (starts(t, "DIS")) {
        const auto f = split(t, ',');
        if (f.size() < 3 || f[1].size() < 3 || f[2].empty()) return false;
        w.kind("Dispatch message", ACARS_READ_TEXT);
        w.time("Time", f[1].substr(2) + "00"); w.f("Callsign", f[2]);
        Str x; for (size_t i = 3; i < f.size(); i++) x += f[i];
        w.text(x);
        return true;
    }
    if (starts(t, "DOOR")) {
        const auto f = split(t, ' ');
        if (f.size() != 3) return false;
        const auto a = split(f[0], '/');
        if (a.size() < 2) return false;
        w.kind("Door event", ACARS_READ_OOOI);
        w.f("Door", a[1]); w.f("State", f[1]); w.time("Time", f[2] + "00");
        return true;
    }
    if (starts(t, "01")) {
        const size_t eol = t.find('\n');
        if (eol == Str::npos) return false;
        const Str l = trim(t.substr(0, eol));
        const size_t sl = l.find('/');
        if (l.size() < 8 || sl == Str::npos || l.size() < sl + 15) return false;
        w.kind("State change", ACARS_READ_OOOI);
        w.f("From state", l.substr(2, 2)); w.f("To state", l.substr(4, 2));
        w.f("Callsign", trim(l.substr(6, sl - 6))); w.time("Time", l.substr(sl + 1, 6));
        w.dep(l.substr(sl + 7, 4)); w.arr(l.substr(sl + 11, 4));
        return true;
    }
    const auto f = split(t, ',');
    if (f.size() == 11) {
        if (!digits(trim(f[0])) || f[0].size() != 6) return false;
        w.kind("Flight report", ACARS_READ_OOOI);
        w.time("Time", f[0]); w.f("Registration", f[2]); w.f("Callsign", f[3]); w.dep(f[4]); w.arr(f[5]);
        return true;
    }
    if (f.size() == 6 && !f[0].empty() && (f[0][0] == 'N' || f[0][0] == 'S')) {
        double la, lo;
        if (!coords(f[0], la, lo, false)) return false;
        w.kind("Position report", ACARS_READ_POSITION);
        if (!w.pos(la, lo)) return false;
        w.alt(atof(sub(f[1], 6, 3).c_str()) * 100); w.route(trim(sub(f[0], 13)) + " " + hhmm(sub(f[1], 0, 6)), trim(sub(f[1], 9)) + " " + hhmm(f[2]));
        return true;
    }
    if (f.size() == 6) {
        if (!digits(trim(f[0])) || f[0].size() != 6) return false;
        Str c; for (char ch : f[4] + f[5]) if (ch != ' ' && ch != '.') c += ch;
        double la, lo;
        if (!coords(c, la, lo, false)) return false;
        w.kind("Position report", ACARS_READ_POSITION);
        if (!w.pos(la, lo)) return false;
        w.time("Time", f[0]); w.time("ETA", f[1]); w.alt(num(f[3]));
        return true;
    }
    return false;
}

bool label4N(const Str& t, W& w) {
    const auto f = split(t, ',');
    if (t.size() == 51) {
        Str c = sub(t, 30, 15);
        if (c.size() < 13 || c[1] != '0') return false;
        Str d; d += c[0]; d += c.substr(2);
        double la, lo;
        if (!coords(d, la, lo, false)) return false;
        w.kind("Position report", ACARS_READ_POSITION);
        if (!w.pos(la, lo)) return false;
        w.dep(sub(t, 8, 3)); w.arr(sub(t, 13, 3)); w.alt(atof(sub(t, 48, 3).c_str()) * 100);
        return true;
    }
    if (f.size() == 33) {
        w.kind("Flight data", ACARS_READ_ROUTE);
        w.f("Date", f[3]);
        if (f[1] == "B") { if (numberText(f[4]) && numberText(f[5]) && !w.pos(num(f[4]), num(f[5]))) return false; w.alt(num(f[6])); }
        w.dep(f[8]); w.arr(f[9]); w.alternate(f[10]);
        w.f("Arrival runway", sub(f[11], 0, f[11].find('/')));
        return true;
    }
    return false;
}

bool label4T(const Str& t, W& w) {
    if (starts(t, "AGFSR")) {
        const auto d = split(t.substr(5), '/');
        if (d.size() != 20 || d[6].size() < 15) return false;
        const Str a = d[6].substr(0, 7), b = d[6].substr(7, 8);
        if (!digits(a.substr(0, 6)) || !digits(b.substr(0, 7))) return false;
        w.kind("Position report", ACARS_READ_POSITION);
        if (!w.pos(dirOf(a[6]) * (atof(a.substr(0, 2).c_str()) + atof(a.substr(2, 4).c_str()) / 100 / 60), dirOf(b[7]) * (atof(b.substr(0, 3).c_str()) + atof(b.substr(3, 4).c_str()) / 100 / 60))) return false;
        w.flight(d[0]); w.dep(sub(d[3], 0, 3)); w.arr(sub(d[3], 3)); w.time("Time", sub(d[4], 0, 4)); w.alt(100 * num(d[7]));
        return true;
    }
    if (starts(t, "ETA")) {
        const auto d = split(t.substr(3), '/');
        if (d.size() != 3) return false;
        const auto e = split(d[2], ' ');
        if (e.size() < 3) return false;
        w.kind("ETA report", ACARS_READ_ETA);
        w.flight(d[0]); w.arr(e[1]); w.time("ETA", sub(e[2], 0, 4));
        return true;
    }
    return false;
}

bool label2P(const Str& t, W& w) {
    const auto p = split(t, ',');
    if (p.size() == 7 && t.find("FM3 ") != Str::npos) {
        const Str lat = stripSpaces(p[2]), lon = stripSpaces(p[3]);
        if (lat.empty() || lon.empty()) return false;
        w.kind("Position report", ACARS_READ_POSITION);
        if (!signedDecimal(w, lat, lon)) return false;
        w.time("ETA", p[1]); w.alt(num(p[4]));
        const size_t at = p[0].find("FM3 ");
        if (at >= 4) w.flight(p[0].substr(4, at - 4));
        const Str h = p[0].substr(at + 4); if (digits(h)) w.time("Time", h);
        return true;
    }
    if (p.size() == 10 && t.find("FM4") != Str::npos) {
        const size_t at = p[0].find("FM4");
        w.kind("Position report", ACARS_READ_POSITION);
        if (!signedDecimal(w, p[4], p[5])) return false;
        if (at >= 4) w.flight(p[0].substr(4, at - 4));
        w.dep(p[0].substr(at + 3)); w.arr(p[1]); w.time("Time", sub(p[2], 2)); w.time("ETA", p[3]); w.alt(num(p[6])); w.heading(num(p[7]));
        return true;
    }
    if (p.size() == 12 && t.find("FM5 ") != Str::npos) {
        const size_t at = p[0].find("FM5 ");
        w.kind("Position report", ACARS_READ_POSITION);
        if (!signedDecimal(w, p[4], p[5])) return false;
        w.dep(p[0].substr(at + 4)); w.arr(p[1]); w.time("Time", p[2]); w.time("ETA", p[3]); w.alt(num(p[6])); w.flight(p[10]);
        return true;
    }
    return false;
}

bool label22(const Str& t, W& w) {
    if (starts(t, "OFF01")) {
        const auto f = split(t.substr(5), '/');
        if (f.size() != 2 || f[1].size() < 22) return false;
        w.kind("OOOI: wheels off", ACARS_READ_OOOI);
        w.flight(f[0]); w.time("Time", f[1].substr(4, 4)); w.dep(f[1].substr(8, 4)); w.arr(f[1].substr(12, 4)); w.time("Off", f[1].substr(16, 6));
        return true;
    }
    if (starts(t, "OFF02\r\n") || starts(t, "OFF02\n")) {
        const auto f = split(t.substr(t.find('\n') + 1), ',');
        if (f.size() != 4) return false;
        w.kind("OOOI: wheels off", ACARS_READ_OOOI);
        w.dep(f[0]); w.arr(f[1]); w.time("Off", f[2]);
        return true;
    }
    if (starts(t, "OFF02")) {
        const auto f = split(t.substr(5), '/');
        double la, lo;
        if (f.size() != 2 || f[1].size() < 40 || !coords(f[1].substr(0, 14), la, lo, true)) return false;
        w.kind("OOOI: wheels off", ACARS_READ_OOOI);
        if (!w.pos(la, lo)) return false;
        w.flight(f[0]); w.time("Time", f[1].substr(16, 4)); w.dep(f[1].substr(20, 4)); w.arr(f[1].substr(24, 4)); w.time("Off", f[1].substr(28, 4)); w.time("ETA", f[1].substr(36, 4));
        return true;
    }
    if (t[0] == 'N' || t[0] == 'S') {
        const auto f = split(t, ',');
        if (f.size() != 11 || f[0].size() < 15 || !numberText(stripSpaces(f[0].substr(1, 7))) || !numberText(stripSpaces(f[0].substr(9)))) return false;
        w.kind("Position report", ACARS_READ_POSITION);
        if (!w.pos(dirOf(f[0][0]) * num(stripSpaces(f[0].substr(1, 7))) / 10000, dirOf(f[0][8]) * num(stripSpaces(f[0].substr(9))) / 10000)) return false;
        w.time("Time", f[2]); w.alt(num(f[3]));
        return true;
    }
    return false;
}

bool label80(const Str& t, W& w) {
    Str q; for (char c : t) if (c != '\r') q += c;
    const auto lines = split(q, '\n');
    auto header = [&](const Str& line) {
        const auto h2 = split(line, '/');
        if (h2.size() < 3) return false;
        const auto a2 = words(h2[0]); if (a2.size() >= 3 && digits(a2[2])) w.flight(a2[2]);
        const auto b2 = words(h2[1]); if (b2.size() == 2) w.dep(b2[1]);
        const auto c2 = words(h2[2]); if (!c2.empty()) { w.arr(c2[0]); if (c2.size() > 1) w.f("Registration", c2[1]); }
        return true;
    };
    if (lines[0].find(',') != Str::npos && lines[0].find("POSRPT") != Str::npos) {      // "/FB 0105/AD KCHS/N3950.1,W07548.3,3P01 POSRPT  0267/20 KBOS/KCHS .N327"
        const auto c = split(lines[0], ',');
        if (c.size() < 3) return false;
        size_t g = 0;
        while (g < c.size() && c[g].find("POSRPT") == Str::npos) g++;
        if (g < 2 || g >= c.size()) return false;
        const auto parts = split(c[g - 2], '/');
        const Str la = parts.back(), lo = c[g - 1];
        if (la.size() < 2 || lo.size() < 2) return false;
        w.kind("Position report", ACARS_READ_POSITION);
        if (!w.pos(dirOf(la[0]) * num(la.substr(1)) / 100, dirOf(lo[0]) * num(lo.substr(1)) / 100)) return false;
        return header(c[g]);
    }
    if (lines.size() == 1 && q.find(',') != Str::npos) {
        const auto c = split(q, ',');
        const auto h2 = words(c[0]);
        double la, lo;
        if (h2.size() < 3 || h2[1] != "POS" || !coords(h2[2], la, lo, false)) return false;
        w.kind("Position report", ACARS_READ_POSITION);
        if (!w.pos(la, lo)) return false;
        if (c.size() >= 3) w.time("Time", c[2]);
        return true;
    }
    if (lines.size() < 2) {
        if (!header(q)) return false;
        w.kind("Flight status", ACARS_READ_OOOI);
        return true;
    }
    const auto h = split(lines[0], '/');
    if (h.size() < 3) return false;
    bool any = false;
    for (size_t i = 1; i < lines.size(); i++) {
        for (const auto& part : split(lines[i], '/')) {
            const Str s = trim(part);
            const size_t sp = s.find_first_of(" \t");
            if (sp == Str::npos) continue;
            const Str k = s.substr(0, sp), v = trim(s.substr(sp));
            if (k == "POS") {
                size_t e = v.find_first_of("EW");
                if (v.size() > 2 && e != Str::npos && (v[0] == 'N' || v[0] == 'S')) {
                    const double sc = v.find('.') != Str::npos ? 100 : 1000;
                    any |= w.pos(dirOf(v[0]) * num(v.substr(1, e - 1)) / sc, dirOf(v[e]) * num(v.substr(e + 1)) / sc);
                }
            } else if (k == "ALT") { w.alt(atof(v.c_str())); any = true; }
            else if (k == "FL") { w.alt(atof(v.c_str()) * 100); any = true; }
            else if (k == "HDG") { w.heading(atof(v.c_str())); any = true; }
            else if (k == "FB" || k == "FOB") { Str d; for (char c : v) if (c >= '0' && c <= '9') d += c; w.fuel(d, "lb or kg"); any = true; }
            else if (k == "ETA") { w.time("ETA", v.substr(0, v.find('.'))); any = true; }
            else if (k == "UTC") { w.time("Time", v); any = true; }
            else if (k == "TAS" || k == "SPD") { w.f(k == "TAS" ? "True airspeed" : "Ground speed", v + " kn"); any = true; }
            else if (k == "MCH") { w.f("Mach", v); any = true; }
        }
    }
    if (!any) return false;
    w.kind("Position report", ACARS_READ_POSITION);
    header(lines[0]);
    return true;
}

bool label83(const Str& t, W& w) {
    if (starts(t, "001PR")) {
        Str p; for (char c : sub(t, 13, 15)) if (c != '.') p += c;
        double la, lo;
        if (!coords(p, la, lo, true)) return false;
        w.kind("Position report", ACARS_READ_POSITION);
        if (!w.pos(la, lo)) return false;
        w.alt(atof(sub(t, 28, 5).c_str()));
        return true;
    }
    if (starts(t, "4DH3 ETAT2")) {
        const auto f = words(t);
        if (f.size() < 7) return false;
        const auto s = split(f[3], '/');
        if (s.size() < 2) return false;
        w.kind("ETA report", ACARS_READ_ETA);
        w.dep(s[0]); w.arr(s[1]); w.f("Registration", f[4]); w.time("ETA", f[6] + "00");
        return true;
    }
    Str q; for (char c : t) if (c != ' ' && c != '\r' && c != '\n') q += c;
    const auto f = split(q, ',');
    if (f.size() != 9 || !numberText(f[3]) || !numberText(f[4])) return false;
    w.kind("Position report", ACARS_READ_POSITION);
    if (!w.pos(num(f[3]), num(f[4]))) return false;
    w.dep(f[0]); w.arr(f[1]); w.alt(num(f[5])); w.f("Ground speed", trim(f[6]) + " kn"); w.heading(num(f[7]));
    return true;
}

bool h1(const Str& t, W& w) {
    if (starts(t, "FLR") || starts(t, "#CFBFLR") || starts(t, "WRN") || starts(t, "#CFBWRN")) {
        const bool fl = t.find("FLR") != Str::npos;
        const char* key = fl ? "/FR" : "/WN";
        const size_t at = t.find(key);
        if (at == Str::npos || t.size() < at + 3 + 20) return false;
        const Str data = t.substr(at + 3, 20), msg = trim(t.substr(at + 23));
        w.kind(fl ? "Fault report" : "Warning message", ACARS_READ_FAULT);
        w.f("Time", sub(data, 4, 2) + "-" + sub(data, 2, 2) + " " + sub(data, 6, 2) + ":" + sub(data, 8, 2) + " UTC");
        w.text(msg);
        return true;
    }
    if (starts(t, "*POS")) {
        if (t.size() != 43) return false;
        w.kind("Position report", ACARS_READ_POSITION);
        if (!w.pos(dirOf(t[12]) * (atof(t.substr(13, 2).c_str()) + atof(t.substr(15, 2).c_str()) / 60), dirOf(t[17]) * (atof(t.substr(18, 3).c_str()) + atof(t.substr(21, 2).c_str()) / 60))) return false;
        w.f("Date", t.substr(4, 2) + "-" + t.substr(6, 2)); w.time("Time", t.substr(8, 4)); w.alt(atof(t.substr(23, 5).c_str()));
        return true;
    }
    if (starts(t, "(POS-")) {
        const size_t sp = t.find_first_of(" ", 5);
        const size_t sl = t.find('/', 5);
        if (sp == Str::npos || sl == Str::npos || sl < sp || t.size() < sl + 7) return false;
        Str ll = trim(t.substr(sp, sl - sp));
        size_t ns = ll.find_first_of("NS");
        if (ns == Str::npos || ns + 1 >= ll.size()) return false;
        Str a = ll.substr(0, ns + 1), b = ll.substr(ns + 1);
        if (!a.empty() && a[0] == '-') a = a.substr(1);      // the examples carry a minus that the hemisphere letter overrides
        if (b.size() != 6 || a.size() < 5) return false;
        const double la = (atof(a.substr(0, a.size() - 3).c_str()) + atof(a.substr(a.size() - 3, 2).c_str()) / 60) * dirOf(a.back());
        const double lo = (atof(b.substr(0, 3).c_str()) + atof(b.substr(3, 2).c_str()) / 60) * dirOf(b.back());
        w.kind("Position report", ACARS_READ_POSITION);
        if (!w.pos(la, lo)) return false;
        w.flight(t.substr(5, sp - 5));
        w.time("Time", t.substr(sl + 1, 6));
        const size_t f = t.find(" F", sl);
        if (f != Str::npos) w.alt(atof(sub(t, f + 2, 3).c_str()) * 100);
        return true;
    }
    if (starts(t, "76401")) {
        const auto ls = split(t, '\n');
        if (ls.size() < 3) return false;
        const Str l1 = trim(ls[1]);
        w.kind("Position report", ACARS_READ_POSITION);
        w.dep(sub(l1, 5, 4)); w.arr(sub(l1, 9, 4));
        for (size_t i = 2; i < ls.size(); i++) {
            const Str l = trim(ls[i]);
            double la, lo;
            if (l.size() >= 19 && coords(l.substr(0, 13), la, lo, false)) { w.pos(la, lo); w.time("Time", l.substr(13, 6)); }
        }
        return w.o.hasPos;
    }
    if (starts(t, "M") && t.size() > 8 && digits(t.substr(1, 2)) && t[3] == 'A' && digits(t.substr(6, 4))) {
        const auto f = split(t.substr(10), ',');
        if (f.size() < 7) return false;
        if (!numberText(stripSpaces(f[3])) || !numberText(stripSpaces(f[4]))) return false;
        w.kind("Position report", ACARS_READ_POSITION);
        if (!w.pos(num(stripSpaces(f[3])), num(stripSpaces(f[4])))) return false;
        w.flight(t.substr(4, 6)); w.dep(f[0]); w.arr(f[1]); w.alt(num(f[5])); w.heading(num(f[6]));
        return true;
    }
    if (starts(t, "L") && t.size() > 12 && t.find(".TI2/") != Str::npos && digits(t.substr(1, 2))) {
        const size_t sl = t.find('/'), ti = t.find(".TI2/");
        if (sl == Str::npos || ti <= sl) return false;
        w.kind("D-ATIS request", ACARS_READ_WEATHER);
        w.flight(t.substr(4, sl - 4));
        w.f("Airport", t.substr(sl + 1, 4));
        return true;
    }
    if (arinc702(t, w)) return true;
    if (starts(t, "(FPL-")) {
        Str q; for (char c : t) if (c != '\r') q += c;
        Str body = q; for (auto& c : body) if (c == '\n') c = ' ';
        const auto p = split(body, '-');
        if (p.size() < 7) return false;
        w.kind("Flight plan", ACARS_READ_ROUTE);
        w.flight(p[1]);
        w.dep(sub(p[4], 0, 4)); if (p[5].size() > 8) w.text(sub(p[5], 0, 180));
        for (size_t i = 6; i < p.size(); i++) { if (p[i].size() >= 8 && airport(sub(p[i], 0, 4)) && digits(sub(p[i], 4, 4))) { w.arr(sub(p[i], 0, 4)); w.time("ETA", sub(p[i], 4, 4)); break; } }
        return true;
    }
    return false;
}

bool label5Z(const Str& t, W& w) {
    if (!starts(t, "/")) return false;
    const auto lines = split(t, '\n');
    const auto d = split(trim(lines[0]), '/');
    if (d.size() < 2) return false;
    const auto h = split(d[1], ' ');
    const Str type = h[0];
    if (type == "B3" && d.size() >= 3 && d[2].size() > 4) {
        const auto i = words(d[2]);
        if (i.size() < 4) return false;
        w.kind("Takeoff data request", ACARS_READ_ETA);
        w.dep(i[0]); w.arr(i[1]);
        return true;
    }
    if (type == "C3" && d.size() >= 3) {
        const auto i = words(d[2]);
        if (i.size() < 4) return false;
        w.kind("Gate request", ACARS_READ_ETA);
        w.dep(i[0]); w.arr(i[1]);
        return true;
    }
    if (d.size() >= 4) {
        const auto a = words(d[2]);
        const auto e = words(d[3]);
        if (a.size() >= 3 && airport(a[0]) && airport(a[1]) && !e.empty()) {
            w.kind("Airline report", ACARS_READ_ETA);
            w.dep(a[0]); w.arr(a[1]);
            w.time("Time", e[e.size() > 1 ? 1 : 0]);
            return true;
        }
    }
    return false;
}

bool h202e(const Str& t, W& w) {
    if (!starts(t, "02E")) return false;
    const auto p = split(t, ' ');
    if (p.size() < 2 || p.back() != "Q" || p[0].size() != 45) return false;
    w.kind("Wind report", ACARS_READ_WEATHER);
    w.dep(p[0].substr(5, 4)); w.arr(p[0].substr(9, 4));
    double la, lo;
    int n = 0;
    auto add = [&](const Str& s) { if (s.size() != 32 || !coords(s.substr(0, 13), la, lo, false)) return; if (!n) w.pos(la, lo); char b[48]; snprintf(b, sizeof b, "FL%d: %d deg / %d kn", atoi(s.substr(17, 3).c_str()), atoi(s.substr(25, 3).c_str()), atoi(s.substr(28, 3).c_str())); w.f("Wind", b); n++; };
    add(p[0].substr(13));
    for (size_t i = 1; i + 1 < p.size(); i++) add(sub(p[i], 1));
    return n > 0;
}

bool labelHX(const Str& t, W& w) {
    if (!starts(t, "RA FMT ")) return false;
    const auto p = split(t, ' ');
    if (p.size() >= 5 && p[2] == "LOCATION" && p[3].size() >= 7 && p[4].size() >= 8) {
        w.kind("Position report", ACARS_READ_POSITION);
        return w.pos(dirOf(p[3][0]) * (atof(p[3].substr(1, 2).c_str()) + atof(p[3].substr(3, 4).c_str()) / 60), dirOf(p[4][0]) * (atof(p[4].substr(1, 3).c_str()) + atof(p[4].substr(4, 4).c_str()) / 60));
    }
    if (p.size() >= 4 && p[2] == "43") { w.kind("Flight data", ACARS_READ_ROUTE); w.dep(p[3]); return true; }
    return false;
}

bool labelSQ(const Str& t, W& w) {
    if (t.size() < 16 || t[0] != '0' || t[2] != 'X') return false;
    const size_t v = t.find('V', 9);
    if (v == Str::npos) return false;
    Str iata = t.substr(4, 3), icao = t.substr(7, 4), rest = t.substr(12, v - 12);
    const size_t ns = rest.find_first_of("NS"), ew = rest.find_first_of("EW", ns == Str::npos ? 0 : ns);
    if (ns == Str::npos || ew == Str::npos || !digits(rest.substr(0, ns)) || !digits(rest.substr(ns + 1, ew - ns - 1))) return false;
    w.kind("Ground station squitter", ACARS_READ_OTHER);
    w.f("Station", iata + " / " + icao);
    if (!w.pos(atof(rest.substr(0, ns).c_str()) / 100 * dirOf(rest[ns]), atof(rest.substr(ns + 1, ew - ns - 1).c_str()) / 100 * dirOf(rest[ew]))) return false;
    const size_t sl = t.find('/', v);
    if (sl != Str::npos) { char b[24]; snprintf(b, sizeof b, "%.3f MHz", atof(t.substr(v + 1, sl - v - 1).c_str()) / 1000); w.f("VDL frequency", b); }
    return true;
}

} // namespace

static bool interpretOne(const std::string& label, const std::string& text, AcarsReading& o);

bool acarsInterpret(const std::string& label, const std::string& text, AcarsReading& o) {
    o = AcarsReading();
    try { return interpretOne(label, text, o); }
    catch (const std::exception&) { o = AcarsReading(); return false; }      // a text shorter than its layout
}

static bool interpretOne(const std::string& label, const std::string& text, AcarsReading& o) {
    if (label.size() != 2 || text.empty()) return false;
    W w{o};
    const Str& t = text;
    bool ok = false;
    const Str l = label;
    if (l == "10") ok = label10(t, w);
    else if (l == "11") ok = label11(t, w);
    else if (l == "12") ok = nSpace(t, w, true) || label12Pos(t, w);
    else if (l == "13" || l == "14" || l == "17" || l == "18") ok = label13to18(l, t, w);
    else if (l == "15") ok = paren2(t, w, false) || fst(t, w) || label13to18(l, t, w);
    else if (l == "16") ok = autpos(t, w) || paren2(t, w, true) || nSpace(t, w, false) || posa1(t, w) || label13to18(l, t, w) || tod16(t, w);
    else if (l == "1L") ok = label1L(t, w);
    else if (l == "1M") {
        if (starts(t, "/")) {
            Str q; for (char c : t) q += c == '\n' ? '/' : c;
            const auto r = split(q, '/');
            if (r.size() > 8) { w.kind("ETA report", ACARS_READ_ETA); w.flight(r[1]); w.dep(r[4]); w.arr(r[5]); w.alternate(r[6]); w.time("ETA", r[8] + "00"); ok = true; }
        }
    }
    else if (l == "20") {
        if (starts(t, "POS")) {
            const auto f = split(t.substr(3), ',');
            double la, lo;
            if ((f.size() == 11 || f.size() == 5) && coords(f[0], la, lo, false)) { w.kind("Position report", ACARS_READ_POSITION); ok = w.pos(la, lo); }
        }
    }
    else if (l == "21") {
        if (starts(t, "POS")) {
            const auto f = split(t.substr(3), ',');
            if (f.size() == 9 && trim(f[0]).size() == 16) {
                const Str v = trim(f[0]);
                w.kind("Position report", ACARS_READ_POSITION);
                if (w.pos(dirOf(v[0]) * atof(v.substr(1, 6).c_str()) / 1000, dirOf(v[8]) * atof(v.substr(9, 6).c_str()) / 1000)) { w.time("Time", f[2]); w.alt(num(f[3])); w.time("ETA", f[7]); w.arr(f[8]); ok = true; }
            }
        }
    }
    else if (l == "22") ok = label22(t, w);
    else if (l == "24") {
        const auto f = split(t, '/');
        if (f.size() == 10 && f[0].empty() && f[9].empty() && f[5].size() > 1 && f[6].size() > 1 && numberText(f[5].substr(1)) && numberText(f[6].substr(1))) {
            w.kind("Position report", ACARS_READ_POSITION);
            ok = w.pos(dirOf(f[5][0]) * num(f[5].substr(1)), dirOf(f[6][0]) * num(f[6].substr(1)));
            w.flight(f[3]); w.alt(num(f[4])); w.time("ETA", f[8]);
        }
    }
    else if (l == "2P") ok = label2P(t, w) || arinc702(t, w);
    else if (l == "30") {
        if (starts(t, "/EA")) {
            Str q; for (char c : t) q += c == '\n' ? '/' : c;
            const auto r = split(q, '/');
            if (r.size() >= 2) { w.kind("ETA report", ACARS_READ_ETA); w.time("ETA", sub(r[1], 2, 4)); if (r.size() >= 3 && starts(r[2], "DS")) w.arr(sub(r[2], 2, 4)); ok = true; }
        }
    }
    else if (l == "44") ok = label44(t, w);
    else if (l == "4A") ok = label4A(t, w);
    else if (l == "4N") ok = label4N(t, w);
    else if (l == "4T") ok = label4T(t, w);
    else if (l == "58") {
        const auto d = split(t, '/');
        if (d.size() == 8 && d[3].size() > 1 && d[4].size() > 1 && numberText(d[3].substr(1)) && numberText(d[4].substr(1))) {
            w.kind("Position report", ACARS_READ_POSITION);
            ok = w.pos(dirOf(d[3][0]) * num(d[3].substr(1)), dirOf(d[4][0]) * num(d[4].substr(1)));
            w.flight(d[0]); w.time("Time", d[2]); w.alt(num(d[5]));
        }
    }
    else if (l == "5Z") ok = label5Z(t, w);
    else if (l == "80") ok = label80(t, w);
    else if (l == "83") ok = label83(t, w);
    else if (l == "8E") {
        const auto f = split(t, ',');
        if (f.size() == 2 && f[0].size() == 4 && upper(f[0]) && f[1].size() == 4 && digits(f[1])) { w.kind("ETA report", ACARS_READ_ETA); w.arr(f[0]); w.time("ETA", f[1]); ok = true; }
    }
    else if (l == "B6") { if (starts(t, "/") && t.size() > 1) { w.kind("Airline message", ACARS_READ_TEXT); w.text(t.substr(1)); ok = true; } }
    else if (l == ":;") { if (digits(t) && t.size() >= 5) { char b[32]; snprintf(b, sizeof b, "%.3f MHz", atof(t.c_str()) / 1000); w.kind("Datalink frequency", ACARS_READ_OTHER); w.f("Frequency", b); ok = true; } }
    else if (l == "H1") ok = h1(t, w);
    else if (l == "H2") ok = h202e(t, w);
    else if (l == "HX") ok = labelHX(t, w);
    else if (l == "SQ") ok = labelSQ(t, w);
    else if (l == "QQ" && sub(t, 12, 7) == "\r\n001FE" && t.size() >= 48) {
        w.kind("OOOI: wheels off", ACARS_READ_OOOI);
        w.dep(t.substr(0, 4)); w.arr(t.substr(4, 4)); w.time("Off", t.substr(21, 6));
        ok = w.pos(dirOf(t[27]) * (atof(t.substr(28, 2).c_str()) + atof(t.substr(30, 4).c_str()) / 60), dirOf(t[34]) * (atof(t.substr(35, 3).c_str()) + atof(t.substr(38, 4).c_str()) / 60));
        if (ok && digits(t.substr(45, 3))) w.f("Ground speed", t.substr(45, 3) + " kn");
    }
    else if (l == "1J" || l == "2J" || l == "2A" || l == "4J") ok = arinc702(t, w);
    if (!ok) { o = AcarsReading(); return false; }
    return true;
}

std::string acarsInterpretLine(const std::string& label, const std::string& text) {
    AcarsReading r;
    if (!acarsInterpret(label, text, r)) return "";
    std::string s = r.kind + ":";
    bool first = true;
    auto add = [&](const std::string& v) { if (v.empty()) return; s += first ? " " : ", "; s += v; first = false; };
    char b[48];
    if (r.hasPos) { snprintf(b, sizeof b, "%.3f, %.3f", r.lat, r.lon); add(b); }
    for (const auto& f : r.fields) {
        if (f.first == "Text" || f.first == "Route") add(f.second);
        else add(f.first + " " + f.second);
    }
    return s;
}

} // namespace dect2
