#include "dect2/ease/repeaters.h"
#include "dect2/geo/maidenhead.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>

namespace dect2::ease {

namespace {

std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool q = false;
    for (size_t i = 0; i < line.size(); i++) {
        const char c = line[i];
        if (q) {
            if (c == '"') { if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; i++; } else q = false; }
            else cur += c;
        } else if (c == '"') q = true;
        else if (c == ',' || c == ';' || c == '\t') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

int findCol(const std::map<std::string, int>& h, std::initializer_list<const char*> names) {
    for (const char* n : names) { auto it = h.find(n); if (it != h.end()) return it->second; }
    return -1;
}

}  // namespace

std::vector<Repeater> parseRepeaterCsv(const std::string& text, int& skipped, std::string& err) {
    std::vector<Repeater> out;
    skipped = 0; err.clear();
    size_t pos = 0;
    std::map<std::string, int> h;
    bool haveHeader = false;
    int cf = -1, cla = -1, clo = -1, cloc = -1, ccall = -1, cwhere = -1, cmode = -1, ccc = -1;
    while (pos <= text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trim(line).empty()) continue;
        if (!haveHeader) {
            if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
            auto cols = splitCsv(line);
            for (int i = 0; i < (int)cols.size(); i++) h[lower(trim(cols[i]))] = i;
            cf = findCol(h, {"output freq", "output frequency", "output", "frequency", "freq", "rx", "rx freq", "tx freq"});
            cla = findCol(h, {"latitude", "lat"});
            clo = findCol(h, {"longitude", "long", "lon", "lng"});
            cloc = findCol(h, {"locator", "maidenhead", "grid", "qth locator"});
            ccall = findCol(h, {"callsign", "call", "call sign"});
            cwhere = findCol(h, {"nearest city", "city", "location", "name", "site"});
            cmode = findCol(h, {"mode", "modes", "digital mode", "type"});
            ccc = findCol(h, {"color code", "colour code", "colorcode", "cc", "color_code"});
            if (cf < 0 || ((cla < 0 || clo < 0) && cloc < 0)) { err = "the file needs a frequency column and latitude and longitude (or a locator) columns"; return out; }
            haveHeader = true;
            continue;
        }
        const auto c = splitCsv(line);
        auto cell = [&](int i) { return i >= 0 && i < (int)c.size() ? trim(c[(size_t)i]) : std::string(); };
        Repeater r;
        r.mhz = std::atof(cell(cf).c_str());
        if (r.mhz > 1e5) r.mhz /= 1e6;               // a list in Hz
        else if (r.mhz > 3000) r.mhz /= 1e3;         // a list in kHz
        bool posOk = false;
        if (cla >= 0 && clo >= 0 && !cell(cla).empty() && !cell(clo).empty()) {
            r.lat = std::atof(cell(cla).c_str()); r.lon = std::atof(cell(clo).c_str());
            posOk = r.lat >= -90 && r.lat <= 90 && r.lon >= -180 && r.lon <= 180 && (r.lat != 0 || r.lon != 0);
        }
        if (!posOk && cloc >= 0) posOk = fromMaidenhead(cell(cloc), r.lat, r.lon);
        if (r.mhz < 1.0 || r.mhz > 6000.0 || !posOk) { skipped++; continue; }
        r.call = cell(ccall); r.where = cell(cwhere); r.mode = cell(cmode);
        const std::string cc = cell(ccc);
        if (!cc.empty()) { const int v = std::atoi(cc.c_str()); if (v >= 0 && v <= 15) r.colourCode = v; }
        out.push_back(r);
    }
    if (!haveHeader) err = "the file is empty";
    return out;
}

bool repeaterMatchesMode(const Repeater& r, const std::string& mode) {
    if (r.mode.empty()) return true;
    std::string m;
    for (char c : lower(r.mode)) if (std::isalnum((unsigned char)c)) m += c;
    auto has = [&](const char* s) { return m.find(s) != std::string::npos; };
    if (mode == "DMR") return has("dmr") || has("mototrbo");
    if (mode == "D-STAR") return has("dstar");
    if (mode == "Fusion") return has("fusion") || has("ysf") || has("c4fm");
    if (mode == "M17") return has("m17");
    return false;
}

std::vector<RepeaterNear> nearestRepeaters(const std::vector<Repeater>& list, const std::string& mode, double lat, double lon, int maxCount, double maxKm) {
    std::vector<RepeaterNear> out;
    for (const Repeater& r : list) {
        if (!repeaterMatchesMode(r, mode)) continue;
        double km = 0, br = 0;
        greatCircle(lat, lon, r.lat, r.lon, km, br);
        if (km > maxKm) continue;
        out.push_back({&r, km});
    }
    std::sort(out.begin(), out.end(), [](const RepeaterNear& a, const RepeaterNear& b) { return a.km < b.km; });
    if ((int)out.size() > maxCount) out.resize((size_t)maxCount);
    return out;
}

} // namespace dect2::ease

// ---- Open Repeater (openrepeater.org) answers ----------------------------------------------------------------------------------
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
namespace dect2::ease {
namespace {
struct JV { enum K { Null, Num, Str, Bool, Arr, Obj } k = Null; double n = 0; std::string s; std::vector<JV> a; std::map<std::string, JV> o; };
struct JP {
    const std::string& t; size_t i = 0; bool bad = false;
    explicit JP(const std::string& x) : t(x) {}
    void ws() { while (i < t.size() && isspace((unsigned char)t[i])) i++; }
    bool lit(const char* w) { const size_t n = strlen(w); if (t.compare(i, n, w) == 0) { i += n; return true; } return false; }
    std::string str() {
        std::string r; i++;
        while (i < t.size() && t[i] != '"') {
            char c = t[i++];
            if (c == '\\' && i < t.size()) {
                c = t[i++];
                if (c == 'n') r += '\n'; else if (c == 't') r += '\t'; else if (c == 'r') r += '\r';
                else if (c == 'u' && i + 4 <= t.size()) { unsigned v = (unsigned)strtoul(t.substr(i, 4).c_str(), nullptr, 16); i += 4;
                    if (v < 0x80) r += (char)v; else if (v < 0x800) { r += (char)(0xC0 | (v >> 6)); r += (char)(0x80 | (v & 63)); } else { r += (char)(0xE0 | (v >> 12)); r += (char)(0x80 | ((v >> 6) & 63)); r += (char)(0x80 | (v & 63)); } }
                else r += c;
            } else r += c;
        }
        if (i >= t.size()) bad = true; else i++;
        return r;
    }
    JV val(int d) {
        JV v; ws();
        if (bad || i >= t.size() || d > 24) { bad = true; return v; }
        const char c = t[i];
        if (c == '{') {
            v.k = JV::Obj; i++; ws();
            if (i < t.size() && t[i] == '}') { i++; return v; }
            while (!bad) {
                ws(); if (i >= t.size() || t[i] != '"') { bad = true; break; }
                const std::string key = str(); ws();
                if (i >= t.size() || t[i++] != ':') { bad = true; break; }
                v.o[key] = val(d + 1); ws();
                if (i >= t.size()) { bad = true; break; }
                const char e = t[i++]; if (e == '}') break; if (e != ',') bad = true;
            }
        } else if (c == '[') {
            v.k = JV::Arr; i++; ws();
            if (i < t.size() && t[i] == ']') { i++; return v; }
            while (!bad) {
                v.a.push_back(val(d + 1)); ws();
                if (i >= t.size()) { bad = true; break; }
                const char e = t[i++]; if (e == ']') break; if (e != ',') bad = true;
            }
        } else if (c == '"') { v.k = JV::Str; v.s = str(); }
        else if (lit("true")) { v.k = JV::Bool; v.n = 1; }
        else if (lit("false")) { v.k = JV::Bool; }
        else if (lit("null")) {}
        else { char* e = nullptr; v.n = strtod(t.c_str() + i, &e); if (e == t.c_str() + i) bad = true; else { v.k = JV::Num; i = (size_t)(e - t.c_str()); } }
        return v;
    }
};
bool numOf(const JV* v, double& out) {
    if (!v) return false;
    if (v->k == JV::Num) { out = v->n; return std::isfinite(out); }
    if (v->k == JV::Str && !v->s.empty()) { char* e = nullptr; out = strtod(v->s.c_str(), &e); return e != v->s.c_str() && std::isfinite(out); }
    return false;
}
const JV* field(const JV& o, const char* a, const char* b = nullptr) {
    auto it = o.o.find(a); if (it != o.o.end()) return &it->second;
    if (b) { it = o.o.find(b); if (it != o.o.end()) return &it->second; }
    return nullptr;
}
std::string textOf(const JV* v) { return v && v->k == JV::Str ? v->s : std::string(); }
} // namespace

std::string openRepeaterSearchUrl(double lat, double lon, double radiusKm, int page, int limit) {
    char b[200];
    snprintf(b, sizeof b, "https://www.openrepeater.org/api/v1/search?lat=%.4f&lng=%.4f&radius=%.0f&page=%d&limit=%d", lat, lon, radiusKm, page < 1 ? 1 : page, limit < 1 ? 1 : limit);
    return b;
}

std::vector<Repeater> parseOpenRepeaterJson(const std::string& text, int& skipped, std::string& err) {
    skipped = 0; err.clear();
    JP p(text); const JV root = p.val(0); p.ws();
    if (p.bad || p.i != text.size()) { err = "the answer is not JSON"; return {}; }
    const JV* arr = nullptr;
    if (root.k == JV::Arr) arr = &root;
    else if (root.k == JV::Obj) {
        for (const char* k : {"data", "repeaters", "results", "items"}) { auto it = root.o.find(k); if (it != root.o.end() && it->second.k == JV::Arr) { arr = &it->second; break; } }
        if (!arr) { const JV* m = field(root, "error", "message"); err = m && m->k == JV::Str ? m->s : "the answer has no list of repeaters"; return {}; }
    } else { err = "the answer has no list of repeaters"; return {}; }
    std::vector<Repeater> out;
    for (const JV& e : arr->a) {
        Repeater r; double f = 0, la = 0, lo = 0;
        if (e.k != JV::Obj || !numOf(field(e, "frequency", "freq"), f) || !numOf(field(e, "lat", "latitude"), la) || !numOf(field(e, "lng", "lon"), lo)) { skipped++; continue; }
        if (f > 1e6) f /= 1e6; else if (f > 1e4) f /= 1e3;   // Hz or kHz to MHz
        if (f < 0.1 || f > 100000 || std::fabs(la) > 90 || std::fabs(lo) > 180 || (la == 0 && lo == 0)) { skipped++; continue; }
        r.mhz = f; r.lat = la; r.lon = lo;
        r.call = textOf(field(e, "callsign", "call")); r.where = textOf(field(e, "city", "location")); r.mode = textOf(field(e, "mode"));
        out.push_back(std::move(r));
    }
    return out;
}
} // namespace dect2::ease
