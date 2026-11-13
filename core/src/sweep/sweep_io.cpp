// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/sweep/sweep.h"
#include "dect2/common/timecompat.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace dect2 {
namespace sweep {

namespace {
int64_t daysFromCivil(int y, unsigned m, unsigned d) {   // Howard Hinnant's algorithm
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}
}

bool writeCsv(const std::string& path, const std::vector<Sweep>& sweeps, std::string& err) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { err = "cannot write " + path; return false; }
    for (const auto& s : sweeps) {
        struct tm g;
        gmTime((time_t)s.timeSec, &g);
        const int U = std::max(1, s.binsPerHop > 0 ? s.binsPerHop : (int)s.bins());
        for (size_t a = 0; a < s.bins(); a += U) {
            const size_t n = std::min<size_t>(U, s.bins() - a);
            const double lo = s.freqOf(a) - s.binHz / 2, hi = lo + n * s.binHz;
            fprintf(f, "%04d-%02d-%02d, %02d:%02d:%02d, %.0f, %.0f, %.2f, %d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec,
                    lo, hi, s.binHz, std::max(1, s.fftSize * std::max(1, s.averages)));
            for (size_t j = 0; j < n; j++) fprintf(f, ", %.2f", s.db[a + j]);
            fprintf(f, "\n");
        }
    }
    const bool ok = fclose(f) == 0;
    if (!ok) err = "write failed";
    return ok;
}

bool readCsv(const std::string& path, std::vector<Sweep>& out, std::string& err) {
    std::ifstream in(path);
    if (!in) { err = "cannot read " + path; return false; }
    std::string line;
    Sweep cur;
    double prevHi = 0, prevLo = 0;
    bool have = false;
    auto finish = [&]() {
        if (!have || cur.db.empty()) return;
        cur.floorDb.assign(cur.db.size(), -150.f);
        const size_t U = cur.binsPerHop > 0 ? cur.binsPerHop : cur.db.size();
        std::vector<float> hf;
        for (size_t a = 0; a < cur.db.size(); a += U) {
            const size_t n = std::min(U, cur.db.size() - a);
            const float fl = noiseFloorDb(cur.db.data() + a, n, 50, 16);
            hf.push_back(fl);
            for (size_t j = 0; j < n; j++) cur.floorDb[a + j] = fl;
        }
        std::nth_element(hf.begin(), hf.begin() + hf.size() / 2, hf.end());
        cur.floorOverallDb = hf[hf.size() / 2];
        cur.hops = (int)hf.size();
        cur.signals = findSignals(cur, 8);
        out.push_back(std::move(cur));
        cur = Sweep();
        have = false;
    };
    int rows = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::vector<std::string> t;
        std::stringstream ss(line);
        std::string x;
        while (std::getline(ss, x, ',')) t.push_back(x);
        if (t.size() < 7) continue;
        int Y, M, D, h, m, sec;
        if (sscanf(t[0].c_str(), " %d-%d-%d", &Y, &M, &D) != 3 || sscanf(t[1].c_str(), " %d:%d:%d", &h, &m, &sec) != 3) continue;
        const double lo = atof(t[2].c_str()), hi = atof(t[3].c_str()), step = atof(t[4].c_str());
        if (step <= 0 || hi <= lo) continue;
        const size_t n = t.size() - 6;
        const double tsec = (double)daysFromCivil(Y, (unsigned)M, (unsigned)D) * 86400.0 + h * 3600 + m * 60 + sec;
        if (have && (lo <= prevLo || std::fabs(step - cur.binHz) > 1e-3 * step)) finish();   // frequency wrapped: a new sweep
        if (!have) {
            cur.startHz = lo + step / 2; cur.binHz = step; cur.timeSec = tsec;
            cur.fftSize = 0; cur.averages = 0; cur.binsPerHop = (int)n;
            have = true;
        } else {
            // fill a gap between rows with floor-less bins
            const double expect = cur.startHz + cur.db.size() * step - step / 2;
            const long gap = std::lround((lo - expect) / step);
            if (gap > 0 && gap < 1000000) cur.db.insert(cur.db.end(), (size_t)gap, -150.f);
        }
        for (size_t j = 0; j < n; j++) cur.db.push_back((float)atof(t[6 + j].c_str()));
        prevLo = lo; prevHi = hi;
        rows++;
    }
    (void)prevHi;
    finish();
    if (!rows) { err = "no rows"; return false; }
    return true;
}

bool writeSignalsCsv(const std::string& path, const std::vector<SignalRecord>& r, std::string& err) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { err = "cannot write " + path; return false; }
    fprintf(f, "freq_hz,bandwidth_hz,peak_dbfs,snr_db,max_snr_db,first_seen_unix,last_seen_unix,count\n");
    for (const auto& x : r) fprintf(f, "%.0f,%.0f,%.2f,%.2f,%.2f,%.0f,%.0f,%d\n", x.freqHz, x.bwHz, x.peakDb, x.snrDb, x.maxSnrDb, x.firstSeen, x.lastSeen, x.count);
    const bool ok = fclose(f) == 0;
    if (!ok) err = "write failed";
    return ok;
}

SweepDiff compareSweeps(const Sweep& a, const Sweep& b) {
    SweepDiff d;
    d.deltaDb.assign(a.bins(), 0.f);
    std::vector<float> fd;
    double sum = 0;
    size_t cnt = 0;
    bool first = true;
    for (size_t i = 0; i < a.bins(); i++) {
        const double f = a.freqOf(i);
        const long j = std::lround((f - b.startHz) / b.binHz);
        if (j < 0 || (size_t)j >= b.bins()) continue;
        const float dl = b.db[j] - a.db[i];
        d.deltaDb[i] = dl;
        sum += dl; cnt++;
        fd.push_back(b.floorDb[j] - a.floorDb[i]);
        if (first || dl > d.maxRiseDb) { d.maxRiseDb = dl; d.maxRiseHz = f; }
        if (first || dl < d.maxFallDb) { d.maxFallDb = dl; d.maxFallHz = f; }
        first = false;
    }
    if (cnt) {
        d.meanChangeDb = (float)(sum / cnt);
        std::nth_element(fd.begin(), fd.begin() + fd.size() / 2, fd.end());
        d.floorChangeDb = fd[fd.size() / 2];
    }
    return d;
}

} // namespace sweep
} // namespace dect2
