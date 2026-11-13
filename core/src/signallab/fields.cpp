// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal Lab stage 6: which bits change. Per-column statistics of the aligned frames, suggested field boundaries (constant runs,
// counters, ASCII text, few-valued enums, the checksum found in stage 5) and the interpretations shown in the bit viewer.
#include "dect2/signallab/signallab.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace dect2::signallab {

const char* fieldTypeName(FieldType t) {
    switch (t) {
        case FieldType::Constant: return "constant"; case FieldType::Counter: return "counter"; case FieldType::Checksum: return "checksum";
        case FieldType::Ascii: return "ascii"; case FieldType::Enum: return "enum"; case FieldType::Value: return "uint"; case FieldType::Bytes: return "bytes";
    }
    return "";
}

std::vector<ColumnStat> columnStats(const std::vector<Bits>& frames) {
    size_t L = 0;
    for (const Bits& f : frames) L = std::max(L, f.size());
    std::vector<ColumnStat> c(L);
    for (size_t k = 0; k < L; k++) {
        int ones = 0, n = 0, ch = 0, np = 0;
        int prev = -1;
        for (const Bits& f : frames) {
            if (k >= f.size()) { prev = -1; continue; }
            ones += f[k]; n++;
            if (prev >= 0) { np++; ch += f[k] != prev; }
            prev = f[k];
        }
        ColumnStat& s = c[k];
        s.n = n;
        s.p1 = n ? (double)ones / n : 0;
        const double p = s.p1;
        s.entropy = (p <= 0 || p >= 1) ? 0 : -(p * std::log2(p) + (1 - p) * std::log2(1 - p));
        s.change = np ? (double)ch / np : 0;
    }
    return c;
}

uint64_t bitsValue(const Bits& b, size_t start, size_t len, bool lsbFirst) {
    uint64_t v = 0;
    len = std::min<size_t>(len, 64);
    for (size_t i = 0; i < len && start + i < b.size(); i++) {
        if (lsbFirst) v |= (uint64_t)b[start + i] << i;
        else v = (v << 1) | b[start + i];
    }
    return v;
}

std::string interpretHex(const Bits& b, size_t start, size_t len, bool lsbFirst) {
    std::string o;
    char t[4];
    for (size_t i = 0; i < len; i += 8) {
        const size_t n = std::min<size_t>(8, len - i);
        uint64_t v = bitsValue(b, start + i, n, lsbFirst);
        if (!lsbFirst && n < 8) v <<= (8 - n);
        snprintf(t, sizeof t, "%02X", (unsigned)(v & 0xFF));
        if (!o.empty()) o += ' ';
        o += t;
    }
    return o;
}

std::string interpretAscii(const Bits& b, size_t start, size_t len, bool lsbFirst) {
    std::string o;
    for (size_t i = 0; i + 8 <= len; i += 8) {
        const unsigned v = (unsigned)bitsValue(b, start + i, 8, lsbFirst);
        o += (v >= 0x20 && v < 0x7F) ? (char)v : '.';
    }
    return o;
}

std::string interpretDecimal(const Bits& b, size_t start, size_t len, bool lsbFirst, bool isSigned) {
    len = std::min<size_t>(len, 64);
    const uint64_t v = bitsValue(b, start, len, lsbFirst);
    char t[32];
    if (isSigned && len > 0 && len < 64 && (v >> (len - 1)) & 1) snprintf(t, sizeof t, "%lld", (long long)(v | (~0ULL << len)));
    else if (isSigned && len == 64) snprintf(t, sizeof t, "%lld", (long long)v);
    else snprintf(t, sizeof t, "%llu", (unsigned long long)v);
    return t;
}

std::string interpretBits(const Bits& b, size_t start, size_t len, int group) {
    std::string o;
    for (size_t i = 0; i < len && start + i < b.size(); i++) {
        if (group > 0 && i && i % (size_t)group == 0) o += ' ';
        o += (char)('0' + b[start + i]);
    }
    return o;
}

Bits diffFrames(const Bits& a, const Bits& b) {
    Bits d(std::min(a.size(), b.size()));
    for (size_t i = 0; i < d.size(); i++) d[i] = (uint8_t)(a[i] ^ b[i]);
    return d;
}

namespace {
// values of [start, start+len) across frames, in order
std::vector<uint64_t> values(const std::vector<Bits>& fr, size_t start, size_t len, bool lsb) {
    std::vector<uint64_t> v;
    for (const Bits& f : fr) if (start + len <= f.size()) v.push_back(bitsValue(f, start, len, lsb));
    return v;
}
bool isCounter(const std::vector<uint64_t>& v, size_t len) {
    if (v.size() < 4) return false;
    const uint64_t m = len >= 64 ? ~0ULL : ((1ULL << len) - 1);
    int inc = 0;
    for (size_t i = 1; i < v.size(); i++) { const uint64_t d = (v[i] - v[i - 1]) & m; if (d == 1) inc++; }
    return inc >= (int)((double)(v.size() - 1) * 0.6);
}
}

std::vector<Field> suggestFields(const std::vector<Bits>& frames, const std::vector<ChecksumHit>& checks) {
    std::vector<Field> out;
    if (frames.empty()) return out;
    const std::vector<ColumnStat> cs = columnStats(frames);
    size_t L = SIZE_MAX;
    for (const Bits& f : frames) L = std::min(L, f.size());
    if (L == SIZE_MAX || L == 0) return out;
    // the checksum, when stage 5 found one in fixed-length frames
    size_t ckA = SIZE_MAX, ckB = SIZE_MAX;
    if (!checks.empty() && !checks[0].lengthFromEnd) {
        ckA = checks[0].coverEnd;
        ckB = ckA + (size_t)checkAlgos()[(size_t)checks[0].algo].width;
        if (ckB > L) ckA = ckB = SIZE_MAX;
    }
    auto addRegion = [&](size_t a, size_t b) {   // a varying region [a, b): split into bytes and classify
        size_t p = a;
        while (p < b) {
            // counter of 8 or 16 bits starting here?
            bool done = false;
            for (size_t len : {16, 8}) {
                if (p + len > b) continue;
                for (bool lsb : {false, true}) {
                    if (isCounter(values(frames, p, len, lsb), len)) {
                        Field f; f.start = p; f.length = len; f.type = FieldType::Counter; f.name = "counter";
                        out.push_back(f); p += len; done = true; break;
                    }
                }
                if (done) break;
            }
            if (done) continue;
            Field f; f.start = p; f.length = std::min<size_t>(8 - (p - a) % 8, b - p);
            const auto v = values(frames, p, f.length, false);
            std::set<uint64_t> distinct(v.begin(), v.end());
            if (f.length == 8) {
                size_t printable = 0;
                for (uint64_t x : v) if (x >= 0x20 && x < 0x7F) printable++;
                if (v.size() >= 3 && printable == v.size() && distinct.size() > 1) f.type = FieldType::Ascii;
                else if (v.size() >= 6 && distinct.size() <= 4) f.type = FieldType::Enum;
                else f.type = FieldType::Value;
            } else f.type = distinct.size() <= 4 && v.size() >= 6 ? FieldType::Enum : FieldType::Value;
            out.push_back(f);
            p += f.length;
        }
    };
    size_t i = 0;
    while (i < L) {
        if (i == ckA) {
            Field f; f.start = ckA; f.length = ckB - ckA; f.type = FieldType::Checksum; f.name = checkAlgos()[(size_t)checks[0].algo].name;
            out.push_back(f); i = ckB; continue;
        }
        const size_t lim = (i < ckA && ckA != SIZE_MAX) ? ckA : L;
        // byte by byte: a byte is constant when none of its bits changes (the fixed top bits of a counter stay in the counter)
        auto constByte = [&](size_t p) { for (size_t k = p; k < std::min(p + 8, lim); k++) if (cs[k].entropy >= 0.05) return false; return true; };
        size_t j = i;
        const bool c = constByte(i);
        while (j < lim && constByte(j) == c) j = std::min(j + 8, lim);
        if (c) { Field f; f.start = i; f.length = j - i; f.type = FieldType::Constant; f.name = "constant"; out.push_back(f); }
        else addRegion(i, j);
        i = j;
    }
    // merge neighbouring plain values into byte runs (a long payload is one field, not twenty)
    std::vector<Field> m;
    for (const Field& f : out) {
        if (!m.empty() && f.type == FieldType::Value && (m.back().type == FieldType::Value || m.back().type == FieldType::Bytes) &&
            m.back().start + m.back().length == f.start && m.back().length + f.length > 32) {
            m.back().length += f.length; m.back().type = FieldType::Bytes;
        } else if (!m.empty() && f.type == FieldType::Ascii && m.back().type == FieldType::Ascii && m.back().start + m.back().length == f.start) {
            m.back().length += f.length;
        } else m.push_back(f);
    }
    int n = 0;
    for (Field& f : m) {
        if (f.name.empty()) { char t[24]; snprintf(t, sizeof t, "field%d", ++n); f.name = t; }
        f.value = f.type == FieldType::Ascii ? interpretAscii(frames[0], f.start, f.length) : interpretHex(frames[0], f.start, f.length);
    }
    return m;
}

} // namespace dect2::signallab
