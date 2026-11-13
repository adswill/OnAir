// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL station ID in the sweep: see kaal_sweepid.h.
#include "dect2/kaal/kaal_sweepid.h"
#include "dect2/kaal/kaal_phy.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {
namespace kaal {

namespace {
const double kPi = 3.14159265358979323846;
using cd = std::complex<double>;
}

const char* const kIdAlphabet = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/.";
const char* const kIdSuffix[16] = {"", "/P", "/M", "/MM", "/A", "/QRP", "/B", "/AM", "/R", "/S", "/T", "/X", "/1", "/2", "/3", "/4"};

uint8_t crc8(const uint8_t* data, size_t n) {
    uint8_t c = 0xFF;
    for (size_t i = 0; i < n; i++) {
        c ^= data[i];
        for (int b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
    }
    return c;
}

static int alphabetIndex(char ch) {
    const char* p = std::strchr(kIdAlphabet, ch);
    return (p && ch != 0) ? (int)(p - kIdAlphabet) : -1;
}

bool packSweepId(const std::string& baseIn, int suffix, uint8_t bits[44]) {
    if (suffix < 0 || suffix > 15 || baseIn.size() > 6) return false;
    std::string base = baseIn;
    base.resize(6, ' ');
    uint64_t v = 0;
    for (char ch : base) {
        const int i = alphabetIndex(ch);
        if (i < 0) return false;
        v = v * 40 + (uint64_t)i;
    }
    const uint64_t payload = ((uint64_t)suffix << 32) | v;   // 36 bits
    uint8_t by[5];
    for (int i = 0; i < 5; i++) by[i] = (uint8_t)((payload >> (8 * (4 - i))) & 0xFF);
    const uint64_t word = (payload << 8) | crc8(by, 5);       // 44 bits
    for (int i = 0; i < 44; i++) bits[i] = (uint8_t)((word >> (43 - i)) & 1);
    return true;
}

bool unpackSweepId(const uint8_t bits[44], std::string& base, int& suffix) {
    uint64_t word = 0;
    for (int i = 0; i < 44; i++) word = (word << 1) | (bits[i] & 1);
    const uint64_t payload = word >> 8;
    const uint8_t crc = (uint8_t)(word & 0xFF);
    uint8_t by[5];
    for (int i = 0; i < 5; i++) by[i] = (uint8_t)((payload >> (8 * (4 - i))) & 0xFF);
    if (crc8(by, 5) != crc) return false;
    uint64_t v = payload & 0xFFFFFFFFULL;
    const int sfx = (int)(payload >> 32);
    if (v >= 4096000000ULL) return false;
    char ch[7] = {};
    for (int i = 5; i >= 0; i--) { ch[i] = kIdAlphabet[v % 40]; v /= 40; }
    std::string s(ch, 6);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s.find_first_not_of(' ') == std::string::npos) return false;
    base = s;
    suffix = sfx;
    return true;
}

std::array<uint8_t, kIdCycle> sweepIdSymbols(const std::string& base, int suffix) {
    std::array<uint8_t, kIdCycle> out{};
    uint8_t b[44];
    if (!packSweepId(base, suffix, b)) { out.fill(0); return out; }
    out[0] = kSwFast1;
    out[1] = kSwFast2;
    for (int i = 0; i < kIdSymbols; i++) out[2 + i] = (uint8_t)((b[2 * i] << 1) | b[2 * i + 1]);
    return out;
}

bool splitCallsign(const std::string& callsign, std::string& base, int& suffix) {
    std::string s;
    for (char c : callsign) s += (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    if (s.empty()) return false;
    int sfx = 0;
    size_t cut = s.size(), best = 0;
    for (int i = 1; i < 16; i++) {   // the longest matching suffix
        const std::string t = kIdSuffix[i];
        if (s.size() > t.size() && t.size() > best && s.compare(s.size() - t.size(), t.size(), t) == 0) { sfx = i; best = t.size(); cut = s.size() - t.size(); }
    }
    std::string b = s.substr(0, cut);
    if (b.empty() || b.size() > 6) return false;
    for (char c : b) if (alphabetIndex(c) < 0) return false;
    base = b;
    suffix = sfx;
    return true;
}

std::string joinCallsign(const std::string& base, int suffix) {
    return base + ((suffix >= 0 && suffix < 16) ? kIdSuffix[suffix] : "");
}

cd sweepVariantFormula(int v, double tt) {
    const double lo = kSweepLowHz, hi = kSweepHighHz, R = kSwRate;
    double ph, amp = 1.0;
    switch (v) {
        case kSwUp: ph = 2 * kPi * (lo * tt + 0.5 * R * tt * tt); break;
        case kSwDown: ph = 2 * kPi * (hi * tt - 0.5 * R * tt * tt); break;
        case kSwFast1: ph = 2 * kPi * (lo * tt + R * tt * tt); amp = std::sqrt(2.0); break;
        default: {
            const double u = tt - kSwPeriodS / 2;
            ph = 2 * kPi * (lo * u + R * u * u);
            amp = std::sqrt(2.0);
        }
    }
    return cd(amp * std::cos(ph), amp * std::sin(ph));
}

void sweepVariantSupport(int v, int& from, int& to) {
    from = 0; to = kSwL;
    if (v == kSwFast1) to = kSwL / 2;
    else if (v == kSwFast2) from = kSwL / 2;
}

cd sweepSampleAt(int v, int m) {
    int a, b;
    sweepVariantSupport(v, a, b);
    if (m < a || m >= b) return cd(0, 0);
    return sweepVariantFormula(v, (double)m / kFs);
}

const std::vector<std::complex<float>>& sweepVariantTemplate(int v) {
    static const std::array<std::vector<std::complex<float>>, 4> t = [] {
        std::array<std::vector<std::complex<float>>, 4> a;
        for (int q = 0; q < 4; q++) {
            a[(size_t)q].resize(kSwL);
            for (int m = 0; m < kSwL; m++) { const cd z = sweepSampleAt(q, m); a[(size_t)q][(size_t)m] = std::complex<float>((float)z.real(), (float)z.imag()); }
        }
        return a;
    }();
    return t[(size_t)(v & 3)];
}

// ---------------------------------------------------------------- decoder
SweepIdDecoder::SweepIdDecoder() { reset(); }

void SweepIdDecoder::reset() {
    std::memset(acc_, 0, sizeof acc_);
    std::memset(visits_, 0, sizeof visits_);
    gateRing_.fill(0.f);
    gateSum_ = 0;
    n_ = lastK_ = candK_ = 0;
    res_ = SweepIdResult();
    cand_ = SweepIdResult();
    candValid_ = fresh_ = false;
    candCount_ = 0;
}

void SweepIdDecoder::addPeriod(uint64_t k, const double s[4], double gateDb) {
    const int pos = (int)(k % kIdCycle);
    double sum = s[0] + s[1] + s[2] + s[3];
    if (!(sum > 0)) sum = 1;
    for (int v = 0; v < 4; v++) acc_[pos][v] = (visits_[pos] ? 0.85 * acc_[pos][v] : 0.0) + s[v] / sum;
    visits_[pos]++;
    const size_t gi = (size_t)(n_ % kIdCycle);
    gateSum_ += gateDb - gateRing_[gi];
    gateRing_[gi] = (float)gateDb;
    n_++;
    lastK_ = k;
    tryDecode();
}

void SweepIdDecoder::tryDecode() {
    for (int p = 0; p < kIdCycle; p++) if (!visits_[p]) return;
    int sym[kIdCycle];
    for (int p = 0; p < kIdCycle; p++) {
        int b = 0;
        for (int v = 1; v < 4; v++) if (acc_[p][v] > acc_[p][b]) b = v;
        sym[p] = b;
    }
    const double gate = gateMean();
    if (gate < gateMin_) return;
    for (int r = 0; r < kIdCycle; r++) {
        if (sym[r] != kSwFast1 || sym[(r + 1) % kIdCycle] != kSwFast2) continue;
        uint8_t bits[44];
        for (int i = 0; i < kIdSymbols; i++) {
            const int d = sym[(r + 2 + i) % kIdCycle];
            bits[2 * i] = (uint8_t)(d >> 1);
            bits[2 * i + 1] = (uint8_t)(d & 1);
        }
        std::string base;
        int sfx;
        if (!unpackSweepId(bits, base, sfx)) continue;
        SweepIdResult c;
        c.valid = true; c.base = base; c.suffix = sfx; c.callsign = joinCallsign(base, sfx); c.gateDb = (float)gate;
        if (candValid_ && cand_.base == base && cand_.suffix == sfx) {
            if (lastK_ >= candK_ + 12 && candCount_ < 2) candCount_ = 2;
            else if (candCount_ < 1) candCount_ = 1;
        } else {
            cand_ = c; candValid_ = true; candK_ = lastK_; candCount_ = 1;
        }
        if (candCount_ >= 2 && (!res_.valid || res_.base != base || res_.suffix != sfx)) {
            res_ = c;
            res_.confirmations = candCount_;
            fresh_ = true;
        } else if (res_.valid && res_.base == base && res_.suffix == sfx) {
            res_.gateDb = (float)gate;
        }
        return;
    }
}

bool SweepIdDecoder::poll() {
    const bool f = fresh_;
    fresh_ = false;
    return f;
}

}  // namespace kaal
}  // namespace dect2
