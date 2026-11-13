// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pass report card: see pass_report.h.
#include "dect2/meteor/pass_report.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>

namespace dect2 {

namespace {
std::string iso(int64_t t) {
    const time_t tt = (time_t)t; struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char b[32]; std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &g); return b;
}
int64_t fromIso(const std::string& s) {
    int Y, M, D, h, m, sec;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%dZ", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
    Y -= M <= 2;
    const int64_t era = (Y >= 0 ? Y : Y - 399) / 400;
    const unsigned yoe = (unsigned)(Y - era * 400), doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + (int64_t)doe - 719468) * 86400 + h * 3600 + m * 60 + sec;
}
}  // namespace

PassCompare passCompare(const std::vector<PassReport>& h, const PassReport& p, double win) {
    PassCompare c;
    if (p.maxElDeg < 0) return c;
    for (const auto& x : h) {
        if (x.startUtc == p.startUtc || x.maxElDeg < 0 || std::fabs(x.maxElDeg - p.maxElDeg) > win || x.framesOk + x.framesBad == 0) continue;
        if (!p.satellite.empty() && !x.satellite.empty() && x.satellite.compare(0, 8, p.satellite, 0, 8) != 0) continue;   // the same family (METEOR-M ...)
        c.similar++; c.meanLines += x.lines; c.meanSync += x.syncPct(); c.meanSnr += x.snrMeanDb;
    }
    if (!c.similar) return c;
    c.meanLines /= c.similar; c.meanSync /= c.similar; c.meanSnr /= c.similar;
    const double dl = c.meanLines > 0 ? (p.lines - c.meanLines) / c.meanLines : 0, ds = p.syncPct() - c.meanSync;
    if (dl > 0.15 || ds > 10) c.verdict = "better than usual";
    else if (dl < -0.15 || ds < -10) c.verdict = "worse than usual";
    else c.verdict = "as usual";
    return c;
}

std::string passCsv(const std::vector<PassReport>& v, bool header) {
    std::string o = header ? "start,end,satellite,max_el,snr_mean,snr_max,frames_ok,frames_bad,sync_pct,packets_lost,lines,gap_lines\n" : "";
    char b[200];
    for (const auto& p : v) {
        std::string sat = p.satellite; for (char& c : sat) if (c == ',' || c == '\n') c = ' ';
        std::snprintf(b, sizeof b, ",%.1f,%.1f,%.1f,%llu,%llu,%.1f,%llu,%d,%d\n", p.maxElDeg, p.snrMeanDb, p.snrMaxDb, (unsigned long long)p.framesOk, (unsigned long long)p.framesBad, p.syncPct(),
                      (unsigned long long)p.packetsLost, p.lines, p.gapLines);
        o += iso(p.startUtc) + "," + iso(p.endUtc) + "," + sat + b;
    }
    return o;
}

std::vector<PassReport> passFromCsv(const std::string& text) {
    std::vector<PassReport> v;
    std::stringstream ss(text);
    std::string l;
    while (std::getline(ss, l)) {
        std::vector<std::string> f;
        std::stringstream ls(l);
        std::string x;
        while (std::getline(ls, x, ',')) f.push_back(x);
        if (f.size() < 12) continue;
        PassReport p;
        p.startUtc = fromIso(f[0]); p.endUtc = fromIso(f[1]);
        if (!p.startUtc) continue;
        p.satellite = f[2]; p.maxElDeg = std::atof(f[3].c_str()); p.snrMeanDb = (float)std::atof(f[4].c_str()); p.snrMaxDb = (float)std::atof(f[5].c_str());
        p.framesOk = std::strtoull(f[6].c_str(), nullptr, 10); p.framesBad = std::strtoull(f[7].c_str(), nullptr, 10); p.packetsLost = std::strtoull(f[9].c_str(), nullptr, 10);
        p.lines = std::atoi(f[10].c_str()); p.gapLines = std::atoi(f[11].c_str());
        v.push_back(p);
    }
    return v;
}

void PassTracker::update(int64_t utc, int state, float snr, uint64_t ok, uint64_t bad, uint64_t lost, int lines, int gaps) {
    if (!active_) {
        if (state < 3) { ok0_ = ok; bad0_ = bad; lost0_ = lost; return; }   // counts before the pass belong to nothing
        active_ = true;
        cur_ = PassReport();
        cur_.startUtc = utc;
        lastFrame_ = utc;
        snrSum_ = 0; snrN_ = 0;
        if (ok < ok0_ || bad < bad0_) { ok0_ = bad0_ = lost0_ = 0; }
    }
    if (ok > ok0_ + cur_.framesOk) lastFrame_ = utc;
    cur_.endUtc = utc;
    cur_.framesOk = ok >= ok0_ ? ok - ok0_ : ok;
    cur_.framesBad = bad >= bad0_ ? bad - bad0_ : bad;
    cur_.packetsLost = lost >= lost0_ ? lost - lost0_ : lost;
    cur_.lines = lines; cur_.gapLines = gaps;
    if (state >= 2) { snrSum_ += snr; snrN_++; cur_.snrMeanDb = (float)(snrSum_ / snrN_); cur_.snrMaxDb = std::max(cur_.snrMaxDb, snr); }
}

bool PassTracker::finish(int64_t utc, PassReport& out, int quietSec, bool force) {
    if (!active_) return false;
    if (!force && utc - lastFrame_ < quietSec) return false;
    active_ = false;
    ok0_ += cur_.framesOk; bad0_ += cur_.framesBad; lost0_ += cur_.packetsLost;
    if (cur_.framesOk == 0) return false;
    out = cur_;
    return true;
}

}  // namespace dect2
