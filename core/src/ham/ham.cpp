// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/ham/ham.h"
#include "dect2/ham/ham_safety.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace dect2::ham {

namespace {
std::string up(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }
std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> v;
    std::istringstream is(s);
    std::string t;
    while (is >> t) v.push_back(t);
    return v;
}
std::string stripAngle(std::string s) {
    s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '<' || c == '>'; }), s.end());
    return s;
}
// days since 1970-01-01 of a civil date (Hinnant)
long daysFromCivil(long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}
void civilFromDays(long z, long& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}
void dateTime(double utc, std::string& date, std::string& tm) {
    const long t = (long)std::floor(utc);
    long days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    long y; unsigned m, d;
    civilFromDays(days, y, m, d);
    char a[16], b[16];
    snprintf(a, sizeof a, "%04ld%02u%02u", y, m, d);
    snprintf(b, sizeof b, "%02ld%02ld%02ld", rem / 3600, rem / 60 % 60, rem % 60);
    date = a; tm = b;
}
double utcFrom(const std::string& date, const std::string& tm) {
    if (date.size() != 8) return 0;
    const long y = atol(date.substr(0, 4).c_str());
    const unsigned m = (unsigned)atoi(date.substr(4, 2).c_str()), d = (unsigned)atoi(date.substr(6, 2).c_str());
    double s = (double)daysFromCivil(y, m, d) * 86400.0;
    if (tm.size() >= 4) s += atoi(tm.substr(0, 2).c_str()) * 3600 + atoi(tm.substr(2, 2).c_str()) * 60 + (tm.size() >= 6 ? atoi(tm.substr(4, 2).c_str()) : 0);
    return s;
}
void field(std::string& o, const char* name, const std::string& v) {
    if (v.empty()) return;
    o += "<" + std::string(name) + ":" + std::to_string(v.size()) + ">" + v + " ";
}
bool gridLike(const std::string& s) {
    return s.size() == 4 && s[0] >= 'A' && s[0] <= 'R' && s[1] >= 'A' && s[1] <= 'R' && isdigit((unsigned char)s[2]) && isdigit((unsigned char)s[3]);
}
}  // namespace

// ---- bands ----
const std::vector<HamBand>& hamBands() {
    static const std::vector<HamBand> b = {
        {"160m", 1.8e6, 2.0e6},      {"80m", 3.5e6, 4.0e6},        {"60m", 5.3515e6, 5.3665e6}, {"40m", 7.0e6, 7.3e6},
        {"30m", 10.1e6, 10.15e6},    {"20m", 14.0e6, 14.35e6},     {"17m", 18.068e6, 18.168e6}, {"15m", 21.0e6, 21.45e6},
        {"12m", 24.89e6, 24.99e6},   {"10m", 28.0e6, 29.7e6},      {"6m", 50.0e6, 54.0e6},      {"4m", 70.0e6, 70.5e6},
        {"2m", 144.0e6, 148.0e6},    {"70cm", 420.0e6, 450.0e6},   {"23cm", 1240.0e6, 1300.0e6}};
    return b;
}
const HamBand* hamBandAt(double hz) {
    for (const auto& b : hamBands()) if (hz >= b.loHz && hz <= b.hiHz) return &b;
    return nullptr;
}
bool hamSignalInBand(double dialHz, double audioHz, double bwHz) {
    const double lo = dialHz + audioHz, hi = lo + std::max(0.0, bwHz);
    const HamBand* b = hamBandAt(lo);
    return b && hi <= b->hiHz;
}
std::string bandNameFor(double hz) { const HamBand* b = hamBandAt(hz); return b ? b->name : ""; }
// 11 m is the CB band, not an amateur one. It is never in hamBands(), so hamSignalInBand is false for it; transmitting there shows a warning instead of being refused (3.2).
bool hamCbBand(double hz) { return hz >= 26.965e6 && hz <= 27.405e6; }
std::string rxBandNameFor(double hz) { return hamCbBand(hz) ? "11m" : bandNameFor(hz); }

bool hamCallValid(const std::string& callIn) {
    std::string c = up(callIn), core;
    size_t pos = 0;
    while (pos <= c.size()) {   // the longest part between slashes is the call
        const size_t e = c.find('/', pos);
        const std::string p = c.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        if (p.size() > core.size()) core = p;
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    if (core.size() < 3 || core.size() > 6) return false;
    for (char ch : core) if (!isalnum((unsigned char)ch)) return false;
    size_t d = core.find_last_of("0123456789");
    if (d == std::string::npos || d == 0 || d + 1 >= core.size() || core.size() - d - 1 > 3) return false;
    for (size_t i = d + 1; i < core.size(); i++) if (!isalpha((unsigned char)core[i])) return false;
    return true;
}
bool hamGridValid(const std::string& g0) {
    const std::string g = up(g0);
    if (g.size() != 4 && g.size() != 6) return false;
    if (!gridLike(g.substr(0, 4))) return false;
    if (g.size() == 6 && !(g[4] >= 'A' && g[4] <= 'X' && g[5] >= 'A' && g[5] <= 'X')) return false;
    return true;
}
std::vector<std::string> hamTxWarnings(const HamTxCheck& c) {
    std::vector<std::string> w;
    if (!c.licenceConfirmed) w.push_back("No amateur licence is set. You are responsible for being allowed to transmit.");
    if (!hamCallValid(c.call)) w.push_back("The callsign is empty or does not look valid. You are responsible for identifying your transmissions correctly.");
    if (!hamGridValid(c.grid)) w.push_back("The grid locator is empty or not valid.");
    if (std::isfinite(c.dialHz) && std::isfinite(c.audioHz) && std::isfinite(c.bwHz) && c.dialHz > 0) {
        char f[64];
        snprintf(f, sizeof f, "%.3f MHz", c.dialHz / 1e6);
        if (!hamSignalInBand(c.dialHz, c.audioHz, c.bwHz))
            w.push_back(std::string(f) + " is not an amateur band" + (hamCbBand(c.dialHz + c.audioHz) ? " (11 m is CB)" : "") + ". You are responsible for being allowed to transmit here.");
        else if (!c.country.empty()) {
            std::string why;
            if (!hamSignalInCountryBand(c.country, c.dialHz, c.audioHz, c.bwHz, why)) w.push_back(std::string(f) + ": " + why + ". You are responsible for being allowed to transmit here.");
        }
    }
    return w;
}
bool hamTxAllowed(const HamTxCheck& c, std::string& why) {
    why.clear();
    if (!c.armed) { why = "TX is not armed"; return false; }
    if (!std::isfinite(c.dialHz) || !std::isfinite(c.audioHz) || !std::isfinite(c.bwHz) || c.dialHz <= 0 || c.bwHz < 0) { why = "the frequency is not valid"; return false; }
    for (const auto& s : hamTxWarnings(c)) { if (!why.empty()) why += " "; why += s; }
    return true;
}

// ---- ADIF ----
std::string adifHeader() { return "OnAir ADIF export\n<ADIF_VER:5>3.1.4 <PROGRAMID:5>OnAir <EOH>\n"; }
std::string adifRecord(const QsoRecord& q) {
    std::string o, d1, t1, d2, t2;
    dateTime(q.startUtc, d1, t1);
    dateTime(q.endUtc > 0 ? q.endUtc : q.startUtc, d2, t2);
    field(o, "CALL", q.call);
    field(o, "GRIDSQUARE", q.grid);
    if (up(q.mode) == "FT4") { field(o, "MODE", "MFSK"); field(o, "SUBMODE", "FT4"); }
    else if (!q.submode.empty()) { field(o, "MODE", up(q.mode)); field(o, "SUBMODE", up(q.submode)); }
    else field(o, "MODE", up(q.mode));
    field(o, "BAND", bandNameFor(q.freqHz));
    char f[32];
    snprintf(f, sizeof f, "%.6f", q.freqHz / 1e6);
    field(o, "FREQ", f);
    field(o, "RST_SENT", q.rstSent);
    field(o, "RST_RCVD", q.rstRcvd);
    field(o, "QSO_DATE", d1);
    field(o, "TIME_ON", t1);
    field(o, "QSO_DATE_OFF", d2);
    field(o, "TIME_OFF", t2);
    field(o, "STATION_CALLSIGN", q.myCall);
    field(o, "MY_GRIDSQUARE", q.myGrid);
    return o + "<EOR>\n";
}
bool adifAppend(const std::string& path, const QsoRecord& q, std::string& err) {
    err.clear();
    bool isNew = false;
    { std::ifstream t(path, std::ios::binary); isNew = !t.good() || t.peek() == std::ifstream::traits_type::eof(); }
    std::ofstream o(path, std::ios::binary | std::ios::app);
    if (!o) { err = "cannot write " + path; return false; }
    if (isNew) o << adifHeader();
    o << adifRecord(q);
    o.flush();
    if (!o) { err = "write failed: " + path; return false; }
    return true;
}
std::vector<QsoRecord> adifParse(const std::string& s) {
    std::vector<QsoRecord> out;
    QsoRecord cur;
    std::string date, tm, sub;
    bool any = false, inHeader = true;
    size_t i = 0;
    if (up(s).find("<EOH>") == std::string::npos) inHeader = false;
    while (i < s.size()) {
        const size_t lt = s.find('<', i);
        if (lt == std::string::npos) break;
        const size_t gt = s.find('>', lt);
        if (gt == std::string::npos) break;
        const std::string spec = s.substr(lt + 1, gt - lt - 1);
        const size_t c1 = spec.find(':');
        const std::string name = up(spec.substr(0, c1));
        i = gt + 1;
        if (name == "EOH") { inHeader = false; cur = QsoRecord(); any = false; continue; }
        if (name == "EOR") {
            if (any) {
                if (!sub.empty()) cur.mode = sub;
                cur.startUtc = utcFrom(date, tm);
                cur.endUtc = cur.startUtc;
                out.push_back(cur);
            }
            cur = QsoRecord(); date.clear(); tm.clear(); sub.clear(); any = false;
            continue;
        }
        if (c1 == std::string::npos || inHeader) continue;
        const size_t c2 = spec.find(':', c1 + 1);
        const long len = atol(spec.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1).c_str());
        if (len < 0 || i + (size_t)len > s.size()) break;
        const std::string v = s.substr(i, (size_t)len);
        i += (size_t)len;
        any = true;
        if (name == "CALL") cur.call = up(v);
        else if (name == "GRIDSQUARE") cur.grid = up(v);
        else if (name == "MODE") cur.mode = up(v);
        else if (name == "SUBMODE") sub = up(v);
        else if (name == "FREQ") cur.freqHz = atof(v.c_str()) * 1e6;
        else if (name == "RST_SENT") cur.rstSent = v;
        else if (name == "RST_RCVD") cur.rstRcvd = v;
        else if (name == "QSO_DATE") date = v;
        else if (name == "TIME_ON") tm = v;
        else if (name == "STATION_CALLSIGN") cur.myCall = up(v);
        else if (name == "MY_GRIDSQUARE") cur.myGrid = up(v);
    }
    return out;
}
void WorkedLog::add(const QsoRecord& q) { keys_.push_back(up(q.call) + "|" + bandNameFor(q.freqHz) + "|" + up(q.mode)); }
bool WorkedLog::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    for (const auto& q : adifParse(ss.str())) add(q);
    return true;
}
bool WorkedLog::worked(const std::string& call) const {
    const std::string p = up(call) + "|";
    for (const auto& k : keys_) if (k.compare(0, p.size(), p) == 0) return true;
    return false;
}
bool WorkedLog::workedOn(const std::string& call, const std::string& band, const std::string& mode) const {
    const std::string k = up(call) + "|" + band + "|" + up(mode);
    return std::find(keys_.begin(), keys_.end(), k) != keys_.end();
}

// ---- timing ----
double slotPeriod(int mode) {
    switch (mode) { case 1: return 7.5; case 2: return 3.75; case 3: return 120.0; case 4: case 5: case 6: case 7: case 10: return 60.0; default: return 15.0; }   // Q65 / FST4: their 60 s default period
}
const char* hamModeName(int mode) {
    switch (mode) { case 0: return "FT8"; case 1: return "FT4"; case 2: return "FT2"; case 3: return "WSPR"; case 4: return "JT65"; case 5: return "JT9"; case 6: return "Q65"; case 7: return "FST4"; case 8: return "MSK144"; case 10: return "JT4"; default: return "FT8"; }
}
bool hamModeHasQso(int mode) { return mode == 0 || mode == 1 || mode == 2 || mode == 4 || mode == 5 || mode == 6 || mode == 7 || mode == 8 || mode == 10; }
bool hamModeUsesRRR(int mode) { return mode == 4 || mode == 5 || mode == 10; }
double hamModeBwHz(int mode, int sub) {
    switch (mode) {
    case 0: return 50;
    case 1: return 90;
    case 2: return 170;
    case 3: return 6;
    case 4: return sub == 1 ? 360 : sub == 2 ? 720 : 180;   // JT65 A 177.6 Hz, B 355, C 710 (tone spacing 2.7 Hz x 1/2/4, 65 tones + sync)
    case 5: return 16;
    case 10: { static const double a[] = {18, 36, 72, 144, 288, 576, 1152}; return a[sub < 0 ? 0 : sub > 6 ? 6 : sub]; }   // 4 tones, spacing 4.375 Hz x 1 .. 64
    case 6: return 110.0 * (1 << (sub < 0 ? 0 : sub > 4 ? 4 : sub));   // Q65-60: 65 tones of 1.67 Hz x 1 .. 16 (A .. E)
    case 7: return 50;     // FST4-60: 4-GFSK, about 13 Hz; 50 Hz leaves room for the shorter periods
    case 8: return 2400;   // MSK144: 2000 Bd MSK
    default: return 50;
    }
}
long slotIndex(double utc, double period) { return (long)std::floor(utc / period + 1e-9); }
bool slotEven(double utc, double period) { return (slotIndex(utc, period) & 1) == 0; }
double slotStart(long index, double period) { return (double)index * period; }
double txStartDelay(int mode) { return mode == 2 ? 0.2 : (mode == 3 || mode == 4 || mode == 5 || mode == 6 || mode == 7 || mode == 10) ? 1.0 : 0.5; }

// ---- messages ----
std::string reportText(int snr) {
    snr = std::max(-30, std::min(32, snr));
    char b[8];
    snprintf(b, sizeof b, "%+03d", snr);
    return b;
}
std::string msgCq(const std::string& me, const std::string& grid) { return "CQ " + up(me) + (grid.empty() ? "" : " " + up(grid).substr(0, 4)); }
std::string msgCqMod(const std::string& mod, const std::string& me, const std::string& grid) {
    std::string m;
    for (char c : up(mod)) if (isalpha((unsigned char)c)) m += c;
    if (m.size() > 4) m = m.substr(0, 4);
    return m.empty() ? msgCq(me, grid) : "CQ " + m + " " + up(me) + (grid.empty() ? "" : " " + up(grid).substr(0, 4));
}
std::string msgGrid(const std::string& to, const std::string& me, const std::string& grid) { return up(to) + " " + up(me) + " " + up(grid).substr(0, 4); }
std::string msgReport(const std::string& to, const std::string& me, int snr, bool roger) { return up(to) + " " + up(me) + " " + (roger ? "R" : "") + reportText(snr); }
std::string msgRR73(const std::string& to, const std::string& me) { return up(to) + " " + up(me) + " RR73"; }
std::string msgRRR(const std::string& to, const std::string& me) { return up(to) + " " + up(me) + " RRR"; }
std::string msg73(const std::string& to, const std::string& me) { return up(to) + " " + up(me) + " 73"; }
std::string msgFree(const std::string& t) {
    std::string o;
    for (char c : up(t)) if (isalnum((unsigned char)c) || c == ' ' || c == '+' || c == '-' || c == '.' || c == '/' || c == '?') o += c;
    return o.substr(0, 13);
}

std::string msgExch(const std::string& to, const std::string& me, const std::string& exch, bool roger) {
    std::string e;
    for (const auto& w : split(up(exch))) e += (e.empty() ? "" : " ") + w;
    return up(to) + " " + up(me) + (roger ? " R " : " ") + e;
}
namespace {
// the contest forms (4.0): [TU;] TO FROM [R] 6A WI (Field Day) / [TU;] TO FROM [R] 579 WI|0012 (RTTY Roundup)
bool parseContest(std::vector<std::string> t, ParsedMsg& p) {
    if (!t.empty() && t[0] == "TU;") t.erase(t.begin());
    if (t.size() != 4 && t.size() != 5) return false;
    const bool r = t.size() == 5;
    if (r && t[2] != "R") return false;
    const std::string& e1 = t[r ? 3 : 2];
    const std::string& e2 = t[r ? 4 : 3];
    auto digits = [](const std::string& x) { return !x.empty() && std::all_of(x.begin(), x.end(), [](char c) { return isdigit((unsigned char)c) != 0; }); };
    int contest = 0;
    if (e1.size() >= 2 && e1.size() <= 3 && e1.back() >= 'A' && e1.back() <= 'F' && digits(e1.substr(0, e1.size() - 1)) &&
        e2.size() >= 2 && e2.size() <= 3 && std::all_of(e2.begin(), e2.end(), [](char c) { return isalpha((unsigned char)c) != 0; })) contest = 1;
    else if (e1.size() == 3 && e1[0] == '5' && e1[2] == '9' && e1[1] >= '2' && e1[1] <= '9' && !e2.empty() && e2.size() <= 4) contest = 2;
    if (!contest) return false;
    p.to = stripAngle(t[0]);
    p.from = stripAngle(t[1]);
    p.kind = r ? ParsedMsg::RExch : ParsedMsg::Exch;
    p.exch = e1 + " " + e2;
    p.contest = contest;
    return true;
}
}  // namespace

ParsedMsg parseMessage(const std::string& msg) {
    ParsedMsg p;
    const auto t = split(up(msg));
    {
        ParsedMsg c;
        if (parseContest(t, c)) return c;
    }
    if (t.size() >= 2 && t[0] == "CQ") {
        p.kind = ParsedMsg::Cq;
        size_t k = 1;
        if (t.size() >= 4 && t[1].size() <= 4 && std::all_of(t[1].begin(), t[1].end(), [](char c) { return isalpha((unsigned char)c); }) && !hamCallValid(t[1])) k = 2;
        p.from = stripAngle(t[k]);
        if (k + 1 < t.size() && gridLike(t[k + 1])) p.grid = t[k + 1];
        return p;
    }
    if (t.size() < 3) return p;
    p.to = stripAngle(t[0]);
    p.from = stripAngle(t[1]);
    const std::string& x = t[2];
    if (x == "RR73") p.kind = ParsedMsg::RR73;
    else if (x == "RRR") p.kind = ParsedMsg::Rrr;
    else if (x == "73") p.kind = ParsedMsg::Bye73;
    else if (gridLike(x)) { p.kind = ParsedMsg::Grid; p.grid = x; }
    else {
        const bool r = x.size() > 1 && x[0] == 'R';
        const std::string n = r ? x.substr(1) : x;
        if (n.size() >= 2 && (n[0] == '+' || n[0] == '-') && std::all_of(n.begin() + 1, n.end(), [](char c) { return isdigit((unsigned char)c); })) {
            p.kind = r ? ParsedMsg::RReport : ParsedMsg::Report;
            p.report = atoi(n.c_str());
        }
    }
    return p;
}

// ---- engine ----
void QsoEngine::startCq(double now) {
    s_ = Cq; resumeCq_ = true; dx_.clear(); sentCount_ = 0; wd_ = false; lastUser_ = now; nowUtc_ = now;
}
void QsoEngine::callStation(double now, const std::string& call, const std::string& grid, int snr, double theirSlotUtc, double theirHz) {
    if (up(call) == up(c_.myCall) || call.empty()) return;
    dx_ = up(call); dxGrid_ = up(grid); dxSnr_ = snr; rstRcvd_.clear();
    s_ = Tx1Grid; resumeCq_ = false; sentCount_ = 0; wd_ = false; logged_ = false;
    lastUser_ = now; nowUtc_ = now; startUtc_ = now;
    c_.txEven = !slotEven(theirSlotUtc, slotPeriod(c_.mode));
    if (!c_.hold && theirHz > 0) c_.txHz = theirHz;
}
void QsoEngine::halt() { s_ = Idle; dx_.clear(); sentCount_ = 0; resumeCq_ = false; lastDx_.clear(); lastDxUntil_ = 0; }
void QsoEngine::finish(double slotUtc) {
    // the QSO is over: remember the station for a few slots, so a repeated R-report (our RR73 was not copied) gets the RR73 again
    lastDx_ = dx_; lastDxUntil_ = slotUtc + 4 * slotPeriod(c_.mode);
    s_ = resumeCq_ ? Cq : Idle;
    dx_.clear(); sentCount_ = 0;
}

void QsoEngine::logQso(double now) {
    if (logged_) return;
    logged_ = true;
    QsoRecord q;
    q.call = dx_; q.grid = dxGrid_; q.mode = hamModeName(c_.mode);
    q.rstSent = reportText(dxSnr_); q.rstRcvd = rstRcvd_;
    q.myCall = up(c_.myCall); q.myGrid = up(c_.myGrid);
    q.startUtc = startUtc_ > 0 ? startUtc_ : now; q.endUtc = now;
    if (c_.contest) {   // the exchange: Field Day has no RST, RTTY Roundup starts with one
        auto splitRst = [this](const std::string& e, std::string& rst, std::string& rest) {
            const auto w = split(up(e));
            rst.clear(); rest.clear();
            for (size_t i = 0; i < w.size(); i++) {
                if (i == 0 && c_.contest == 2) { rst = w[0]; continue; }
                rest += (rest.empty() ? "" : " ") + w[i];
            }
        };
        splitRst(c_.myExch, q.rstSent, q.stx);
        splitRst(rcvdExch_, q.rstRcvd, q.srx);
        q.contestId = c_.contest == 1 ? "ARRL-FD" : "ARRL-RTTY";
    }
    if (onLog) onLog(q);
}

void QsoEngine::onDecode(double now, const std::string& msg, int snr, double slotUtc, double hz) {
    nowUtc_ = now;
    if (!c_.autoSeq) return;
    const ParsedMsg m = parseMessage(msg);
    if (m.to != up(c_.myCall) || m.from.empty()) return;
    using K = ParsedMsg;
    // a station whose QSO we just finished repeats its R-report: it did not copy our RR73 (WSJT-X sends it again; logged once)
    if ((s_ == Cq || s_ == Idle) && !lastDx_.empty() && m.from == lastDx_ && (m.kind == K::RReport || m.kind == K::RExch) && slotUtc <= lastDxUntil_) {
        dx_ = lastDx_; s_ = Tx4RR73; sentCount_ = 0; lastDx_.clear();
        c_.txEven = !slotEven(slotUtc, slotPeriod(c_.mode));
        return;
    }
    if (s_ == Idle) return;
    // CQ: several callers in the same slot; a better one (the priority hook: needed entity first ...) replaces the first before we answered it
    if (resumeCq_ && (s_ == Tx2Report || s_ == Tx3RReport) && sentCount_ == 0 && slotUtc == pickSlot_ && m.from != dx_ && priority &&
        (m.kind == K::Grid || m.kind == K::Report) && priority(m.from) > pickPri_) {
        s_ = Cq;
    }
    auto rep = [&](bool r) { rstRcvd_ = reportText(m.report); (void)r; };
    auto done = [&] {
        if (resumeCq_) { s_ = Cq; } else s_ = Idle;
        dx_.clear(); sentCount_ = 0;
    };
    if (s_ == Cq && c_.contest && m.kind == K::Exch) {   // contest: the caller sends the exchange at once; we answer R + ours
        dx_ = m.from; dxGrid_.clear(); dxSnr_ = snr; sentCount_ = 0; logged_ = false; startUtc_ = now; rcvdExch_ = m.exch; rstRcvd_.clear();
        pickSlot_ = slotUtc; pickPri_ = priority ? priority(m.from) : 0; lastDx_.clear();
        c_.txEven = !slotEven(slotUtc, slotPeriod(c_.mode));
        s_ = Tx3RReport;
        return;
    }
    if (s_ == Cq) {
        if (m.kind == K::Grid || m.kind == K::Report) {
            dx_ = m.from; dxGrid_ = m.grid; dxSnr_ = snr; sentCount_ = 0; logged_ = false; startUtc_ = now; rstRcvd_.clear();
            pickSlot_ = slotUtc; pickPri_ = priority ? priority(m.from) : 0; lastDx_.clear();
            c_.txEven = !slotEven(slotUtc, slotPeriod(c_.mode));
            (void)hz;
            if (m.kind == K::Grid) s_ = Tx2Report;
            else { rep(false); s_ = Tx3RReport; }
        }
        return;
    }
    if (m.from != dx_) return;
    if (c_.contest) {
        if (s_ == Tx1Grid && (m.kind == K::RExch || m.kind == K::Exch)) { rcvdExch_ = m.exch; dxSnr_ = snr; logQso(now); s_ = Tx4RR73; sentCount_ = 0; return; }
        if (s_ == Tx3RReport && (m.kind == K::RR73 || m.kind == K::Rrr || m.kind == K::Bye73)) { logQso(now); finish(slotUtc); return; }
        if (s_ == Tx3RReport && m.kind == K::Exch) { rcvdExch_ = m.exch; sentCount_ = 0; return; }   // they repeat: ours was not copied
    }
    switch (s_) {
    case Tx1Grid:
        if (m.kind == K::Report) { dxSnr_ = snr; rep(false); s_ = Tx3RReport; sentCount_ = 0; }
        else if (m.kind == K::RReport) { dxSnr_ = snr; rep(true); logQso(now); s_ = Tx4RR73; sentCount_ = 0; }
        else if (m.kind == K::RR73 || m.kind == K::Rrr) { logQso(now); s_ = Tx5Bye; sentCount_ = 0; }
        else if (m.kind == K::Bye73) { logQso(now); done(); }
        break;
    case Tx2Report:
        if (m.kind == K::RReport) { rep(true); logQso(now); s_ = Tx4RR73; sentCount_ = 0; }
        else if (m.kind == K::Grid) sentCount_ = 0;   // they call again: our report was not copied, keep sending it
        else if (m.kind == K::RR73 || m.kind == K::Rrr || m.kind == K::Bye73) { logQso(now); done(); }
        break;
    case Tx3RReport:
        if (m.kind == K::RR73 || m.kind == K::Rrr) { logQso(now); s_ = Tx5Bye; sentCount_ = 0; }
        else if (m.kind == K::Report) { dxSnr_ = snr; rep(false); sentCount_ = 0; }   // they repeat their report: our R-report was not copied
        else if (m.kind == K::Bye73) { logQso(now); done(); }
        break;
    case Tx4RR73:
        if (m.kind == K::Bye73) done();
        else if (m.kind == K::RReport) sentCount_ = 0;   // they did not get our RR73: send it again
        break;
    default: break;
    }
}

std::string QsoEngine::messageForSlot(double slotUtc) {
    if (s_ == Idle) return "";
    if (nowUtc_ < slotUtc) nowUtc_ = slotUtc;
    if (c_.watchdogSec > 0 && nowUtc_ - lastUser_ > c_.watchdogSec) { halt(); wd_ = true; note("watchdog"); return ""; }
    if (slotEven(slotUtc, slotPeriod(c_.mode)) != c_.txEven) return "";
    const std::string& me = c_.myCall;
    if (s_ != Cq && sentCount_ >= c_.retryLimit) {
        note("retries");
        if (resumeCq_) { s_ = Cq; dx_.clear(); sentCount_ = 0; return msgCqMod(c_.cqMod, me, c_.myGrid); }
        halt();
        return "";
    }
    switch (s_) {
    case Cq: return msgCqMod(c_.cqMod, me, c_.myGrid);
    case Tx1Grid: return c_.contest ? msgExch(dx_, me, c_.myExch, false) : msgGrid(dx_, me, c_.myGrid);
    case Tx2Report: return msgReport(dx_, me, dxSnr_, false);
    case Tx3RReport: return c_.contest ? msgExch(dx_, me, c_.myExch, true) : msgReport(dx_, me, dxSnr_, true);
    case Tx4RR73: return hamModeUsesRRR(c_.mode) ? msgRRR(dx_, me) : msgRR73(dx_, me);
    case Tx5Bye: return msg73(dx_, me);
    default: return "";
    }
}

void QsoEngine::txSent(double slotUtc) {
    (void)slotUtc;
    if (s_ == Tx5Bye) { finish(slotUtc); return; }
    // RR73 needs no answer (WSJT-X): one is enough, then CQ again (or idle). RRR (JT65 / JT9 / JT4) waits for the 73.
    if (s_ == Tx4RR73 && !hamModeUsesRRR(c_.mode)) { finish(slotUtc); return; }
    if (s_ != Cq) sentCount_++;
}

}  // namespace dect2::ham
