// FM broadcast transmitter for tests (see fm_gen.h).
#include "dect2/fm_gen.h"
#include "fm_noise.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

namespace {
constexpr double kPi = 3.14159265358979323846;
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

constexpr uint32_t kOffA = 0x0FC, kOffB = 0x198, kOffC = 0x168, kOffD = 0x1B4;
}

FmGenerator::FmGenerator(const FmGenConfig& c) : c_(c), rng_(c.seed) {
    auto g = [&](double f) { const double w = 2 * kPi * f * c_.preemphUs * 1e-6; return std::sqrt(1 + w * w); };
    lGain_ = g(c_.leftHz); rGain_ = g(c_.rightHz);
    if (c_.ps.size() < 8) c_.ps.resize(8, ' ');
    c_.ps.resize(8);
    nextGroup();
}

void FmGenerator::nextGroup() {
    const int k = groupNo_++;
    const bool rtGroup = k % 5 == 4;
    uint16_t b[4];
    b[0] = (uint16_t)c_.pi;
    uint16_t common = (uint16_t)((c_.tp ? 1 : 0) << 10 | (c_.pty & 31) << 5);
    if (!rtGroup) {   // group 0A: station name, two characters per group
        const int seg = k % 5;
        b[1] = (uint16_t)(0x0000 | common | (c_.ta ? 16 : 0) | (c_.music ? 8 : 0) | 4 | seg);
        b[2] = 0xCDCD;
        b[3] = (uint16_t)((uint8_t)c_.ps[seg * 2] << 8 | (uint8_t)c_.ps[seg * 2 + 1]);
    } else {          // group 2A: radio text, four characters per group
        std::string rt = c_.rt;
        if (rt.size() < 64) rt += '\r';
        rt.resize(((rt.size() + 3) / 4) * 4, ' ');
        const int nseg = (int)rt.size() / 4;
        const int seg = (k / 5) % nseg;
        b[1] = (uint16_t)(0x2000 | common | seg);   // text A/B flag 0
        b[2] = (uint16_t)((uint8_t)rt[seg * 4] << 8 | (uint8_t)rt[seg * 4 + 1]);
        b[3] = (uint16_t)((uint8_t)rt[seg * 4 + 2] << 8 | (uint8_t)rt[seg * 4 + 3]);
    }
    const uint32_t off[4] = {kOffA, kOffB, kOffC, kOffD};
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
    const double dphMul = 2 * kPi * 75000.0 * dt, dphCfo = 2 * kPi * c_.cfoHz * dt;
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
            const double l = lA * lo_[1], r = rA * ro_[1];
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
