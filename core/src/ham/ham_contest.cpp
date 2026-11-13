// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ham contest helpers: exchanges, cut numbers, the received exchange, dupes and Cabrillo (ham_contest.h).
#include "dect2/ham/ham_contest.h"
#include "dect2/ham/ham.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace dect2::ham {

namespace {
using F = ContestField;
std::string up(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }
std::vector<std::string> words(const std::string& t) {
    std::vector<std::string> w;
    std::string cur;
    for (char c : up(t)) {
        if (isalnum((unsigned char)c) || c == '/') cur += c;
        else if (!cur.empty()) { w.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) w.push_back(cur);
    return w;
}
bool allDigits(const std::string& s) { if (s.empty()) return false; for (char c : s) if (!isdigit((unsigned char)c)) return false; return true; }
bool inList(const std::string& s, const char* const* l) { for (; *l; l++) if (s == *l) return true; return false; }
const char* const kStates[] = {"AL","AK","AZ","AR","CA","CO","CT","DE","FL","GA","HI","ID","IL","IN","IA","KS","KY","LA","ME","MD","MA","MI","MN","MS",
    "MO","MT","NE","NV","NH","NJ","NM","NY","NC","ND","OH","OK","OR","PA","RI","SC","SD","TN","TX","UT","VT","VA","WA","WV","WI","WY","DC", nullptr};
const char* const kProv[] = {"NB","NS","QC","ON","MB","SK","AB","BC","NL","NF","LB","NU","YT","PE","NT", nullptr};
const char* const kSections[] = {"CT","EMA","ME","NH","RI","VT","WMA","ENY","NLI","NNJ","NNY","SNJ","WNY","DE","EPA","MDC","WPA","AL","GA","KY","NC","NFL",
    "PR","SC","SFL","TN","VA","VI","WCF","AR","LA","MS","NM","NTX","OK","STX","WTX","EB","LAX","ORG","PAC","SB","SCV","SDG","SF","SJV","SV","AK","AZ","EWA",
    "ID","MT","NV","OR","UT","WWA","WY","MI","OH","WV","IL","IN","WI","CO","IA","KS","MN","MO","ND","NE","SD","AB","BC","GH","MB","NB","NL","NS","ONE","ONN",
    "ONS","PE","QC","SK","TER","NT","GTA","MAR", nullptr};
// words that are never a name or a field
const char* const kNoise[] = {"DE","TU","TNX","K","KN","BK","R","QSL","UR","NR","NAME","OP","5NN","599","CFM","QRZ","CQ","TEST","AGN","PSE","ES","AND","IS",
    "MY","HR","HERE","FER","GL","73","EE","SK", nullptr};
}  // namespace

bool isUsState(const std::string& s) { return inList(up(s), kStates); }
bool isVeProvince(const std::string& s) { return inList(up(s), kProv); }
bool isArrlSection(const std::string& s) { return inList(up(s), kSections); }

const std::vector<KeyerContestDef>& keyerContestDefs() {
    static const std::vector<KeyerContestDef> v = {
        {"cqww-rtty", "CQ WW RTTY", "CQ-WW-RTTY", {F::Rst, F::CqZone, F::State}, false},
        {"cqwpx-rtty", "CQ WPX RTTY", "CQ-WPX-RTTY", {F::Rst, F::Serial}, false},
        {"arrl-rtty", "ARRL RTTY Roundup", "ARRL-RTTY", {F::Rst, F::StateOrSerial}, false},
        {"naqp-rtty", "NAQP RTTY", "NAQP-RTTY", {F::Name, F::State}, false},
        {"bartg-rtty", "BARTG HF RTTY", "BARTG-RTTY", {F::Rst, F::Serial, F::Time}, false},
        {"iaru-hf", "IARU HF Championship", "IARU-HF", {F::Rst, F::ItuZone}, false},
        {"cqww-cw", "CQ WW DX CW", "CQ-WW-CW", {F::Rst, F::CqZone}, true},
        {"cqwpx-cw", "CQ WPX CW", "CQ-WPX-CW", {F::Rst, F::Serial}, true},
        {"arrl-fd", "ARRL Field Day", "ARRL-FD", {F::Class, F::Section}, false},
        {"serial", "Generic serial", "", {F::Rst, F::Serial}, false},
    };
    return v;
}
const KeyerContestDef* contestById(const std::string& id) {
    for (const auto& d : keyerContestDefs()) if (id == d.id) return &d;
    return nullptr;
}
const char* contestFieldName(ContestField f) {
    switch (f) {
    case F::Rst: return "RST"; case F::Serial: return "Serial"; case F::CqZone: return "CQ zone"; case F::ItuZone: return "ITU zone";
    case F::State: return "State"; case F::StateOrSerial: return "State / serial"; case F::Name: return "Name"; case F::Class: return "Class";
    case F::Section: return "Section"; case F::Time: return "Time"; case F::Power: return "Power"; case F::Grid: return "Grid";
    }
    return "?";
}

std::string cutNumbers(const std::string& s, bool all) {
    std::string o = s;
    for (auto& c : o) {
        if (c == '0') c = 'T';
        else if (c == '9') c = 'N';
        else if (all && c == '1') c = 'A';
        else if (all && c == '5') c = 'E';
    }
    return o;
}
std::string uncutNumbers(const std::string& sIn) {
    const std::string s = up(sIn);
    // only a token that has a digit or is made of cut letters alone and at least one of them is a digit-like letter in a numeric place
    bool digit = false, onlyCut = true;
    for (char c : s) { if (isdigit((unsigned char)c)) digit = true; else if (!strchr("TONAE", c)) onlyCut = false; }
    if (!onlyCut || (!digit && s.size() < 2)) return s;
    if (!digit) {   // "TT" "ENN": all cut letters; "TEN" "ONE" "NAN" are words, keep them
        static const char* const kWords[] = {"TEN","ONE","NONE","NO","ON","AN","AT","TO","EAT","NET","TEA","ANT","TAN","OAT","NOT","NOTE","TONE","NEAT","NAE", nullptr};
        if (inList(s, kWords)) return s;
    }
    std::string o = s;
    for (auto& c : o) { if (c == 'T' || c == 'O') c = '0'; else if (c == 'N') c = '9'; else if (c == 'A') c = '1'; else if (c == 'E') c = '5'; }
    return o;
}

std::string ContestExchange::get(ContestField f) const {
    for (const auto& p : v) if (p.first == f) return p.second;
    return "";
}

std::string contestExchangeText(const KeyerContestDef& d, const ContestStation& s, int serial, const std::string& hhmm, bool twice) {
    std::string one;
    for (F f : d.fields) {
        std::string t;
        char b[16];
        switch (f) {
        case F::Rst: t = "599"; break;
        case F::Serial: snprintf(b, sizeof b, "%03d", std::max(1, serial)); t = b; break;
        case F::CqZone: snprintf(b, sizeof b, "%02d", s.cqZone); t = s.cqZone > 0 ? b : ""; break;
        case F::ItuZone: snprintf(b, sizeof b, "%02d", s.ituZone); t = s.ituZone > 0 ? b : ""; break;
        case F::State: t = up(s.myState); break;
        case F::StateOrSerial:
            if (!s.myState.empty()) t = up(s.myState);
            else { snprintf(b, sizeof b, "%03d", std::max(1, serial)); t = b; }
            break;
        case F::Name: t = up(s.myName); break;
        case F::Class: t = up(s.myClass); break;
        case F::Section: t = up(s.mySection); break;
        case F::Time: t = hhmm; break;
        case F::Power: t = up(s.power); break;
        case F::Grid: t = up(s.myGrid).substr(0, 4); break;
        }
        if (t.empty()) continue;
        if (d.cwCut) t = f == F::Rst ? "5NN" : cutNumbers(t);
        one += (one.empty() ? "" : " ") + t;
    }
    return twice ? one + " " + one : one;
}

std::vector<ContestMacro> contestMacros(const KeyerContestDef& d, const ContestStation& s, const std::string& call, const std::string& exch) {
    const std::string me = up(s.myCall), dx = up(call);
    const bool cw = d.cwCut;
    std::vector<ContestMacro> m;
    m.push_back({"CQ", cw ? "CQ TEST " + me + " " + me + " TEST" : "CQ TEST " + me + " " + me + " CQ\n"});
    m.push_back({"Exchange", cw ? dx + " " + exch : dx + " " + exch + " " + dx + "\n"});
    m.push_back({"TU", cw ? "TU " + me + " TEST" : "TU " + me + " TEST\n"});
    m.push_back({"My call", cw ? me : me + " " + me + "\n"});
    m.push_back({"Again?", cw ? "AGN?" : "AGN AGN?\n"});
    m.push_back({"Exch?", cw ? "NR?" : "NR? NR?\n"});
    return m;
}

bool contestParseExchange(const KeyerContestDef& d, const std::string& text, ContestExchange& out, std::string* why) {
    out.v.clear();
    std::vector<std::string> w;
    for (std::string t : words(text)) {
        if (hamCallValid(t) && !allDigits(t)) {
            // a call: what came before it is not the exchange (the decoded line often starts with "W1AW DE K1ABC")
            bool hasLetterAfterDigit = false;
            for (size_t i = 1; i < t.size(); i++) if (isdigit((unsigned char)t[i - 1]) && isalpha((unsigned char)t[i])) hasLetterAfterDigit = true;
            if (hasLetterAfterDigit) { w.clear(); continue; }
        }
        w.push_back(t);
    }
    size_t p = 0;
    for (F f : d.fields) {
        bool found = false;
        for (size_t k = p; k < w.size() && !found; k++) {
            const std::string raw = w[k];
            const std::string num = d.cwCut || f == F::Rst ? uncutNumbers(raw) : raw;
            std::string val;
            switch (f) {
            case F::Rst: if (num.size() == 3 && allDigits(num) && num[0] >= '1' && num[0] <= '5' && num[1] >= '1' && num[2] >= '1') val = num; break;
            case F::Serial: if (allDigits(num) && num.size() <= 5 && std::stoi(num) > 0) val = std::to_string(std::stoi(num)); break;
            case F::CqZone: if (allDigits(num) && num.size() <= 2 && std::stoi(num) >= 1 && std::stoi(num) <= 40) val = std::to_string(std::stoi(num)); break;
            case F::ItuZone: if (allDigits(num) && num.size() <= 2 && std::stoi(num) >= 1 && std::stoi(num) <= 90) val = std::to_string(std::stoi(num)); break;
            case F::State: if (isUsState(raw) || isVeProvince(raw)) val = raw; break;
            case F::StateOrSerial:
                if (isUsState(raw) || isVeProvince(raw)) val = raw;
                else if (allDigits(num) && num.size() <= 5 && std::stoi(num) > 0) val = std::to_string(std::stoi(num));
                break;
            case F::Name: if (!allDigits(raw) && raw.size() >= 2 && raw.size() <= 12 && !inList(raw, kNoise) && !isUsState(raw)) { bool alpha = true; for (char c : raw) alpha = alpha && isalpha((unsigned char)c); if (alpha) val = raw; } break;
            case F::Class: if (raw.size() >= 2 && raw.size() <= 3 && isdigit((unsigned char)raw[0]) && strchr("ABCDEF", raw.back())) val = raw; break;
            case F::Section: if (isArrlSection(raw)) val = raw; break;
            case F::Time: if (raw.size() == 4 && allDigits(raw) && std::stoi(raw.substr(0, 2)) < 24 && std::stoi(raw.substr(2)) < 60) val = raw; break;
            case F::Power: if (raw == "KW" || (allDigits(num) && std::stoi(num) > 0)) val = raw == "KW" ? raw : num; break;
            case F::Grid: if (hamGridValid(raw)) val = raw.substr(0, 4); break;
            }
            if (!val.empty()) { out.v.push_back({f, val}); p = k + 1; found = true; }
        }
        if (!found) {
            if (f == F::State && d.fields.size() == 3 && std::string(d.id) == "cqww-rtty") continue;   // only W / VE send it
            if (why) *why = std::string("no ") + contestFieldName(f) + " in the text";
            return false;
        }
    }
    return true;
}

bool ContestLog::dupe(const std::string& call, const std::string& band, const std::string& mode) const {
    const std::string c = up(call);
    for (const auto& q : q_) if (up(q.call) == c && q.band == band && q.mode == mode) return true;
    return false;
}

std::string ContestLog::cabrilloLine(const ContestQso& q, const std::string& myCall) const {
    char date[16] = "0000-00-00", hm[8] = "0000";
    const time_t t = (time_t)q.utc;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    strftime(date, sizeof date, "%Y-%m-%d", &g);
    strftime(hm, sizeof hm, "%H%M", &g);
    char b[256];
    snprintf(b, sizeof b, "QSO: %5ld %s %s %s %-13s %-10s %-13s %s", (long)(q.freqHz / 1000 + 0.5), q.mode.c_str(), date, hm, up(myCall).c_str(),
             q.sent.c_str(), up(q.call).c_str(), q.rcvd.c_str());
    std::string s = b;
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::string ContestLog::cabrillo(const ContestStation& s) const {
    std::string o = "START-OF-LOG: 3.0\n";
    o += "CREATED-BY: OnAir\n";
    o += std::string("CONTEST: ") + d_.cabrillo + "\n";
    o += "CALLSIGN: " + up(s.myCall) + "\n";
    if (!s.myGrid.empty()) o += "GRID-LOCATOR: " + up(s.myGrid) + "\n";
    for (const auto& q : q_) o += cabrilloLine(q, s.myCall) + "\n";
    o += "END-OF-LOG:\n";
    return o;
}

}  // namespace dect2::ham
