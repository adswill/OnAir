// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// cdma2000 / IS-95: pilot PN sequences, the K = 9 code, CRC-30, the interleaver and the Sync Channel Message (cdma.h for the references).
#include "dect2/cdma/cdma.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>

namespace dect2::cdma {

namespace {
// A maximum length sequence from the recursion of C.S0002 3.1.3.1.13.1, with the extra '0' after the run of 14 zeros, rotated so that
// index 0 is the first '1' after the 15 zeros.
std::vector<uint8_t> makePn(const int* taps, int ntaps) {
    std::vector<uint8_t> m(32767 + 15);
    for (int i = 0; i < 15; i++) m[(size_t)i] = i == 14;   // any non-zero start
    for (size_t n = 15; n < m.size(); n++) {
        int v = 0;
        for (int t = 0; t < ntaps; t++) v ^= m[n - (size_t)taps[t]];
        m[n] = (uint8_t)v;
    }
    std::vector<uint8_t> s(m.begin(), m.begin() + 32767);   // one period
    // the run of 14 zeros: find its end
    int run = 0; size_t end = 0;
    for (size_t k = 0; k < 2 * s.size(); k++) {
        if (s[k % s.size()] == 0) { if (++run == 14) { end = k % s.size(); break; } }
        else run = 0;
    }
    // index 0 = the first '1' after the zeros, then one more '0' at the end makes the run 15 long
    std::vector<uint8_t> out; out.reserve(kPnLen);
    for (size_t k = 1; k <= s.size(); k++) out.push_back(s[(end + k) % s.size()]);
    out.push_back(0);
    return out;
}
}  // namespace

const std::vector<uint8_t>& pnI() {
    static const std::vector<uint8_t> s = [] { const int t[] = {15, 10, 8, 7, 6, 2}; return makePn(t, 6); }();
    return s;
}
const std::vector<uint8_t>& pnQ() {
    static const std::vector<uint8_t> s = [] { const int t[] = {15, 12, 11, 10, 9, 5, 4, 3}; return makePn(t, 8); }();
    return s;
}

void ConvEnc::push(int b, Bits& out) {
    sr = ((sr << 1) | (uint32_t)(b & 1)) & 0x1FF;   // bit 0 = the newest input; g's MSB (octal 753 = 1 1110 1011) taps the newest bit
    auto par = [](uint32_t v) { return __builtin_popcount(v) & 1; };
    // g as written: bit 8 = the current input ... bit 0 = the oldest (8 bits back); reverse sr to match
    uint32_t r = 0;
    for (int i = 0; i < 9; i++) r |= ((sr >> i) & 1) << (8 - i);
    out.push_back((uint8_t)par(r & 0753));
    out.push_back((uint8_t)par(r & 0561));
}

Bits viterbiK9(const std::vector<float>& llr) {
    const size_t T = llr.size() / 2;
    Bits out(T);
    if (!T) return out;
    // state = the last 8 inputs, bit 0 newest; next state = ((s << 1) | b) & 255
    static const auto tab = [] {
        std::array<std::array<uint8_t, 2>, 512> t{};
        for (uint32_t s = 0; s < 256; s++)
            for (int b = 0; b < 2; b++) {
                ConvEnc e; e.sr = s; Bits o; e.push(b, o);
                t[s * 2 + (uint32_t)b] = {o[0], o[1]};
            }
        return t;
    }();
    std::vector<float> m(256, 0.f), nm(256);
    std::vector<uint8_t> dec(T * 256);   // the previous state's top bit (which predecessor) per state per step
    for (size_t t = 0; t < T; t++) {
        const float a = llr[2 * t], b = llr[2 * t + 1];
        std::fill(nm.begin(), nm.end(), -std::numeric_limits<float>::infinity());
        for (uint32_t s = 0; s < 256; s++) {
            const float ms = m[s];
            for (int u = 0; u < 2; u++) {
                const auto& c = tab[s * 2 + (uint32_t)u];
                const float bm = (c[0] ? -a : a) + (c[1] ? -b : b);
                const uint32_t ns = ((s << 1) | (uint32_t)u) & 255;
                const float v = ms + bm;
                if (v > nm[ns]) { nm[ns] = v; dec[t * 256 + ns] = (uint8_t)(s >> 7); }
            }
        }
        float mx = nm[0];
        for (float v : nm) mx = std::max(mx, v);
        for (uint32_t s = 0; s < 256; s++) m[s] = nm[s] - mx;
    }
    uint32_t s = (uint32_t)(std::max_element(m.begin(), m.end()) - m.begin());
    for (size_t t = T; t-- > 0;) {
        out[t] = (uint8_t)(s & 1);
        s = (s >> 1) | ((uint32_t)dec[t * 256 + s] << 7);
    }
    return out;
}

uint32_t crc30(const Bits& b, size_t n) {
    const uint32_t poly = 0x2030B9C7;   // x30 + x29 + x21 + x20 + x15 + x13 + x12 + x11 + x8 + x7 + x6 + x2 + x + 1
    uint32_t r = 0x3FFFFFFF;
    for (size_t i = 0; i < n && i < b.size(); i++) {
        const uint32_t fb = ((r >> 29) & 1) ^ (b[i] & 1);
        r = (r << 1) & 0x3FFFFFFF;
        if (fb) r ^= poly;
    }
    return ~r & 0x3FFFFFFF;
}

int bro(int v, int m) { int r = 0; for (int i = 0; i < m; i++) r |= ((v >> i) & 1) << (m - 1 - i); return r; }
int interleaveAddr(int i, int m, int J) { return (1 << m) * (i % J) + bro(i / J, m); }

namespace {
void put(Bits& b, uint64_t v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((uint8_t)((v >> i) & 1)); }
struct Rd {
    const Bits& b; size_t p = 0;
    bool ok(int n) const { return p + (size_t)n <= b.size(); }
    uint64_t get(int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v = (v << 1) | b[p++]; return v; }
};
}  // namespace

Bits encodeSyncPdu(const SyncMsg& m) {
    Bits b;
    put(b, kMsgTypeSync, 8);
    put(b, (uint64_t)m.pRev, 8); put(b, (uint64_t)m.minPRev, 8); put(b, (uint64_t)m.sid, 15); put(b, (uint64_t)m.nid, 16); put(b, (uint64_t)m.pilotPn, 9);
    put(b, m.lcState, 42); put(b, m.sysTime, 36); put(b, (uint64_t)m.lpSec, 8); put(b, (uint64_t)(m.ltmOff & 63), 6); put(b, m.daylt, 1); put(b, (uint64_t)m.prat, 2);
    put(b, (uint64_t)m.cdmaFreq, 11); put(b, (uint64_t)m.extCdmaFreq, 11);
    while (b.size() % 8 != 2) b.push_back(0);   // PDU_PADDING: 8k + 2 bits
    return b;
}

Bits encapsulate(const Bits& pdu) {
    Bits c;
    put(c, (pdu.size() + 38) / 8, 8);
    c.insert(c.end(), pdu.begin(), pdu.end());
    const uint32_t r = crc30(c, c.size());
    put(c, r, 30);
    return c;
}

bool parseSyncPdu(const Bits& pdu, SyncMsg& m) {
    Rd r{pdu};
    if (!r.ok(8 + 173) || r.get(8) != kMsgTypeSync) return false;
    m.pRev = (int)r.get(8); m.minPRev = (int)r.get(8); m.sid = (int)r.get(15); m.nid = (int)r.get(16); m.pilotPn = (int)r.get(9);
    m.lcState = r.get(42); m.sysTime = r.get(36); m.lpSec = (int)r.get(8);
    int lo = (int)r.get(6); if (lo & 32) lo -= 64; m.ltmOff = lo;
    m.daylt = r.get(1) != 0; m.prat = (int)r.get(2); m.cdmaFreq = (int)r.get(11); m.extCdmaFreq = (int)r.get(11);
    return true;   // later protocol revisions may add fields: they are ignored (C.S0005 2.6.1.3)
}

void SyncReassembler::frame(const uint8_t* f, std::vector<Bits>& pdus) {
    if (f[0]) { buf.clear(); active = true; }
    else if (!active) return;
    buf.insert(buf.end(), f + 1, f + 32);
    if (buf.size() < 8) return;
    int len = 0;
    for (int i = 0; i < 8; i++) len = (len << 1) | buf[(size_t)i];
    if (len < 5) { buf.clear(); active = false; return; }
    if (buf.size() < (size_t)len * 8) return;
    const size_t nb = (size_t)len * 8 - 30;
    uint32_t got = 0;
    for (size_t i = nb; i < nb + 30; i++) got = (got << 1) | buf[i];
    if (crc30(buf, nb) == got) pdus.emplace_back(buf.begin() + 8, buf.begin() + (ptrdiff_t)nb);
    else crcFails++;
    buf.clear(); active = false;
}

std::string sysTimeText(uint64_t t80, int lpSec, int ltmOff, bool local) {
    // System Time starts at 1980-01-06 00:00:00 UTC (the GPS epoch, 315964800 s after the Unix epoch) and does not count leap seconds
    double s = (double)t80 * 0.08 - lpSec;
    if (local) s += ltmOff * 1800.0;
    const time_t u = (time_t)(315964800 + (int64_t)std::floor(s));
    struct tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &u);
#else
    gmtime_r(&u, &tm);
#endif
    char b[64];
    const int cs = (int)std::lround((s - std::floor(s)) * 100) % 100;
    snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d.%02d%s", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, cs, local ? "" : " UTC");
    return b;
}

double cdmaChannelHz(int bc, int n) {   // C.S0002 2.1.1.1.1 (band class 0), 2.1.1.1.2 (1), 2.1.1.1.5 (4)
    if (bc == 0) { if (n >= 1 && n <= 799) return (870.0 + 0.030 * n) * 1e6; if (n >= 991 && n <= 1023) return (870.0 + 0.030 * (n - 1023)) * 1e6; return 0; }
    if (bc == 1) return n >= 0 && n <= 1199 ? (1930.0 + 0.050 * n) * 1e6 : 0;
    if (bc == 4) return n >= 0 && n <= 599 ? (1840.0 + 0.050 * n) * 1e6 : 0;
    return 0;
}
int cdmaBandClassAt(double hz) {
    const double m = hz / 1e6;
    if (m >= 869 && m <= 894) return 0;
    if (m >= 1930 && m <= 1990) return 1;
    if (m >= 1840 && m <= 1870) return 4;
    return -1;
}
const char* pratText(int p) { return p == 0 ? "9600 bps" : p == 1 ? "4800 bps" : "reserved"; }

}  // namespace dect2::cdma
