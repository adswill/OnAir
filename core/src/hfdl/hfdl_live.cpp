// HFDL ground stations near the user: the community list parser, the merge with the squitters, the sort, the hints and the tune point (hfdl_live.h).
#include "dect2/hfdl/hfdl_live.h"
#include "dect2/hfdl/hfdl_hop.h"
#include "dect2/hfdl/hfdl_proto.h"
#include "dect2/geo/maidenhead.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

namespace dect2 {

namespace {

// A small JSON reader: enough for the station list (objects, arrays, numbers, strings, true/false/null). Depth and size are bounded.
struct JV {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    double n = 0;
    bool b = false;
    std::string s;
    std::vector<JV> a;
    std::vector<std::string> keys;   // an object: keys[i] names vals[i]
    std::vector<JV> vals;
    const JV* get(const char* k) const { if (t != Obj) return nullptr; for (size_t j = 0; j < keys.size(); j++) if (keys[j] == k) return &vals[j]; return nullptr; }
};

struct Reader {
    const std::string& s;
    size_t i = 0;
    std::string err;
    explicit Reader(const std::string& x) : s(x) {}
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++; }
    bool fail(const char* m) { if (err.empty()) err = std::string(m) + " at offset " + std::to_string(i); return false; }
    bool str(std::string& out) {
        if (i >= s.size() || s[i] != '"') return fail("expected a string");
        i++;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c == '\\') {
                if (i >= s.size()) return fail("bad escape");
                const char e = s[i++];
                switch (e) {
                    case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break; case 'b': out += '\b'; break; case 'f': out += '\f'; break;
                    case 'u': {
                        if (i + 4 > s.size()) return fail("bad \\u escape");
                        const unsigned cp = (unsigned)strtoul(s.substr(i, 4).c_str(), nullptr, 16);
                        i += 4;
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
                        else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }   // surrogates kept as they are (names only)
                        break;
                    }
                    default: out += e;
                }
            } else out += c;
        }
        if (i >= s.size()) return fail("unterminated string");
        i++;
        return true;
    }
    bool val(JV& v, int depth) {
        if (depth > 32) return fail("nested too deep");
        ws();
        if (i >= s.size()) return fail("unexpected end");
        const char c = s[i];
        if (c == '{') {
            v.t = JV::Obj; i++; ws();
            if (i < s.size() && s[i] == '}') { i++; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (i >= s.size() || s[i] != ':') return fail("expected ':'");
                i++;
                v.keys.push_back(std::move(k));
                v.vals.emplace_back();
                if (!val(v.vals.back(), depth + 1)) return false;
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == '}') { i++; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.t = JV::Arr; i++; ws();
            if (i < s.size() && s[i] == ']') { i++; return true; }
            for (;;) {
                v.a.emplace_back();
                if (!val(v.a.back(), depth + 1)) return false;
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ']') { i++; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v.t = JV::Str; return str(v.s); }
        if (s.compare(i, 4, "true") == 0) { v.t = JV::Bool; v.b = true; i += 4; return true; }
        if (s.compare(i, 5, "false") == 0) { v.t = JV::Bool; i += 5; return true; }
        if (s.compare(i, 4, "null") == 0) { v.t = JV::Null; i += 4; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const char* p = s.c_str() + i;
            char* e = nullptr;
            v.n = strtod(p, &e);
            if (e == p) return fail("bad number");
            v.t = JV::Num; i += (size_t)(e - p);
            return true;
        }
        return fail("unexpected character");
    }
};

double num(const JV* v, double def) { return v && v->t == JV::Num && std::isfinite(v->n) ? v->n : def; }

bool sameKhz(int a, int b) { return std::abs(a - b) <= 1; }   // lists round to the kHz differently now and then

}  // namespace

bool hfdlParseLiveList(const std::string& json, double nowSec, std::vector<HfdlLiveStation>& out, std::string* err) {
    out.clear();
    if (json.size() > (4u << 20)) { if (err) *err = "the list is too large"; return false; }
    Reader r(json);
    JV root;
    if (!r.val(root, 0)) { if (err) *err = r.err; return false; }
    const JV* gs = root.get("ground_stations");
    if (!gs || gs->t != JV::Arr) { if (err) *err = "no ground_stations array"; return false; }
    for (const JV& g : gs->a) {
        if (g.t != JV::Obj) continue;
        const int id = (int)num(g.get("id"), 0);
        if (id <= 0 || id > 127) continue;
        HfdlLiveStation st;
        st.id = id;
        if (const JV* n = g.get("name")) if (n->t == JV::Str) st.name = n->s;
        double t = num(g.get("last_updated"), 0);
        if (t < 0) t += nowSec;            // seconds before now (the hfdlobserver convention)
        st.updatedSec = t > 0 ? t : 0;
        st.stratum = (int)num(g.get("stratum"), -1);
        st.updateSource = (int)num(g.get("update_source"), 0);
        if (const JV* f = g.get("frequencies")) if (const JV* act = f->get("active")) if (act->t == JV::Arr)
            for (const JV& k : act->a) {
                const double v = num(&k, 0);
                if (v >= 2000 && v <= 30000) st.activeKhz.push_back((int)std::lround(v));
            }
        std::sort(st.activeKhz.begin(), st.activeKhz.end());
        st.activeKhz.erase(std::unique(st.activeKhz.begin(), st.activeKhz.end()), st.activeKhz.end());
        out.push_back(std::move(st));
    }
    return true;
}

std::vector<HfdlNearby> hfdlNearby(const std::vector<HfdlLiveStation>& live, const std::vector<HfdlHeardStation>& heard, bool havePos, double lat,
                                   double lon, double nowSec, const HfdlNearbyConfig& cfg) {
    struct Both { const HfdlLiveStation* l = nullptr; const HfdlHeardStation* h = nullptr; };
    std::map<int, Both> by;
    for (const auto& l : live) { auto& b = by[l.id]; if (!b.l || l.updatedSec > b.l->updatedSec) b.l = &l; }
    for (const auto& h : heard) { if (h.id <= 0) continue; auto& b = by[h.id]; if (!b.h || h.heardSec > b.h->heardSec) b.h = &h; }
    auto fresh = [&](double t) { return t > 0 && nowSec - t <= cfg.maxAgeSec && t - nowSec <= 300; };   // a time far in the future is a bad clock
    std::vector<HfdlNearby> out;
    for (const auto& [id, b] : by) {
        const bool lf = b.l && fresh(b.l->updatedSec) && !b.l->activeKhz.empty();
        const bool hf = b.h && fresh(b.h->heardSec) && !b.h->activeKhz.empty();
        if (!lf && !hf && !cfg.keepStale) continue;
        HfdlNearby n;
        n.id = id;
        const HfdlStationInfo* info = hfdlStation(id);
        if (info) { n.name = info->name; n.hasPos = true; n.lat = info->lat; n.lon = info->lon; }
        else if (b.l) n.name = b.l->name;
        if (n.name.empty()) n.name = "station " + std::to_string(id);
        // the newer fresh source gives the list
        const bool useLive = lf && (!hf || b.l->updatedSec >= b.h->heardSec);
        const std::vector<int>* main = useLive ? &b.l->activeKhz : hf ? &b.h->activeKhz : nullptr;
        const std::vector<int>* other = useLive ? (hf ? &b.h->activeKhz : nullptr) : (lf ? &b.l->activeKhz : nullptr);
        const int mainSrc = useLive ? kHfdlSrcLive : kHfdlSrcSquitter, otherSrc = useLive ? kHfdlSrcSquitter : kHfdlSrcLive;
        if (main) {
            n.source = mainSrc;
            n.updatedSec = useLive ? b.l->updatedSec : b.h->heardSec;
            std::vector<int> k = *main;
            std::sort(k.begin(), k.end());
            k.erase(std::unique(k.begin(), k.end()), k.end());
            for (int v : k) {
                HfdlNearbyFreq f; f.khz = v; f.sources = mainSrc;
                if (other) for (int w : *other) if (sameKhz(v, w)) { f.sources |= otherSrc; n.source |= otherSrc; break; }
                n.freqs.push_back(f);
            }
        } else n.updatedSec = std::max(b.l ? b.l->updatedSec : 0.0, b.h ? b.h->heardSec : 0.0);
        if (havePos && n.hasPos) greatCircle(lat, lon, n.lat, n.lon, n.km, n.bearingDeg);
        out.push_back(std::move(n));
    }
    std::stable_sort(out.begin(), out.end(), [&](const HfdlNearby& a, const HfdlNearby& b) {
        if (havePos) {
            const bool ak = a.km >= 0, bk = b.km >= 0;
            if (ak != bk) return ak;
            if (ak && a.km != b.km) return a.km < b.km;
        }
        return a.id < b.id;
    });
    return out;
}

std::vector<int> hfdlHintKhz(const std::vector<HfdlNearby>& n, double maxKm, size_t minStations) {
    std::vector<int> out;
    size_t used = 0;
    for (const auto& s : n) {
        if (s.freqs.empty()) continue;
        const bool near = s.km < 0 || s.km <= maxKm;
        if (!near && used >= minStations) continue;
        used++;
        for (const auto& f : s.freqs) if (std::find(out.begin(), out.end(), f.khz) == out.end()) out.push_back(f.khz);
    }
    return out;
}

double hfdlTunePointMhz(int band, const std::vector<int>& wantedKhz, double sampleRateHz) {
    const auto& bands = hfdlBands();
    if (band < 0 || (size_t)band >= bands.size()) return 0;
    const HfdlBand& B = bands[(size_t)band];
    const double carrier = kHfdlCarrierHz / 1e3;   // the signal sits this far above the dial
    const double half = 0.4 * sampleRateHz / 1e3 - 2.0;   // kHz either side of the centre that the channels may use (2 kHz for the signal itself)
    std::vector<double> p;   // the wanted signals inside the band, most wanted first
    for (int k : wantedKhz) if (k >= B.loKhz && k <= B.hiKhz) { const double v = k + carrier; if (std::find(p.begin(), p.end(), v) == p.end()) p.push_back(v); }
    const double bandLo = B.loKhz + carrier, bandHi = B.hiKhz + carrier;
    if (p.empty() || sampleRateHz <= 0 || half * 2 >= bandHi - bandLo) return B.centreMhz;
    if (half <= 0) return (p[0]) / 1e3;
    // the window must hold p[0]; try the edges at every other wanted signal and keep the one that holds most
    std::vector<double> cands = {p[0]};
    for (double q : p) { cands.push_back(q - half); cands.push_back(q + half); }
    double bestC = p[0]; int bestN = -1;
    for (double c : cands) {
        c = std::min(std::max(c, p[0] - half), p[0] + half);
        int nIn = 0;
        for (double q : p) nIn += std::fabs(q - c) <= half + 1e-6;
        if (nIn > bestN) { bestN = nIn; bestC = c; }
    }
    double lo = 1e18, hi = -1e18;
    for (double q : p) if (std::fabs(q - bestC) <= half + 1e-6) { lo = std::min(lo, q); hi = std::max(hi, q); }
    double c = 0.5 * (lo + hi);
    // a lone signal is kept off the centre (the radio's DC spike): 10 kHz below it when the span allows
    if (hi - lo < 10 && half >= (hi - lo) / 2 + 12) c -= 10;
    return c / 1e3;
}

}  // namespace dect2
