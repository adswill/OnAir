// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The "Predicted course" NMEA stream (dect2/sonde/sonde_course.h): the course from the sonde to its landing, the points one step of flight
// time apart, the GGA / RMC checksums, the aim point, and the sequencer that starts over after the landing.
#include "dect2/sonde/sonde_course.h"
#include "../common/test_util.h"
#include <cmath>
#include <string>

using namespace dect2;
using namespace dect2::sondecourse;

static bool crcOk(const std::string& line) {
    const size_t st = line.find('$'), en = line.find('*');
    return st == 0 && en != std::string::npos && line.compare(en + 1, 2, sondenmea::checksum(line.substr(1, en - 1))) == 0 && line.compare(en + 3, std::string::npos, "\r\n") == 0;
}

int main() {
    // a course of 10 min: 10 km east, 5000 m down to 100 m, via two path points
    const double la = 54.0, lo = 25.0, dlon = 10000.0 / (111320.0 * std::cos(la * 3.14159265 / 180));
    const std::vector<Point> c = build(la, lo, 5000, 100, la, lo + dlon, 600, {{la, lo + dlon * 0.3}, {la, lo + dlon * 0.6}, {la, lo + dlon * 0.6}});   // a doubled point is dropped
    CHECK(c.size() == 4 && c.front().t == 0 && std::fabs(c.back().t - 600) < 1e-9 && std::fabs(c.back().altM - 100) < 1e-9 && c.front().altM == 5000, "build: start, 2 path points, landing (%zu)", c.size());
    CHECK(std::fabs(c[1].t - 180) < 1.5 && std::fabs(c[2].t - 360) < 1.5 && c[1].altM < 5000 && c[1].altM > c[2].altM, "time and height spread along the path (%.1f s)", c[1].t);
    const auto r = resample(c, 60);
    CHECK(r.size() == 11, "60 s steps over 600 s: 10 points and the landing (%zu)", r.size());
    for (size_t i = 0; i + 1 < r.size(); i++) CHECK(std::fabs(r[i].t - 60.0 * i) < 1e-9, "spacing at %zu", i);
    CHECK(std::fabs(r.back().lat - c.back().lat) < 1e-12 && std::fabs(r.back().lon - c.back().lon) < 1e-12 && r.back().t == 600, "ends on the landing");
    CHECK(std::fabs(distM(r[1].lat, r[1].lon, r[2].lat, r[2].lon) - 1000) < 15, "1 km per 60 s (%.1f m)", distM(r[1].lat, r[1].lon, r[2].lat, r[2].lon));
    CHECK(resample(c, 0).size() == 11 && resample(c, 700).size() == 2, "step 0 falls back to 60 s; a step longer than the flight gives start + landing");
    const auto seq = sentencesFor(r, 1'800'000'000.0);
    CHECK(seq.size() == r.size(), "one pair per point");
    for (const auto& t : seq) {
        const size_t nl = t.find("\r\n");
        CHECK(nl != std::string::npos && t.compare(0, 6, "$GPGGA") == 0 && t.find("$GPRMC") == nl + 2 && crcOk(t.substr(0, nl + 2)) && crcOk(t.substr(nl + 2)), "sentences and checksums: %s", t.c_str());
    }
    CHECK(seq[0].find("GPGGA,080000.00,") != std::string::npos, "first time is the start time: %s", seq[0].c_str());  
    // the course is east: RMC course about 90 degrees, 1000 m / 60 s = 32.4 knots
    const size_t rm = seq[1].find("$GPRMC");
    CHECK(seq[1].find(",32.4,90.0,", rm) != std::string::npos, "RMC speed and course: %s", seq[1].c_str() + rm);
    CHECK(seq.back().find(",0.0,90.0,") != std::string::npos, "the last point: speed 0, course kept: %s", seq.back().c_str());
    // aim point: the landing alone, always the same
    const std::string aim = aimSentences(c.back(), 1'800'000'000.0);
    CHECK(!aim.empty() && aim.find("GPGGA") == 1 && aim.find("100.0,M") != std::string::npos, "aim point: %s", aim.c_str());
    // sequencer: one per call, then again from the first
    Sequencer q;
    q.set(seq);
    for (size_t i = 0; i < seq.size(); i++) CHECK(q.next() == seq[i], "in order %zu", i);
    CHECK(q.next() == seq[0] && q.next() == seq[1], "loops");
    q.set(std::vector<std::string>(seq.begin(), seq.begin() + 2));   // a new, shorter course keeps the place modulo its length
    CHECK(q.index() < 2 && q.size() == 2, "replaced");
    Sequencer e; CHECK(e.next().empty() && e.size() == 0, "empty gives nothing");
    // degenerate: landing at the sonde -> two points, still valid
    const auto d = build(la, lo, 300, 100, la, lo, 5, {});
    CHECK(d.size() == 2 && sentencesFor(resample(d, 60), 1e9).size() == 2, "a landing on the spot");
    return testSummary("sonde_nmea_course");
}
