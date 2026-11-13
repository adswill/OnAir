// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Transmitter from cell ID (see cell_tx.h).
#include "dect2/smart5/cell_tx.h"
#include <cmath>
#include <cstdio>
#include <sstream>

namespace dect2 {
namespace smart5 {

namespace {
const double kPi = 3.14159265358979323846;
// EN 300 468 6.2.7: latitude is a 16-bit two's complement number scaled 90 / 2^15 degrees, longitude 180 / 2^15;
// the 12-bit extents are unsigned in the same units
double lat16(unsigned v) { return (int16_t)v * 90.0 / 32768.0; }
double lon16(unsigned v) { return (int16_t)v * 180.0 / 32768.0; }
}

std::vector<NitCell> parseCellList(const uint8_t* p, size_t len) {
    std::vector<NitCell> out;
    size_t i = 0;
    while (i + 10 <= len) {
        NitCell c;
        c.cellId = (uint16_t)(p[i] << 8 | p[i + 1]);
        c.latDeg = lat16(p[i + 2] << 8 | p[i + 3]);
        c.lonDeg = lon16(p[i + 4] << 8 | p[i + 5]);
        const unsigned el = (unsigned)p[i + 6] << 4 | p[i + 7] >> 4;
        const unsigned eo = (unsigned)(p[i + 7] & 0x0F) << 8 | p[i + 8];
        c.extentLatDeg = el * 90.0 / 32768.0;
        c.extentLonDeg = eo * 180.0 / 32768.0;
        const size_t sub = p[i + 9];
        i += 10;
        out.push_back(c);
        const size_t end = i + sub;
        if (end > len) break;
        while (i + 8 <= end) {
            NitCell s;
            s.cellId = c.cellId;
            s.cellIdExtension = p[i];
            s.latDeg = lat16(p[i + 1] << 8 | p[i + 2]);
            s.lonDeg = lon16(p[i + 3] << 8 | p[i + 4]);
            s.extentLatDeg = ((unsigned)p[i + 5] << 4 | p[i + 6] >> 4) * 90.0 / 32768.0;
            s.extentLonDeg = ((unsigned)(p[i + 6] & 0x0F) << 8 | p[i + 7]) * 180.0 / 32768.0;
            out.push_back(s);
            i += 8;
        }
        i = end;
    }
    return out;
}

CellAnswer CellTxBook::lookup(uint16_t id, int ext) const {
    CellAnswer a;
    auto u = user_.find(id);
    if (u != user_.end()) { a.known = true; a.name = u->second; }
    auto n = nit_.find(key(id, ext));
    if (n == nit_.end() && ext >= 0) n = nit_.find(key(id, -1));
    if (n != nit_.end()) {
        a.known = true; a.fromNit = true;
        // the descriptor gives the south-west corner and the extent: report the centre of the cell
        a.latDeg = n->second.latDeg + n->second.extentLatDeg / 2;
        a.lonDeg = n->second.lonDeg + n->second.extentLonDeg / 2;
        if (haveUser_) {
            const double p1 = ulat_ * kPi / 180, p2 = a.latDeg * kPi / 180, dl = (a.lonDeg - ulon_) * kPi / 180;
            const double h = std::sin((p2 - p1) / 2) * std::sin((p2 - p1) / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
            a.distKm = 2 * 6371.0 * std::asin(std::sqrt(h));
            double br = std::atan2(std::sin(dl) * std::cos(p2), std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl)) * 180 / kPi;
            a.bearingDeg = br < 0 ? br + 360 : br;
        }
    }
    return a;
}

std::string CellTxBook::text(uint16_t id, int ext) const {
    const CellAnswer a = lookup(id, ext);
    char b[220];
    if (!a.known) { snprintf(b, sizeof b, "cell 0x%04X: not in the NIT or your book", id); return b; }
    std::string who = a.name.empty() ? "" : a.name + ", ";
    if (a.fromNit && a.distKm >= 0) snprintf(b, sizeof b, "cell 0x%04X: %scentre %.3f, %.3f, %.0f km at %.0f deg", id, who.c_str(), a.latDeg, a.lonDeg, a.distKm, a.bearingDeg);
    else if (a.fromNit) snprintf(b, sizeof b, "cell 0x%04X: %scentre %.3f, %.3f", id, who.c_str(), a.latDeg, a.lonDeg);
    else snprintf(b, sizeof b, "cell 0x%04X: %s", id, a.name.c_str());
    return b;
}

std::string CellTxBook::save() const {
    std::ostringstream o;
    for (auto& kv : user_) o << kv.first << '\t' << kv.second << '\n';
    return o.str();
}

void CellTxBook::load(const std::string& s) {
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        const size_t t = line.find('\t');
        if (t == std::string::npos) continue;
        try { user_[(uint16_t)std::stoul(line.substr(0, t))] = line.substr(t + 1); } catch (...) {}
    }
}

} // namespace smart5
} // namespace dect2
