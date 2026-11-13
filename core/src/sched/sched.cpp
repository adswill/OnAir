// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/sched/sched.h"
#include "dect2/automation/json_mini.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace dect2::sched {
namespace js = dect2::automation::json;

namespace {
int64_t floorDiv(int64_t a, int64_t b) { int64_t q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
int daysInMonth(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : d[m - 1];
}
int64_t lastSunday(int y, int m) {
    int64_t d = daysFromCivil(y, m, daysInMonth(y, m));
    return d - weekdayFromDays(d);
}
int64_t nthSunday(int y, int m, int n) {
    int64_t d = daysFromCivil(y, m, 1);
    d += (7 - weekdayFromDays(d)) % 7;
    return d + 7 * (n - 1);
}
int yearOf(int64_t utc) { int y, m, d; civilFromDays(floorDiv(utc, 86400), y, m, d); return y; }
}  // namespace

int64_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
void civilFromDays(int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y = (int)(yy + (m <= 2));
}
int weekdayFromDays(int64_t z) { return (int)(((z % 7) + 11) % 7); }   // 1970-01-01 was a Thursday (4)

Zone zoneUtc() { return [](int64_t) { return 0; }; }
Zone zoneFixed(int off) { return [off](int64_t) { return off; }; }
Zone zoneEu(int stdOff) {
    return [stdOff](int64_t t) {
        const int y = yearOf(t);
        const int64_t a = lastSunday(y, 3) * 86400 + 3600, b = lastSunday(y, 10) * 86400 + 3600;
        return stdOff + (t >= a && t < b ? 3600 : 0);
    };
}
Zone zoneUs(int stdOff) {
    return [stdOff](int64_t t) {
        const int y = yearOf(t + stdOff);
        const int64_t a = nthSunday(y, 3, 2) * 86400 + 7200 - stdOff, b = nthSunday(y, 11, 1) * 86400 + 7200 - (stdOff + 3600);
        return stdOff + (t >= a && t < b ? 3600 : 0);
    };
}

int64_t localToUtc(const Zone& z, int y, int m, int d, int hh, int mm, int ss) {
    const int64_t L = daysFromCivil(y, m, d) * 86400 + hh * 3600 + mm * 60 + ss;
    const int o1 = z(L - 86400), o2 = z(L + 86400);
    if (o1 == o2) return L - o1;
    const int64_t u1 = L - o1, u2 = L - o2;
    const bool v1 = z(u1) == o1, v2 = z(u2) == o2;
    if (v1 && v2) return std::min(u1, u2);
    if (v1) return u1;
    if (v2) return u2;
    return L - o1;   // the hour that does not exist: the instant after the gap
}

// ---- cron --------------------------------------------------------------------------------------------------------------------------
namespace {
bool parseField(const std::string& f, int lo, int hi, std::vector<bool>& set, bool& any, std::string& err, const char* what) {
    any = !f.empty() && f[0] == '*';
    size_t p = 0;
    while (p <= f.size()) {
        size_t q = f.find(',', p);
        if (q == std::string::npos) q = f.size();
        std::string item = f.substr(p, q - p);
        p = q + 1;
        if (item.empty()) { err = std::string("empty entry in ") + what; return false; }
        int step = 1;
        const size_t sl = item.find('/');
        if (sl != std::string::npos) {
            step = atoi(item.c_str() + sl + 1);
            if (step <= 0) { err = std::string("bad step in ") + what; return false; }
            item = item.substr(0, sl);
        }
        int a = lo, b = hi;
        if (item != "*") {
            const size_t dash = item.find('-');
            char* e = nullptr;
            a = (int)strtol(item.c_str(), &e, 10);
            if (e == item.c_str()) { err = std::string("bad number in ") + what; return false; }
            if (dash != std::string::npos) b = atoi(item.c_str() + dash + 1);
            else b = sl != std::string::npos ? hi : a;
        }
        if (a < lo || b > hi || a > b) { err = std::string(what) + " out of range"; return false; }
        for (int v = a; v <= b; v += step) set[v == 7 && hi == 7 ? 0 : v] = true;
    }
    return true;
}
bool dayMatches(const CronFields& c, int d, int dow) {
    if (c.domAny && c.dowAny) return true;
    if (c.domAny) return c.dow[dow];
    if (c.dowAny) return c.dom[d];
    return c.dom[d] || c.dow[dow];
}
}  // namespace

bool parseCron(const std::string& text, CronFields& o, std::string& err) {
    std::vector<std::string> f;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) i++;
        size_t j = i;
        while (j < text.size() && text[j] != ' ' && text[j] != '\t') j++;
        if (j > i) f.push_back(text.substr(i, j - i));
        i = j;
    }
    if (f.size() != 5) { err = "a cron line has five fields: minute hour day month weekday"; return false; }
    o = CronFields();
    o.min.assign(60, false); o.hour.assign(24, false); o.dom.assign(32, false); o.mon.assign(13, false); o.dow.assign(7, false);
    bool any;
    if (!parseField(f[0], 0, 59, o.min, any, err, "minute")) return false;
    if (!parseField(f[1], 0, 23, o.hour, any, err, "hour")) return false;
    if (!parseField(f[2], 1, 31, o.dom, o.domAny, err, "day of month")) return false;
    if (!parseField(f[3], 1, 12, o.mon, any, err, "month")) return false;
    if (!parseField(f[4], 0, 7, o.dow, o.dowAny, err, "weekday")) return false;
    return true;
}

static bool fieldsOf(const Schedule& s, CronFields& c) {
    std::string err;
    if (s.kind == Schedule::Cron) return parseCron(s.cron, c, err);
    c = CronFields();
    c.min.assign(60, false); c.hour.assign(24, false); c.dom.assign(32, true); c.mon.assign(13, true); c.dow.assign(7, false);
    if (s.hour < 0 || s.hour > 23 || s.minute < 0 || s.minute > 59) return false;
    c.min[s.minute] = true; c.hour[s.hour] = true;
    if (s.kind == Schedule::Weekly) {
        for (int i = 0; i < 7; i++) c.dow[i] = (s.weekdays >> i) & 1;
        c.dowAny = false;
        if (!(s.weekdays & 0x7f)) return false;
    } else c.dowAny = true;
    return true;
}

int64_t nextRun(const Schedule& s, const Zone& z, int64_t after) {
    switch (s.kind) {
    case Schedule::Once: return s.at > after ? s.at : -1;
    case Schedule::Every:
        if (s.everySec <= 0) return -1;
        if (after < s.at) return s.at;
        return s.at + ((after - s.at) / s.everySec + 1) * s.everySec;
    case Schedule::Pass: return -1;
    default: break;
    }
    CronFields c;
    if (!fieldsOf(s, c)) return -1;
    const int64_t d0 = floorDiv(after + z(after), 86400);
    for (int k = 0; k < 366 * 9; k++) {
        int y, m, d;
        civilFromDays(d0 + k, y, m, d);
        if (!c.mon[m] || !dayMatches(c, d, weekdayFromDays(d0 + k))) continue;
        int64_t best = -1;
        for (int h = 0; h < 24; h++) {
            if (!c.hour[h]) continue;
            for (int mi = 0; mi < 60; mi++) {
                if (!c.min[mi]) continue;
                const int64_t u = localToUtc(z, y, m, d, h, mi);
                if (u > after && (best < 0 || u < best)) best = u;
            }
        }
        if (best >= 0) return best;
    }
    return -1;
}

std::vector<int64_t> runsBetween(const Schedule& s, const Zone& z, int64_t from, int64_t to, size_t max) {
    std::vector<int64_t> r;
    int64_t t = from;
    while (r.size() < max) {
        t = nextRun(s, z, t);
        if (t < 0 || t > to) break;
        r.push_back(t);
    }
    return r;
}

// ---- the file name -----------------------------------------------------------------------------------------------------------------
const char* const kDefaultNamePattern = "{date}_{time}_{mode}_{freq}";

std::string sanitizeName(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c < 32 || c == 127 || c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*' || c == '/' || c == '\\') o += '_';
        else o += (char)c;
    }
    return o;
}

namespace {
// path pieces separated by '/': drops empty, "." and ".." pieces and trailing dots / spaces, then limits the length
std::string cleanPath(const std::string& p) {
    std::string out, seg;
    auto flush = [&] {
        while (!seg.empty() && (seg.back() == '.' || seg.back() == ' ')) seg.pop_back();
        size_t b = 0;
        while (b < seg.size() && seg[b] == ' ') b++;
        seg = seg.substr(b);
        if (!seg.empty()) { if (!out.empty()) out += '/'; out += seg; }
        seg.clear();
    };
    for (char c : p) { if (c == '/') flush(); else seg += c; }
    flush();
    if (out.size() > 200) out.resize(200);
    return out;
}

bool tokenValue(const std::string& tok, int pad, const NameFields& f, std::string& out) {
    const int64_t lt = f.utc + f.zoneOffsetSec;
    int y, m, d;
    civilFromDays(floorDiv(lt, 86400), y, m, d);
    const int sod = (int)(lt - floorDiv(lt, 86400) * 86400);
    int uy, um, ud;
    civilFromDays(floorDiv(f.utc, 86400), uy, um, ud);
    const int usod = (int)(f.utc - floorDiv(f.utc, 86400) * 86400);
    char b[64];
    if (tok == "date") snprintf(b, sizeof b, "%04d-%02d-%02d", y, m, d);
    else if (tok == "time") snprintf(b, sizeof b, "%02d-%02d-%02d", sod / 3600, sod / 60 % 60, sod % 60);
    else if (tok == "datetime") snprintf(b, sizeof b, "%04d%02d%02d-%02d%02d%02d", y, m, d, sod / 3600, sod / 60 % 60, sod % 60);
    else if (tok == "yyyy") snprintf(b, sizeof b, "%04d", y);
    else if (tok == "mm") snprintf(b, sizeof b, "%02d", m);
    else if (tok == "dd") snprintf(b, sizeof b, "%02d", d);
    else if (tok == "HH") snprintf(b, sizeof b, "%02d", sod / 3600);
    else if (tok == "MM") snprintf(b, sizeof b, "%02d", sod / 60 % 60);
    else if (tok == "SS") snprintf(b, sizeof b, "%02d", sod % 60);
    else if (tok == "utcdate") snprintf(b, sizeof b, "%04d-%02d-%02d", uy, um, ud);
    else if (tok == "utctime") snprintf(b, sizeof b, "%02d-%02d-%02d", usod / 3600, usod / 60 % 60, usod % 60);
    else if (tok == "freq") { if (f.freqHz > 0) snprintf(b, sizeof b, "%.3fMHz", f.freqHz / 1e6); else snprintf(b, sizeof b, "nofreq"); }
    else if (tok == "freq_mhz") snprintf(b, sizeof b, "%.3f", f.freqHz / 1e6);
    else if (tok == "freq_khz") snprintf(b, sizeof b, "%.0f", f.freqHz / 1e3);
    else if (tok == "freq_hz") snprintf(b, sizeof b, "%.0f", f.freqHz);
    else if (tok == "n") snprintf(b, sizeof b, "%0*d", pad, f.counter);
    else if (tok == "mode") { out = sanitizeName(f.mode.empty() ? "nomode" : f.mode); return true; }
    else if (tok == "job") { out = sanitizeName(f.job.empty() ? "job" : f.job); return true; }
    else if (tok == "kind") { out = sanitizeName(f.kind); return true; }
    else if (tok == "sat") { out = sanitizeName(f.satellite); return true; }
    else return false;
    out = b;
    return true;
}
}  // namespace

std::string expandName(const std::string& pattern, const NameFields& f) {
    const std::string& pat = pattern.empty() ? std::string(kDefaultNamePattern) : pattern;
    std::string out;
    for (size_t i = 0; i < pat.size();) {
        if (pat[i] != '{') { out += pat[i++]; continue; }
        const size_t e = pat.find('}', i);
        if (e == std::string::npos) { out += pat[i++]; continue; }
        std::string tok = pat.substr(i + 1, e - i - 1);
        int pad = 1;
        const size_t c = tok.find(':');
        if (c != std::string::npos) { pad = std::max(1, std::min(9, atoi(tok.c_str() + c + 1))); tok = tok.substr(0, c); }
        std::string v;
        if (tokenValue(tok, pad, f, v)) out += v;
        i = e + 1;
    }
    // characters a file name may not have, in the literal parts too ('/' stays: it makes folders)
    std::string cleaned;
    for (unsigned char ch : out) cleaned += (ch < 32 || ch == 127 || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '|' || ch == '?' || ch == '*' || ch == '\\') ? '_' : (char)ch;
    std::string r = cleanPath(cleaned);
    return r.empty() ? "recording" : r;
}

std::string validateName(const std::string& pattern) {
    NameFields f;
    for (size_t i = 0; i < pattern.size();) {
        if (pattern[i] == '}') return "a } without a {";
        if (pattern[i] != '{') { i++; continue; }
        const size_t e = pattern.find('}', i);
        if (e == std::string::npos) return "a { is not closed";
        std::string tok = pattern.substr(i + 1, e - i - 1);
        const size_t c = tok.find(':');
        if (c != std::string::npos) {
            if (tok.substr(0, c) != "n") return "only {n} takes a :width";
            tok = tok.substr(0, c);
        }
        std::string v;
        if (!tokenValue(tok, 1, f, v)) return "unknown {" + tok + "}";
        i = e + 1;
    }
    return "";
}

// ---- overlaps, missed runs, disk ----------------------------------------------------------------------------------------------------
std::vector<PlannedRun> resolveOverlaps(std::vector<PlannedRun> runs, const ConflictPolicy& p) {
    std::stable_sort(runs.begin(), runs.end(), [](const PlannedRun& a, const PlannedRun& b) {
        if (a.start != b.start) return a.start < b.start;
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.jobId < b.jobId;
    });
    std::vector<PlannedRun> out;
    int cur = -1;
    for (PlannedRun r : runs) {
        if (cur < 0 || r.start >= out[cur].end) { out.push_back(r); cur = (int)out.size() - 1; continue; }
        PlannedRun& c = out[cur];
        if (r.priority > c.priority) {
            c.end = r.start;
            if (c.end - c.start < p.minRunSec) { c.state = PlannedRun::Dropped; c.note = "cut by " + r.jobId + ", too short to run"; }
            else { c.state = PlannedRun::Shortened; c.note = "cut short by " + r.jobId; }
            out.push_back(r);
            cur = (int)out.size() - 1;
            continue;
        }
        const int64_t delay = c.end - r.start;
        const int64_t ns = c.end, ne = r.fixedEnd ? r.end : r.end + delay;
        if (p.queue && delay <= p.maxDelaySec && ne - ns >= p.minRunSec) {
            r.start = ns; r.end = ne; r.state = PlannedRun::Delayed; r.note = "waits for " + c.jobId;
            out.push_back(r);
            cur = (int)out.size() - 1;
        } else {
            r.state = PlannedRun::Dropped; r.note = "overlaps " + c.jobId;
            out.push_back(r);
        }
    }
    return out;
}

std::vector<PlannedRun> missedRuns(const Job& j, const std::vector<PlannedRun>& missed, int64_t now, int minRunSec) {
    std::vector<PlannedRun> r;
    if (j.missed != Missed::RunLate || missed.empty()) return r;
    PlannedRun m = missed.back();
    if (now - m.start > j.graceSec) return r;
    const int64_t dur = m.end - m.start;
    int64_t ne;
    if (m.end > now + minRunSec) ne = m.end;                  // still inside its window: for the rest of it
    else if (j.fixedEnd || j.when.kind == Schedule::Pass) return r;   // a pass that is over is over
    else ne = now + dur;
    m.start = now; m.end = ne; m.state = PlannedRun::Delayed; m.note = "missed, run late";
    r.push_back(m);
    return r;
}

bool wokeUp(int64_t lastTick, int64_t now, int tickSec) {
    if (lastTick <= 0) return false;
    return now < lastTick || now - lastTick > std::max(3 * tickSec, 30);
}

DiskState diskGuard(int64_t freeBytes, int64_t limitBytes, double bps, int warnSec) {
    if (freeBytes < limitBytes) return DiskState::Stop;
    if (bps > 0 && (double)(freeBytes - limitBytes) < bps * warnSec) return DiskState::Warn;
    return DiskState::Ok;
}

double estimateBytesPerSec(const Action& a, double iqRateHz, int iqBytesPerSample) {
    double b = 0;
    if (a.recordIq) b += iqRateHz * iqBytesPerSample;
    if (a.recordAudio) b += 48000.0 * 2;
    if (a.recordLog) b += 200;
    return b;
}

const char* resultName(Result r) {
    switch (r) {
    case Result::Done: return "done";
    case Result::Error: return "error";
    case Result::Missed: return "missed";
    case Result::Skipped: return "skipped";
    case Result::Preempted: return "preempted";
    case Result::DiskFull: return "disk full";
    case Result::Stopped: return "stopped";
    }
    return "?";
}

std::string describe(const Schedule& s) {
    char b[160];
    switch (s.kind) {
    case Schedule::Once: return "once";
    case Schedule::Every:
        if (s.everySec % 3600 == 0) snprintf(b, sizeof b, "every %lld h", (long long)(s.everySec / 3600));
        else if (s.everySec % 60 == 0) snprintf(b, sizeof b, "every %lld min", (long long)(s.everySec / 60));
        else snprintf(b, sizeof b, "every %lld s", (long long)s.everySec);
        return b;
    case Schedule::Daily: snprintf(b, sizeof b, "daily %02d:%02d", s.hour, s.minute); return b;
    case Schedule::Weekly: {
        static const char* dn[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        std::string d;
        for (int i = 0; i < 7; i++) if ((s.weekdays >> i) & 1) { if (!d.empty()) d += ','; d += dn[i]; }
        snprintf(b, sizeof b, " %02d:%02d", s.hour, s.minute);
        return d + b;
    }
    case Schedule::Cron: return "cron " + s.cron;
    case Schedule::Pass: snprintf(b, sizeof b, " above %.0f deg", s.minElevationDeg); return s.satellite + b;
    }
    return "";
}

// ---- the scheduler -----------------------------------------------------------------------------------------------------------------
const Job* Scheduler::find(const std::string& id) const {
    for (const Job& j : jobs) if (j.id == id) return &j;
    return nullptr;
}

void Scheduler::addRecord(RunRecord r) {
    history.push_back(std::move(r));
    if (history.size() > settings.historyMax) history.erase(history.begin(), history.begin() + (history.size() - settings.historyMax));
}

static int64_t durationOf(const Job& j) { return j.durationSec > 0 ? j.durationSec : 600; }

bool Scheduler::begin(const Job& j, int64_t planned, int64_t now, int64_t end, Host& host) {
    RunRecord rec;
    rec.jobId = j.id; rec.jobName = j.name; rec.planned = planned; rec.start = now; rec.end = now;
    const bool rec_any = j.action.recordAudio || j.action.recordIq || j.action.recordLog;
    std::string dir = j.action.folder;
    if (rec_any) {
        if (host.freeBytes(dir) < settings.minFreeBytes) {
            rec.result = Result::DiskFull; rec.error = "not enough free disk space to start";
            warning_ = rec.error + " (" + j.name + ")"; host.warn(warning_);
            addRecord(rec);
            return false;
        }
    }
    before_ = host.snapshot();
    std::string err;
    if (!host.apply(j.action, err)) {
        rec.result = Result::Error; rec.error = err.empty() ? "could not tune the radio" : err;
        host.restore(before_);
        addRecord(rec);
        return false;
    }
    if (rec_any) {
        NameFields nf;
        nf.utc = now; nf.zoneOffsetSec = zone_(now); nf.mode = j.action.mode; nf.job = j.name; nf.freqHz = j.action.freqHz;
        nf.satellite = j.when.satellite; nf.counter = ++counter_; nf.kind = "rec";
        if (nf.mode.empty() || nf.freqHz <= 0) {
            const RadioState rs = host.snapshot();
            if (nf.mode.empty()) nf.mode = rs.mode;
            if (nf.freqHz <= 0) nf.freqHz = rs.freqHz;
        }
        RecordSpec rs;
        rs.audio = j.action.recordAudio; rs.iq = j.action.recordIq; rs.log = j.action.recordLog;
        rs.dir = dir; rs.baseName = expandName(j.action.namePattern, nf);
        if (!host.startRecording(rs, err)) {
            rec.result = Result::Error; rec.error = err.empty() ? "could not start the recording" : err;
            host.restore(before_);
            addRecord(rec);
            return false;
        }
    }
    rate_ = estimateBytesPerSec(j.action, host.iqBytesPerSec(), 1);
    cur_ = rec; cur_.end = 0; cur_.result = Result::Done;
    curEnd_ = end; curPriority_ = j.priority; active_ = true;
    return true;
}

void Scheduler::finish(int64_t now, Host& host, Result why, const std::string& err) {
    if (!active_) return;
    const Job* j = find(cur_.jobId);
    if (j && (j->action.recordAudio || j->action.recordIq || j->action.recordLog)) cur_.files = host.stopRecording();
    host.restore(before_);
    cur_.end = std::max(now, cur_.start);
    cur_.result = why; cur_.error = err;
    addRecord(cur_);
    active_ = false;
    cur_ = RunRecord();
}

void Scheduler::stopActive(int64_t now, Host& host, Result why) { finish(now, host, why, ""); }

void Scheduler::collectDue(int64_t now, Host& host) {
    const int64_t base0 = lastTick_ > 0 ? lastTick_ : (settings.lastSeen > 0 && settings.lastSeen < now ? settings.lastSeen : now);
    const int lateOk = std::max(2 * settings.tickSec, 10);
    for (const Job& j : jobs) {
        if (!j.enabled) continue;
        const bool pass = j.when.kind == Schedule::Pass;
        auto it = nextDue_.find(j.id);
        if (it == nextDue_.end()) {
            if (pass) {
                if (nextQuery_.count(j.id) && nextQuery_[j.id] > now) continue;
                const auto ps = host.passes(j.when.satellite, j.when.minElevationDeg, base0, now + settings.passHorizonSec);
                bool got = false;
                for (const PassWindow& w : ps) {
                    const int64_t st = w.aos - j.when.leadSec;
                    if (st > base0 && !got) { nextDue_[j.id] = st; passEnd_[j.id] = w.los; got = true; }
                }
                if (!got) { nextQuery_[j.id] = now + 600; continue; }
            } else {
                const int64_t n = nextRun(j.when, zone_, base0);
                if (n < 0) continue;
                nextDue_[j.id] = n;
            }
            it = nextDue_.find(j.id);
        }
        if (it->second > now) continue;
        // due. Which occurrences were missed because nobody looked (sleep, closed)?
        std::vector<PlannedRun> missed;
        const int64_t first = it->second;
        std::vector<int64_t> starts;
        if (pass) starts.push_back(first);
        else starts = runsBetween(j.when, zone_, first - 1, now, 200);
        for (int64_t st : starts) {
            PlannedRun r;
            r.jobId = j.id; r.priority = j.priority; r.fixedEnd = j.fixedEnd || pass;
            r.start = st;
            r.end = pass ? passEnd_[j.id] : st + durationOf(j);
            if (now - st <= lateOk || (pass && now < r.end && now - st <= lateOk)) { pending_.push_back(r); continue; }
            missed.push_back(r);
        }
        if (!missed.empty()) {
            const auto late = missedRuns(j, missed, now, settings.conflict.minRunSec);
            const int64_t keep = late.empty() ? -1 : missed.back().start;
            size_t shown = 0;
            for (const PlannedRun& m : missed) {
                if (m.start == keep) continue;
                if (++shown > 20) break;
                RunRecord rr; rr.jobId = j.id; rr.jobName = j.name; rr.planned = m.start;
                rr.result = Result::Missed; rr.error = "the program was not running or the computer was asleep";
                addRecord(rr);
            }
            for (const PlannedRun& l : late) pending_.push_back(l);
        }
        if (pass) {
            nextDue_.erase(j.id);
            // the next pass is looked up with this pass as the base
            const auto ps = host.passes(j.when.satellite, j.when.minElevationDeg, std::max(now, passEnd_[j.id]), now + settings.passHorizonSec);
            for (const PassWindow& w : ps) {
                const int64_t st = w.aos - j.when.leadSec;
                if (st > now) { nextDue_[j.id] = st; passEnd_[j.id] = w.los; break; }
            }
            if (!nextDue_.count(j.id)) nextQuery_[j.id] = now + 600;
        } else {
            const int64_t n = nextRun(j.when, zone_, std::max(now, first));
            if (n < 0) nextDue_[j.id] = INT64_MAX; else nextDue_[j.id] = n;
        }
    }
}

void Scheduler::tick(int64_t now, Host& host) {
    if (lastTick_ > 0 && wokeUp(lastTick_, now, settings.tickSec)) {
        warning_ = now < lastTick_ ? "the clock went back" : "woke up after " + std::to_string((now - lastTick_) / 60) + " min";
        host.warn(warning_);
    }
    collectDue(now, host);
    lastTick_ = now;
    settings.lastSeen = now;
    const ConflictPolicy& cp = settings.conflict;

    if (active_) {
        if (now >= curEnd_) finish(now, host, Result::Done, "");
        else {
            const Job* j = find(cur_.jobId);
            if (j && (j->action.recordAudio || j->action.recordIq || j->action.recordLog)) {
                const DiskState d = diskGuard(host.freeBytes(j->action.folder), settings.minFreeBytes, rate_);
                if (d == DiskState::Stop) {
                    warning_ = "free disk space is below the limit: the recording of " + j->name + " was stopped";
                    host.warn(warning_);
                    finish(now, host, Result::DiskFull, "free disk space below the limit");
                } else if (d == DiskState::Warn && warning_.find("running out") == std::string::npos) {
                    warning_ = "disk space is running out during " + j->name;
                    host.warn(warning_);
                }
            }
        }
    }

    // stale or hopeless entries in the queue
    for (size_t i = 0; i < pending_.size();) {
        const PlannedRun& p = pending_[i];
        const Job* j = find(p.jobId);
        const bool stale = !j || now - p.start > cp.maxDelaySec || (p.fixedEnd && p.end - now < cp.minRunSec) || !j->enabled;
        if (stale) {
            if (j) {
                RunRecord rr; rr.jobId = j->id; rr.jobName = j->name; rr.planned = p.start; rr.result = Result::Skipped;
                rr.error = p.fixedEnd && p.end - now < cp.minRunSec ? "the window was over before it could start" : "waited too long behind another job";
                addRecord(rr);
            }
            pending_.erase(pending_.begin() + i);
        } else i++;
    }
    if (pending_.empty()) return;
    auto better = [](const PlannedRun& a, const PlannedRun& b) { return a.priority != b.priority ? a.priority > b.priority : a.start < b.start; };
    std::sort(pending_.begin(), pending_.end(), better);
    if (active_ && pending_[0].priority > curPriority_) finish(now, host, Result::Preempted, "cut short by a higher priority job");
    if (!active_) {
        while (!pending_.empty() && !active_) {
            PlannedRun p = pending_.front();
            pending_.erase(pending_.begin());
            const Job* j = find(p.jobId);
            if (!j) continue;
            const int64_t dur = p.end - p.start;
            const int64_t end = p.fixedEnd ? p.end : now + dur;
            if (end - now < cp.minRunSec) continue;
            begin(*j, p.start, now, end, host);
        }
    }
    if (!cp.queue) {
        for (const PlannedRun& p : pending_) {
            const Job* j = find(p.jobId);
            if (!j) continue;
            RunRecord rr; rr.jobId = j->id; rr.jobName = j->name; rr.planned = p.start; rr.result = Result::Skipped; rr.error = "another job was running";
            addRecord(rr);
        }
        pending_.clear();
    }
}

bool Scheduler::runNow(const std::string& id, int64_t now, Host& host) {
    const Job* j = find(id);
    if (!j) return false;
    if (active_) {
        if (j->priority <= curPriority_) return false;
        finish(now, host, Result::Preempted, "cut short by " + j->name);
    }
    return begin(*j, now, now, now + durationOf(*j), host);
}

int64_t Scheduler::nextRunOf(const Job& j, int64_t now, Host& host) const {
    if (j.when.kind == Schedule::Pass) {
        for (const PassWindow& w : host.passes(j.when.satellite, j.when.minElevationDeg, now, now + settings.passHorizonSec))
            if (w.aos - j.when.leadSec > now) return w.aos - j.when.leadSec;
        return -1;
    }
    return nextRun(j.when, zone_, now);
}

std::vector<PlannedRun> Scheduler::upcoming(int64_t now, int64_t horizonSec, Host& host) const {
    std::vector<PlannedRun> all;
    for (const Job& j : jobs) {
        if (!j.enabled) continue;
        if (j.when.kind == Schedule::Pass) {
            for (const PassWindow& w : host.passes(j.when.satellite, j.when.minElevationDeg, now, now + horizonSec)) {
                PlannedRun r; r.jobId = j.id; r.priority = j.priority; r.fixedEnd = true;
                r.start = w.aos - j.when.leadSec; r.end = w.los;
                if (r.start > now) all.push_back(r);
            }
        } else {
            for (int64_t st : runsBetween(j.when, zone_, now, now + horizonSec, 60)) {
                PlannedRun r; r.jobId = j.id; r.priority = j.priority; r.fixedEnd = j.fixedEnd;
                r.start = st; r.end = st + durationOf(j);
                all.push_back(r);
            }
        }
    }
    return resolveOverlaps(all, settings.conflict);
}

// ---- JSON --------------------------------------------------------------------------------------------------------------------------
static js::Value jobToValue(const Job& j) {
    js::Value v = js::Value::object();
    v.set("id", js::Value::string(j.id)); v.set("name", js::Value::string(j.name));
    v.set("enabled", js::Value::boolean(j.enabled)); v.set("priority", js::Value::number(j.priority));
    v.set("durationSec", js::Value::number(j.durationSec)); v.set("missed", js::Value::string(j.missed == Missed::RunLate ? "run-late" : "skip"));
    v.set("graceSec", js::Value::number(j.graceSec)); v.set("fixedEnd", js::Value::boolean(j.fixedEnd));
    js::Value w = js::Value::object();
    static const char* kn[] = {"once", "every", "daily", "weekly", "cron", "pass"};
    w.set("kind", js::Value::string(kn[j.when.kind]));
    w.set("at", js::Value::number((double)j.when.at)); w.set("everySec", js::Value::number((double)j.when.everySec));
    w.set("hour", js::Value::number(j.when.hour)); w.set("minute", js::Value::number(j.when.minute));
    w.set("weekdays", js::Value::number(j.when.weekdays)); w.set("cron", js::Value::string(j.when.cron));
    w.set("satellite", js::Value::string(j.when.satellite)); w.set("minElevationDeg", js::Value::number(j.when.minElevationDeg));
    w.set("leadSec", js::Value::number(j.when.leadSec));
    v.set("when", w);
    js::Value a = js::Value::object();
    a.set("freqHz", js::Value::number(j.action.freqHz)); a.set("mode", js::Value::string(j.action.mode));
    a.set("radio", js::Value::string(j.action.radio)); a.set("gainDb", js::Value::number(j.action.gainDb));
    a.set("recordAudio", js::Value::boolean(j.action.recordAudio)); a.set("recordIq", js::Value::boolean(j.action.recordIq));
    a.set("recordLog", js::Value::boolean(j.action.recordLog)); a.set("namePattern", js::Value::string(j.action.namePattern));
    a.set("folder", js::Value::string(j.action.folder));
    v.set("action", a);
    return v;
}

std::string toJson(const std::vector<Job>& jobs, const Settings& s) {
    js::Value root = js::Value::object();
    root.set("version", js::Value::number(1));
    js::Value st = js::Value::object();
    st.set("queue", js::Value::boolean(s.conflict.queue)); st.set("maxDelaySec", js::Value::number(s.conflict.maxDelaySec));
    st.set("minRunSec", js::Value::number(s.conflict.minRunSec)); st.set("minFreeBytes", js::Value::number((double)s.minFreeBytes));
    st.set("lastSeen", js::Value::number((double)s.lastSeen));
    root.set("settings", st);
    js::Value arr = js::Value::array();
    for (const Job& j : jobs) arr.push(jobToValue(j));
    if (jobs.empty()) arr.type = js::Value::Arr;
    root.set("jobs", arr);
    return js::dump(root, true);
}

bool fromJson(const std::string& text, std::vector<Job>& jobs, Settings& s, std::string& err) {
    js::Value root;
    if (!js::parse(text, root, err)) return false;
    jobs.clear();
    if (const js::Value* st = root.get("settings")) {
        s.conflict.queue = st->flag("queue", true); s.conflict.maxDelaySec = (int)st->num("maxDelaySec", 1800);
        s.conflict.minRunSec = (int)st->num("minRunSec", 30); s.minFreeBytes = (int64_t)st->num("minFreeBytes", (double)s.minFreeBytes);
        s.lastSeen = (int64_t)st->num("lastSeen", 0);
    }
    const js::Value* arr = root.get("jobs");
    if (!arr || arr->type != js::Value::Arr) { err = "no jobs list"; return false; }
    for (const js::Value& v : arr->a) {
        Job j;
        j.id = v.str("id"); j.name = v.str("name"); j.enabled = v.flag("enabled", true); j.priority = (int)v.num("priority");
        j.durationSec = (int)v.num("durationSec", 600); j.missed = v.str("missed") == "run-late" ? Missed::RunLate : Missed::Skip;
        j.graceSec = (int)v.num("graceSec", 900); j.fixedEnd = v.flag("fixedEnd");
        if (const js::Value* w = v.get("when")) {
            const std::string k = w->str("kind", "once");
            j.when.kind = k == "every" ? Schedule::Every : k == "daily" ? Schedule::Daily : k == "weekly" ? Schedule::Weekly : k == "cron" ? Schedule::Cron : k == "pass" ? Schedule::Pass : Schedule::Once;
            j.when.at = (int64_t)w->num("at"); j.when.everySec = (int64_t)w->num("everySec", 3600);
            j.when.hour = (int)w->num("hour"); j.when.minute = (int)w->num("minute"); j.when.weekdays = (int)w->num("weekdays", 0x7f);
            j.when.cron = w->str("cron"); j.when.satellite = w->str("satellite"); j.when.minElevationDeg = w->num("minElevationDeg", 10);
            j.when.leadSec = (int)w->num("leadSec");
        }
        if (const js::Value* a = v.get("action")) {
            j.action.freqHz = a->num("freqHz"); j.action.mode = a->str("mode"); j.action.radio = a->str("radio"); j.action.gainDb = a->num("gainDb", -1);
            j.action.recordAudio = a->flag("recordAudio"); j.action.recordIq = a->flag("recordIq"); j.action.recordLog = a->flag("recordLog");
            j.action.namePattern = a->str("namePattern"); j.action.folder = a->str("folder");
        }
        if (j.id.empty()) { err = "a job has no id"; return false; }
        jobs.push_back(std::move(j));
    }
    return true;
}

std::string historyToJson(const std::vector<RunRecord>& h) {
    js::Value arr = js::Value::array();
    for (const RunRecord& r : h) {
        js::Value v = js::Value::object();
        v.set("jobId", js::Value::string(r.jobId)); v.set("jobName", js::Value::string(r.jobName));
        v.set("planned", js::Value::number((double)r.planned)); v.set("start", js::Value::number((double)r.start)); v.set("end", js::Value::number((double)r.end));
        v.set("result", js::Value::number((int)r.result)); v.set("error", js::Value::string(r.error));
        js::Value fs = js::Value::array();
        for (const FileResult& f : r.files) {
            js::Value fv = js::Value::object();
            fv.set("path", js::Value::string(f.path)); fv.set("kind", js::Value::string(f.kind)); fv.set("bytes", js::Value::number((double)f.bytes));
            fs.push(fv);
        }
        fs.type = js::Value::Arr;
        v.set("files", fs);
        arr.push(v);
    }
    arr.type = js::Value::Arr;
    js::Value root = js::Value::object();
    root.set("history", arr);
    return js::dump(root, true);
}

bool historyFromJson(const std::string& text, std::vector<RunRecord>& h, std::string& err) {
    js::Value root;
    if (!js::parse(text, root, err)) return false;
    h.clear();
    const js::Value* arr = root.get("history");
    if (!arr || arr->type != js::Value::Arr) { err = "no history list"; return false; }
    for (const js::Value& v : arr->a) {
        RunRecord r;
        r.jobId = v.str("jobId"); r.jobName = v.str("jobName"); r.planned = (int64_t)v.num("planned"); r.start = (int64_t)v.num("start");
        r.end = (int64_t)v.num("end"); r.result = (Result)(int)v.num("result"); r.error = v.str("error");
        if (const js::Value* fs = v.get("files")) for (const js::Value& f : fs->a) r.files.push_back({f.str("path"), f.str("kind"), (uint64_t)f.num("bytes")});
        h.push_back(std::move(r));
    }
    return true;
}

}  // namespace dect2::sched
