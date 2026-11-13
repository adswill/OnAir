// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// RINEX 3.04 observation output: the calendar of GPS weeks, the fixed columns of the header and the records, pseudoranges from the
// transmit times, BeiDou's 14 s, the week roll-over, the millisecond clock steering of a drifting sample clock, GLONASS left out.
#include "dect2/gnss/gnss_rinex.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static GnssMeasurement meas(int sys, int prn, double rx, double tx, double cn0 = 40) {
    GnssMeasurement m; m.sys = sys; m.prn = prn; m.rxTime = rx; m.txTow = tx; m.dopplerHz = -1234.5; m.cn0 = cn0; return m;
}

static std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> v; std::istringstream is(s); std::string l;
    while (std::getline(is, l)) v.push_back(l);
    return v;
}

int main() {
    {   // GPS week 2000 began on 2018-05-06; week 2400 + 3.5 days is Wednesday 2026-01-07 12:00
        int y, mo, d, h, mi; double s;
        GnssRinexWriter::gpsToCalendar(2000, 0, y, mo, d, h, mi, s);
        CHECK(y == 2018 && mo == 5 && d == 6 && h == 0 && mi == 0 && s == 0, "week 2000: %d-%d-%d", y, mo, d);
        GnssRinexWriter::gpsToCalendar(2400, 3.5 * 86400 + 61.25, y, mo, d, h, mi, s);
        CHECK(y == 2026 && mo == 1 && d == 7 && h == 12 && mi == 1 && std::fabs(s - 1.25) < 1e-9, "week 2400: %d-%d-%d %d:%d:%f", y, mo, d, h, mi, s);
        GnssRinexWriter::gpsToCalendar(2400, 604800.0 + 10, y, mo, d, h, mi, s);   // into the next week
        CHECK(y == 2026 && mo == 1 && d == 11 && h == 0 && mi == 0 && std::fabs(s - 10) < 1e-9, "roll-over: %d-%d-%d", y, mo, d);
    }
    {
        GnssRinexInfo info; info.gpsWeek = 2400; info.approxEcef[0] = 3.5e6;
        GnssRinexWriter w(info);
        const double tow = 302400.0;
        // travel times 68, 75, 80 ms; BeiDou's stamp is 14 s behind; GLONASS is not written
        std::vector<GnssMeasurement> e1 = {meas(GnssGps, 5, 10.0, tow - 0.068), meas(GnssGps, 12, 10.0, tow - 0.075, 33),
                                           meas(GnssBeidou, 19, 10.0, tow - 14.0 - 0.080), meas(GnssGlonass, 3, 10.0, 1000.0)};
        const std::string r1 = w.epoch(e1);
        const auto L = lines(r1);
        CHECK(L.size() == 4, "records: %zu\n%s", L.size(), r1.c_str());
        if (L.size() == 4) {
            CHECK(L[0].substr(0, 2) == "> " && L[0].size() == 35 && L[0].substr(32) == "  3", "epoch line '%s'", L[0].c_str());
            CHECK(L[1].substr(0, 3) == "G05" && L[2].substr(0, 3) == "G12" && L[3].substr(0, 3) == "C19", "order");
            // the receive time is put 70 ms after the latest transmit time, on a whole millisecond: tow + 2 ms
            const double p5 = atof(L[1].substr(3, 14).c_str()), p12 = atof(L[2].substr(3, 14).c_str()), p19 = atof(L[3].substr(3, 14).c_str());
            CHECK(std::fabs(p5 - 299792458.0 * 0.070) < 0.05, "G05 %.3f", p5);
            CHECK(std::fabs(p12 - p5 - 299792458.0 * 0.007) < 0.05 && std::fabs(p19 - p5 - 299792458.0 * 0.012) < 0.05, "differences %.3f %.3f", p12 - p5, p19 - p5);
            CHECK(L[1].size() == 3 + 3 * 16, "record width %zu", L[1].size());
            CHECK(L[2][3 + 15] == '5' && L[1][3 + 15] == '6', "signal strength digits '%c' '%c'", L[1][18], L[2][18]);
            CHECK(std::fabs(atof(L[1].substr(19, 14).c_str()) + 1234.5) < 1e-3 && std::fabs(atof(L[2].substr(35, 14).c_str()) - 33) < 1e-3, "Doppler / C/N0");
        }
        // a sample clock 200 ppm fast: the travel times grow 0.2 ms an epoch; the clock steps back by whole ms (once, 20 ms)
        double lastMean = 0; bool stepped = false; int steps = 0;
        for (int k = 1; k <= 200; k++) {
            const double rx = 10.0 + k * 1.0002;
            const double t = tow + k;
            const std::string r = w.epoch({meas(GnssGps, 5, rx, t - 0.068), meas(GnssGps, 12, rx, t - 0.075)});
            const auto Lk = lines(r);
            if (Lk.size() != 3) { CHECK(false, "epoch %d: %zu lines", k, Lk.size()); break; }
            const double m = 0.5 * (atof(Lk[1].substr(3, 14).c_str()) + atof(Lk[2].substr(3, 14).c_str())) / 299792458.0;
            CHECK(m > 0.059 && m < 0.096, "epoch %d: mean travel %.4f s", k, m);
            if (k > 1 && std::fabs(m - lastMean) > 0.0005) { stepped = true; steps++; CHECK(std::fabs(std::fabs(m - lastMean) - 0.020) < 0.0015, "step %.4f", m - lastMean); }
            lastMean = m;
        }
        CHECK(stepped && steps <= 2, "clock steps: %d", steps);
        // across the end of the week: the transmit times wrap to 0, the pseudoranges do not jump
        GnssRinexWriter w2(info);
        const std::string a = w2.epoch({meas(GnssGps, 7, 0.0, 604799.5 - 0.070)});
        const std::string b = w2.epoch({meas(GnssGps, 7, 1.0, 0.5 - 0.070)});
        const auto La = lines(a), Lb = lines(b);
        CHECK(La.size() == 2 && Lb.size() == 2 && std::fabs(atof(La[1].substr(3, 14).c_str()) - atof(Lb[1].substr(3, 14).c_str())) < 1e-3, "week wrap\n%s%s", a.c_str(), b.c_str());
        if (Lb.size() == 2) CHECK(Lb[0].substr(2, 10) == "2026 01 11", "next week's date '%s'", Lb[0].c_str());
        // nothing written: no satellite of a written system
        CHECK(w2.epoch({meas(GnssGlonass, 1, 2.0, 100.0)}).empty(), "GLONASS only");
        // the header: 80 columns at most, the labels at column 61, the systems seen with their observation types
        const std::string h = w.header();
        const auto H = lines(h);
        bool widthOk = true, gps = false, bds = false, gal = false, end = false, first = false;
        for (const auto& l : H) {
            if (l.size() > 80 || l.size() < 61) widthOk = false;
            const std::string label = l.size() > 60 ? l.substr(60) : "";
            if (label.rfind("SYS / # / OBS TYPES", 0) == 0) { if (l[0] == 'G') gps = l.substr(0, 18) == "G    3 C1C D1C S1C"; if (l[0] == 'C') bds = l.substr(0, 18) == "C    3 C2I D2I S2I"; if (l[0] == 'E') gal = true; }
            if (label.rfind("END OF HEADER", 0) == 0) end = true;
            if (label.rfind("TIME OF FIRST OBS", 0) == 0) first = l.substr(0, 18) == "  2026     1     7";
        }
        CHECK(widthOk && gps && bds && !gal && end && first, "header\n%s", h.c_str());
        CHECK(H.size() > 0 && H[0].substr(0, 9) == "     3.04" && H[0].substr(20, 16) == "OBSERVATION DATA" && H[0][40] == 'M', "version line '%s'", H.empty() ? "" : H[0].c_str());
    }
    return testSummary("gnss rinex");
}
