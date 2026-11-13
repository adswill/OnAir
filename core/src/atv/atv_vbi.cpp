// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/atv/atv_vbi.h"
#include "dect2/common/diag.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {

namespace {
constexpr double kTtxBitUs = 1.0 / 6.9375;
uint8_t ham84(int nib) {
    const int d1 = nib & 1, d2 = (nib >> 1) & 1, d3 = (nib >> 2) & 1, d4 = (nib >> 3) & 1;
    const int p1 = 1 ^ d1 ^ d3 ^ d4, p2 = 1 ^ d1 ^ d2 ^ d4, p3 = 1 ^ d1 ^ d2 ^ d3;
    const int p4 = 1 ^ p1 ^ d1 ^ p2 ^ d2 ^ p3 ^ d3 ^ d4;
    return (uint8_t)(p1 | d1 << 1 | p2 << 2 | d2 << 3 | p3 << 4 | d3 << 5 | p4 << 6 | d4 << 7);
}
uint8_t oddP(uint8_t c) { c &= 0x7F; int n = 0; for (int i = 0; i < 7; i++) n += (c >> i) & 1; return (uint8_t)(c | ((n & 1) ? 0 : 0x80)); }
const char* const kRunIn = "11111000111000111000111000111";    // run-in, 29 elements
const char* const kStart = "000111100011110000011111";          // start code, 24 elements
}

const char* atvWssAspectName(int b) {
    switch (b & 15) {
    case 0b0001: return "full format 4:3";
    case 0b1000: return "box 14:9 centre";
    case 0b0100: return "box 14:9 top";
    case 0b1101: return "box 16:9 centre";
    case 0b0010: return "box 16:9 top";
    case 0b1011: return "box wider than 16:9 centre";
    case 0b0111: return "full format 4:3 (shoot and protect 14:9)";
    case 0b1110: return "full format 16:9 (anamorphic)";
    default: return "not defined";
    }
}

std::vector<uint8_t> atvTtxPacketBits(int mag, int row, int page, const std::string& text) {
    std::vector<uint8_t> bytes = {ham84((mag & 7) | ((row & 1) << 3)), ham84((row >> 1) & 15)};
    if (row == 0) {
        bytes.push_back(ham84(page & 15)); bytes.push_back(ham84((page >> 4) & 15));
        for (int i = 0; i < 4; i++) bytes.push_back(ham84(0));
        bytes.push_back(ham84(0)); bytes.push_back(ham84(0));
        for (int i = 0; i < 32; i++) bytes.push_back(oddP(i < (int)text.size() ? (uint8_t)text[(size_t)i] : (uint8_t)' '));
    } else for (int i = 0; i < 40; i++) bytes.push_back(oddP(i < (int)text.size() ? (uint8_t)text[(size_t)i] : (uint8_t)' '));
    std::vector<uint8_t> b;
    for (int i = 0; i < 16; i++) b.push_back(i % 2 ? 0 : 1);
    for (char c : std::string("11100100")) b.push_back(c == '1');
    for (uint8_t v : bytes) for (int i = 0; i < 8; i++) b.push_back((v >> i) & 1);
    return b;
}

std::vector<uint8_t> atvWssElements(int a) {
    std::vector<uint8_t> e;
    for (const char* p = kRunIn; *p; p++) e.push_back(*p == '1');
    for (const char* p = kStart; *p; p++) e.push_back(*p == '1');
    for (int b = 0; b < 14; b++) {
        int v = b < 4 ? (a >> (3 - b)) & 1 : 0;
        for (int i = 0; i < 3; i++) e.push_back(v ? 1 : 0);
        for (int i = 0; i < 3; i++) e.push_back(v ? 0 : 1);
    }
    return e;
}

std::vector<uint8_t> atvCcBits(uint8_t c1, uint8_t c2) {
    std::vector<uint8_t> b = {0, 0, 1};
    for (uint8_t c : {oddP(c1), oddP(c2)}) for (int i = 0; i < 8; i++) b.push_back((c >> i) & 1);
    return b;
}

AtvVbi::AtvVbi() : ttx_(std::make_shared<TeletextDecoder>()) {}
void AtvVbi::reset() { st_ = AtvVbiStatus(); ttx_ = std::make_shared<TeletextDecoder>(); }

bool AtvVbi::tryTeletext(const Level& v) {
    const char* const pat = "101010101010101011100100";
    double best = -1e9, t0 = 0;
    // the penultimate 1 of the run-in is nominally 12 us after the sync; the reference may lie 1 us earlier or 0.4 later (clause 6.3)
    for (double t = 12.0 - 12.5 * kTtxBitUs - 1.2; t <= 12.0 - 12.5 * kTtxBitUs + 0.6; t += 0.02) {
        double s = 0;
        for (int i = 0; i < 24; i++) s += (pat[i] == '1' ? 1.0 : -1.0) * v(t + (i + 0.5) * kTtxBitUs);
        if (s > best) { best = s; t0 = t; }
    }
    double hi = 0, lo = 0; int nh = 0, nl = 0;
    for (int i = 0; i < 24; i++) { const float x = v(t0 + (i + 0.5) * kTtxBitUs); if (pat[i] == '1') { hi += x; nh++; } else { lo += x; nl++; } }
    hi /= nh; lo /= nl;
    if (hi - lo < 0.2) { DIAG_DEBUG_EVERY(409939, 10, "A teletext line was skipped because its clock run-in was too weak (contrast %.2f of at least 0.20).", hi - lo); return false; }
    const double thr = 0.5 * (hi + lo);
    for (int i = 16; i < 24; i++) if ((v(t0 + (i + 0.5) * kTtxBitUs) > thr) != (pat[i] == '1')) return false;
    uint8_t unit[44] = {};
    unit[0] = 0x00; unit[1] = 0xE4;
    double bitT = t0 + 24 * kTtxBitUs;
    for (int by = 0; by < 42; by++) {
        uint8_t val = 0;
        for (int i = 0; i < 8; i++) { const bool b = v(bitT + (i + 0.5) * kTtxBitUs) > thr; if (b) val |= (uint8_t)(0x80 >> i); }
        unit[2 + by] = val;
        bitT += 8 * kTtxBitUs;
    }
    if (ttxHamming84(ttxReverse8(unit[2])) < 0 && ttxHamming84(unit[2]) < 0) { DIAG_DEBUG_EVERY(424293, 10, "A teletext line was dropped because its magazine and row address failed the Hamming 8/4 check."); return false; }
    uint8_t pes[9 + 1 + 2 + 44] = {0, 0, 1, 0xBD, 0, 0, 0x80, 0, 0};
    pes[4] = 0; pes[5] = (uint8_t)(sizeof pes - 6);
    pes[9] = 0x10; pes[10] = 0x02; pes[11] = 44;
    std::memcpy(pes + 12, unit, 44);
    ttx_->feedPes(pes, sizeof pes);
    st_.ttxPackets++;
    return true;
}

bool AtvVbi::tryWss(const Level& v) {
    const double Ts = 0.2;
    std::string pre = std::string(kRunIn) + kStart;
    double best = -1e9, t0 = 0;
    for (double t = 10.5; t <= 11.6; t += 0.02) {
        double s = 0;
        for (size_t i = 0; i < pre.size(); i++) s += (pre[i] == '1' ? 1.0 : -1.0) * v(t + (i + 0.5) * Ts);
        if (s > best) { best = s; t0 = t; }
    }
    double hi = 0, lo = 0; int nh = 0, nl = 0;
    for (size_t i = 0; i < pre.size(); i++) { const float x = v(t0 + (i + 0.5) * Ts); if (pre[i] == '1') { hi += x; nh++; } else { lo += x; nl++; } }
    hi /= nh; lo /= nl;
    if (hi - lo < 0.25) return false;
    const double thr = 0.5 * (hi + lo);
    int wrong = 0;
    for (size_t i = 0; i < pre.size(); i++) if ((v(t0 + (i + 0.5) * Ts) > thr) != (pre[i] == '1')) wrong++;
    if (wrong > 2) return false;
    int bits = 0;
    for (int b = 0; b < 14; b++) {
        double s = 0; const double tb = t0 + (pre.size() + b * 6) * Ts;
        for (int i = 0; i < 3; i++) s += v(tb + (i + 0.5) * Ts) - v(tb + (i + 3.5) * Ts);
        bits = (bits << 1) | (s > 0 ? 1 : 0);
    }
    const int a = (bits >> 10) & 15;
    int p = 0; for (int i = 0; i < 4; i++) p += (a >> i) & 1;
    if (!(p & 1)) return false;                     // odd parity over b0..b3
    st_.wssValid = true; st_.wssAspect = a; st_.wssText = atvWssAspectName(a);
    return true;
}

bool AtvVbi::line625(const Level& v) { return tryTeletext(v) || tryWss(v); }

bool AtvVbi::line525(const Level& v) {
    const double T = 1.0 / 0.5034965;
    double best = -1e9, t0 = 0;
    for (double t = 25.0; t <= 31.0; t += 0.05) {
        const double s = -v(t + 0.5 * T) - v(t + 1.5 * T) + v(t + 2.5 * T);
        if (s > best) { best = s; t0 = t; }
    }
    if (best < 0.2) return false;
    const double thr = 0.25;
    for (int k = 0; k < 2; k++) { (void)k; }
    uint8_t c[2];
    for (int by = 0; by < 2; by++) {
        uint8_t val = 0; int n = 0;
        for (int i = 0; i < 8; i++) { const bool b = v(t0 + (3 + by * 8 + i + 0.5) * T) > thr; if (b) { val |= (uint8_t)(1 << i); n++; } }
        if (!(n & 1)) return false;
        c[by] = val & 0x7F;
    }
    for (uint8_t ch : c) if (ch >= 0x20 && ch < 0x7F) st_.captions += (char)ch;
    if (c[0] == 0x14 && (c[1] == 0x2D || c[1] == 0x2C)) st_.captions += '\n';
    if (st_.captions.size() > 200) st_.captions.erase(0, st_.captions.size() - 200);
    return true;
}

} // namespace dect2
