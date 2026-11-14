// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Logbook tools for the Ham screen (4.0): see ham_logtools.h.
#include "dect2/ham/ham_logtools.h"
#include "dect2/common/diag.h"
#include "dect2/ham/ham.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <sstream>
#include <system_error>

namespace dect2::ham {

namespace fs = std::filesystem;

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
bool rstLike(const std::string& s) {
    const std::string u = upper(s);
    if (u == "5NN" || u == "599" || u == "59") return true;
    if (u.size() < 2 || u.size() > 3) return false;
    if (u[0] < '1' || u[0] > '5') return false;
    for (size_t i = 1; i < u.size(); i++) if (!(std::isdigit((unsigned char)u[i]) || u[i] == 'N')) return false;
    return true;
}
std::string join(const std::vector<std::string>& v, size_t a, size_t b) {
    std::string o;
    for (size_t i = a; i < b && i < v.size(); i++) { if (!o.empty()) o += ' '; o += v[i]; }
    return o;
}
std::string extraOf(const LogQso& q, const char* k) { const auto it = q.extra.find(k); return it == q.extra.end() ? "" : it->second; }
std::string dayOf(double utc) { return adifDate(utc); }
}  // namespace

// ---- Cabrillo ----
std::string cabrilloToAdifMode(const std::string& mo) {
    const std::string u = upper(trim(mo));
    if (u == "CW") return "CW";
    if (u == "RY") return "RTTY";
    if (u == "PH") return "SSB";
    if (u == "FM") return "FM";
    return "DIGI";
}

CabrilloLog cabrilloParse(const std::string& text) {
    CabrilloLog L;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string tag = upper(trim(line.substr(0, colon))), val = trim(line.substr(colon + 1));
        if (tag != "QSO" && tag != "X-QSO") {
            if (tag == "SOAPBOX" && L.header.count(tag)) L.header[tag] += "\n" + val;
            else L.header[tag] = val;
            continue;
        }
        if (tag == "X-QSO") continue;   // a QSO the operator marked as not counting
        auto w = words(val);
        // freq mo date time mycall ... call ...
        if (w.size() < 7) { L.bad++; continue; }
        LogQso q;
        char* end = nullptr;
        const double khz = std::strtod(w[0].c_str(), &end);
        if (!end || *end || khz <= 0) {
            // the VHF bands are written as "50" "144" "432" "1.2G" ...: a band, not a frequency
            if (w[0] == "1.2G" || w[0] == "2.3G") q.freqHz = (w[0] == "1.2G" ? 1296.0 : 2304.0) * 1e6;
            else { L.bad++; continue; }
        } else q.freqHz = khz < 1000 ? khz * 1e6 : khz * 1e3;   // "50" (MHz band) or "14025" (kHz)
        q.mode = cabrilloToAdifMode(w[1]);
        std::string d = w[2];
        d.erase(std::remove(d.begin(), d.end(), '-'), d.end());
        q.startUtc = adifUtc(d, w[3]);
        if (q.startUtc <= 0) { L.bad++; continue; }
        q.myCall = upper(w[4]);
        std::vector<std::string> rest(w.begin() + 5, w.end());
        if (rest.size() % 2 == 0 && (rest.back() == "0" || rest.back() == "1")) rest.pop_back();   // transmitter id
        if (rest.size() < 1 || rest.size() % 2 == 0) { L.bad++; continue; }
        const size_t n = (rest.size() - 1) / 2;
        q.call = upper(rest[n]);
        auto half = [&](size_t a, size_t b, std::string& rst, std::string& x) {
            if (b > a && rstLike(rest[a])) { rst = upper(rest[a]); x = join(rest, a + 1, b); }
            else x = join(rest, a, b);
        };
        half(0, n, q.rstSent, q.stx);
        half(n + 1, rest.size(), q.rstRcvd, q.srx);
        q.contestId = L.header.count("CONTEST") ? L.header["CONTEST"] : "";
        q.band = bandNameFor(q.freqHz);
        L.qsos.push_back(q);
    }
    return L;
}

// ---- confirmations ----
MergeResult mergeConfirmations(Logbook& log, const std::string& text, QslSource src) {
    MergeResult r;
    for (const AdifFields& f : adifReadRecords(text)) {
        const LogQso d = adifToQso(f);
        if (d.call.empty() || d.startUtc <= 0) { r.unmatched++; continue; }
        const std::string band = d.band.empty() ? bandNameFor(d.freqHz) : d.band, cls = modeClass(d.mode);
        const std::string bc = baseCall(d.call);
        int best = -1;
        double bestDt = 1801;
        const auto& qs = log.qsos();
        for (size_t i = 0; i < qs.size(); i++) {
            const LogQso& q = qs[i];
            if (baseCall(q.call) != bc) continue;
            const std::string qb = q.band.empty() ? bandNameFor(q.freqHz) : q.band;
            if (!band.empty() && !qb.empty() && qb != band) continue;
            if (!cls.empty() && modeClass(q.mode) != cls) continue;
            const double dt = std::fabs(q.startUtc - d.startUtc);
            if (dt < bestDt) { bestDt = dt; best = (int)i; }
        }
        if (best < 0) { r.unmatched++; continue; }
        r.matched++;
        LogQso q = qs[(size_t)best];
        auto has = [&](const char* k) { const auto it = f.find(k); return it != f.end() && (upper(it->second) == "Y" || upper(it->second) == "V"); };
        bool changed = false;
        const bool wasConf = q.confirmed();
        switch (src) {
        case QslSource::Lotw:
            if ((has("QSL_RCVD") || has("LOTW_QSL_RCVD")) && !q.lotwRcvd) { q.lotwRcvd = true; changed = true; }
            break;
        case QslSource::Eqsl:
            if (extraOf(q, "EQSL_QSL_RCVD") != "Y") { q.extra["EQSL_QSL_RCVD"] = "Y"; changed = true; }
            break;
        case QslSource::Card:
            if (!q.qslRcvd) { q.qslRcvd = true; changed = true; }
            break;
        case QslSource::ClubLog: break;
        }
        const bool newly = !wasConf && q.confirmed();
        // what the service knows that the log lacks
        if (!q.dxcc && d.dxcc) { q.dxcc = d.dxcc; changed = true; }
        if (!q.cqz && d.cqz) { q.cqz = d.cqz; changed = true; }
        if (!q.ituz && d.ituz) { q.ituz = d.ituz; changed = true; }
        if (q.state.empty() && !d.state.empty()) { q.state = d.state; changed = true; }
        if ((q.grid.empty() || (q.grid.size() < d.grid.size() && d.grid.compare(0, q.grid.size(), q.grid) == 0)) && !d.grid.empty()) { q.grid = d.grid; changed = true; }
        if (d.extra.count("CNTY") && !q.extra.count("CNTY")) { q.extra["CNTY"] = d.extra.at("CNTY"); changed = true; }
        if (changed) log.update(q);
        if (newly) r.newlyConfirmed++;
    }
    return r;
}

QslSummary qslSummary(const std::vector<LogQso>& log) {
    QslSummary s;
    for (const auto& q : log) {
        s.qsos++;
        if (q.qslSent) s.sent++;
        if (q.qslRcvd) s.rcvd++;
        if (q.lotwRcvd) s.lotw++;
        if (extraOf(q, "EQSL_QSL_RCVD") == "Y") s.eqsl++;
        if (q.confirmed()) s.confirmed++;
        if (q.qslSent && !q.confirmed()) s.pending++;
    }
    return s;
}

// ---- backups ----
std::vector<std::string> logBackups(const std::string& dir) {
    std::vector<std::string> v;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return v;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("onair-log-", 0) == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, ".adi") == 0) v.push_back(e.path().string());
    }
    std::sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) { return a > b; });   // the name carries the time: newest first
    return v;
}
std::string logBackup(const std::string& logPath, const std::string& dir, double now, int keep, std::string& err) {
    std::error_code ec;
    if (!fs::exists(logPath, ec)) { err = "there is no log file yet"; return ""; }
    fs::create_directories(dir, ec);
    const std::string d = adifDate(now), t = adifTime(now);
    const std::string dst = (fs::path(dir) / ("onair-log-" + d + "-" + t + ".adi")).string();
    fs::copy_file(logPath, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) { DIAG(529503, "Copying the ham log %s to the backup folder %s failed (%s), so no backup was made.", logPath.c_str(), dir.c_str(), ec.message().c_str()); err = "cannot copy the log: " + ec.message(); return ""; }
    if (keep > 0) {
        const auto all = logBackups(dir);
        for (size_t i = (size_t)keep; i < all.size(); i++) fs::remove(all[i], ec);
    }
    err.clear();
    return dst;
}
bool logBackupDue(const std::string& dir, double now, double minGap) {
    const auto all = logBackups(dir);
    if (all.empty()) return true;
    // onair-log-YYYYMMDD-HHMMSS.adi
    const std::string n = fs::path(all.front()).filename().string();
    if (n.size() < 25) return true;
    const double t = adifUtc(n.substr(10, 8), n.substr(19, 6));
    return t <= 0 || now - t >= minGap;
}

// ---- TQSL ----
std::string tqslCommand(const std::string& tqsl, const std::string& location, const std::string& file) {
    auto q = [](const std::string& v) { std::string o; for (char c : v) if (c != '"' && c != '\n' && c != '\r') o += c; return "\"" + o + "\""; };
    std::string c = q(trim(tqsl).empty() ? "tqsl" : trim(tqsl)) + " -q -d -u -a compliant -x";
    if (!trim(location).empty()) c += " -l " + q(trim(location));
    return c + " " + q(file);
}
bool tqslOk(int code) { return code == 0 || code == 9; }

// ---- activations ----
const char* programName(Program p) {
    switch (p) {
    case Program::Pota: return "POTA";
    case Program::Sota: return "SOTA";
    case Program::Wwff: return "WWFF";
    case Program::Iota: return "IOTA";
    default: return "";
    }
}
Program programFromName(const std::string& s) {
    const std::string u = upper(trim(s));
    if (u == "POTA") return Program::Pota;
    if (u == "SOTA") return Program::Sota;
    if (u == "WWFF") return Program::Wwff;
    if (u == "IOTA") return Program::Iota;
    return Program::None;
}
std::string refNormal(const std::string& r) {
    std::string o;
    for (char c : r) if (!std::isspace((unsigned char)c)) o += (char)std::toupper((unsigned char)c);
    return o;
}
bool refValid(Program p, const std::string& ref0) {
    const std::string r = refNormal(ref0);
    auto alnum = [](const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum((unsigned char)c) != 0; }); };
    auto digits = [](const std::string& s, size_t lo, size_t hi) {
        return s.size() >= lo && s.size() <= hi && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit((unsigned char)c) != 0; });
    };
    const size_t dash = r.rfind('-');
    if (dash == std::string::npos || dash == 0) return false;
    const std::string a = r.substr(0, dash), n = r.substr(dash + 1);
    switch (p) {
    case Program::Pota: return alnum(a) && a.size() <= 4 && digits(n, 4, 5);
    case Program::Wwff: return alnum(a) && a.size() >= 3 && a.size() <= 5 && a.find("FF") != std::string::npos && digits(n, 4, 4);
    case Program::Iota: return (a == "AF" || a == "AN" || a == "AS" || a == "EU" || a == "NA" || a == "OC" || a == "SA") && digits(n, 3, 3);
    case Program::Sota: {
        const size_t sl = a.find('/');
        if (sl == std::string::npos || sl == 0 || sl + 1 >= a.size()) return false;
        return alnum(a.substr(0, sl)) && alnum(a.substr(sl + 1)) && a.substr(sl + 1).size() == 2 && digits(n, 3, 3);
    }
    default: return false;
    }
}
namespace {
const char* myRefField(Program p) {
    switch (p) { case Program::Pota: return "MY_POTA_REF"; case Program::Sota: return "MY_SOTA_REF"; case Program::Wwff: return "MY_WWFF_REF"; case Program::Iota: return "MY_IOTA"; default: return ""; }
}
const char* refField(Program p) {
    switch (p) { case Program::Pota: return "POTA_REF"; case Program::Sota: return "SOTA_REF"; case Program::Wwff: return "WWFF_REF"; case Program::Iota: return "IOTA"; default: return ""; }
}
std::string joinRefs(const std::vector<std::string>& v) {
    std::string o;
    for (const auto& r : v) { const std::string n = refNormal(r); if (n.empty()) continue; if (!o.empty()) o += ','; o += n; }
    return o;
}
bool carries(const LogQso& q, Program p, const std::string& ref) {
    const std::string v = extraOf(q, myRefField(p));
    if (v.empty()) return false;
    std::stringstream ss(v);
    std::string t;
    while (std::getline(ss, t, ',')) {
        const size_t at = t.find('@');   // POTA allows "US-0001@US-CA"
        if (refNormal(at == std::string::npos ? t : t.substr(0, at)) == refNormal(ref)) return true;
    }
    return false;
}
}  // namespace
void applyActivation(LogQso& q, const Activation& a) {
    if (!a.active()) return;
    const std::string refs = joinRefs(a.refs);
    if (refs.empty()) return;
    q.extra[myRefField(a.prog)] = refs;
    if (a.prog != Program::Iota) { q.extra["MY_SIG"] = programName(a.prog); q.extra["MY_SIG_INFO"] = refs; }
}
void applyHunted(LogQso& q, Program p, const std::string& ref) {
    const std::string r = refNormal(ref);
    if (p == Program::None || r.empty()) return;
    q.extra[refField(p)] = r;
    if (p != Program::Iota) { q.extra["SIG"] = programName(p); q.extra["SIG_INFO"] = r; }
}
ActivationProgress activationProgress(const std::vector<LogQso>& log, const Activation& a, double now) {
    ActivationProgress pr;
    if (!a.active()) return pr;
    pr.needed = a.prog == Program::Pota ? 10 : a.prog == Program::Sota ? 4 : a.prog == Program::Wwff ? 44 : 0;
    const std::string day = dayOf(now), ref = a.refs.front();
    std::set<std::string> seen;
    for (const auto& q : log) {
        if (!carries(q, a.prog, ref) || dayOf(q.startUtc) != day) continue;
        if (a.prog == Program::Sota && a.startUtc > 0 && q.startUtc < a.startUtc) continue;
        const std::string band = q.band.empty() ? bandNameFor(q.freqHz) : q.band;
        const std::string key = a.prog == Program::Sota ? baseCall(q.call) : baseCall(q.call) + "|" + band + "|" + upper(q.mode);
        if (!seen.insert(key).second) continue;
        pr.qsos++;
        pr.perBand[band]++;
    }
    pr.done = pr.qsos >= pr.needed;
    return pr;
}
std::string potaFileName(const std::string& myCall, const std::string& ref, double utc) {
    std::string c = upper(myCall);
    std::replace(c.begin(), c.end(), '/', '_');
    return c + "@" + refNormal(ref) + "-" + adifDate(utc) + ".adi";
}
std::vector<size_t> activationQsos(const std::vector<LogQso>& log, Program p, const std::string& ref, double utc) {
    std::vector<size_t> v;
    const std::string day = dayOf(utc);
    for (size_t i = 0; i < log.size(); i++) if (carries(log[i], p, ref) && dayOf(log[i].startUtc) == day) v.push_back(i);
    return v;
}
std::string sotaCsv(const std::vector<LogQso>& log, const std::string& summit, const std::string& myCall, double fromUtc) {
    std::string o;
    for (const auto& q : log) {
        if (!carries(q, Program::Sota, summit) || q.startUtc < fromUtc) continue;
        const std::string d = adifDate(q.startUtc), t = adifTime(q.startUtc);
        if (d.size() != 8 || t.size() < 4) continue;
        char f[32];
        if (q.freqHz > 0) snprintf(f, sizeof f, "%gMHz", std::round(q.freqHz / 100.0) / 1e4);   // 100 Hz steps: 7.0325MHz
        else snprintf(f, sizeof f, "%s", "");
        const std::string mode = modeClass(q.mode) == "PHONE" ? (upper(q.mode) == "FM" ? "FM" : "SSB") : upper(q.mode);
        const std::string theirs = extraOf(q, "SOTA_REF");
        o += "V2," + upper(q.myCall.empty() ? myCall : q.myCall) + "," + refNormal(summit) + "," + d.substr(6, 2) + "/" + d.substr(4, 2) + "/" + d.substr(2, 2) +
             "," + t.substr(0, 4) + "," + f + "," + mode + "," + upper(q.call) + "," + theirs + ",\n";
    }
    return o;
}

}  // namespace dect2::ham
