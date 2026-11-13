// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Ham logbook: see ham_log.h.
#include "dect2/ham/ham_log.h"
#include "dect2/ham/ham.h"
#include "dect2/geo/maidenhead.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

namespace dect2::ham {

namespace {
std::string upper(std::string s) { for (char& c : s) c = (char)std::toupper((unsigned char)c); return s; }
std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
bool yes(const std::string& v) { return !v.empty() && (v[0] == 'Y' || v[0] == 'y' || v[0] == 'V' || v[0] == 'v'); }   // ADIF Y, V (verified)
std::string grid4(const std::string& g) { return g.size() >= 4 ? upper(g.substr(0, 4)) : std::string(); }
// days since 1970-01-01 for a civil date (no time zone, no library)
long long daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}
void civil(double utc, int& y, int& mo, int& d, int& h, int& mi, int& s) {
    const long long t = (long long)std::floor(utc);
    long long z = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    const long long secs = t - z * 86400;
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    mo = (int)(mp < 10 ? mp + 3 : mp - 9);
    y = (int)(yoe + era * 400 + (mo <= 2));
    h = (int)(secs / 3600); mi = (int)(secs / 60 % 60); s = (int)(secs % 60);
}
void field(std::string& o, const char* name, const std::string& v) {
    if (v.empty()) return;
    o += "<"; o += name; o += ":" + std::to_string(v.size()) + ">" + v + " ";
}
const char* kKnown[] = {"CALL", "GRIDSQUARE", "MODE", "SUBMODE", "BAND", "FREQ", "QSO_DATE", "TIME_ON", "QSO_DATE_OFF", "TIME_OFF", "RST_SENT",
                        "RST_RCVD", "NAME", "QTH", "COMMENT", "STATE", "COUNTRY", "CONT", "STATION_CALLSIGN", "OPERATOR", "MY_GRIDSQUARE", "TX_PWR",
                        "DXCC", "CQZ", "ITUZ", "QSL_SENT", "QSL_RCVD", "LOTW_QSL_RCVD", "STX_STRING", "SRX_STRING", "STX", "SRX", "CONTEST_ID"};
bool known(const std::string& k) { for (const char* n : kKnown) if (k == n) return true; return false; }
}  // namespace

std::string adifDate(double utc) { int y, mo, d, h, mi, s; civil(utc, y, mo, d, h, mi, s); char b[16]; snprintf(b, sizeof b, "%04d%02d%02d", y, mo, d); return b; }
std::string adifTime(double utc) { int y, mo, d, h, mi, s; civil(utc, y, mo, d, h, mi, s); char b[16]; snprintf(b, sizeof b, "%02d%02d%02d", h, mi, s); return b; }
double adifUtc(const std::string& date, const std::string& tm) {
    if (date.size() < 8) return 0;
    for (int i = 0; i < 8; i++) if (!std::isdigit((unsigned char)date[(size_t)i])) return 0;
    const int y = std::atoi(date.substr(0, 4).c_str()), mo = std::atoi(date.substr(4, 2).c_str()), d = std::atoi(date.substr(6, 2).c_str());
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    int h = 0, mi = 0, s = 0;
    if (tm.size() >= 4) { h = std::atoi(tm.substr(0, 2).c_str()); mi = std::atoi(tm.substr(2, 2).c_str()); }
    if (tm.size() >= 6) s = std::atoi(tm.substr(4, 2).c_str());
    return (double)(daysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + s);
}

std::string modeClass(const std::string& m) {
    const std::string u = upper(m);
    if (u == "CW") return "CW";
    if (u == "SSB" || u == "USB" || u == "LSB" || u == "AM" || u == "FM" || u == "PH" || u == "PHONE" || u == "DIGITALVOICE" || u == "DSTAR" ||
        u == "C4FM" || u == "DMR" || u == "FREEDV" || u == "M17") return "PHONE";
    return u.empty() ? "" : "DIGI";
}

std::vector<AdifFields> adifReadRecords(const std::string& s) {
    std::vector<AdifFields> out;
    AdifFields cur;
    size_t i = 0;
    // a file without a header starts with a tag; one with a header has text first and <EOH> somewhere
    {
        const std::string u = upper(s.substr(0, std::min<size_t>(s.size(), 1 << 16)));
        const size_t eoh = u.find("<EOH>");
        if (!s.empty() && s[0] != '<' && eoh != std::string::npos) i = eoh + 5;
        else if (eoh != std::string::npos && u.find("<CALL") > eoh) i = eoh + 5;
    }
    while (i < s.size()) {
        const size_t lt = s.find('<', i);
        if (lt == std::string::npos) break;
        const size_t gt = s.find('>', lt);
        if (gt == std::string::npos) break;
        std::string tag = s.substr(lt + 1, gt - lt - 1);
        i = gt + 1;
        const size_t c1 = tag.find(':');
        std::string name = upper(c1 == std::string::npos ? tag : tag.substr(0, c1));
        if (name == "EOR") { if (!cur.empty()) out.push_back(cur); cur.clear(); continue; }
        if (name == "EOH") { cur.clear(); continue; }
        if (c1 == std::string::npos) continue;
        const std::string rest = tag.substr(c1 + 1);
        const long len = std::strtol(rest.c_str(), nullptr, 10);
        if (len < 0 || i + (size_t)len > s.size()) break;
        cur[name] = s.substr(i, (size_t)len);
        i += (size_t)len;
    }
    if (!cur.empty() && cur.count("CALL")) out.push_back(cur);   // a last record without <EOR>
    return out;
}

LogQso adifToQso(const AdifFields& f) {
    LogQso q;
    auto g = [&f](const char* k) { auto it = f.find(k); return it == f.end() ? std::string() : it->second; };
    q.call = upper(g("CALL")); q.grid = upper(g("GRIDSQUARE")); q.mode = upper(g("MODE")); q.submode = upper(g("SUBMODE"));
    q.band = lower(g("BAND"));
    const std::string fr = g("FREQ");
    if (!fr.empty()) q.freqHz = std::atof(fr.c_str()) * 1e6;
    if (q.band.empty() && q.freqHz > 0) q.band = bandNameFor(q.freqHz);
    q.startUtc = adifUtc(g("QSO_DATE"), g("TIME_ON"));
    const std::string doff = g("QSO_DATE_OFF").empty() ? g("QSO_DATE") : g("QSO_DATE_OFF");
    q.endUtc = g("TIME_OFF").empty() ? q.startUtc : adifUtc(doff, g("TIME_OFF"));
    if (q.endUtc < q.startUtc && q.endUtc > 0) q.endUtc += 86400;   // past midnight with no QSO_DATE_OFF
    q.rstSent = g("RST_SENT"); q.rstRcvd = g("RST_RCVD"); q.name = g("NAME"); q.qth = g("QTH"); q.comment = g("COMMENT");
    q.state = upper(g("STATE")); q.country = g("COUNTRY"); q.cont = upper(g("CONT"));
    q.myCall = upper(g("STATION_CALLSIGN").empty() ? g("OPERATOR") : g("STATION_CALLSIGN"));
    q.myGrid = upper(g("MY_GRIDSQUARE")); q.txPwr = g("TX_PWR");
    q.dxcc = std::atoi(g("DXCC").c_str()); q.cqz = std::atoi(g("CQZ").c_str()); q.ituz = std::atoi(g("ITUZ").c_str());
    q.qslSent = yes(g("QSL_SENT")); q.qslRcvd = yes(g("QSL_RCVD")); q.lotwRcvd = yes(g("LOTW_QSL_RCVD"));
    q.stx = g("STX_STRING").empty() ? g("STX") : g("STX_STRING");
    q.srx = g("SRX_STRING").empty() ? g("SRX") : g("SRX_STRING");
    q.contestId = g("CONTEST_ID");
    for (const auto& kv : f) if (!known(kv.first)) q.extra[kv.first] = kv.second;
    if (f.count("OPERATOR") && f.count("STATION_CALLSIGN")) q.extra["OPERATOR"] = g("OPERATOR");
    return q;
}

std::string qsoToAdif(const LogQso& q) {
    std::string o;
    field(o, "CALL", q.call);
    field(o, "GRIDSQUARE", q.grid);
    field(o, "MODE", q.mode);
    field(o, "SUBMODE", q.submode);
    field(o, "BAND", q.band.empty() ? bandNameFor(q.freqHz) : q.band);
    if (q.freqHz > 0) { char b[32]; snprintf(b, sizeof b, "%.6f", q.freqHz / 1e6); field(o, "FREQ", b); }
    if (q.startUtc > 0) { field(o, "QSO_DATE", adifDate(q.startUtc)); field(o, "TIME_ON", adifTime(q.startUtc)); }
    if (q.endUtc > 0) { field(o, "QSO_DATE_OFF", adifDate(q.endUtc)); field(o, "TIME_OFF", adifTime(q.endUtc)); }
    field(o, "RST_SENT", q.rstSent); field(o, "RST_RCVD", q.rstRcvd);
    field(o, "NAME", q.name); field(o, "QTH", q.qth); field(o, "COMMENT", q.comment);
    field(o, "STATE", q.state); field(o, "COUNTRY", q.country); field(o, "CONT", q.cont);
    if (q.dxcc > 0) field(o, "DXCC", std::to_string(q.dxcc));
    if (q.cqz > 0) field(o, "CQZ", std::to_string(q.cqz));
    if (q.ituz > 0) field(o, "ITUZ", std::to_string(q.ituz));
    field(o, "STATION_CALLSIGN", q.myCall); field(o, "MY_GRIDSQUARE", q.myGrid); field(o, "TX_PWR", q.txPwr);
    if (q.qslSent) field(o, "QSL_SENT", "Y");
    if (q.qslRcvd) field(o, "QSL_RCVD", "Y");
    if (q.lotwRcvd) field(o, "LOTW_QSL_RCVD", "Y");
    field(o, "STX_STRING", q.stx); field(o, "SRX_STRING", q.srx); field(o, "CONTEST_ID", q.contestId);
    for (const auto& kv : q.extra) field(o, kv.first.c_str(), kv.second);
    o += "<EOR>\n";
    return o;
}

std::string baseCall(const std::string& call) {
    const std::string c = upper(call);
    std::string best;
    size_t p = 0;
    while (p <= c.size()) {
        size_t e = c.find('/', p);
        if (e == std::string::npos) e = c.size();
        const std::string part = c.substr(p, e - p);
        bool dig = false, alpha = false;
        for (char ch : part) { dig |= (bool)std::isdigit((unsigned char)ch); alpha |= (bool)std::isalpha((unsigned char)ch); }
        if (dig && alpha && part.size() > best.size()) best = part;
        p = e + 1;
    }
    return best.empty() ? c : best;
}

// ---- Logbook ----
const LogQso* Logbook::find(long long id) const { const int i = indexOf(id); return i < 0 ? nullptr : &q_[(size_t)i]; }
int Logbook::indexOf(long long id) const {
    for (size_t i = 0; i < q_.size(); i++) if (q_[i].id == id) return (int)i;
    return -1;
}
namespace {
void tidy(LogQso& q) {
    q.call = upper(q.call); q.grid = upper(q.grid); q.mode = upper(q.mode); q.state = upper(q.state);
    if (q.band.empty() && q.freqHz > 0) q.band = bandNameFor(q.freqHz);
    q.band = lower(q.band);
    if (q.endUtc <= 0) q.endUtc = q.startUtc;
}
}  // namespace

void Logbook::push(Step s) { undo_.push_back(std::move(s)); if (undo_.size() > 200) undo_.erase(undo_.begin()); redo_.clear(); }

long long Logbook::add(LogQso q) {
    tidy(q);
    if (enrich) enrich(q);
    q.id = nextId_++;
    q_.push_back(q);
    Step s; s.kind = 0; s.after.push_back({(int)q_.size() - 1, q}); s.label = "add " + q.call;
    push(std::move(s));
    return q.id;
}
bool Logbook::update(const LogQso& in) {
    const int i = indexOf(in.id);
    if (i < 0) return false;
    LogQso q = in;
    tidy(q);
    Step s; s.kind = 1; s.before.push_back({i, q_[(size_t)i]}); s.after.push_back({i, q}); s.label = "edit " + q.call;
    q_[(size_t)i] = q;
    push(std::move(s));
    return true;
}
bool Logbook::remove(long long id) {
    const int i = indexOf(id);
    if (i < 0) return false;
    Step s; s.kind = 2; s.before.push_back({i, q_[(size_t)i]}); s.label = "delete " + q_[(size_t)i].call;
    q_.erase(q_.begin() + i);
    push(std::move(s));
    return true;
}
// forward: remove `before` entries (by id), then put `after` at their indices; back: the other way round
void Logbook::apply(const Step& s, bool forward) {
    const auto& out = forward ? s.before : s.after;
    const auto& in = forward ? s.after : s.before;
    std::set<long long> gone;
    for (const auto& e : out) gone.insert(e.second.id);
    if (!gone.empty()) q_.erase(std::remove_if(q_.begin(), q_.end(), [&gone](const LogQso& q) { return gone.count(q.id) > 0; }), q_.end());
    for (const auto& e : in) {
        const size_t at = std::min((size_t)std::max(0, e.first), q_.size());
        q_.insert(q_.begin() + (long)at, e.second);
    }
}
bool Logbook::undo() {
    if (undo_.empty()) return false;
    Step s = undo_.back(); undo_.pop_back();
    // remove in reverse index order is handled by id lookups; insert the old ones in ascending index order
    std::sort(s.before.begin(), s.before.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    apply(s, false);
    redo_.push_back(std::move(s));
    return true;
}
bool Logbook::redo() {
    if (redo_.empty()) return false;
    Step s = redo_.back(); redo_.pop_back();
    apply(s, true);
    undo_.push_back(std::move(s));
    return true;
}
std::string Logbook::undoLabel() const { return undo_.empty() ? "" : "Undo " + undo_.back().label; }
std::string Logbook::redoLabel() const { return redo_.empty() ? "" : "Redo " + redo_.back().label; }

int Logbook::findDupe(const std::string& call, const std::string& band, const std::string& mode, bool perBand, bool perMode, double utc, double withinSec,
                      long long exceptId) const {
    const std::string bc = baseCall(call), b = lower(band), m = upper(mode), mc = modeClass(mode);
    for (size_t i = q_.size(); i-- > 0;) {
        const LogQso& q = q_[i];
        if (q.id == exceptId && exceptId != 0) continue;
        if (baseCall(q.call) != bc) continue;
        if (perBand && q.band != b) continue;
        if (perMode && q.mode != m && !(modeClass(q.mode) == mc && mc != "DIGI")) continue;   // SSB = USB = LSB; FT8 is not FT4
        if (withinSec > 0 && std::fabs(q.startUtc - utc) > withinSec) continue;
        return (int)i;
    }
    return -1;
}

std::vector<size_t> Logbook::search(const std::string& query) const {
    std::vector<std::string> words;
    {
        std::istringstream is(query);
        std::string w;
        while (is >> w) words.push_back(upper(w));
    }
    std::vector<size_t> out;
    for (size_t i = q_.size(); i-- > 0;) {
        const LogQso& q = q_[i];
        bool all = true;
        for (const std::string& w : words) {
            const size_t c = w.find(':');
            if (c != std::string::npos && c > 0) {
                const std::string k = w.substr(0, c), v = w.substr(c + 1);
                std::string f;
                bool exact = true;
                if (k == "BAND") f = upper(q.band); else if (k == "DXCC") f = std::to_string(q.dxcc); else if (k == "CONT") f = q.cont;
                else if (k == "STATE") f = q.state; else if (k == "CQZ") f = std::to_string(q.cqz);
                else if (k == "QSL") f = q.confirmed() ? (v.empty() || v[0] == 'Y' ? v : "Y") : (v.empty() || v[0] == 'N' ? v : "N");
                else { exact = false; f = k == "MODE" ? q.mode + " " + q.submode : k == "CALL" ? q.call : k == "GRID" ? q.grid : std::string(); }
                if (exact ? f != v : f.find(v) == std::string::npos) { all = false; break; }
                continue;
            }
            const std::string hay = q.call + " " + q.grid + " " + q.mode + " " + q.submode + " " + upper(q.band) + " " + upper(q.name) + " " + upper(q.qth) +
                                    " " + upper(q.comment) + " " + upper(q.country) + " " + q.state + " " + (q.startUtc > 0 ? adifDate(q.startUtc) : "");
            if (hay.find(w) == std::string::npos) { all = false; break; }
        }
        if (all) out.push_back(i);
    }
    return out;
}

Logbook::ImportResult Logbook::importAdif(const std::string& text, bool skipDupes) {
    ImportResult r;
    Step s; s.kind = 3;
    for (const AdifFields& f : adifReadRecords(text)) {
        LogQso q = adifToQso(f);
        if (q.call.size() < 3) { r.bad++; continue; }
        tidy(q);
        if (skipDupes && findDupe(q.call, q.band, q.mode, true, true, q.startUtc, 60) >= 0) { r.dupes++; continue; }
        if (enrich) enrich(q);
        q.id = nextId_++;
        q_.push_back(q);
        s.after.push_back({(int)q_.size() - 1, q});
        r.added++;
    }
    if (r.added) { s.label = "import of " + std::to_string(r.added) + " QSOs"; push(std::move(s)); }
    return r;
}

std::string Logbook::exportAdif() const {
    std::vector<size_t> all(q_.size());
    for (size_t i = 0; i < all.size(); i++) all[i] = i;
    return exportAdif(all);
}
std::string Logbook::exportAdif(const std::vector<size_t>& idx) const {
    std::string o = adifHeader();
    for (size_t i : idx) if (i < q_.size()) o += qsoToAdif(q_[i]);
    return o;
}

bool Logbook::load(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    clear();
    if (!f) return true;   // no log yet
    std::stringstream ss; ss << f.rdbuf();
    if (f.bad()) { err = "cannot read " + path; return false; }
    for (const AdifFields& r : adifReadRecords(ss.str())) {
        LogQso q = adifToQso(r);
        if (q.call.empty()) continue;
        tidy(q);
        if (enrich) enrich(q);
        q.id = nextId_++;
        q_.push_back(q);
    }
    return true;
}
bool Logbook::save(const std::string& path, std::string& err) const {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { err = "cannot write " + tmp; return false; }
        f << exportAdif();
        if (!f) { err = "cannot write " + tmp; return false; }
    }
    std::remove(path.c_str());   // Windows rename does not replace
    if (std::rename(tmp.c_str(), path.c_str()) != 0) { err = "cannot replace " + path; return false; }
    return true;
}

// ---- awards ----
const char* awardName(Award a) {
    switch (a) { case Award::Dxcc: return "DXCC"; case Award::Was: return "WAS"; case Award::Grid: return "Grids"; default: return "CQ zones"; }
}
int awardTotal(Award a) { switch (a) { case Award::Dxcc: return 340; case Award::Was: return 50; case Award::Cqz: return 40; default: return 0; } }
const std::vector<std::string>& usStates() {
    static const std::vector<std::string> s = {"AL", "AK", "AZ", "AR", "CA", "CO", "CT", "DE", "FL", "GA", "HI", "ID", "IL", "IN", "IA", "KS", "KY",
                                               "LA", "ME", "MD", "MA", "MI", "MN", "MS", "MO", "MT", "NE", "NV", "NH", "NJ", "NM", "NY", "NC", "ND",
                                               "OH", "OK", "OR", "PA", "RI", "SC", "SD", "TN", "TX", "UT", "VT", "VA", "WA", "WV", "WI", "WY"};
    return s;
}
std::string awardKey(const LogQso& q, Award a) {
    switch (a) {
    case Award::Dxcc: return q.dxcc > 0 ? std::to_string(q.dxcc) : upper(q.country);
    case Award::Was: {
        // the US entities with states: 291 (USA), 6 (Alaska), 110 (Hawaii); without a number, a country name containing "UNITED STATES"
        const bool us = q.dxcc == 291 || q.dxcc == 6 || q.dxcc == 110 || (q.dxcc == 0 && upper(q.country).find("UNITED STATES") != std::string::npos);
        if (!us) return "";
        std::string st = q.state;
        if (q.dxcc == 6) st = "AK";
        if (q.dxcc == 110) st = "HI";
        for (const auto& s : usStates()) if (s == st) return st;
        return "";
    }
    case Award::Grid: { const std::string g = grid4(q.grid); return hamGridValid(g) ? g : ""; }
    default: return q.cqz > 0 && q.cqz <= 40 ? std::to_string(q.cqz) : "";
    }
}

int bandOrder(const std::string& band) {
    static const char* kOrder[] = {"2190m", "630m", "560m", "160m", "80m", "60m", "40m", "30m", "20m", "17m", "15m", "12m", "10m", "8m", "6m", "5m",
                                   "4m", "2m", "1.25m", "70cm", "33cm", "23cm", "13cm", "9cm", "6cm", "3cm", "1.25cm", "6mm", "4mm", "2.5mm", "2mm", "1mm"};
    const std::string b = lower(band);
    for (int i = 0; i < (int)(sizeof kOrder / sizeof *kOrder); i++) if (b == kOrder[i]) return i;
    return 100;
}

AwardTable awardTable(const std::vector<LogQso>& log, Award a, const std::string& modeFilter, const std::string& bandFilter) {
    AwardTable t;
    t.total = awardTotal(a);
    std::map<std::string, AwardRow> rows;
    std::set<std::string> bands;
    std::map<std::string, std::pair<std::set<std::string>, std::set<std::string>>> pb;
    const std::string mf = upper(modeFilter), bf = lower(bandFilter);
    auto mark = [](char& c, bool conf) { if (conf) c = 'C'; else if (c != 'C') c = 'W'; };
    for (const LogQso& q : log) {
        if (!mf.empty() && q.mode != mf && modeClass(q.mode) != mf) continue;
        if (!bf.empty() && q.band != bf) continue;
        const std::string k = awardKey(q, a);
        if (k.empty()) continue;
        AwardRow& r = rows[k];
        r.key = k;
        if (r.label.empty()) r.label = a == Award::Dxcc && !q.country.empty() ? q.country : k;
        const bool c = q.confirmed();
        mark(r.any, c);
        if (!q.band.empty()) { mark(r.byBand[q.band], c); bands.insert(q.band); pb[q.band].first.insert(k); if (c) pb[q.band].second.insert(k); }
        const std::string mc = modeClass(q.mode);
        if (!mc.empty()) mark(r.byClass[mc], c);
    }
    for (auto& kv : rows) { t.worked++; if (kv.second.any == 'C') t.confirmed++; t.rows.push_back(kv.second); }
    if (a == Award::Cqz) std::sort(t.rows.begin(), t.rows.end(), [](const AwardRow& x, const AwardRow& y) { return std::atoi(x.key.c_str()) < std::atoi(y.key.c_str()); });
    if (a == Award::Dxcc) std::sort(t.rows.begin(), t.rows.end(), [](const AwardRow& x, const AwardRow& y) { return x.label < y.label; });
    t.bands.assign(bands.begin(), bands.end());
    std::sort(t.bands.begin(), t.bands.end(), [](const std::string& x, const std::string& y) { return bandOrder(x) < bandOrder(y); });
    for (const auto& kv : pb) t.perBand[kv.first] = {(int)kv.second.first.size(), (int)kv.second.second.size()};
    return t;
}

LogStats logStats(const std::vector<LogQso>& log, const std::string& myGrid) {
    LogStats s;
    std::set<std::string> calls, dx, grids;
    double myLat = 0, myLon = 0;
    const bool haveMe = fromMaidenhead(myGrid, myLat, myLon);
    for (const LogQso& q : log) {
        s.qsos++;
        calls.insert(baseCall(q.call));
        const std::string d = awardKey(q, Award::Dxcc);
        if (!d.empty()) dx.insert(d);
        const std::string g = awardKey(q, Award::Grid);
        if (!g.empty()) grids.insert(g);
        if (q.confirmed()) s.confirmed++;
        if (!q.band.empty()) s.perBand[q.band]++;
        if (!q.mode.empty()) s.perMode[q.mode]++;
        if (!q.cont.empty()) s.perCont[q.cont]++;
        if (q.startUtc > 0) {
            s.perHour[(int)(std::fmod(q.startUtc, 86400) / 3600)]++;
            if (s.firstUtc == 0 || q.startUtc < s.firstUtc) s.firstUtc = q.startUtc;
            s.lastUtc = std::max(s.lastUtc, q.startUtc);
        }
        double la, lo, la1 = myLat, lo1 = myLon;
        bool me = haveMe;
        if (!q.myGrid.empty()) me = fromMaidenhead(q.myGrid, la1, lo1);
        if (me && !q.grid.empty() && fromMaidenhead(q.grid, la, lo)) {
            double km, brg;
            greatCircle(la1, lo1, la, lo, km, brg);
            if (km > s.bestKm) { s.bestKm = km; s.bestCall = q.call; }
        }
    }
    for (const LogQso& q : log) if (q.startUtc > s.lastUtc - 3600 && q.startUtc <= s.lastUtc && q.startUtc > 0) s.qsoPerHourLast++;
    s.calls = (int)calls.size(); s.dxcc = (int)dx.size(); s.grids = (int)grids.size();
    return s;
}

// ---- worked before ----
void WorkedIndex::build(const std::vector<LogQso>& log) {
    call_.clear(); callBandMode_.clear(); dxcc_.clear(); dxccBand_.clear(); grid_.clear(); gridBand_.clear();
    for (const LogQso& q : log) add(q);
}
void WorkedIndex::add(const LogQso& q) {
    const std::string c = baseCall(q.call);
    call_.insert(c);
    callBandMode_.insert(c + "|" + q.band + "|" + upper(q.mode));
    const std::string d = awardKey(q, Award::Dxcc);
    if (!d.empty()) { dxcc_.insert(d); dxccBand_.insert(d + "|" + q.band); }
    const std::string g = awardKey(q, Award::Grid);
    if (!g.empty()) { grid_.insert(g); gridBand_.insert(g + "|" + q.band); }
}
DecodeMarks WorkedIndex::mark(const std::string& call, const std::string& grid, const std::string& band, const std::string& mode, const std::string& dxccKey) const {
    DecodeMarks m;
    if (call.empty()) return m;
    const std::string c = baseCall(call), b = lower(band);
    m.workedCall = call_.count(c) > 0;
    m.workedCallBand = callBandMode_.count(c + "|" + b + "|" + upper(mode)) > 0;
    if (!dxccKey.empty()) { m.newDxcc = !dxcc_.count(dxccKey); m.newDxccBand = !dxccBand_.count(dxccKey + "|" + b); }
    const std::string g = grid4(grid);
    if (hamGridValid(g) && g != "RR73") { m.newGrid = !grid_.count(g); m.newGridBand = !gridBand_.count(g + "|" + b); }
    return m;
}

}  // namespace dect2::ham
