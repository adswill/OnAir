#include "acars_chan.h"
#include "dect2/acars_proto.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>

namespace dect2 {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kMskOver = 12;                  // matched filter oversampling (acarsdec MFLTOVER)
constexpr double kPllG = 38e-4, kPllC = 0.52; // acarsdec msk.c
constexpr int kMaxParityErr = 3;              // acarsdec MAXPERR

double besselI0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 40; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; }
    return s;
}

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
    h2_ = lowpass(kN2, 3600.0 / fs2, 6.0);
    w0_ = 2 * kPi * 1800.0 / audioRate_;
    flen_ = (int)(audioRate_ / 1200.0) + 1;
    const int flenO = flen_ * kMskOver + 1;
    hm_.assign((size_t)flenO, 0.f);
    for (int i = 0; i < flenO; i++) {
        const double v = std::cos(2 * kPi * 600.0 / audioRate_ / kMskOver * (i - (flenO - 1) / 2));
        hm_[(size_t)i] = v > 0 ? (float)v : 0.f;
    }
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
    buf2_.assign(2 * kN2, 0.f); pos2_ = 0; cnt2_ = 0;
    carrier_ = 0;
    for (int k = 0; k < kInst; k++) {
        Inst& in = inst_[k];
        in = Inst();
        in.inb.assign((size_t)flen_, cf32(0, 0));
        in.clk = 1.5 * kPi * k / kInst;                  // the starting phases differ by a third of a bit
        in.phi = kPi * 0.5 * k / kInst;
    }
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
    buf2_[(size_t)pos2_] = a; buf2_[(size_t)(pos2_ + kN2)] = a;
    const int p2 = pos2_;
    pos2_ = pos2_ + 1 == kN2 ? 0 : pos2_ + 1;
    if (++cnt2_ < 2) return;
    cnt2_ = 0;
    const float* w = &buf2_[(size_t)(p2 + 1)];
    float in = 0;
    for (int k = 0; k < kN2; k++) in += h2_[(size_t)k] * w[k];
    audioCount_++;
    for (auto& d : inst_) demod(d, in);
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

void AcarsChannelRx::demod(Inst& d, float in) {
    if (d.st == Inst::TXT || d.st == Inst::CRC1 || d.st == Inst::CRC2) { d.lvlSum += carrier_; d.lvlN++; }
    // MSK demodulator (acarsdec msk.c): VCO at 1800 Hz, 3 pi/2 of VCO phase per bit, matched filter over two bits
    const double s = w0_ + d.df;
    d.phi += s;
    if (d.phi >= 2 * kPi) d.phi -= 2 * kPi;
    d.inb[(size_t)d.idx] = cf32(in * (float)std::cos(d.phi), -in * (float)std::sin(d.phi));
    d.idx = d.idx + 1 == flen_ ? 0 : d.idx + 1;
    d.clk += s;
    if (d.clk >= 1.5 * kPi - s / 2) {
        d.clk -= 1.5 * kPi;
        int o = (int)(kMskOver * (d.clk / s + 0.5));
        o = std::max(0, std::min(kMskOver, o));
        cf32 v(0, 0);
        int j2 = d.idx;
        for (int j = 0; j < flen_; j++, o += kMskOver) {
            v += hm_[(size_t)o] * d.inb[(size_t)j2];
            j2 = j2 + 1 == flen_ ? 0 : j2 + 1;
        }
        const float lvl = std::abs(v);
        v /= lvl + 1e-8f;
        float vo, dphi;
        if (d.s & 1) { vo = v.imag(); dphi = vo >= 0 ? -v.real() : v.real(); }
        else { vo = v.real(); dphi = vo >= 0 ? v.imag() : -v.imag(); }
        putBit(d, (d.s & 2) ? -vo : vo);
        d.s++;
        d.df = kPllC * d.df + (1.0 - kPllC) * kPllG * dphi;
    }
}

void AcarsChannelRx::putBit(Inst& d, float v) {
    d.outbits >>= 1;
    if (v > 0) d.outbits |= 0x80;
    if (rec_ && &d == &inst_[0]) bits_.push_back(v > 0 ? 1 : 0);
    if (--d.nbits <= 0) byteReady(d);
}

void AcarsChannelRx::framerReset(Inst& d) {
    d.st = Inst::WSYN;
    d.df = 0;
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
        if (r == (uint8_t)~kAcarsSyn) { d.s ^= 2; d.st = Inst::SYN2; d.nbits = 8; return; }
        d.nbits = 1;
        return;
    case Inst::SYN2:
        if (r == kAcarsSyn) { d.st = Inst::SOH1; d.nbits = 8; return; }
        if (r == (uint8_t)~kAcarsSyn) { d.s ^= 2; d.nbits = 8; return; }
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
