// Airport weather from datalink text (3.9, docs/SMART5-avia-3.9.md): METAR, SPECI and TAF reports inside ACARS, VDL2 and HFDL messages
// (weather requests, D-ATIS uplinks, printer messages) are found in the free text and parsed into one row per airport: wind, gust,
// visibility, ceiling, temperature and dew point, QNH, and the flight category. Format: WMO No. 306 FM 15 / FM 16 / FM 51 and the ICAO
// Annex 3 code, with the US variants (SM visibility, A altimeter in inHg). Flight category after the FAA definitions (ceiling and
// visibility): LIFR < 500 ft or < 1 SM, IFR < 1000 ft or < 3 SM, MVFR <= 3000 ft or <= 5 SM, else VFR.
// Pure text logic, no clock.
#pragma once
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace dect2 {

struct WxReport {
    std::string kind;            // METAR, SPECI, TAF
    std::string station;         // ICAO location indicator
    int day = 0, hour = 0, minute = 0;   // issue time (UTC), day of month
    bool hasWind = false, variable = false; int windDir = 0, windKt = 0, gustKt = 0;
    bool hasVis = false; double visM = 0; bool cavok = false;
    bool hasCeiling = false; int ceilingFt = 0;   // lowest BKN, OVC or VV layer
    std::string clouds;          // as sent, "FEW030 BKN045"
    bool hasTemp = false; int tempC = 0, dewC = 0;
    bool hasQnh = false; double qnhHpa = 0;
    std::string category;        // VFR, MVFR, IFR, LIFR (METAR and SPECI only; empty when visibility and ceiling are both unknown)
    std::string raw;             // the report text as found
};

namespace wxd {
inline bool isStation(const std::string& t) {
    if (t.size() != 4) return false;
    for (char c : t) if (!std::isupper((unsigned char)c) && !std::isdigit((unsigned char)c)) return false;
    return std::isupper((unsigned char)t[0]) && t != "AUTO" && t != "CAVO" && t != "NOSI" && t != "TEMP" && t != "BECM";
}
inline bool isTime(const std::string& t) {   // DDHHMMZ
    if (t.size() != 7 || t[6] != 'Z') return false;
    for (int i = 0; i < 6; i++) if (!std::isdigit((unsigned char)t[i])) return false;
    const int d = std::atoi(t.substr(0, 2).c_str()), h = std::atoi(t.substr(2, 2).c_str()), m = std::atoi(t.substr(4, 2).c_str());
    return d >= 1 && d <= 31 && h <= 23 && m <= 59;
}
inline bool allDigits(const std::string& s, size_t a, size_t n) { if (a + n > s.size()) return false; for (size_t i = a; i < a + n; i++) if (!std::isdigit((unsigned char)s[i])) return false; return true; }
}

inline std::string wxCategory(bool hasCeil, int ceilFt, bool hasVis, double visM) {
    if (!hasCeil && !hasVis) return "";
    const double sm = visM / 1609.344;
    const int c = hasCeil ? ceilFt : 100000; const double v = hasVis ? sm : 100;
    if (c < 500 || v < 1) return "LIFR";
    if (c < 1000 || v < 3) return "IFR";
    if (c <= 3000 || v <= 5) return "MVFR";
    return "VFR";
}

// Every report in a text. Tokens are split on white space; a report starts at "METAR", "SPECI" or "TAF" (optionally followed by COR /
// AMD), or at a bare "<station> <DDHHMMZ>", and runs to the next "=", the next report start, or the end.
inline std::vector<WxReport> parseWxReports(const std::string& text) {
    std::vector<WxReport> out;
    std::vector<std::string> tok;
    { std::string cur; for (char ch : text) { if (std::isspace((unsigned char)ch)) { if (!cur.empty()) tok.push_back(cur), cur.clear(); } else if (ch == '=') { if (!cur.empty()) tok.push_back(cur), cur.clear(); tok.push_back("="); } else cur += ch; } if (!cur.empty()) tok.push_back(cur); }
    auto startAt = [&](size_t i, std::string& kind, size_t& st) -> bool {
        size_t j = i; kind = "METAR";
        if (tok[i] == "METAR" || tok[i] == "SPECI" || tok[i] == "TAF") { kind = tok[i]; j++; while (j < tok.size() && (tok[j] == "COR" || tok[j] == "AMD")) j++; }
        if (j + 1 < tok.size() && wxd::isStation(tok[j]) && wxd::isTime(tok[j + 1])) { st = j; return true; }
        return false;
    };
    for (size_t i = 0; i < tok.size();) {
        std::string kind; size_t st = 0;
        if (!startAt(i, kind, st)) { i++; continue; }
        WxReport r; r.kind = kind; r.station = tok[st];
        r.day = std::atoi(tok[st + 1].substr(0, 2).c_str()); r.hour = std::atoi(tok[st + 1].substr(2, 2).c_str()); r.minute = std::atoi(tok[st + 1].substr(4, 2).c_str());
        size_t j = st + 2; std::string raw = r.station + " " + tok[st + 1];
        bool taf = kind == "TAF", inTrend = false;
        for (; j < tok.size(); j++) {
            const std::string& t = tok[j];
            std::string k2; size_t s2;
            if (t == "=" ) { j++; break; }
            if (startAt(j, k2, s2)) break;
            raw += " " + t;
            if (t == "NOSIG" || t == "BECMG" || t == "TEMPO" || t == "RMK" || (t.rfind("FM", 0) == 0 && t.size() == 8) || t.rfind("PROB", 0) == 0) { inTrend = true; continue; }
            if (inTrend) continue;   // the first group only: the trend and the TAF changes are not the observation
            if (t == "AUTO" || t == "COR" || t == "NIL") continue;
            // wind dddffKT, dddffGggKT, VRBffKT, also MPS
            if (!r.hasWind && t.size() >= 7 && (t.find("KT") == t.size() - 2 || t.find("MPS") == t.size() - 3)) {
                const bool mps = t.find("MPS") != std::string::npos;
                const bool vrb = t.rfind("VRB", 0) == 0;
                if ((vrb || wxd::allDigits(t, 0, 3)) && wxd::allDigits(t, 3, 2)) {
                    r.hasWind = true; r.variable = vrb; r.windDir = vrb ? 0 : std::atoi(t.substr(0, 3).c_str());
                    size_t p = 3; int ff = 0; while (p < t.size() && std::isdigit((unsigned char)t[p])) ff = ff * 10 + (t[p++] - '0');
                    int gg = 0; if (p < t.size() && t[p] == 'G') { p++; while (p < t.size() && std::isdigit((unsigned char)t[p])) gg = gg * 10 + (t[p++] - '0'); }
                    r.windKt = mps ? (int)std::lround(ff * 1.943844) : ff; r.gustKt = mps ? (int)std::lround(gg * 1.943844) : gg;
                    continue;
                }
            }
            if (taf && t.size() == 9 && t[4] == '/' && wxd::allDigits(t, 0, 4) && wxd::allDigits(t, 5, 4)) continue;   // validity period
            if (t == "CAVOK") { r.cavok = true; r.hasVis = true; r.visM = 10000; continue; }
            if (!r.hasVis && t.size() == 4 && wxd::allDigits(t, 0, 4)) { r.hasVis = true; r.visM = t == "9999" ? 10000 : std::atoi(t.c_str()); continue; }
            if (!r.hasVis && t.size() >= 3 && t.substr(t.size() - 2) == "SM") {   // 10SM, 1/2SM, P6SM, M1/4SM; "1 1/2SM" is two tokens
                std::string v = t.substr(0, t.size() - 2); if (!v.empty() && (v[0] == 'P' || v[0] == 'M')) v = v.substr(1);
                double sm = -1; const size_t sl = v.find('/');
                if (sl != std::string::npos) { const double a = std::atof(v.substr(0, sl).c_str()), b = std::atof(v.substr(sl + 1).c_str()); if (b > 0) sm = a / b; }
                else if (!v.empty() && wxd::allDigits(v, 0, v.size())) sm = std::atof(v.c_str());
                if (sm >= 0) {
                    if (sl != std::string::npos && j > st + 2 && tok[j - 1].size() <= 2 && wxd::allDigits(tok[j - 1], 0, tok[j - 1].size())) sm += std::atof(tok[j - 1].c_str());   // "1 1/2SM"
                    r.hasVis = true; r.visM = sm * 1609.344; continue;
                }
            }
            // clouds FEW/SCT/BKN/OVC + hhh (+CB/TCU), VVhhh; NSC, SKC, CLR, NCD
            if ((t.rfind("FEW", 0) == 0 || t.rfind("SCT", 0) == 0 || t.rfind("BKN", 0) == 0 || t.rfind("OVC", 0) == 0) && wxd::allDigits(t, 3, 3)) {
                r.clouds += (r.clouds.empty() ? "" : " ") + t;
                const int h = std::atoi(t.substr(3, 3).c_str()) * 100;
                if ((t[0] == 'B' || t[0] == 'O') && (!r.hasCeiling || h < r.ceilingFt)) { r.hasCeiling = true; r.ceilingFt = h; }
                continue;
            }
            if (t.rfind("VV", 0) == 0 && wxd::allDigits(t, 2, 3)) { r.clouds += (r.clouds.empty() ? "" : " ") + t; const int h = std::atoi(t.substr(2, 3).c_str()) * 100; if (!r.hasCeiling || h < r.ceilingFt) { r.hasCeiling = true; r.ceilingFt = h; } continue; }
            if (t == "NSC" || t == "SKC" || t == "CLR" || t == "NCD") { r.clouds += (r.clouds.empty() ? "" : " ") + t; continue; }
            // temperature / dew point: 08/03, M02/M10
            if (!r.hasTemp && !taf) {
                const size_t sl = t.find('/');
                if (sl != std::string::npos && sl >= 2 && t.size() - sl - 1 >= 2) {
                    auto num = [](const std::string& s, int& v) { std::string x = s; bool neg = false; if (!x.empty() && x[0] == 'M') { neg = true; x = x.substr(1); } if (x.size() != 2 || !wxd::allDigits(x, 0, 2)) return false; v = std::atoi(x.c_str()) * (neg ? -1 : 1); return true; };
                    int a = 0, b = 0;
                    if (num(t.substr(0, sl), a) && num(t.substr(sl + 1), b)) { r.hasTemp = true; r.tempC = a; r.dewC = b; continue; }
                }
            }
            if (!r.hasQnh && t.size() == 5 && t[0] == 'Q' && wxd::allDigits(t, 1, 4)) { r.hasQnh = true; r.qnhHpa = std::atoi(t.substr(1).c_str()); continue; }
            if (!r.hasQnh && t.size() == 5 && t[0] == 'A' && wxd::allDigits(t, 1, 4)) { r.hasQnh = true; r.qnhHpa = std::atoi(t.substr(1).c_str()) / 100.0 * 33.8639; continue; }
        }
        if (r.cavok && !r.hasCeiling) { /* CAVOK: no cloud below 5000 ft */ }
        if (!taf) {
            r.category = wxCategory(r.hasCeiling, r.ceilingFt, r.hasVis, r.visM);
            if (r.cavok && r.category.empty()) r.category = "VFR";
        }
        r.raw = raw;
        // a report needs at least one weather group to be believed (an address that only looks like a station and time is not weather)
        if (r.hasWind || r.hasVis || r.hasTemp || r.hasQnh || !r.clouds.empty()) out.push_back(r);
        i = j > i ? j : i + 1;
    }
    return out;
}

// The latest report per airport and kind, with when it was heard
class WxBook {
public:
    struct Row { WxReport rep; int64_t heard = 0; std::string source; int count = 0; };
    // returns the number of reports taken from the text
    int addText(const std::string& text, int64_t heard, const std::string& source) {
        int n = 0;
        for (auto& r : parseWxReports(text)) {
            Row& row = rows_[r.station + "|" + (r.kind == "TAF" ? "TAF" : "METAR")];
            const bool newer = row.count == 0 || r.day != row.rep.day || r.hour * 60 + r.minute >= row.rep.hour * 60 + row.rep.minute;
            row.count++;
            if (newer) { row.rep = r; row.heard = heard; row.source = source; }
            n++;
        }
        return n;
    }
    const std::map<std::string, Row>& rows() const { return rows_; }
    void clear() { rows_.clear(); }
private:
    std::map<std::string, Row> rows_;
};

} // namespace dect2
