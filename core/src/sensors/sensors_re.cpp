// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/sensors/sensors_re.h"
#include "dect2/protolab/protolab.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>

namespace dect2 { namespace sensors {

namespace {
uint32_t bitsAt(const std::vector<uint8_t>& p, int off, int n) {
    uint32_t v = 0;
    for (int i = off; i < off + n; i++) v = (v << 1) | ((p[(size_t)i >> 3] >> (7 - i % 8)) & 1u);
    return v;
}
bool cleanScale(double a) {       // the factors sensors actually use
    a = std::fabs(a);
    for (double s : {1.0, 0.5, 0.25, 0.2, 0.1, 0.05, 0.01, 0.02, 2.0, 10.0}) if (std::fabs(a / s - 1.0) < 0.004) return true;
    return false;
}
struct Fit { double a = 0, b = 0, r2 = 0; };
Fit fitLine(const std::vector<double>& x, const std::vector<double>& y) {
    Fit f; const size_t n = x.size();
    double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
    for (size_t i = 0; i < n; i++) { sx += x[i]; sy += y[i]; sxx += x[i] * x[i]; sxy += x[i] * y[i]; syy += y[i] * y[i]; }
    const double d = n * sxx - sx * sx;
    if (std::fabs(d) < 1e-12) return f;
    f.a = (n * sxy - sx * sy) / d; f.b = (sy - f.a * sx) / n;
    double res = 0, tot = 0; const double my = sy / n;
    for (size_t i = 0; i < n; i++) { const double e = y[i] - (f.a * x[i] + f.b); res += e * e; tot += (y[i] - my) * (y[i] - my); }
    f.r2 = tot > 0 ? 1.0 - res / tot : 0;
    return f;
}
bool extract(const std::vector<uint8_t>& p, int off, int bits, int kind, double& v) {
    if (kind == 0) { v = bitsAt(p, off, bits); return true; }
    if (kind == 1) { if (bits != 16) return false; const uint32_t w = bitsAt(p, off, 16); v = ((w & 0xFF) << 8) | (w >> 8); return true; }
    if (bits % 4) return false;
    double o = 0;
    for (int i = 0; i < bits; i += 4) { const uint32_t d = bitsAt(p, off + i, 4); if (d > 9) return false; o = o * 10 + d; }
    v = o; return true;
}
void describe(ReField& f, const char* name) {
    char b[160];
    const char* k = f.kind == 2 ? "BCD " : f.kind == 1 ? "low byte first " : "";
    snprintf(b, sizeof b, "bits %d-%d %sx %g %+g (R2 %.4f)", f.bitOffset, f.bitOffset + f.bits - 1, k, f.scale, f.offset, f.r2);
    f.text = b;
    if (f.kind == 0) { snprintf(b, sizeof b, "get=@%d:{%d}:%s", f.bitOffset, f.bits, name); f.flexGet = b; }
}
}

ReChecksum reFindChecksum(const std::vector<std::vector<uint8_t>>& packets) {
    ReChecksum r;
    if (packets.size() < 3) return r;
    protolab::CrcOptions o; o.generic = true; o.maxStart = 4; o.maxSlack = 1; o.minPackets = 3;
    const auto hits = protolab::searchCrc(packets, -1, o);
    if (hits.empty()) return r;
    const auto& h = hits[0];
    r.found = true; r.name = h.name; r.sum = h.isChecksum; r.start = h.dataStart; r.width = h.width; r.position = h.coveredBytes;
    return r;
}

std::vector<ReField> reFindFields(const std::vector<std::vector<uint8_t>>& packets, const std::vector<double>& readings, const char* name, int maxResults) {
    std::vector<ReField> out;
    const size_t n = std::min(packets.size(), readings.size());
    if (n < 4 || std::set<double>(readings.begin(), readings.begin() + (long)n).size() < 4) return out;
    size_t minLen = packets[0].size();
    for (size_t i = 0; i < n; i++) minLen = std::min(minLen, packets[i].size());
    const int totalBits = (int)minLen * 8;
    for (int bits : {8, 10, 11, 12, 14, 16}) for (int off = 0; off + bits <= totalBits; off++) for (int kind : {0, 1, 2}) {
        if ((kind == 1 && (bits != 16 || off % 8)) || (kind == 2 && bits % 4)) continue;
        std::vector<double> x(n); bool ok = true;
        for (size_t i = 0; i < n && ok; i++) ok = extract(packets[i], off, bits, kind, x[i]);
        if (!ok || std::set<double>(x.begin(), x.end()).size() < 4) continue;
        const Fit f = fitLine(x, readings);
        if (f.r2 < 0.995 || !cleanScale(f.a)) continue;
        ReField r; r.bitOffset = off; r.bits = bits; r.kind = kind; r.scale = f.a; r.offset = std::fabs(f.b) < 1e-6 ? 0 : f.b; r.r2 = f.r2;
        describe(r, name);
        out.push_back(r);
    }
    // the shortest explanation first among equal fits: a field padded with constant bits fits as well
    std::sort(out.begin(), out.end(), [](const ReField& a, const ReField& b) { const double e = 1e-6; if (std::fabs(a.r2 - b.r2) > e) return a.r2 > b.r2; return a.bits != b.bits ? a.bits < b.bits : a.bitOffset < b.bitOffset; });
    if ((int)out.size() > maxResults) out.resize((size_t)maxResults);
    return out;
}

std::vector<ReField> reFindRamps(const std::vector<std::vector<uint8_t>>& packets, int maxResults) {
    std::vector<ReField> out;
    const size_t n = packets.size();
    if (n < 6) return out;
    size_t minLen = packets[0].size();
    for (const auto& p : packets) minLen = std::min(minLen, p.size());
    const int totalBits = (int)minLen * 8;
    for (int bits : {8, 12, 16}) for (int off = 0; off + bits <= totalBits; off += 4) {
        std::vector<double> x(n);
        for (size_t i = 0; i < n; i++) x[i] = bitsAt(packets[i], off, bits);
        const double range = *std::max_element(x.begin(), x.end()) - *std::min_element(x.begin(), x.end());
        if (range < 3 || range > (1 << bits) * 0.3) continue;
        int up = 0, down = 0, flat = 0; bool wild = false;
        for (size_t i = 1; i < n; i++) { const double d = x[i] - x[i - 1]; if (d > 0) up++; else if (d < 0) down++; else flat++; if (std::fabs(d) > 0.5 * range + 1) wild = true; }
        if (wild) continue;
        const int dom = std::max(up, down), against = std::min(up, down);
        if (dom < (int)n / 2 || against * 5 > dom) continue;
        std::vector<double> t(n); for (size_t i = 0; i < n; i++) t[i] = (double)i;
        const Fit f = fitLine(t, x);
        if (f.r2 < 0.8) continue;
        ReField r; r.bitOffset = off; r.bits = bits; r.r2 = f.r2; r.scale = 1;
        describe(r, "value");
        out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](const ReField& a, const ReField& b) { return a.r2 > b.r2; });
    if ((int)out.size() > maxResults) out.resize((size_t)maxResults);
    return out;
}

std::vector<std::vector<uint8_t>> reParseHexRows(const std::string& text) {
    std::vector<std::vector<uint8_t>> v;
    std::stringstream ss(text); std::string line;
    while (std::getline(ss, line)) {
        std::string h;
        bool bad = false;
        for (char c : line) { if (std::isxdigit((unsigned char)c)) h += c; else if (c != ' ' && c != '\r' && c != '\t' && c != ':') bad = true; }
        if (bad || h.size() < 4) continue;
        std::vector<uint8_t> b;
        for (size_t i = 0; i + 1 < h.size(); i += 2) b.push_back((uint8_t)std::stoi(h.substr(i, 2), nullptr, 16));
        v.push_back(std::move(b));
    }
    return v;
}

} } // namespace dect2::sensors
