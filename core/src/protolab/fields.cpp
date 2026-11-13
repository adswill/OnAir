// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Protocol lab: field analysis across packets (constant / counter / checksum / varying bytes, entropy, correlation) and byte interpretations.
#include "dect2/protolab/protolab.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace dect2::protolab {

FieldReport analyseFields(const std::vector<std::vector<uint8_t>>& pk, int crcBytes) {
    FieldReport rep;
    if (pk.empty()) return rep;
    size_t minLen = pk[0].size(), maxLen = pk[0].size();
    for (const auto& p : pk) { minLen = std::min(minLen, p.size()); maxLen = std::max(maxLen, p.size()); }
    const size_t ncol = minLen;
    const double n = (double)pk.size();
    std::vector<std::vector<double>> colv(ncol);
    for (size_t c = 0; c < ncol; c++) {
        Column col;
        col.index = (int)c;
        std::map<uint8_t, int> hist;
        for (const auto& p : pk) { hist[p[c]]++; colv[c].push_back(p[c]); }
        col.distinct = (int)hist.size();
        for (auto& kv : hist) { const double q = kv.second / n; col.entropy -= q * std::log2(q); }
        col.constantValue = pk[0][c];
        if (col.distinct == 1) col.kind = FieldKind::Constant;
        else {
            // counter: the same non-zero step between consecutive packets most of the time
            std::map<int, int> steps;
            for (size_t i = 1; i < pk.size(); i++) steps[(pk[i][c] - pk[i - 1][c]) & 0xFF]++;
            int bestStep = 0, bestCount = 0;
            for (auto& kv : steps) if (kv.second > bestCount) { bestCount = kv.second; bestStep = kv.first; }
            if (pk.size() >= 4 && bestStep != 0 && bestCount >= (int)((n - 1) * 0.8)) { col.kind = FieldKind::Counter; col.counterStep = bestStep; }
            else if (col.distinct <= std::max(3, (int)(n / 3))) col.kind = FieldKind::Enum;
            else col.kind = FieldKind::Varying;
        }
        rep.cols.push_back(col);
    }
    if (crcBytes > 0 && minLen == maxLen)   // equal-length packets: the last bytes are the check value
        for (size_t c = ncol >= (size_t)crcBytes ? ncol - (size_t)crcBytes : 0; c < ncol; c++) rep.cols[c].kind = FieldKind::Checksum;
    // correlated columns
    if (pk.size() >= 5) {
        for (size_t a = 0; a < ncol; a++) {
            if (rep.cols[a].kind == FieldKind::Constant || rep.cols[a].kind == FieldKind::Checksum) continue;
            double ma = 0;
            for (double v : colv[a]) ma += v;
            ma /= n;
            for (size_t b = 0; b < a; b++) {
                if (rep.cols[b].kind == FieldKind::Constant || rep.cols[b].kind == FieldKind::Checksum) continue;
                double mb = 0;
                for (double v : colv[b]) mb += v;
                mb /= n;
                double sab = 0, saa = 0, sbb = 0;
                for (size_t i = 0; i < pk.size(); i++) {
                    sab += (colv[a][i] - ma) * (colv[b][i] - mb);
                    saa += (colv[a][i] - ma) * (colv[a][i] - ma);
                    sbb += (colv[b][i] - mb) * (colv[b][i] - mb);
                }
                if (saa <= 0 || sbb <= 0) continue;
                const double r = sab / std::sqrt(saa * sbb);
                if (std::abs(r) >= 0.9 && std::abs(r) > std::abs(rep.cols[a].correlation)) { rep.cols[a].correlatedWith = (int)b; rep.cols[a].correlation = r; }
            }
        }
    }
    for (const Column& c : rep.cols) {
        const char k = c.kind == FieldKind::Constant ? 'C' : c.kind == FieldKind::Counter ? 'N' : c.kind == FieldKind::Checksum ? 'K' : c.kind == FieldKind::Enum ? 'E' : 'V';
        if (!rep.layout.empty()) rep.layout += ' ';
        rep.layout += k;
    }
    return rep;
}

std::vector<Interpretation> interpret(const std::vector<uint8_t>& b) {
    std::vector<Interpretation> out;
    const size_t n = b.size();
    // ASCII runs of 4 or more printable characters
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j < n && b[j] >= 0x20 && b[j] < 0x7F) j++;
        if (j - i >= 4) {
            out.push_back({(int)i, "ASCII", std::string(b.begin() + (long)i, b.begin() + (long)j)});
            i = j;
        } else i = j > i ? j : i + 1;
    }
    // BCD runs: both nibbles 0..9, at least 3 bytes, not just one repeated value
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j < n && (b[j] >> 4) <= 9 && (b[j] & 15) <= 9) j++;
        if (j - i >= 3) {
            bool allSame = true;
            for (size_t k = i + 1; k < j; k++) allSame &= b[k] == b[i];
            bool text = true;   // digits and spaces as ASCII are also valid BCD: not interesting
            for (size_t k = i; k < j; k++) text &= b[k] >= 0x20 && b[k] <= 0x39;
            if (!allSame && !text) {
                std::string s;
                for (size_t k = i; k < j; k++) { s += (char)('0' + (b[k] >> 4)); s += (char)('0' + (b[k] & 15)); }
                out.push_back({(int)i, "BCD", s});
            }
            i = j;
        } else i = j > i ? j : i + 1;
    }
    // float32 candidates in a sensible range
    for (size_t i = 0; i + 4 <= n; i++) {
        for (int be = 0; be < 2; be++) {
            uint8_t q[4];
            for (int k = 0; k < 4; k++) q[k] = be ? b[i + (size_t)k] : b[i + 3 - (size_t)k];
            const uint32_t u = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
            float f;
            std::memcpy(&f, &u, 4);
            const float a = std::fabs(f);
            if (std::isfinite(f) && a >= 0.01f && a <= 100000.f && (u & 0x7FFFFF) != 0) {
                char t[48];
                snprintf(t, sizeof t, "%.4g", (double)f);
                out.push_back({(int)i, be ? "float32 BE" : "float32 LE", t});
            }
        }
    }
    return out;
}

std::string hexDump(const std::vector<uint8_t>& b) {
    std::string s;
    char t[4];
    for (size_t i = 0; i < b.size(); i++) {
        snprintf(t, sizeof t, "%02X", b[i]);
        if (i) s += ' ';
        s += t;
    }
    return s;
}

std::string bitDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    std::string la, lb, ld;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) {
        for (int k = 7; k >= 0; k--) {
            const int x = (a[i] >> k) & 1, y = (b[i] >> k) & 1;
            la += (char)('0' + x);
            lb += (char)('0' + y);
            ld += x != y ? '^' : ' ';
        }
        la += ' '; lb += ' '; ld += ' ';
    }
    std::string o = la + "\n" + lb + "\n" + ld;
    if (a.size() != b.size()) o += "\n(lengths differ: " + std::to_string(a.size()) + " and " + std::to_string(b.size()) + " bytes)";
    return o;
}

}   // namespace dect2::protolab
