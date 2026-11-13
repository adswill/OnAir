// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FM broadcast transmitter for tests (see fm_gen.h).
#include "dect2/fm/fm_gen.h"
#include <ctime>
#include "fm_noise.h"
#include <algorithm>
#include <cmath>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kRdsBaud = 1187.5;

// remainder of a polynomial (bit i = coefficient of x^i) divided by the RDS generator x^10 + x^8 + x^7 + x^5 + x^4 + x^3 + 1
uint32_t rdsRem(uint32_t v, int nbits) {
    for (int i = nbits - 1; i >= 10; i--) if ((v >> i) & 1) v ^= 0x5B9u << (i - 10);
    return v;
}

// 26-bit block: 16 information bits, then the checkword (remainder of info * x^10, plus the offset word)
uint32_t rdsBlock(uint16_t info, uint32_t offset) {
    return ((uint32_t)info << 10) | ((rdsRem((uint32_t)info << 10, 26) ^ offset) & 0x3FF);
}

constexpr uint32_t kOffA = 0x0FC, kOffB = 0x198, kOffC = 0x168, kOffD = 0x1B4, kOffC2 = 0x350;
}

FmGenerator::FmGenerator(const FmGenConfig& c) : c_(c), rng_(c.seed) {
    auto g = [&](double f) { const double w = 2 * kPi * f * c_.preemphUs * 1e-6; return std::sqrt(1 + w * w); };
    lGain_ = g(c_.leftHz); rGain_ = g(c_.rightHz);
    if (c_.ps.size() < 8) c_.ps.resize(8, ' ');
    c_.ps.resize(8);
    if (c_.deviationHz <= 0) c_.deviationHz = 75000;
    nextGroup();
}

void FmGenerator::pullLive() {
    if (!c_.pull48k) return;
    float blk[256 * 2];
    c_.pull48k(blk, 256);
    const double k = c_.preemphUs * 1e-6 * 48000.0;   // H(s) = 1 + s tau, at 48 kHz
    live_.reserve(live_.size() + 512);
    for (int i = 0; i < 256; i++) {
        const float l = blk[2 * i], r = blk[2 * i + 1];
        const float lp = (float)(l + k * (l - preL_));
        const float rp = (float)(r + k * (r - preR_));
        preL_ = l; preR_ = r;
        live_.push_back(std::max(-1.f, std::min(1.f, lp)));
        live_.push_back(std::max(-1.f, std::min(1.f, rp)));
    }
}

void FmGenerator::nextGroup() {
    const int k = groupNo_++;
    if (!curInit_) {
        curInit_ = true;
        cur_.ps = c_.ps; cur_.ps.resize(8, ' '); cur_.rt = c_.rt; cur_.pi = c_.pi; cur_.pty = c_.pty;
        cur_.tp = c_.tp; cur_.ta = c_.ta; cur_.music = c_.music;
        if (c_.live) { cur_ = c_.live->get(); }
        rtCur_ = cur_.rt; rtAb_ = cur_.abFlag;
    }
    // a cycle is five groups (four of the name, one of text): the live state is read at its start. The text changes at the start of its own cycle.
    if (c_.live && k % 5 == 0) {
        const std::string keepRt = rtCur_; const int keepAb = rtAb_; const int keepSeg = rtSeg_;
        cur_ = c_.live->get();
        rtCur_ = keepRt; rtAb_ = keepAb; rtSeg_ = keepSeg;
        if (rtSeg_ == 0) { rtCur_ = cur_.rt; rtAb_ = cur_.abFlag; }
    }
    const bool rtGroup = k % 5 == 4;
    uint16_t b[4];
    b[0] = (uint16_t)cur_.pi;
    uint16_t common = (uint16_t)((cur_.tp ? 1 : 0) << 10 | (cur_.pty & 31) << 5);
    if (cur_.ct && rtGroup && ++ctGroups_ % 12 == 0) {   // the clock group in place of a text group, about every five seconds
        rdsClockWords((int64_t)::time(nullptr), cur_.ctOffsetHalfHours, cur_.tp, cur_.pty, b[1], b[2], b[3]);
    } else if (!c_.extraGroups.empty() && k % 6 == 5) {   // a group of the test's own
        const auto& g = c_.extraGroups[extraNo_++ % c_.extraGroups.size()];
        b[1] = g[0]; b[2] = g[1]; b[3] = g[2];
    } else if (!rtGroup) {   // group 0A: station name, two characters per group
        const int seg = k % 5;
        b[1] = (uint16_t)(0x0000 | common | (cur_.ta ? 16 : 0) | (cur_.music ? 8 : 0) | 4 | seg);
        b[2] = 0xCDCD;
        b[3] = (uint16_t)((uint8_t)cur_.ps[seg * 2] << 8 | (uint8_t)cur_.ps[seg * 2 + 1]);
    } else {          // group 2A: radio text, four characters per group
        std::string rt = rtCur_;
        if (rt.size() < 64) rt += '\r';
        rt.resize(((rt.size() + 3) / 4) * 4, ' ');
        const int nseg = (int)rt.size() / 4;
        if (rtSeg_ >= nseg) rtSeg_ = 0;
        const int seg = rtSeg_;
        rtSeg_ = (rtSeg_ + 1) % nseg;
        b[1] = (uint16_t)(0x2000 | common | (rtAb_ ? 16 : 0) | seg);   // text A/B flag: flips when the text changes
        b[2] = (uint16_t)((uint8_t)rt[seg * 4] << 8 | (uint8_t)rt[seg * 4 + 1]);
        b[3] = (uint16_t)((uint8_t)rt[seg * 4 + 2] << 8 | (uint8_t)rt[seg * 4 + 3]);
    }
    const uint32_t off[4] = {kOffA, kOffB, ((b[1] >> 11) & 1) ? kOffC2 : kOffC, kOffD};   // a version B group has C' (and the PI in it)
    for (int i = 0; i < 4; i++) {
        const uint32_t blk = rdsBlock(b[i], off[i]);
        for (int j = 25; j >= 0; j--) {
            const uint8_t d = (uint8_t)(((blk >> j) & 1) ^ lastDiff_);
            bits_.push_back(d);
            lastDiff_ = d;
        }
    }
}

namespace {
inline void rotate(double* o, double cr, double ci) { const double r = o[0] * cr - o[1] * ci; o[1] = o[0] * ci + o[1] * cr; o[0] = r; }
inline void renorm(double* o) { const double m = 1.0 / std::sqrt(o[0] * o[0] + o[1] * o[1]); o[0] *= m; o[1] *= m; }
}

void FmGenerator::generate(size_t n, std::vector<cf32>& out) {
    const size_t o = out.size();
    out.resize(o + n);
    generate(out.data() + o, n);
}

void FmGenerator::generate(cf32* dst, size_t n, float gain) {
    const double fs = c_.rate, dt = 1.0 / fs;
    const bool noisy = c_.cnrDb < 150;
    const float sigma = noisy ? gain * (float)(0.3 * std::sqrt(std::pow(10.0, -c_.cnrDb / 10.0) * fs / 200e3 / 2.0)) : 0.f;
    const double amp = 0.3 * gain;              // headroom for the ADC
    const double pilot = c_.pilotPct / 100.0, rdsAmp = c_.rdsDevKhz / 75.0;
    const double lc = std::cos(2 * kPi * c_.leftHz * dt), ls = std::sin(2 * kPi * c_.leftHz * dt);
    const double rc = std::cos(2 * kPi * c_.rightHz * dt), rs = std::sin(2 * kPi * c_.rightHz * dt);
    const double pc = std::cos(2 * kPi * 19000.0 * dt), ps = std::sin(2 * kPi * 19000.0 * dt);
    const double bc = std::cos(2 * kPi * kRdsBaud * dt), bs = std::sin(2 * kPi * kRdsBaud * dt);
    const double lA = c_.leftAmp * lGain_, rA = c_.rightAmp * rGain_;
    const double dphMul = 2 * kPi * c_.deviationHz * dt, dphCfo = 2 * kPi * c_.cfoHz * dt;
    const bool live = (bool)c_.pull48k;
    const double liveAdv = 48000.0 / fs;
    const FmNoiseTable& tab = FmNoiseTable::get();
    const float* nz = tab.t.data();
    const uint64_t nmask = tab.t.size() - 1;
    uint64_t ns = noiseState_ ? noiseState_ : (0x9E3779B97F4A7C15ull ^ ((uint64_t)c_.seed * 0xD1B54A32D192ED03ull)) | 1;
    constexpr size_t kBlk = 512;
    float rr[kBlk], qq[kBlk];
    for (size_t i0 = 0; i0 < n; i0 += kBlk) {
        const size_t nb = std::min(kBlk, n - i0);
        renorm(lo_); renorm(ro_); renorm(po_); renorm(bo_);
        // the multiplex and the carrier phase, a sample at a time (recurrences)
        for (size_t i = 0; i < nb; i++) {
            double l, r;
            if (live) {
                while (liveFrac_ >= 1.0) {
                    liveFrac_ -= 1.0;
                    liveL_[0] = liveL_[1];
                    liveR_[0] = liveR_[1];
                    if (livePos_ + 1 >= live_.size()) {
                        if (livePos_ > 4096) { live_.erase(live_.begin(), live_.begin() + (long)(livePos_ & ~1ull)); livePos_ = 0; }
                        pullLive();
                    }
                    if (livePos_ + 1 < live_.size()) { liveL_[1] = live_[livePos_]; liveR_[1] = live_[livePos_ + 1]; livePos_ += 2; }
                    else { liveL_[1] = 0; liveR_[1] = 0; }
                }
                const float f = (float)liveFrac_;
                l = liveL_[0] + (liveL_[1] - liveL_[0]) * f;
                r = liveR_[0] + (liveR_[1] - liveR_[0]) * f;
                liveFrac_ += liveAdv;
            } else {
                l = lA * lo_[1]; r = rA * ro_[1];
            }
            const double s1 = po_[1], c1 = po_[0];                     // sin and cos of the pilot phase
            double mpx;
            if (c_.stereo) {
                const double s2 = 2 * s1 * c1;
                mpx = 0.45 * (l + r) + 0.45 * (l - r) * s2 + pilot * s1;
            } else mpx = 0.9 * 0.5 * (l + r);
            if (c_.rds) {
                const double pos = t_ * kRdsBaud;
                const size_t bi = (size_t)pos;
                while (bitBase_ + bits_.size() <= bi + 1) nextGroup();
                if (bi - bitBase_ > 4096) { bits_.erase(bits_.begin(), bits_.begin() + 4000); bitBase_ += 4000; }
                const double sym = bits_[bi - bitBase_] ? 1.0 : -1.0;
                const double c3 = c1 * (c1 * c1 - 3 * s1 * s1);       // cos of three times the pilot phase
                mpx += rdsAmp * sym * bo_[1] * c3;
            }
            ph_ += dphCfo + dphMul * mpx;
            if (ph_ > kPi || ph_ < -kPi) ph_ -= 2 * kPi * std::nearbyint(ph_ * (0.5 / kPi));
            // quadrant and remainder of the phase for the sine and cosine below
            const double kq = std::nearbyint(ph_ * (2.0 / kPi));
            rr[i] = (float)((ph_ - kq * 1.5707963267948966) - kq * 6.123233995736766e-17);
            qq[i] = (float)kq;
            rotate(lo_, lc, ls); rotate(ro_, rc, rs); rotate(po_, pc, ps); rotate(bo_, bc, bs);
            t_ += dt;
        }
        // sine and cosine of the carrier phase: polynomials on |r| <= pi / 4, the quadrant picks the pair (written so that the compiler vectorises it)
        const float a0 = (float)amp;
        cf32* d = dst + i0;
        for (size_t i = 0; i < nb; i++) {
            const float r = rr[i], r2 = r * r;
            const float sp = r * (1.f + r2 * (-1.f / 6 + r2 * (1.f / 120 + r2 * (-1.f / 5040 + r2 * (1.f / 362880)))));
            const float cp = 1.f + r2 * (-0.5f + r2 * (1.f / 24 + r2 * (-1.f / 720 + r2 * (1.f / 40320 + r2 * (-1.f / 3628800)))));
            const int q = ((int)qq[i]) & 3;
            const float sn = (q & 1) ? cp : sp, cs = (q & 1) ? sp : cp;
            const float sg = (q & 2) ? -a0 : a0, cg = (q == 1 || q == 2) ? -a0 : a0;
            d[i] = cf32(cg * cs, sg * sn);
        }
        if (noisy) {
            for (size_t i = 0; i < nb; i++) {
                ns ^= ns << 13; ns ^= ns >> 7; ns ^= ns << 17;
                d[i] += cf32(sigma * nz[ns & nmask], sigma * nz[(ns >> 24) & nmask]);
            }
        }
    }
    noiseState_ = ns;
}

} // namespace dect2
