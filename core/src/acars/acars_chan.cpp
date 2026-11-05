#include "acars_chan.h"
#include "dect2/dsp/baseband_filter.h"
#include "dect2/acars/acars_proto.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include "dect2/common/mathconst.h"
#include "dect2/common/window.h"

namespace dect2 {

namespace {

using dect2::math::kPi;
constexpr double kBaud = 2400.0, kCentreHz = 1800.0;   // ARINC 618: 2400 bit/s MSK, tones 1200 Hz and 2400 Hz around 1800 Hz
constexpr double kTimingGain = 0.015;                    // bit-clock correction per bit, as a fraction of a bit at full early-late error
constexpr int kMaxParityErr = 3;                        // parity errors a block may have and still be tried for repair
constexpr int kN3 = 25;
constexpr float kRefKeep = 0.97f;                        // memory of the carrier reference, per bit

double besselI0(double x) { return dect2::window::besselI0(x, 40, 0.0); }

// Kaiser-windowed sinc low-pass; cutoff as a fraction of the sample rate, unity gain at DC
std::vector<float> lowpass(int n, double fc, double beta) {
    std::vector<float> h((size_t)n);
    double sum = 0;
    const double m = (n - 1) / 2.0, i0b = besselI0(beta);
    for (int i = 0; i < n; i++) {
        const double x = i - m;
        const double sinc = x == 0 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x);
        const double r = x / m;
        const double w = besselI0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0b;
        h[(size_t)i] = (float)(sinc * w);
        sum += sinc * w;
    }
    for (auto& v : h) v = (float)(v / sum);
    return h;
}

constexpr int kN1 = 65, kN2 = 33;

} // namespace

void AcarsChannelRx::configure(double inRate, double offsetHz) {
    inRate_ = inRate; offsetHz_ = offsetHz;
    d1_ = std::max(1, (int)std::lround(inRate / 100e3));
    const double fs1 = inRate / d1_, fs2 = fs1 / 4;
    audioRate_ = fs2 / 2;
    cicScale_ = (float)(1.0 / (std::pow((double)d1_, 4) * 32768.0));
    step_ = std::polar(1.f, (float)(-2 * kPi * offsetHz / inRate));
    h1_ = lowpass(kN1, 7500.0 / fs1, 7.0);
    h2_ = bbf::overrideTaps("acars", lowpass(kN2, 3600.0 / fs2, 6.0), fs2, 7200.0);
    bitLen_ = audioRate_ / kBaud;
    loStep_ = std::polar(1.f, (float)(-2 * kPi * kCentreHz / audioRate_));
    h3_ = lowpass(kN3, 1600.0 / audioRate_, 5.0);
    reset();
}

void AcarsChannelRx::setOffset(double offsetHz) {
    offsetHz_ = offsetHz;
    step_ = std::polar(1.f, (float)(-2 * kPi * offsetHz / inRate_));
    reset();
}

void AcarsChannelRx::reset() {
    ph_ = cf32(1.f, 0.f); phCount_ = 0;
    memset(integ_, 0, sizeof integ_); memset(comb_, 0, sizeof comb_);
    cicCount_ = 0;
    buf1_.assign(2 * kN1, cf32(0, 0)); pos1_ = 0; cnt1_ = 0;
    buf2_.assign(2 * h2_.size(), 0.f); pos2_ = 0; cnt2_ = 0;
    carrier_ = 0;
    for (int k = 0; k < kInst; k++) {
        Inst& in = inst_[k];
        in = Inst();
        in.t = bitLen_ * (1.0 + (double)k / kInst);      // the bit clocks start a third of a bit apart
    }
    lo_ = cf32(1.f, 0.f); loCount_ = 0;
    buf3_.assign(2 * kN3, cf32(0, 0)); pos3_ = 0;
    for (auto& v : zRing_) v = cf32(0, 0);
    audioCount_ = 0;
    pending_.clear();
    blocks_.clear();
    bits_.clear();
}

float AcarsChannelRx::carrierDb() const { return 20.f * std::log10(std::max(carrier_, 1e-6f)); }

void AcarsChannelRx::process(const cf32* x, size_t n) {
    const float sc = 32768.f;
    for (size_t i = 0; i < n; i++) {
        // mix the channel to 0 Hz
        const cf32 y = x[i] * ph_;
        ph_ *= step_;
        if (++phCount_ >= 256) { phCount_ = 0; ph_ /= std::abs(ph_); }
        float re = y.real(), im = y.imag();
        re = std::max(-2.f, std::min(2.f, re)); im = std::max(-2.f, std::min(2.f, im));
        uint64_t v0 = (uint64_t)(int64_t)(re * sc), v1 = (uint64_t)(int64_t)(im * sc);
        // integrators (wrap-around arithmetic on purpose)
        integ_[0][0] += v0; integ_[0][1] += integ_[0][0]; integ_[0][2] += integ_[0][1]; integ_[0][3] += integ_[0][2];
        integ_[1][0] += v1; integ_[1][1] += integ_[1][0]; integ_[1][2] += integ_[1][1]; integ_[1][3] += integ_[1][2];
        if (++cicCount_ < d1_) continue;
        cicCount_ = 0;
        uint64_t c[2];
        for (int q = 0; q < 2; q++) {
            uint64_t t = integ_[q][3];
            for (int s = 0; s < 4; s++) { const uint64_t d = t - comb_[q][s]; comb_[q][s] = t; t = d; }
            c[q] = t;
        }
        const cf32 z((float)(int64_t)c[0] * cicScale_, (float)(int64_t)c[1] * cicScale_);
        // channel filter, decimate by 4
        buf1_[(size_t)pos1_] = z; buf1_[(size_t)(pos1_ + kN1)] = z;
        const int p1 = pos1_;
        pos1_ = pos1_ + 1 == kN1 ? 0 : pos1_ + 1;
        if (++cnt1_ < 4) continue;
        cnt1_ = 0;
        const cf32* w = &buf1_[(size_t)(p1 + 1)];
        float ar = 0, ai = 0;
        for (int k = 0; k < kN1; k++) { ar += h1_[(size_t)k] * w[k].real(); ai += h1_[(size_t)k] * w[k].imag(); }
        audioSample(std::sqrt(ar * ar + ai * ai));
    }
}

void AcarsChannelRx::audioSample(float env) {
    carrier_ += (env - carrier_) * 0.01f;       // 4 ms at 25 kS/s: a high-pass of about 40 Hz on the audio
    const float a = env - carrier_;
    buf2_[(size_t)pos2_] = a; buf2_[(size_t)pos2_ + h2_.size()] = a;
    const int p2 = pos2_;
    pos2_ = pos2_ + 1 == (int)h2_.size() ? 0 : pos2_ + 1;
    if (++cnt2_ < 2) return;
    cnt2_ = 0;
    const float* w = &buf2_[(size_t)(p2 + 1)];
    float in = 0;
    for (size_t k = 0; k < h2_.size(); k++) in += h2_[(size_t)k] * w[k];
    // mix down by 1800 Hz and low-pass: the tones become +600 Hz (2400) and -600 Hz (1200)
    const cf32 z0 = cf32(in, 0.f) * lo_;
    lo_ *= loStep_;
    if (++loCount_ >= 256) { loCount_ = 0; lo_ /= std::abs(lo_); }
    buf3_[(size_t)pos3_] = z0; buf3_[(size_t)(pos3_ + kN3)] = z0;
    const int p3 = pos3_;
    pos3_ = pos3_ + 1 == kN3 ? 0 : pos3_ + 1;
    const cf32* w3 = &buf3_[(size_t)(p3 + 1)];
    cf32 z(0, 0);
    for (int k = 0; k < kN3; k++) z += h3_[(size_t)k] * w3[k];
    audioCount_++;
    zRing_[audioCount_ % kRing] = z;
    for (auto& d : inst_) demod(d);
    // blocks that the demodulators finished are held for a moment so that the twins of one block can be compared
    const int64_t hold = (int64_t)(0.05 * audioRate_);
    size_t keep = 0;
    for (size_t i = 0; i < pending_.size(); i++) {
        if (audioCount_ - pending_[i].t > hold) {
            blocks_.push_back(pending_[i].b);
            if (blocks_.size() > 64) blocks_.erase(blocks_.begin());
        } else pending_[keep++] = pending_[i];
    }
    pending_.resize(keep);
}

cf32 AcarsChannelRx::boundary(double t) const {
    // The baseband signal of MSK is offset QPSK with half-sine pulses two bits long, so the matched filter is a half-sine window of
    // that length centred on the boundary.
    cf32 y(0, 0);
    const int64_t k0 = (int64_t)std::ceil(t - bitLen_), k1 = (int64_t)std::floor(t + bitLen_);
    for (int64_t k = k0; k <= k1; k++) {
        const float w = (float)std::cos(kPi * ((double)k - t) / (2 * bitLen_));
        y += w * zRing_[((k % kRing) + kRing) % kRing];
    }
    return y;
}

void AcarsChannelRx::demod(Inst& d) {
    if (d.st == Inst::TXT || d.st == Inst::CRC1 || d.st == Inst::CRC2) { d.lvlSum += carrier_; d.lvlN++; }
    const double q = bitLen_ / 4;
    while (d.t + q + bitLen_ + 1 <= (double)audioCount_) {
        // At the boundaries the signal alternates between the real and the imaginary axis (the phase moves by a quarter turn a bit), so
        // multiplying by (-j)^n leaves a real value whose sign flips exactly when the bit was sent as 1200 Hz.
        static const cf32 rot[4] = {cf32(1, 0), cf32(0, -1), cf32(-1, 0), cf32(0, 1)};
        const cf32 u = boundary(d.t) * rot[d.n & 3];
        d.n = (d.n + 1) & 3;
        const bool pos = (u * std::conj(d.ref)).real() >= 0;
        d.ref = kRefKeep * d.ref + (pos ? u : -u);
        // timing error: the matched-filter output is largest at the boundary
        const float el = std::abs(boundary(d.t + q)), ee = std::abs(boundary(d.t - q));
        d.t += bitLen_ * (1.0 + kTimingGain * (el - ee) / (el + ee + 1e-12f));
        // 2400 Hz (the bit equals the one before it) when the sign stays, 1200 Hz when it flips
        if (pos != d.prevPos) d.prevBit = !d.prevBit;
        d.prevPos = pos;
        putBit(d, d.prevBit != d.inv);
    }
}

void AcarsChannelRx::putBit(Inst& d, bool bit) {
    d.outbits >>= 1;
    if (bit) d.outbits |= 0x80;
    if (rec_ && &d == &inst_[0]) bits_.push_back(bit ? 1 : 0);
    if (--d.nbits <= 0) byteReady(d);
}

void AcarsChannelRx::framerReset(Inst& d) {
    d.st = Inst::WSYN;
    d.nbits = 1;
}

void AcarsChannelRx::finish(Inst& d) {
    AcarsRawBlock& b = d.cur;
    b.levelDb = d.lvlN ? 20.f * std::log10(std::max((float)(d.lvlSum / d.lvlN), 1e-6f)) : -120.f;
    b.ok = acarsRepairBlock(b.txt, b.len, b.crc, b.fixedBits, b.parityErrors);
    // the same block from another demodulator is ignored; a good one replaces a bad one
    for (auto& p : pending_) {
        if (std::llabs(p.t - audioCount_) > (int64_t)(0.03 * audioRate_)) continue;
        if ((!p.b.ok && b.ok) || (p.b.ok && b.ok && b.fixedBits < p.b.fixedBits)) { p.b = b; }
        d.st = Inst::END; d.nbits = 8;
        return;
    }
    pending_.push_back({audioCount_, b});
    d.st = Inst::END;
    d.nbits = 8;
}

void AcarsChannelRx::byteReady(Inst& d) {
    const uint8_t r = d.outbits;
    switch (d.st) {
    case Inst::WSYN:
        if (r == kAcarsSyn) { d.st = Inst::SYN2; d.nbits = 8; return; }
        if (r == (uint8_t)~kAcarsSyn) { d.inv = !d.inv; d.st = Inst::SYN2; d.nbits = 8; return; }
        d.nbits = 1;
        return;
    case Inst::SYN2:
        if (r == kAcarsSyn) { d.st = Inst::SOH1; d.nbits = 8; return; }
        if (r == (uint8_t)~kAcarsSyn) { d.inv = !d.inv; d.nbits = 8; return; }
        framerReset(d);
        return;
    case Inst::SOH1:
        if (r == kAcarsSoh) {
            d.st = Inst::TXT; d.nbits = 8;
            d.cur.len = 0; d.perr = 0;
            d.lvlSum = 0; d.lvlN = 0;
            if (&d == &inst_[0]) frames_++;
            return;
        }
        framerReset(d);
        return;
    case Inst::TXT:
        d.cur.txt[d.cur.len++] = r;
        if (!acarsParityOk(r) && ++d.perr > kMaxParityErr + 1) { framerReset(d); return; }
        if (r == kAcarsEtx || r == kAcarsEtb) { d.st = Inst::CRC1; d.nbits = 8; return; }
        if (d.cur.len > 20 && r == kAcarsDel) {      // the suffix was lost: the check bytes sit just before the DEL
            d.cur.len -= 3;
            d.cur.crc[0] = d.cur.txt[d.cur.len]; d.cur.crc[1] = d.cur.txt[d.cur.len + 1];
            finish(d);
            return;
        }
        if (d.cur.len > 240) { framerReset(d); return; }
        d.nbits = 8;
        return;
    case Inst::CRC1:
        d.cur.crc[0] = r; d.st = Inst::CRC2; d.nbits = 8;
        return;
    case Inst::CRC2:
        d.cur.crc[1] = r;
        finish(d);
        return;
    case Inst::END:
        framerReset(d);
        d.nbits = 8;
        return;
    }
}

} // namespace dect2
