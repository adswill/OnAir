// Spots from outside the station (4.0): see ham_spots.h.
#include "dect2/ham/ham_spots.h"
#include "dect2/common/xmlmini.h"
#include "dect2/ham/ham.h"
#include "dect2/ham/ham_ops.h"
#include "dect2/ham/ham_station.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>

namespace dect2::ham {

namespace {
std::string upper(std::string s) { for (char& c : s) c = (char)std::toupper((unsigned char)c); return s; }
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> w;
    std::stringstream ss(s);
    std::string t;
    while (ss >> t) w.push_back(t);
    return w;
}
bool callLike(const std::string& c) {
    if (c.size() < 3 || c.size() > 16) return false;
    bool dig = false, let = false;
    for (char ch : c) {
        if (std::isdigit((unsigned char)ch)) dig = true;
        else if (std::isupper((unsigned char)ch)) let = true;
        else if (ch != '/') return false;
    }
    return dig && let;
}
bool isNumber(const std::string& s, double& v) {
    if (s.empty()) return false;
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end && *end == 0 && std::isfinite(v);
}
bool gridLike(const std::string& g) {
    if (g.size() != 4 && g.size() != 6) return false;
    const std::string u = upper(g);
    if (u[0] < 'A' || u[0] > 'R' || u[1] < 'A' || u[1] > 'R' || !std::isdigit((unsigned char)u[2]) || !std::isdigit((unsigned char)u[3])) return false;
    if (u.size() == 6 && (u[4] < 'A' || u[4] > 'X' || u[5] < 'A' || u[5] > 'X')) return false;
    return true;
}

// days since 1970-01-01 of a civil date (proleptic Gregorian)
long long daysFromCivil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

// A small reader for the JSON the spot APIs answer with: an array of flat objects. Nested values are skipped; strings, numbers, true / false
// and null become text ("" for null). Anything malformed ends the read with what was read so far.
using JObj = std::map<std::string, std::string>;
struct JRead {
    const std::string& s;
    size_t i = 0;
    explicit JRead(const std::string& t) : s(t) {}
    void ws() { while (i < s.size() && std::isspace((unsigned char)s[i])) i++; }
    bool str(std::string& out) {
        out.clear();
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c == '\\' && i < s.size()) {
                const char e = s[i++];
                switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'u': {
                    if (i + 4 > s.size()) return false;
                    const unsigned cp = (unsigned)std::strtoul(s.substr(i, 4).c_str(), nullptr, 16);
                    i += 4;
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
                    else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                    continue;
                }
                default: c = e; break;
                }
            }
            out += c;
        }
        if (i >= s.size()) return false;
        i++;
        return true;
    }
    bool skip(int depth = 0) {   // any value
        ws();
        if (i >= s.size() || depth > 32) return false;
        const char c = s[i];
        if (c == '"') { std::string t; return str(t); }
        if (c == '{' || c == '[') {
            const char close = c == '{' ? '}' : ']';
            i++;
            ws();
            if (i < s.size() && s[i] == close) { i++; return true; }
            while (i < s.size()) {
                if (c == '{') { ws(); std::string k; if (!str(k)) return false; ws(); if (i >= s.size() || s[i] != ':') return false; i++; }
                if (!skip(depth + 1)) return false;
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == close) { i++; return true; }
                return false;
            }
            return false;
        }
        while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && !std::isspace((unsigned char)s[i])) i++;
        return true;
    }
    bool scalar(std::string& out) {
        ws();
        if (i >= s.size()) return false;
        if (s[i] == '"') return str(out);
        if (s[i] == '{' || s[i] == '[') { out.clear(); return skip(); }
        const size_t a = i;
        while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && !std::isspace((unsigned char)s[i])) i++;
        out = s.substr(a, i - a);
        if (out == "null") out.clear();
        return true;
    }
    std::vector<JObj> array() {
        std::vector<JObj> v;
        ws();
        if (i >= s.size() || s[i] != '[') return v;
        i++;
        while (true) {
            ws();
            if (i >= s.size()) return v;
            if (s[i] == ']') { i++; return v; }
            if (s[i] == ',') { i++; continue; }
            if (s[i] != '{') { if (!skip()) return v; continue; }
            i++;
            JObj o;
            while (true) {
                ws();
                if (i >= s.size()) return v;
                if (s[i] == '}') { i++; break; }
                if (s[i] == ',') { i++; continue; }
                std::string k, val;
                if (!str(k)) return v;
                ws();
                if (i >= s.size() || s[i] != ':') return v;
                i++;
                if (!scalar(val)) return v;
                o[k] = val;
            }
            v.push_back(std::move(o));
        }
    }
};
std::string get(const JObj& o, const char* k) { const auto it = o.find(k); return it == o.end() ? "" : it->second; }
}  // namespace

const char* spotSrcName(SpotSrc s) {
    switch (s) {
    case SpotSrc::Own: return "own";
    case SpotSrc::Cluster: return "cluster";
    case SpotSrc::Rbn: return "RBN";
    case SpotSrc::Pota: return "POTA";
    case SpotSrc::Sota: return "SOTA";
    case SpotSrc::PskReporter: return "PSK Reporter";
    }
    return "";
}
std::string ExtSpot::band() const { return bandNameFor(freqHz); }

double isoUtc(const std::string& t) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    double sec = 0;
    if (t.size() < 16) return 0;
    if (std::sscanf(t.c_str(), "%4d-%2d-%2d", &y, &mo, &d) != 3) return 0;
    const char sep = t[10];
    if (sep != 'T' && sep != ' ') return 0;
    if (std::sscanf(t.c_str() + 11, "%2d:%2d", &h, &mi) != 2) return 0;
    if (t.size() >= 19 && t[16] == ':') sec = std::atof(t.c_str() + 17);
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec < 0 || sec >= 61) return 0;
    return (double)daysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400.0 + h * 3600.0 + mi * 60.0 + std::floor(sec);
}

std::string spotModeGuess(const std::string& comment, double hz, int region) {
    static const char* kNames[] = {"FT8", "FT4", "FT2", "JS8", "WSPR", "Q65", "MSK144", "JT65", "JT9", "FST4", "CW", "RTTY", "PSK31", "PSK63",
                                   "PSK125", "OLIVIA", "SSTV", "FM", "AM", "MFSK", "THOR", "HELL", "DSTAR", "DMR", "C4FM", "M17", "FREEDV"};
    for (const auto& w0 : words(upper(comment))) {
        std::string w;
        for (char c : w0) if (std::isalnum((unsigned char)c)) w += c;
        if (w == "USB" || w == "LSB" || w == "SSB") return "SSB";
        if (w == "PSK") return "PSK31";
        if (w == "DV" || w == "D-STAR") return "DSTAR";
        for (const char* n : kNames) if (w == n) return n;
    }
    // the usual dials of the slot modes: a signal up to 3 kHz above one is that mode
    static const struct { int mode; const char* name; } kSlot[] = {{0, "FT8"}, {1, "FT4"}};
    for (const auto& b : bandDials())
        for (const auto& sm : kSlot) {
            const double dial = dialFor(b.band, sm.mode) * 1e6;
            if (dial > 0 && hz >= dial && hz <= dial + 3000) return sm.name;
        }
    const BandSegment* s = segmentAt(region, hz);
    if (!s) return "";
    if (s->use == SegUse::Cw) return "CW";
    if (s->use == SegUse::Fm) return "FM";
    if (s->use == SegUse::All && hz < 30e6) return "SSB";
    return "";
}

bool parseClusterLine(const std::string& line0, double nowUtc, ExtSpot& out) {
    const std::string line = trim(line0);
    if (line.size() < 20 || upper(line.substr(0, 6)) != "DX DE ") return false;
    const size_t colon = line.find(':', 6);
    if (colon == std::string::npos) return false;
    ExtSpot s;
    s.spotter = upper(trim(line.substr(6, colon - 6)));
    if (s.spotter.empty()) return false;
    const auto w = words(line.substr(colon + 1));
    if (w.size() < 2) return false;
    double khz = 0;
    if (!isNumber(w[0], khz) || khz < 100 || khz > 1e7) return false;
    s.freqHz = khz * 1e3;
    s.call = upper(w[1]);
    if (!callLike(s.call)) return false;
    // the time token HHMMZ, the last one in the line; a locator may follow it
    int ti = -1;
    for (int i = (int)w.size() - 1; i >= 2; i--) {
        const std::string& t = w[(size_t)i];
        if (t.size() == 5 && (t[4] == 'Z' || t[4] == 'z') && std::all_of(t.begin(), t.begin() + 4, [](char c) { return std::isdigit((unsigned char)c) != 0; })) { ti = i; break; }
    }
    const size_t cEnd = ti >= 0 ? (size_t)ti : w.size();
    for (size_t i = 2; i < cEnd; i++) { if (!s.comment.empty()) s.comment += ' '; s.comment += w[i]; }
    if (ti >= 0 && (size_t)ti + 1 < w.size() && gridLike(w[(size_t)ti + 1])) s.spotterGrid = upper(w[(size_t)ti + 1]);
    if (ti >= 0) {
        const int hh = std::atoi(w[(size_t)ti].substr(0, 2).c_str()), mm = std::atoi(w[(size_t)ti].substr(2, 2).c_str());
        if (hh > 23 || mm > 59) return false;
        double t = std::floor(nowUtc / 86400.0) * 86400.0 + hh * 3600.0 + mm * 60.0;
        if (t > nowUtc + 600) t -= 86400.0;
        s.utc = t;
    } else s.utc = nowUtc;
    const bool rbn = s.spotter.size() > 2 && s.spotter.compare(s.spotter.size() - 2, 2, "-#") == 0;
    s.src = rbn ? SpotSrc::Rbn : SpotSrc::Cluster;
    if (rbn) {
        const auto cw = words(upper(s.comment));
        if (!cw.empty()) s.mode = cw[0] == "PSK" ? "PSK31" : cw[0];
        for (size_t i = 1; i < cw.size(); i++) {
            double v = 0;
            if (cw[i] == "DB" && isNumber(cw[i - 1], v)) s.snr = (int)std::lround(v);
            if (cw[i] == "WPM" && isNumber(cw[i - 1], v)) s.wpm = (int)std::lround(v);
        }
        s.spotter = s.spotter.substr(0, s.spotter.size() - 2);
    } else s.mode = spotModeGuess(s.comment, s.freqHz, 0);
    out = s;
    return true;
}

std::vector<ExtSpot> parsePotaSpots(const std::string& json) {
    std::vector<ExtSpot> v;
    JRead r(json);
    for (const JObj& o : r.array()) {
        ExtSpot s;
        s.src = SpotSrc::Pota;
        s.call = upper(trim(get(o, "activator")));
        double khz = 0;
        if (!callLike(s.call) || !isNumber(trim(get(o, "frequency")), khz) || khz <= 0) continue;
        s.freqHz = khz * 1e3;
        s.mode = upper(trim(get(o, "mode")));
        if (s.mode == "USB" || s.mode == "LSB") s.mode = "SSB";
        s.ref = upper(trim(get(o, "reference")));
        s.refName = get(o, "name");
        if (s.refName.empty()) s.refName = get(o, "parkName");
        s.spotter = upper(trim(get(o, "spotter")));
        s.comment = get(o, "comments");
        s.utc = isoUtc(get(o, "spotTime"));
        s.grid = get(o, "grid6");
        if (!gridLike(s.grid)) s.grid = get(o, "grid4");
        if (!gridLike(s.grid)) s.grid.clear();
        v.push_back(s);
    }
    return v;
}

std::vector<ExtSpot> parseSotaSpots(const std::string& json) {
    std::vector<ExtSpot> v;
    JRead r(json);
    for (const JObj& o : r.array()) {
        ExtSpot s;
        s.src = SpotSrc::Sota;
        s.call = upper(trim(get(o, "activatorCallsign")));
        double mhz = 0;
        if (!callLike(s.call) || !isNumber(trim(get(o, "frequency")), mhz) || mhz <= 0) continue;
        s.freqHz = mhz * 1e6;
        s.mode = upper(trim(get(o, "mode")));
        if (s.mode == "USB" || s.mode == "LSB") s.mode = "SSB";
        const std::string a = trim(get(o, "associationCode")), c = trim(get(o, "summitCode"));
        s.ref = upper(a.empty() ? c : a + "/" + c);
        s.refName = get(o, "summitDetails");
        s.spotter = upper(trim(get(o, "callsign")));
        s.comment = get(o, "comments");
        s.utc = isoUtc(get(o, "timeStamp"));
        v.push_back(s);
    }
    return v;
}

std::vector<ExtSpot> parsePskReporterXml(const std::string& xml) {
    std::vector<ExtSpot> v;
    const auto root = parseXml(xml);
    if (!root) return v;
    std::vector<std::shared_ptr<XmlNode>> stack{root};
    while (!stack.empty()) {
        const auto n = stack.back();
        stack.pop_back();
        for (const auto& ch : n->children) stack.push_back(ch);
        if (n->local() != "receptionReport") continue;
        ExtSpot s;
        s.src = SpotSrc::PskReporter;
        s.call = upper(n->attr("senderCallsign"));
        s.spotter = upper(n->attr("receiverCallsign"));
        double f = 0, t = 0, snr = 0;
        if (s.call.empty() || s.spotter.empty() || !isNumber(n->attr("frequency"), f) || f <= 0) continue;
        s.freqHz = f;
        if (isNumber(n->attr("flowStartSeconds"), t)) s.utc = t;
        if (isNumber(n->attr("sNR"), snr)) s.snr = (int)std::lround(snr);
        s.mode = upper(n->attr("mode"));
        s.grid = n->attr("senderLocator");
        s.spotterGrid = n->attr("receiverLocator");
        v.push_back(s);
    }
    std::sort(v.begin(), v.end(), [](const ExtSpot& a, const ExtSpot& b) { return a.utc > b.utc; });
    return v;
}

bool parseSplit(const std::string& text, double dxHz, double& txHz) {
    const auto w = words(upper(text));
    // a number, or a range "5-10" (its middle)
    auto amount = [](const std::string& t, double& v) {
        const size_t d = t.find('-');
        double a = 0, b = 0;
        if (d != std::string::npos && d > 0) {
            if (!isNumber(t.substr(0, d), a) || !isNumber(t.substr(d + 1), b)) return false;
            v = (a + b) / 2;
            return true;
        }
        return isNumber(t, v);
    };
    for (size_t i = 0; i < w.size(); i++) {
        const std::string& t = w[i];
        if (t == "QSX" && i + 1 < w.size()) {
            const std::string& n = w[i + 1];
            double f = 0;
            if (!isNumber(n.substr(0, n.find('-')), f) || f <= 0) continue;
            if (n.find('.') != std::string::npos && f < 1000) txHz = f * 1e6;                 // 14.205 MHz
            else if (f >= 1000) txHz = f * 1e3;                                                 // 14205 kHz
            else txHz = std::floor(dxHz / 1e6) * 1e6 + f * 1e3;                                 // 205: kHz on the same MHz
            return true;
        }
        int sign = 0;
        std::string rest;
        for (const char* k : {"UP", "DOWN", "DWN", "DN"}) {
            const std::string ks = k;
            if (t.compare(0, ks.size(), ks) == 0) {
                rest = t.substr(ks.size());
                if (!rest.empty() && !std::isdigit((unsigned char)rest[0])) { rest.clear(); continue; }
                sign = ks == "UP" ? 1 : -1;
                break;
            }
        }
        if (!sign) continue;
        double k = 1, kk = 0;
        bool dotted = false;
        if (!rest.empty()) { if (!amount(rest, kk)) continue; k = kk; dotted = rest.find('.') != std::string::npos; }
        else if (i + 1 < w.size() && amount(w[i + 1], kk)) { k = kk; dotted = w[i + 1].find('.') != std::string::npos; }
        if (dotted && k > 1 && std::fabs(k * 1e6 - dxHz) < 2e5) { txHz = k * 1e6; return true; }   // "UP 14.025": a frequency, not an amount
        if (k <= 0 || k > 50) continue;
        txHz = dxHz + sign * k * 1e3;
        return true;
    }
    return false;
}

// ---- the spot book ----
void SpotBook::add(const ExtSpot& s) {
    if (s.call.empty() || !(s.freqHz > 0)) return;
    const std::string b = s.band();
    for (size_t i = 0; i < v_.size(); i++) {
        Row& r = v_[i];
        if (r.s.src != s.src || r.s.call != s.call || r.s.band() != b || std::fabs(r.s.utc - s.utc) > mergeSec) continue;
        if (s.utc >= r.s.utc) {
            const bool other = s.spotter != r.s.spotter;
            const int n = r.spotters + (other ? 1 : 0);
            r.s = s;
            r.spotters = n;
            Row moved = r;
            v_.erase(v_.begin() + (long)i);
            const auto at = std::find_if(v_.begin(), v_.end(), [&](const Row& x) { return x.s.utc <= moved.s.utc; });
            v_.insert(at, moved);
        }
        return;
    }
    const auto at = std::find_if(v_.begin(), v_.end(), [&](const Row& x) { return x.s.utc <= s.utc; });
    v_.insert(at, Row{s, 1});
    if (v_.size() > 5000) v_.resize(5000);
}
void SpotBook::expire(double now) {
    v_.erase(std::remove_if(v_.begin(), v_.end(), [&](const Row& r) { return now - r.s.utc > maxAgeSec; }), v_.end());
}
std::vector<const SpotBook::Row*> SpotBook::filter(const std::string& band, const std::string& mode, unsigned mask) const {
    std::vector<const Row*> out;
    const std::string m = upper(mode);
    for (const Row& r : v_) {
        if (!(mask >> (int)r.s.src & 1u)) continue;
        if (!band.empty() && r.s.band() != band) continue;
        if (!m.empty() && upper(r.s.mode) != m) continue;
        out.push_back(&r);
    }
    return out;
}

}  // namespace dect2::ham
