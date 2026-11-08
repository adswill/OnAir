#include "dect2/net/satnogs.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace dect2 {
namespace {

// A small JSON reader: enough for the flat objects the API sends (nested values are read and skipped).
struct JKV;   // an object member; not std::pair<std::string, J>, which clang rejects while J is incomplete
struct J {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<J> a;
    std::vector<JKV> o;
    const J* get(const char* k) const;
};
struct JKV { std::string first; J second; };
inline const J* J::get(const char* k) const { for (const auto& p : o) if (p.first == k) return &p.second; return nullptr; }

struct P {
    const std::string& s;
    size_t i = 0;
    int depth = 0;
    void ws() { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; }
    bool str(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c == '\\' && i < s.size()) {
                char e = s[i++];
                switch (e) { case 'n': c = '\n'; break; case 't': c = '\t'; break; case 'r': c = '\r'; break; case 'b': c = '\b'; break; case 'f': c = '\f'; break;
                    case 'u': { if (i + 4 > s.size()) return false; unsigned v = (unsigned)strtoul(s.substr(i, 4).c_str(), nullptr, 16); i += 4;
                        if (v < 0x80) { out += (char)v; } else if (v < 0x800) { out += (char)(0xC0 | (v >> 6)); out += (char)(0x80 | (v & 63)); }
                        else { out += (char)(0xE0 | (v >> 12)); out += (char)(0x80 | ((v >> 6) & 63)); out += (char)(0x80 | (v & 63)); } continue; }
                    default: c = e; }
            }
            out += c;
        }
        if (i >= s.size()) return false;
        ++i;
        return true;
    }
    bool val(J& v) {
        ws();
        if (i >= s.size() || ++depth > 32) return false;
        struct D { int& d; ~D() { --d; } } guard{depth};
        const char c = s[i];
        if (c == '"') { v.t = J::Str; return str(v.s); }
        if (c == '{') {
            v.t = J::Obj; ++i; ws();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            for (;;) {
                ws(); std::string k; if (!str(k)) return false;
                ws(); if (i >= s.size() || s[i] != ':') return false; ++i;
                J x; if (!val(x)) return false;
                v.o.emplace_back(std::move(k), std::move(x));
                ws(); if (i >= s.size()) return false;
                if (s[i] == ',') { ++i; continue; }
                if (s[i] == '}') { ++i; return true; }
                return false;
            }
        }
        if (c == '[') {
            v.t = J::Arr; ++i; ws();
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            for (;;) {
                J x; if (!val(x)) return false;
                v.a.push_back(std::move(x));
                ws(); if (i >= s.size()) return false;
                if (s[i] == ',') { ++i; continue; }
                if (s[i] == ']') { ++i; return true; }
                return false;
            }
        }
        if (s.compare(i, 4, "true") == 0) { v.t = J::Bool; v.b = true; i += 4; return true; }
        if (s.compare(i, 5, "false") == 0) { v.t = J::Bool; i += 5; return true; }
        if (s.compare(i, 4, "null") == 0) { i += 4; return true; }
        char* end = nullptr;
        v.n = strtod(s.c_str() + i, &end);
        if (end == s.c_str() + i) return false;
        v.t = J::Num; i = (size_t)(end - s.c_str());
        return true;
    }
};

bool parseArray(const std::string& json, J& root) {
    P p{json};
    return p.val(root) && root.t == J::Arr;
}
double num(const J& o, const char* k) { const J* v = o.get(k); return v && v->t == J::Num && std::isfinite(v->n) ? v->n : 0; }
std::string text(const J& o, const char* k) { const J* v = o.get(k); return v && v->t == J::Str ? v->s : std::string(); }
std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

// SatNOGS mode name -> OnAir mode family. One table, first match wins, patterns are lower case substrings of the SatNOGS name.
// The families are those of app/ui/mode_table.cpp: 5 FM, 24 APRS (AFSK), 34 CW, 41 Spectrum explorer (no decoder: APT is listened to
// and recorded as IQ, as the pass planner does), 59 Meteor-M LRPT, 61 CubeSat telemetry (BPSK / GMSK / FSK / MSK frames). 0 = no mode in OnAir.
struct ModeMap { const char* pattern; int family; };
const ModeMap kModeMap[] = {
    {"lrpt", 59},
    {"apt", 41},
    {"afsk", 24},          // AFSK1k2 (AX.25 / APRS)
    {"gmsk", 61},          // GMSK1k2 .. GMSK19k2
    {"bpsk", 61},          // BPSK1k2 .. BPSK9k6, BPSK PMT
    {"msk", 61},
    {"gfsk", 61},
    {"fsk", 61},           // FSK1k2 .. FSK9k6 (after AFSK / GFSK, which contain it)
    {"cw", 34},
    {"fm", 5},             // plain FM voice and repeaters (after AFSK / GFSK / FSK, which contain it as text)
};
}

int parseSatnogsSatellites(const std::string& json, std::map<int, std::string>& names) {
    J root;
    if (!parseArray(json, root)) return 0;
    int n = 0;
    for (const J& e : root.a) {
        if (e.t != J::Obj) continue;
        const int no = (int)num(e, "norad_cat_id");
        const std::string nm = text(e, "name");
        if (no > 0 && !nm.empty()) { names[no] = nm; ++n; }
    }
    return n;
}

int parseSatnogsTransmitters(const std::string& json, const std::map<int, std::string>& names, std::vector<SatnogsTx>& out) {
    J root;
    if (!parseArray(json, root)) return 0;
    int n = 0;
    for (const J& e : root.a) {
        if (e.t != J::Obj) continue;
        SatnogsTx t;
        t.uuid = text(e, "uuid"); t.description = text(e, "description"); t.mode = text(e, "mode"); t.status = text(e, "status"); t.service = text(e, "service");
        t.norad = (int)num(e, "norad_cat_id");
        t.downHz = num(e, "downlink_low"); t.upHz = num(e, "uplink_low"); t.baud = num(e, "baud");
        const J* al = e.get("alive");
        t.alive = al && al->t == J::Bool && al->b && (t.status.empty() || t.status == "active");
        auto it = names.find(t.norad);
        t.satName = it != names.end() ? it->second : "NORAD " + std::to_string(t.norad);
        out.push_back(std::move(t));
        ++n;
    }
    return n;
}

int satnogsFamily(const std::string& mode, double baud) {
    const std::string m = lower(mode);
    (void)baud;
    if (m.empty()) return 0;
    // "SSTV", "DUV", "LoRa", "SSB", "USB" and the like have no pattern below and give 0
    for (const ModeMap& e : kModeMap) {
        if (!has(m, e.pattern)) continue;
        return e.family;
    }
    return 0;
}

bool satnogsMatches(const SatnogsTx& t, const SatnogsFilter& f) {
    if (f.aliveOnly && !t.alive) return false;
    const double mhz = t.downHz / 1e6;
    if (f.minMhz > 0 && mhz < f.minMhz) return false;
    if (f.maxMhz > 0 && mhz > f.maxMhz) return false;
    if (!f.modeText.empty() && !has(lower(t.mode), lower(f.modeText).c_str())) return false;
    if (!f.text.empty()) {
        const std::string q = lower(f.text);
        if (!has(lower(t.satName), q.c_str()) && !has(lower(t.description), q.c_str())) return false;
    }
    return true;
}

bool satnogsCacheStale(double nowSec, double fetchedSec, double maxAgeSec) {
    return fetchedSec <= 0 || fetchedSec > nowSec || nowSec - fetchedSec >= maxAgeSec;
}

}
