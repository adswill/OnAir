// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Datawell HXV real-time format decoder (see datawell_hxv.h for the sources and what was found rather than published).
#include "dect2/datawell/datawell_hxv.h"
#include <algorithm>
#include <cmath>

namespace dect2::datawell {

namespace {
constexpr unsigned kGen = 012471;     // x^12 + x^10 + x^8 + x^5 + x^4 + x^3 + 1
constexpr int kExcluded = 27;         // the vector bit (counted from the first one sent) that the code does not cover

unsigned syndrome(uint64_t v) {
    unsigned r = 0;
    for (int i = 0; i < 64; i++) {
        if (i == kExcluded) continue;
        r = (r << 1) | (unsigned)((v >> (63 - i)) & 1);
        if ((r >> 12) & 1) r ^= kGen;
    }
    return r;
}

struct Table {
    int p1[4096], p2[4096], w[4096];
    Table() {
        for (int i = 0; i < 4096; i++) { p1[i] = p2[i] = -1; w[i] = -1; }
        w[0] = 0;
        unsigned s1[64];
        for (int i = 0; i < 64; i++) s1[i] = i == kExcluded ? 0 : syndrome(1ull << (63 - i));
        for (int i = 0; i < 64; i++) {
            if (i == kExcluded) continue;
            if (w[s1[i]] < 0) { w[s1[i]] = 1; p1[s1[i]] = i; }
        }
        for (int i = 0; i < 64; i++) for (int j = i + 1; j < 64; j++) {
            if (i == kExcluded || j == kExcluded) continue;
            const unsigned s = s1[i] ^ s1[j];
            if (w[s] < 0) { w[s] = 2; p1[s] = i; p2[s] = j; }
        }
    }
};
const Table& table() { static const Table t; return t; }
}

int hxvSignMag(unsigned v) { return (v & 0x800) ? -(int)(v & 0x7FF) : (int)(v & 0x7FF); }

static unsigned toSignMag(int x) {
    x = std::max(-2047, std::min(2047, x));
    return x < 0 ? (0x800u | (unsigned)(-x)) : (unsigned)x;
}

uint64_t hxvEncode(uint16_t cyc, int h, int n, int w) {
    uint64_t v = ((uint64_t)cyc << 48) | ((uint64_t)toSignMag(h) << 36) | ((uint64_t)toSignMag(n) << 24) | ((uint64_t)toSignMag(w) << 12);
    return v | syndrome(v);
}

int hxvCorrect(uint64_t& v) {
    const unsigned s = syndrome(v);
    if (!s) return 0;
    const Table& t = table();
    if (t.w[s] < 0) return -1;
    v ^= 1ull << (63 - t.p1[s]);
    if (t.p2[s] >= 0) v ^= 1ull << (63 - t.p2[s]);
    return t.w[s];
}

HxvVector hxvDecodeVector(uint64_t v, int fixed) {
    HxvVector r;
    r.cyc = (uint16_t)(v >> 48);
    r.h = hxvSignMag((unsigned)(v >> 36) & 0xFFF);
    r.n = hxvSignMag((unsigned)(v >> 24) & 0xFFF);
    r.w = hxvSignMag((unsigned)(v >> 12) & 0xFFF);
    r.parity = (uint16_t)(v & 0xFFF);
    r.wrsg = ((v >> 12) & 0xFFF) == 0xAAA;
    r.fixed = fixed;
    return r;
}

HxvDecoder::HxvDecoder() { reset(); }

void HxvDecoder::reset() {
    win_ = 0; nBits_ = 0;
    for (auto& a : cnt_) for (int& c : a) c = 0;
    locked_ = false; inv_ = false; phase_ = 0; badRun_ = 0;
    nVec_ = nClean_ = nFixed_ = nLost_ = nBlocks_ = 0;
    pos_ = -1; blockNo_ = -1;
    for (int i = 0; i < 4; i++) { cur_[i] = 0; curOk_[i] = false; }
    recent_.clear();
    sys_ = HxvSystem();
    for (auto& s : spec_) s = HxvSpecPoint();
}

void HxvDecoder::pushBit(int bit) {
    old_ = (old_ << 1) | (win_ >> 63);
    win_ = (win_ << 1) | (uint64_t)(bit & 1);
    nBits_++;
    if (nBits_ < 64) return;
    const int ph = (int)(nBits_ % 64);
    if (!locked_) {
        // look for the alignment: three clean vectors in a row, in either polarity (a random 64 bits pass with chance 1 / 4096)
        for (int inv = 0; inv < 2; inv++) {
            const uint64_t v = inv ? ~win_ : win_;
            if (syndrome(v) == 0) cnt_[inv][ph]++; else cnt_[inv][ph] = 0;
            if (cnt_[inv][ph] >= 3) {
                // a code word and its complement are both code words: the polarity is told by the displacements (small in the right one,
                // near +-2000 cm in the inverted one) and later by the sync word
                const HxvVector a = hxvDecodeVector(win_), b = hxvDecodeVector(~win_);
                const int ma = std::max(std::abs(a.h), std::abs(a.n)), mb = std::max(std::abs(b.h), std::abs(b.n));
                inv = mb < ma ? 1 : 0;
                locked_ = true; inv_ = inv != 0; phase_ = ph; badRun_ = 0;
                pos_ = -1; searchUntil_ = -1; haveH_ = false;
                break;
            }
        }
        return;
    }
    const uint64_t v = inv_ ? ~win_ : win_;
    if (ph == phase_) vector(v);
    else if (searchUntil_ >= (int64_t)nBits_ && syndrome(v) == 0) {   // a bit slip: the vectors went on at another phase
        phase_ = ph; searchUntil_ = -1;
        vector(v);
    }
}

// a vector that passed: counted, kept, handed to the block layer
bool HxvDecoder::accept(uint64_t v, int fx) {
    const HxvVector d = hxvDecodeVector(v, fx);
    // a repaired vector from a window with three or more errors is repaired to something wrong half of the time: a jump of more than 2 m in
    // 0.78 s is not a wave
    if (fx > 0 && haveH_ && std::abs(d.h - lastH_) > 200) return false;
    lastH_ = d.h; haveH_ = true;
    badRun_ = 0;
    if (fx == 0) nClean_++; else nFixed_++;
    recent_.push_back(d);
    if (recent_.size() > 128) recent_.erase(recent_.begin());
    word(d, true);
    return true;
}

void HxvDecoder::vector(uint64_t v) {
    nVec_++;
    uint64_t u = v;
    const int fx = hxvCorrect(u);
    if (fx == 0 && accept(u, 0)) { searchUntil_ = -1; return; }
    // not clean: did the bits slip? (a window of random bits is "repaired" about half of the time, so a repaired vector proves nothing)
    // look for a clean vector ending a few bits earlier; later ones: the next bits, see pushBit
    for (int k = 1; k <= 12; k++) {
        uint64_t w = (old_ << (64 - k)) | (win_ >> k);
        if (inv_) w = ~w;
        if (syndrome(w) == 0) { phase_ = (int)((nBits_ - (uint64_t)k) % 64); searchUntil_ = -1; accept(w, 0); return; }
    }
    searchUntil_ = (int64_t)nBits_ + 12;
    if (fx > 0 && accept(u, fx)) return;
    nLost_++;
    if (++badRun_ > 8) {   // lost the signal: look for the alignment again
        locked_ = false;
        for (auto& a : cnt_) for (int& c : a) c = 0;
        pos_ = -1;
    }
    HxvVector none;
    word(none, false);
}

void HxvDecoder::word(const HxvVector& vec, bool ok) {
    if (ok && pos_ < 0 && vec.cyc == 0x8000) {   // the sync word upside down (when not inside a block: system word 8 with the value 0 looks the same): the tones are the other way round
        inv_ = !inv_;
        recent_.clear();
        pos_ = 0; nBlocks_++;
        for (int i = 0; i < 4; i++) curOk_[i] = false;
        return;
    }
    if (ok && vec.cyc == 0x7FFF) {   // the sync word starts a block (no other cyclic word looks like it: the top bit of the others is not followed by 15 ones)
        pos_ = 0;
        nBlocks_++;
        for (int i = 0; i < 4; i++) curOk_[i] = false;
        return;
    }
    if (pos_ < 0) return;
    pos_++;
    if (pos_ >= 18) { if (ok) { pos_ = -1; return; } pos_ = 0; nBlocks_++; return; }   // a missing sync vector: carry on counting, a wrong one: wait for the next sync
    if (pos_ == 1) {
        if (ok) {
            const int num = vec.cyc >> 12;
            sys_.have[num] = true;
            sys_.word[num] = vec.cyc & 0xFFF;
            blockNo_ = num;
            sys_ = decodeSystem(sys_);
            if (num == 3) for (auto& p : spec_) if (p.valid) p.psd = p.rpsd * sys_.psdMax;
        } else blockNo_ = blockNo_ >= 0 ? (blockNo_ + 1) & 15 : -1;
        return;
    }
    const int idx = pos_ - 2, g = idx / 4, k = idx % 4;
    cur_[k] = vec.cyc; curOk_[k] = ok;
    if (k != 3) return;
    if (!(curOk_[0] && curOk_[1] && curOk_[2] && curOk_[3]) || blockNo_ < 0) return;
    const unsigned w0 = cur_[0], w1 = cur_[1], w2 = cur_[2], w3 = cur_[3];
    const int n = (int)((w0 >> 8) & 0x3F);
    if (n != 4 * blockNo_ + g) return;   // the frequency index must follow the block number: anything else is a damaged word
    HxvSpecPoint& p = spec_[n];
    p.valid = true; p.n = n;
    p.hz = n < 16 ? 0.025 + 0.005 * n : 0.11 + 0.01 * (n - 16);
    p.dirDeg = (w0 & 0xFF) * 360.0 / 256.0;
    const int slsb = (int)(w0 >> 14) & 3, m2lsb = (int)(w1 >> 14) & 3, n2lsb = (int)(w1 >> 12) & 3;
    p.rpsd = std::exp(-(double)(w1 & 0xFFF) / 200.0);
    p.spreadDeg = 0.4476 * ((double)(w2 >> 8) + slsb / 4.0);
    p.m2 = ((double)(w2 & 0xFF) + m2lsb / 4.0 - 128.0) / 128.0;
    p.n2 = ((double)(w3 >> 8) + n2lsb / 4.0 - 128.0) / 128.0;
    p.k = (double)(w3 & 0xFF) / 100.0;
    if (sys_.have[3]) p.psd = p.rpsd * sys_.psdMax;
}

HxvSystem HxvDecoder::decodeSystem(const HxvSystem& in) {
    HxvSystem s = in;
    const auto& w = s.word;
    if (s.have[0]) { s.tn = (int)(w[0] & 0xF); s.gpsModuleOk = (w[0] >> 4) & 1; s.gpsFix = (w[0] >> 5) & 1; }
    if (s.have[1]) { s.hrms = w[1] / 400.0; s.hs = 4 * s.hrms; }
    if (s.have[2]) { s.fz = (w[2] & 0xFF) / 400.0; s.tz = s.fz > 0 ? 1.0 / s.fz : 0; }
    if (s.have[3]) s.psdMax = 5000.0 * std::exp(-(double)w[3] / 200.0);
    if (s.have[4]) s.tr = (w[4] & 0x3FF) / 20.0 - 5;
    if (s.have[5]) s.tw = (w[5] & 0x3FF) / 20.0 - 5;
    if (s.have[6]) { s.tolWeeks = (int)((w[6] >> 4) & 0xFF); s.battery = (int)(w[6] & 7); }
    auto acc = [&](int i) { return ((w[i] & 0x800) ? -1.0 : 1.0) * (w[i] & 0x7FF) / 800.0; };
    if (s.have[7]) s.av0 = acc(7);
    if (s.have[8]) s.ax0 = acc(8);
    if (s.have[9]) s.ay0 = acc(9);
    if (s.have[10] && s.have[11]) {
        const double lat = (double)(((w[10] & 0x7FF) << 12) | (w[11] & 0xFFF)) / 8388608.0 * 90.0;
        s.lat = (w[10] & 0x800) ? -lat : lat;
        s.havePos = s.have[12] && s.have[13];
    }
    if (s.have[12] && s.have[13]) {
        const double lon = (double)(((w[12] & 0x7FF) << 12) | (w[13] & 0xFFF)) / 8388608.0 * 180.0;
        s.lon = (w[12] & 0x800) ? -lon : lon;
        s.havePos = s.have[10] && s.have[11];
    }
    if (s.have[14]) s.orientDeg = 360.0 * (w[14] & 0xFF) / 256.0;
    if (s.have[15]) s.inclDeg = (90.0 / 128.0) * ((double)(w[15] & 0xFF) - 128.0 + (double)((w[15] >> 8) & 0xF) / 16.0);
    return s;
}

} // namespace dect2::datawell
