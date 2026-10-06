// FM broadcast transmitter for tests (see fm_gen.h).
#include "dect2/fm_gen.h"
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

void FmGenerator::generate(size_t n, std::vector<cf32>& out) {
    const double fs = c_.rate, dt = 1.0 / fs;
    const double sigma = std::sqrt(std::pow(10.0, -c_.cnrDb / 10.0) * fs / 200e3 / 2.0);
    std::normal_distribution<double> nd(0.0, 1.0);
    const double amp = 0.3;                     // headroom for the ADC
    const double pilot = c_.pilotPct / 100.0, rdsAmp = c_.rdsDevKhz / 75.0;
    for (size_t i = 0; i < n; i++) {
        const double pp = 2 * kPi * 19000.0 * t_;
        double l = c_.leftAmp * lGain_ * std::sin(lph_), r = c_.rightAmp * rGain_ * std::sin(rph_);
        double mpx;
        if (c_.stereo) mpx = 0.45 * (l + r) + 0.45 * (l - r) * std::sin(2 * pp) + pilot * std::sin(pp);
        else mpx = 0.9 * 0.5 * (l + r);
        if (c_.rds) {
            const double pos = t_ * kRdsBaud;
            const size_t bi = (size_t)pos;
            while (bitBase_ + bits_.size() <= bi + 1) nextGroup();
            if (bi - bitBase_ > 4096) { bits_.erase(bits_.begin(), bits_.begin() + 4000); bitBase_ += 4000; }
            const double u = pos - std::floor(pos);
            const double sym = bits_[bi - bitBase_] ? 1.0 : -1.0;
            mpx += rdsAmp * sym * std::sin(2 * kPi * u) * std::cos(3 * pp);
        }
        ph_ += 2 * kPi * (c_.cfoHz + 75000.0 * mpx) * dt;
        ph_ = std::fmod(ph_, 2 * kPi);
        cf32 v((float)(amp * std::cos(ph_) + amp * sigma * nd(rng_)), (float)(amp * std::sin(ph_) + amp * sigma * nd(rng_)));
        out.push_back(v);
        lph_ = std::fmod(lph_ + 2 * kPi * c_.leftHz * dt, 2 * kPi);
        rph_ = std::fmod(rph_ + 2 * kPi * c_.rightHz * dt, 2 * kPi);
        t_ += dt;
    }
}

} // namespace dect2
