// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// aprs::parseFull(): parse() plus what a receiver needs on top of it (packet_aprs.h). Written from APRS101 (chapter 5: a '!' position may
// follow up to 40 characters of other text; chapter 17: third-party headers) and the NMEA 0183 RMC / GGA sentence layout.
#include "dect2/packet/packet_aprs.h"
#include <cctype>
#include <cstdlib>
#include <vector>

namespace dect2 {
namespace aprs {

namespace {

std::vector<std::string> split(const std::string& s, char c) {
    std::vector<std::string> v;
    size_t a = 0;
    for (;;) {
        const size_t b = s.find(c, a);
        v.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) return v;
        a = b + 1;
    }
}

// "4807.038" "N" -> 48.1173; false when empty or malformed
bool nmeaCoord(const std::string& v, const std::string& h, int degDigits, double& out) {
    if (v.size() < (size_t)degDigits + 2 || h.size() != 1) return false;
    for (size_t i = 0; i < v.size(); i++) if (!(isdigit((unsigned char)v[i]) || v[i] == '.')) return false;
    const double deg = atof(v.substr(0, (size_t)degDigits).c_str()), mn = atof(v.substr((size_t)degDigits).c_str());
    if (mn >= 60) return false;
    out = deg + mn / 60.0;
    if (h[0] == 'S' || h[0] == 'W') out = -out;
    else if (h[0] != 'N' && h[0] != 'E') return false;
    return true;
}

// $GPRMC / $GPGGA (any talker): the position of a raw GPS sentence
bool nmeaPosition(const std::string& s, Info& o) {
    if (s.size() < 7 || s[0] != '$') return false;
    std::string body = s.substr(1, s.find('*') == std::string::npos ? std::string::npos : s.find('*') - 1);
    const std::vector<std::string> f = split(body, ',');
    if (f.empty() || f[0].size() != 5) return false;
    const std::string kind = f[0].substr(2);
    double lat = 0, lon = 0;
    if (kind == "RMC" && f.size() >= 9) {
        if (f[2] != "A" || !nmeaCoord(f[3], f[4], 2, lat) || !nmeaCoord(f[5], f[6], 3, lon)) return false;
        if (!f[7].empty()) { o.hasSpeed = true; o.speedKnots = atof(f[7].c_str()); }
        if (!f[8].empty()) { o.hasCourse = true; o.courseDeg = (int)atof(f[8].c_str()); }
    } else if (kind == "GGA" && f.size() >= 10) {
        if (f[6].empty() || f[6] == "0" || !nmeaCoord(f[2], f[3], 2, lat) || !nmeaCoord(f[4], f[5], 3, lon)) return false;
        if (!f[9].empty()) { o.hasAlt = true; o.altM = atof(f[9].c_str()); }
    } else return false;
    o.hasPos = true; o.lat = lat; o.lon = lon;
    if (!o.symTable) { o.symTable = '/'; o.symCode = '>'; }   // no symbol in a raw sentence: a car, as most trackers that send them
    o.summary = "Raw GPS " + fmtLat(lat) + " " + fmtLon(lon) + " (" + kind + ")";
    return true;
}

} // namespace

bool parseFull(const std::string& dest, const std::string& info, Info& out, std::string* innerFrom, int depth) {
    if (innerFrom) innerFrom->clear();
    const bool ok = parse(dest, info, out);
    if (!ok) return false;
    if (out.type == "Other" && !info.empty() && info[0] != '!') {
        // a position after up to 40 characters of other text (old TNC beacons: "TheNet X1J4 (BFLD)!4903.50N/07201.75W-")
        const size_t bang = info.find('!');
        if (bang != std::string::npos && bang > 0 && bang < 40) {
            Info p;
            if (parse(dest, info.substr(bang), p) && p.hasPos) {
                p.comment = info.substr(0, bang) + (p.comment.empty() ? "" : " " + p.comment);
                out = p;
            }
        }
    } else if (out.type == "Raw GPS" && !out.hasPos) {
        Info p = out;
        if (nmeaPosition(info, p)) out = p;
    } else if (out.type == "Third-party" && depth < 2 && info.size() > 1) {
        // "}SRC>DEST,PATH:info": the packet a gateway passed on; out becomes the inner packet, innerFrom its source
        const std::string t = info.substr(1);
        const size_t colon = t.find(':'), gt = t.find('>');
        if (colon != std::string::npos && gt != std::string::npos && gt > 0 && gt < colon) {
            const std::string src = t.substr(0, gt);
            const size_t comma = t.find(',', gt);
            const std::string idest = t.substr(gt + 1, (comma != std::string::npos && comma < colon ? comma : colon) - gt - 1);
            Info in;
            std::string deeper;
            if (parseFull(idest, t.substr(colon + 1), in, &deeper, depth + 1)) {
                in.summary = "[third-party " + src + "] " + in.summary;
                out = in;
                if (innerFrom) *innerFrom = deeper.empty() ? src : deeper;
            }
        }
    }
    return true;
}

} // namespace aprs
} // namespace dect2
