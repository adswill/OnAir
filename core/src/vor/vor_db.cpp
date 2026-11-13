// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Navigation beacon list (CSV import) and the two-radial position fix (see vor_db.h).
#include "dect2/vor/vor_db.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include "dect2/common/str.h"

namespace dect2 {

namespace {
constexpr double kPi = 3.141592653589793, kR = 6371000.0;
double rad(double d) { return d * kPi / 180; }
double deg(double r) { return r * 180 / kPi; }

std::vector<std::string> csvLine(const std::string& t, size_t& pos) {   // one record; quotes and doubled quotes
    std::vector<std::string> f;
    std::string cur;
    bool q = false;
    while (pos < t.size()) {
        const char c = t[pos++];
        if (q) {
            if (c == '"') { if (pos < t.size() && t[pos] == '"') { cur += '"'; pos++; } else q = false; }
            else cur += c;
        } else if (c == '"') q = true;
        else if (c == ',' || c == ';') { f.push_back(cur); cur.clear(); }
        else if (c == '\n') break;
        else if (c != '\r') cur += c;
    }
    f.push_back(cur);
    return f;
}
std::string lower(std::string s) { return dect2::str::lower(std::move(s)); }
} // namespace

int navParseCsv(const std::string& text, std::vector<NavStation>& out, std::string& err) {
    size_t pos = 0;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF) pos = 3;   // UTF-8 byte order mark
    const auto head = csvLine(text, pos);
    int cId = -1, cName = -1, cType = -1, cFreq = -1, cFreqKhz = -1, cLat = -1, cLon = -1, cVar = -1, cSlaved = -1, cCountry = -1;
    for (int i = 0; i < (int)head.size(); i++) {
        const std::string h = lower(head[(size_t)i]);
        if (h == "ident" || h == "identifier" || h == "id_code" || h == "callsign") cId = i;
        else if (h == "name") cName = i;
        else if (h == "type") cType = i;
        else if (h == "frequency_khz") cFreqKhz = i;
        else if (h == "frequency" || h == "freq" || h == "frequency_mhz" || h == "frequency.value") cFreq = i;
        else if (h == "latitude_deg" || h == "latitude" || h == "lat") cLat = i;
        else if (h == "longitude_deg" || h == "longitude" || h == "lon" || h == "lng") cLon = i;
        else if (h == "slaved_variation_deg") cSlaved = i;
        else if (h == "magnetic_variation_deg" || h == "magneticdeclination" || h == "declination" || h == "magvar") cVar = i;
        else if (h == "iso_country" || h == "country") cCountry = i;
    }
    if (cId < 0 || (cFreq < 0 && cFreqKhz < 0) || cLat < 0 || cLon < 0) { err = "no ident, frequency, latitude and longitude columns in the header"; return 0; }
    int n = 0;
    while (pos < text.size()) {
        const auto f = csvLine(text, pos);
        auto get = [&](int c) { return c >= 0 && c < (int)f.size() ? f[(size_t)c] : std::string(); };
        if (get(cId).empty()) continue;
        double mhz = 0;
        if (cFreqKhz >= 0 && !get(cFreqKhz).empty()) mhz = atof(get(cFreqKhz).c_str()) / 1000;
        else { const double v = atof(get(cFreq).c_str()); mhz = v > 1000 ? v / 1000 : v; }
        if (!((mhz >= 74 && mhz <= 118) || (mhz >= 328 && mhz <= 336))) continue;
        NavStation s;
        s.ident = get(cId);
        for (auto& c : s.ident) c = (char)std::toupper((unsigned char)c);
        s.name = get(cName); s.type = get(cType); s.country = get(cCountry);
        s.freqMhz = mhz;
        s.lat = atof(get(cLat).c_str()); s.lon = atof(get(cLon).c_str());
        const std::string sv = get(cSlaved);
        s.magVarDeg = !sv.empty() ? atof(sv.c_str()) : atof(get(cVar).c_str());
        if (std::fabs(s.lat) > 90 || std::fabs(s.lon) > 180) continue;
        out.push_back(s);
        n++;
    }
    if (!n) err = "no VOR or ILS stations (74 - 118 or 328 - 336 MHz) in the file";
    return n;
}

const NavStation* navMatch(const std::vector<NavStation>& db, const std::string& ident, double freqMhz) {
    const NavStation* best = nullptr;
    double bd = 1e9;
    int count = 0;
    std::string id = ident;
    for (auto& c : id) c = (char)std::toupper((unsigned char)c);
    for (const auto& s : db) {
        // an ILS identification is keyed with a leading I ("IDXB"); some lists keep only "DXB"
        if (s.ident != id && !(id.size() == 4 && id[0] == 'I' && s.ident == id.substr(1))) continue;
        count++;
        const double d = freqMhz > 0 ? std::fabs(s.freqMhz - freqMhz) : 0;
        if (d < bd) { bd = d; best = &s; }
    }
    if (!best) return nullptr;
    if (freqMhz > 0) return bd < 0.03 ? best : nullptr;
    return count == 1 ? best : nullptr;
}

void navDestination(double lat, double lon, double brg, double dist, double& olat, double& olon) {
    const double d = dist / kR, b = rad(brg), p1 = rad(lat), l1 = rad(lon);
    const double p2 = std::asin(std::sin(p1) * std::cos(d) + std::cos(p1) * std::sin(d) * std::cos(b));
    const double l2 = l1 + std::atan2(std::sin(b) * std::sin(d) * std::cos(p1), std::cos(d) - std::sin(p1) * std::sin(p2));
    olat = deg(p2);
    olon = std::remainder(deg(l2), 360.0);
}
double navDistanceM(double lat1, double lon1, double lat2, double lon2) {
    const double dp = rad(lat2 - lat1), dl = rad(lon2 - lon1);
    const double a = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(rad(lat1)) * std::cos(rad(lat2)) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2 * kR * std::asin(std::min(1.0, std::sqrt(a)));
}
double navBearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = rad(lat1), p2 = rad(lat2), dl = rad(lon2 - lon1);
    const double b = std::atan2(std::sin(dl) * std::cos(p2), std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl));
    return std::fmod(deg(b) + 360, 360);
}

NavFix navFix(double lat1, double lon1, double brg1, double sig1, double lat2, double lon2, double brg2, double sig2) {
    NavFix f;
    // a local plane (east, north) around the midpoint of the stations: good to a fraction of the error for stations up to a few hundred km apart
    const double lat0 = 0.5 * (lat1 + lat2), lon0 = lon1 + 0.5 * std::remainder(lon2 - lon1, 360.0);
    const double kx = kR * std::cos(rad(lat0)) * kPi / 180, ky = kR * kPi / 180;
    const double x1 = std::remainder(lon1 - lon0, 360.0) * kx, y1 = (lat1 - lat0) * ky;
    const double x2 = std::remainder(lon2 - lon0, 360.0) * kx, y2 = (lat2 - lat0) * ky;
    const double d1x = std::sin(rad(brg1)), d1y = std::cos(rad(brg1)), d2x = std::sin(rad(brg2)), d2y = std::cos(rad(brg2));
    const double det = d1x * (-d2y) - (-d2x) * d1y;   // [d1 -d2] [t1 t2]' = s2 - s1
    f.crossDeg = deg(std::asin(std::min(1.0, std::fabs(det))));
    if (f.crossDeg < 3) { f.why = "the radials are nearly parallel"; return f; }
    const double bx = x2 - x1, by = y2 - y1;
    const double t1 = (bx * (-d2y) - (-d2x) * by) / det;
    const double t2 = (d1x * by - d1y * bx) / det;
    if (t1 <= 0 || t2 <= 0) { f.why = "the radials do not meet (one points away)"; return f; }
    const double px = x1 + t1 * d1x, py = y1 + t1 * d1y;
    f.lat = lat0 + py / ky;
    f.lon = std::remainder(lon0 + px / kx, 360.0);
    f.dist1M = t1; f.dist2M = t2;
    // n_i . (p - s_i) = 0 with n_i = (cos b, -sin b); a bearing error rotates the line about the station: offset t_i * dtheta
    const double a11 = d1y, a12 = -d1x, a21 = d2y, a22 = -d2x;
    const double ad = a11 * a22 - a12 * a21;
    const double i11 = a22 / ad, i12 = -a12 / ad, i21 = -a21 / ad, i22 = a11 / ad;
    const double v1 = std::pow(t1 * rad(sig1), 2), v2 = std::pow(t2 * rad(sig2), 2);
    const double cxx = i11 * i11 * v1 + i12 * i12 * v2, cxy = i11 * i21 * v1 + i12 * i22 * v2, cyy = i21 * i21 * v1 + i22 * i22 * v2;
    const double tr = cxx + cyy, dd = std::sqrt(std::max(0.0, (cxx - cyy) * (cxx - cyy) / 4 + cxy * cxy));
    const double l1 = tr / 2 + dd, l2 = std::max(0.0, tr / 2 - dd);
    f.sigMajorM = std::sqrt(l1); f.sigMinorM = std::sqrt(l2);
    const double ang = 0.5 * std::atan2(2 * cxy, cxx - cyy);   // from east, anticlockwise
    f.majorAzDeg = std::fmod(90 - deg(ang) + 360, 180);
    f.ok = true;
    return f;
}

} // namespace dect2
