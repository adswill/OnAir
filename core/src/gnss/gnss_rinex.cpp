// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// RINEX 3.04 observation output (see gnss_rinex.h).
#include "dect2/gnss/gnss_rinex.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {

namespace {
constexpr double kLight = 299792458.0;
constexpr double kWeek = 604800.0;
// the systems written, in this order, with their observation codes (code, Doppler, signal strength)
struct SysObs { int sys; char letter; const char* c; const char* d; const char* s; };
const SysObs kSys[] = {
    {GnssGps, 'G', "C1C", "D1C", "S1C"},
    {GnssGalileo, 'E', "C1B", "D1B", "S1B"},
    {GnssBeidou, 'C', "C2I", "D2I", "S2I"},
    {GnssQzss, 'J', "C1C", "D1C", "S1C"},
    {GnssSbas, 'S', "C1C", "D1C", "S1C"},
};
const SysObs* sysObs(int sys) {
    for (const SysObs& s : kSys) if (s.sys == sys) return &s;
    return nullptr;
}
std::string line(const std::string& body, const char* label) {
    char b[96];
    snprintf(b, sizeof b, "%-60.60s%-20.20s\n", body.c_str(), label);
    return b;
}
} // namespace

bool gnssRinexGpsTime(int sys, double txTow, double& gpsTow) {
    switch (sys) {
    case GnssGps: case GnssGalileo: case GnssQzss: case GnssSbas: gpsTow = txTow; return true;   // Galileo system time keeps the GPS seconds of the week
    case GnssBeidou: gpsTow = txTow + 14.0; return true;
    default: return false;
    }
}

void GnssRinexWriter::gpsToCalendar(int week, double tow, int& y, int& mo, int& d, int& h, int& mi, double& s) {
    const double wk = std::floor(tow / kWeek);
    week += (int)wk; tow -= wk * kWeek;
    const long dayOfWeek = (long)std::floor(tow / 86400.0);
    double sod = tow - (double)dayOfWeek * 86400.0;
    long z = 3657L + (long)week * 7 + dayOfWeek;      // days since 1970-01-01 (the GPS epoch, 1980-01-06, is day 3657)
    // civil from days (proleptic Gregorian)
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const long doe = z - era * 146097;
    const long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    mo = (int)(mp < 10 ? mp + 3 : mp - 9);
    y = (int)(yoe + era * 400 + (mo <= 2 ? 1 : 0));
    h = (int)(sod / 3600.0); sod -= h * 3600.0;
    mi = (int)(sod / 60.0); sod -= mi * 60.0;
    s = sod;
}

std::string GnssRinexWriter::epoch(const std::vector<GnssMeasurement>& m) {
    struct Obs { const SysObs* so; int num; double tow; double dop, cn0; };
    std::vector<Obs> obs;
    double tmax = -1e300;
    for (const GnssMeasurement& x : m) {
        const SysObs* so = sysObs(x.sys);
        double t = 0;
        if (!so || !gnssRinexGpsTime(x.sys, x.txTow, t) || !std::isfinite(t)) continue;
        obs.push_back({so, gnssRinexNumber(x.sys, x.prn), t, x.dopplerHz, x.cn0});
    }
    if (obs.empty()) return "";
    const double rxTime = m.front().rxTime;
    if (!started_) {
        for (const Obs& o : obs) tmax = std::max(tmax, std::fmod(o.tow + kWeek, kWeek));
        t0_ = std::ceil((tmax + 0.070) * 1000.0 - 1e-6) / 1000.0;
        rx0_ = rxTime; week0_ = info_.gpsWeek; steerS_ = 0;
        started_ = true;
    }
    double T = t0_ + (rxTime - rx0_) + steerS_;      // seconds of GPS time from the start of week0_
    // each transmit time next to T (the seconds of the week roll over)
    for (Obs& o : obs) { while (o.tow - T > kWeek / 2) o.tow -= kWeek; while (T - o.tow > kWeek / 2) o.tow += kWeek; }
    double mean = 0;
    for (const Obs& o : obs) mean += T - o.tow;
    mean /= (double)obs.size();
    if (mean < 0.060 || mean > 0.095) {
        const double jump = std::round((0.075 - mean) * 1000.0) / 1000.0;
        steerS_ += jump; T += jump;
    }
    std::sort(obs.begin(), obs.end(), [](const Obs& a, const Obs& b) {
        return a.so != b.so ? a.so - kSys < b.so - kSys : a.num < b.num;
    });
    std::string body;
    int n = 0;
    char b[128];
    for (const Obs& o : obs) {
        const double pr = kLight * (T - o.tow);
        if (!(pr > 1.0e7 && pr < 5.0e7)) continue;     // not a pseudorange of a satellite in view: a wrong transmit time
        const int ssi = std::max(1, std::min(9, (int)std::floor(o.cn0 / 6.0)));
        snprintf(b, sizeof b, "%c%02d%14.3f %d%14.3f %d%14.3f %d\n", o.so->letter, o.num, pr, ssi, o.dop, ssi, o.cn0, ssi);
        body += b;
        sysSeen_ |= 1u << o.so->sys;
        n++;
    }
    if (n == 0) return "";
    int y, mo, d, h, mi; double s;
    gpsToCalendar(week0_, T, y, mo, d, h, mi, s);
    if (nEpochs_ == 0) { firstTow_ = T; firstWeek_ = week0_; }
    nEpochs_++;
    snprintf(b, sizeof b, "> %4d %02d %02d %02d %02d%11.7f  0%3d\n", y, mo, d, h, mi, s, n);
    return b + body;
}

std::string GnssRinexWriter::header() const {
    std::string h;
    char b[96];
    snprintf(b, sizeof b, "%9.2f%11s%-20s%-20s", 3.04, "", "OBSERVATION DATA", "M");
    h += line(b, "RINEX VERSION / TYPE");
    snprintf(b, sizeof b, "%-20.20s%-20.20s%-20.20s", info_.program.c_str(), "", info_.runDate.c_str());
    h += line(b, "PGM / RUN BY / DATE");
    h += line("Code only (no carrier phase). Synthetic or SDR data.", "COMMENT");
    h += line(info_.marker, "MARKER NAME");
    h += line("NON_GEODETIC", "MARKER TYPE");
    h += line("", "OBSERVER / AGENCY");
    snprintf(b, sizeof b, "%-20.20s%-20.20s%-20.20s", "0", "OnAir SDR", "");
    h += line(b, "REC # / TYPE / VERS");
    snprintf(b, sizeof b, "%-20.20s%-20.20s", "0", "UNKNOWN");
    h += line(b, "ANT # / TYPE");
    snprintf(b, sizeof b, "%14.4f%14.4f%14.4f", info_.approxEcef[0], info_.approxEcef[1], info_.approxEcef[2]);
    h += line(b, "APPROX POSITION XYZ");
    snprintf(b, sizeof b, "%14.4f%14.4f%14.4f", 0.0, 0.0, 0.0);
    h += line(b, "ANTENNA: DELTA H/E/N");
    for (const SysObs& s : kSys) {
        if (!(sysSeen_ >> s.sys & 1u)) continue;
        snprintf(b, sizeof b, "%c  %3d %s %s %s", s.letter, 3, s.c, s.d, s.s);
        h += line(b, "SYS / # / OBS TYPES");
    }
    h += line("DBHZ", "SIGNAL STRENGTH UNIT");
    int y = 0, mo = 0, d = 0, hh = 0, mi = 0; double s = 0;
    if (nEpochs_ > 0) gpsToCalendar(firstWeek_, firstTow_, y, mo, d, hh, mi, s);
    snprintf(b, sizeof b, "%6d%6d%6d%6d%6d%13.7f%5s%-3s", y, mo, d, hh, mi, s, "", "GPS");
    h += line(b, "TIME OF FIRST OBS");
    for (const SysObs& s2 : kSys) {
        if (!(sysSeen_ >> s2.sys & 1u)) continue;
        snprintf(b, sizeof b, "%c", s2.letter);
        h += line(b, "SYS / PHASE SHIFT");
    }
    h += line("", "END OF HEADER");
    return h;
}

std::string gnssRinexNavHeader(const std::string& program, const std::string& runDate) {
    char b[96];
    std::string h;
    snprintf(b, sizeof b, "%9.2f%11s%-20s%-20s", 3.04, "", "N: GNSS NAV DATA", "G: GPS");
    h += line(b, "RINEX VERSION / TYPE");
    snprintf(b, sizeof b, "%-20.20s%-20.20s%-20.20s", program.c_str(), "", runDate.c_str());
    h += line(b, "PGM / RUN BY / DATE");
    h += line("", "END OF HEADER");
    return h;
}

std::string gnssRinexNavGps(const GpsEphemeris& e, int week) {
    // the user range accuracy in metres from its index (IS-GPS-200 20.3.3.3.1.3)
    static const double kUra[16] = {2, 2.8, 4, 5.7, 8, 11.3, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 6144};
    // the clock reference time as a date: toc is in the week of toe, or the one next to it
    double toc = e.toc;
    int wk = week;
    if (toc - e.toe > 302400) wk--; else if (e.toe - toc > 302400) wk++;
    int y, mo, d, h, mi; double s;
    GnssRinexWriter::gpsToCalendar(wk, toc, y, mo, d, h, mi, s);
    std::string r;
    char b[128];
    snprintf(b, sizeof b, "G%02d %04d %02d %02d %02d %02d %02d%19.12E%19.12E%19.12E\n", e.prn, y, mo, d, h, mi, (int)std::lround(s), e.af0, e.af1, e.af2);
    r += b;
    auto row = [&](double a, double b2, double c, double d2) { char q[96]; snprintf(q, sizeof q, "    %19.12E%19.12E%19.12E%19.12E\n", a, b2, c, d2); r += q; };
    row((double)e.iode(), e.crs, e.dn, e.m0);
    row(e.cuc, e.e, e.cus, e.sqrtA);
    row(e.toe, e.cic, e.omega0, e.cis);
    row(e.i0, e.crc, e.omega, e.omegaDot);
    row(e.idot, 1.0, (double)week, 0.0);
    row(kUra[e.uraIndex & 15], (double)e.health, e.tgd, (double)e.iodc);
    char q[96]; snprintf(q, sizeof q, "    %19.12E%19.12E\n", e.toe, e.fitFlag ? 6.0 : 4.0); r += q;
    return r;
}

} // namespace dect2
