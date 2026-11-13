// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/kavb/kavb_rx.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdlib>

namespace dect2::kavb {

namespace {

constexpr double kTwoPi = 6.283185307179586;
constexpr int kHalf = kRrcSpan * kSps / 2;   // 24: group delay of the filter in samples

cf cexpj(double a) { return cf(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))); }

// Cubic Lagrange interpolation of m at the fractional index x (0 outside the safe range).
cf interp(const std::vector<cf>& m, double x) {
    const long i = static_cast<long>(std::floor(x));
    if (i < 1 || i + 2 >= static_cast<long>(m.size())) return cf(0, 0);
    const float mu = static_cast<float>(x - static_cast<double>(i));
    const float cm1 = -mu * (mu - 1) * (mu - 2) / 6.0f;
    const float c0 = (mu + 1) * (mu - 1) * (mu - 2) / 2.0f;
    const float c1 = -(mu + 1) * mu * (mu - 2) / 2.0f;
    const float c2 = (mu + 1) * mu * (mu - 1) / 6.0f;
    return m[i - 1] * cm1 + m[i] * c0 + m[i + 1] * c1 + m[i + 2] * c2;
}

std::vector<float> mfTaps() {
    auto h = kris::rrcTaps(kRrcBeta, kSps, kRrcSpan);
    for (auto& v : h) v /= std::sqrt(static_cast<float>(kSps));   // transmit taps carry sqrt(sps): the product is unit gain at the symbol instant
    return h;
}

void filterRange(const std::vector<cf>& x, const std::vector<float>& h, std::vector<cf>& y, long from, long to) {
    const long len = static_cast<long>(x.size());
    const long nt = static_cast<long>(h.size());
    for (long n = from; n < to; n++) {
        cf acc(0, 0);
        const long i0 = std::max(0L, kHalf - n), i1 = std::min(nt, len - n + kHalf);
        for (long i = i0; i < i1; i++) acc += x[n + i - kHalf] * h[i];
        y[n] = acc;
    }
}

struct Fit {
    double a = 1.0, phi0 = 0.0, w = 0.0, kref = 0.0, res = 0.0;
    cf gain(int k) const { return static_cast<float>(a) * cexpj(phi0 + w * (k - kref)); }
};

// Linear phase + amplitude from the preamble and the pilots, by maximum likelihood: the known symbols give v_i = a exp(j(phi + w pos_i)) + noise, so w is the peak of
// |sum v_i exp(-j w pos_i)|. No phase unwrapping (unwrapping slipped on a fifth of the robust frames at 5 dB). The search is centred on the pilot-to-pilot phase
// step (16 symbols apart, unambiguous up to +-0.19 rad/symbol) and covers +-0.03 rad/symbol around it, then a parabola refines the peak.
Fit pilotFit(const std::vector<cf>& s, const Profile& p) {
    const auto& zc = preambleSymbols();
    const auto& rm = regionMap(p.dataSyms);
    const int base = kPreambleSyms + kHeaderCodedBits;
    const size_t np = rm.pilot.size();
    std::vector<cf> v;
    std::vector<double> pos;
    for (int k = 0; k < kPreambleSyms; k++) {
        v.push_back(s[k] * std::conj(zc[k]));
        pos.push_back(k);
    }
    cf dacc(0, 0);
    for (size_t i = 0; i < np; i++) {
        v.push_back(s[base + rm.pilot[i]] * pilotValue(static_cast<int>(i)));
        pos.push_back(static_cast<double>(base + rm.pilot[i]));
    }
    for (size_t i = kPreambleSyms; i + 1 < v.size(); i++)
        if (pos[i + 1] - pos[i] == 16.0) dacc += v[i + 1] * std::conj(v[i]);
    const double wc = std::arg(dacc) / 16.0;
    auto sumAt = [&](double w) {
        std::complex<double> acc(0, 0);
        for (size_t i = 0; i < v.size(); i++) acc += std::complex<double>(v[i]) * std::polar(1.0, -w * pos[i]);
        return acc;
    };
    constexpr double kStep = 0.00025, kRange = 0.03;
    const int nsteps = static_cast<int>(kRange / kStep);
    double bestM = -1, bestW = wc;
    std::vector<double> mets(2 * nsteps + 1);
    int bi = 0;
    {   // the scan: each term's phasor at the first trial is made once and then turned by its own step per trial (a complex product instead of a cos and a sin per term and trial)
        const size_t nv = v.size();
        std::vector<double> ar(nv), ai(nv), sr(nv), si(nv);
        for (size_t t = 0; t < nv; t++) {
            const std::complex<double> a0 = std::complex<double>(v[t]) * std::polar(1.0, -(wc - nsteps * kStep) * pos[t]), st = std::polar(1.0, -kStep * pos[t]);
            ar[t] = a0.real(); ai[t] = a0.imag(); sr[t] = st.real(); si[t] = st.imag();
        }
        for (int i = -nsteps; i <= nsteps; i++) {
            double accR = 0, accI = 0;
            for (size_t t = 0; t < nv; t++) {
                accR += ar[t]; accI += ai[t];
                const double nr = ar[t] * sr[t] - ai[t] * si[t];
                ai[t] = ar[t] * si[t] + ai[t] * sr[t];
                ar[t] = nr;
            }
            mets[i + nsteps] = std::sqrt(accR * accR + accI * accI);
            if (mets[i + nsteps] > bestM) { bestM = mets[i + nsteps]; bestW = wc + i * kStep; bi = i + nsteps; }
        }
    }
    if (bi > 0 && bi < 2 * nsteps) {
        const double m0 = mets[bi - 1], m1 = mets[bi], m2 = mets[bi + 1];
        const double den = m0 - 2 * m1 + m2;
        if (den < -1e-12) bestW += kStep * 0.5 * (m0 - m2) / den;
    }
    const auto acc = sumAt(bestW);
    Fit f;
    f.w = bestW;
    f.kref = 0.0;
    f.phi0 = std::arg(acc);
    f.a = std::max(1e-6, std::abs(acc) / static_cast<double>(v.size()));
    double res = 0;
    for (size_t i = 0; i < v.size(); i++) res += std::norm(v[i] - f.gain(static_cast<int>(pos[i])));
    f.res = res / static_cast<double>(v.size());
    return f;
}

// Solve the n x n complex system A x = b (Gaussian elimination, partial pivoting). False when singular.
bool solve(std::vector<std::complex<double>> A, std::vector<std::complex<double>> b, int n, std::vector<std::complex<double>>& x) {
    for (int c = 0; c < n; c++) {
        int piv = c;
        for (int r = c + 1; r < n; r++)
            if (std::abs(A[r * n + c]) > std::abs(A[piv * n + c])) piv = r;
        if (std::abs(A[piv * n + c]) < 1e-12) return false;
        if (piv != c) {
            for (int k = 0; k < n; k++) std::swap(A[c * n + k], A[piv * n + k]);
            std::swap(b[c], b[piv]);
        }
        for (int r = c + 1; r < n; r++) {
            const auto m = A[r * n + c] / A[c * n + c];
            for (int k = c; k < n; k++) A[r * n + k] -= m * A[c * n + k];
            b[r] -= m * b[c];
        }
    }
    x.assign(n, 0.0);
    for (int r = n - 1; r >= 0; r--) {
        auto acc = b[r];
        for (int k = r + 1; k < n; k++) acc -= A[r * n + k] * x[k];
        x[r] = acc / A[r * n + r];
    }
    return true;
}

}  // namespace

Receiver::Receiver(const Options& o) : o_(o), taps_(mfTaps()) {}

void Receiver::reset() {
    buf_.clear();
    mf_.clear();
    mfDone_ = scanPos_ = 0;
    absBase_ = 0;
    headerRejects_ = 0;
}

void Receiver::computeMf() {
    const long len = static_cast<long>(buf_.size());
    mf_.resize(buf_.size());
    const long to = std::max(mfDone_, len - kHalf);
    filterRange(buf_, taps_, mf_, mfDone_, to);
    mfDone_ = to;
}

double Receiver::metricAt(long n, cf* c1o, cf* c2o) const {
    if (n < 0) return 0.0;
    const auto& zc = preambleSymbols();
    cf c1(0, 0), c2(0, 0);
    double e = 0;
    for (int k = 0; k < kPreambleSyms; k++) {
        const cf y = mf_[n + k * kSps];
        e += std::norm(y);
        (k < kPreambleSyms / 2 ? c1 : c2) += y * std::conj(zc[k]);
    }
    if (c1o) *c1o = c1;
    if (c2o) *c2o = c2;
    if (e <= 1e-20) return 0.0;
    const double s = std::abs(c1) + std::abs(c2);
    return s * s / (static_cast<double>(kPreambleSyms) * e);
}

std::vector<RxFrame> Receiver::push(const cf* iq, size_t n) {
    std::vector<RxFrame> out;
    buf_.insert(buf_.end(), iq, iq + n);
    computeMf();
    const long preSpan = kPreambleSyms * kSps;
    for (;;) {
        const long mfEnd = mfDone_;
        const long last = mfEnd - preSpan - 16;
        long n0 = std::max(0L, scanPos_);
        bool found = false;
        for (; n0 < last; n0++)
            if (metricAt(n0) >= o_.detectThreshold) { found = true; break; }
        if (!found) { scanPos_ = std::max(scanPos_, last); break; }
        long best = n0;
        double bm = 0;
        for (long q = n0; q <= n0 + 8; q++) {
            const double m = metricAt(q);
            if (m > bm) { bm = m; best = q; }
        }
        cf c1, c2;
        const double m0 = metricAt(best, &c1, &c2);
        const double mm = metricAt(best - 1), mp = metricAt(best + 1);
        const double den = mm - 2 * m0 + mp;
        double dl = den < -1e-9 ? 0.5 * (mm - mp) / den : 0.0;
        dl = std::max(-0.5, std::min(0.5, dl));
        const double t0 = static_cast<double>(best) + dl;
        const double f0 = std::arg(c2 * std::conj(c1)) / (kTwoPi * (kPreambleSyms / 2));   // cycles per symbol
        RxFrame rf;
        double endSample = 0;
        const St st = tryFrame(t0, f0, rf, endSample);
        if (st == St::NeedMore) { scanPos_ = n0; break; }
        if (st == St::Reject) { headerRejects_++; scanPos_ = best + 16; continue; }
        rf.startSample = static_cast<double>(absBase_) + t0;
        out.push_back(std::move(rf));
        scanPos_ = std::max(scanPos_, static_cast<long>(endSample) - 16);
    }
    const long drop = scanPos_ - 400;
    if (drop > 0) {
        buf_.erase(buf_.begin(), buf_.begin() + drop);
        mf_.erase(mf_.begin(), mf_.begin() + drop);
        mfDone_ -= drop;
        scanPos_ -= drop;
        absBase_ += drop;
    }
    return out;
}

Receiver::St Receiver::tryFrame(double t0, double f0, RxFrame& rf, double& endSample) {
    const long mfEnd = mfDone_;
    const int hdrEnd = kPreambleSyms + kHeaderCodedBits;
    if (t0 + kSps * (hdrEnd + 1) + 4 >= static_cast<double>(mfEnd)) return St::NeedMore;
    const auto& zc = preambleSymbols();

    // ---- header
    std::vector<cf> s(hdrEnd);
    for (int k = 0; k < hdrEnd; k++) s[k] = interp(mf_, t0 + k * kSps) * cexpj(-kTwoPi * f0 * k);
    cf pre(0, 0);
    for (int k = 0; k < kPreambleSyms; k++) pre += s[k] * std::conj(zc[k]);
    const double a0 = std::abs(pre) / kPreambleSyms;
    if (a0 < 1e-9) return St::Reject;
    const cf rot = std::conj(pre) / static_cast<float>(std::abs(pre)) / static_cast<float>(a0);
    std::vector<cf> z(kHeaderCodedBits);
    for (int k = 0; k < kHeaderCodedBits; k++) z[k] = s[kPreambleSyms + k] * rot;
    // The V-sweep is 10 dB below the data, which makes a closed-form frequency estimate over the 140 header symbols too noisy (the phase would drift by up to
    // a radian at the end). The header is therefore tried with a grid of residual frequencies (|w| <= 0.1 rad/symbol, in the order of increasing |w|); for each one
    // the common phase comes from the squared symbols (BPSK) and the CRC-16 decides.
    FrameHeader hdr;
    double wr = 0.0;
    bool hdrOk = false;
    constexpr double kWStep = 0.008;
    for (int gi = 0; gi < 26 && !hdrOk; gi++) {
        const int idx = (gi + 1) / 2 * (gi % 2 ? 1 : -1);
        const double w = idx * kWStep;
        std::vector<cf> zz(kHeaderCodedBits);
        cf acc2(0, 0);
        for (int k = 0; k < kHeaderCodedBits; k++) {
            zz[k] = z[k] * cexpj(-w * (kPreambleSyms + k - (kPreambleSyms - 1) / 2.0));
            acc2 += zz[k] * zz[k];
        }
        const double th = std::arg(acc2) / 2.0;
        std::vector<float> hl(kHeaderCodedBits);
        for (int k = 0; k < kHeaderCodedBits; k++) hl[k] = 4.0f * (zz[k] * cexpj(-th)).real();
        const auto hb = kris::viterbiDecode(0, hl, true);
        if (hb.size() == static_cast<size_t>(kHeaderInfoBits) && parseHeader(hb.data(), hdr)) { hdrOk = true; wr = w; }
    }
    if (!hdrOk) return St::Reject;
    const Profile p = profileOf(hdr.profile, hdr.robust);
    if (o_.expectedSymRate > 0 && std::abs(p.symRate - o_.expectedSymRate) > 1.0) return St::Reject;
    if (hdr.sweepIdx >= p.sweepFrames) return St::Reject;
    const int F = p.frameSyms;
    if (t0 + kSps * (F + 1) + 8 >= static_cast<double>(mfEnd)) return St::NeedMore;

    // ---- pilot fit on the matched-filter output
    double f1 = f0 + wr / kTwoPi;   // cycles per symbol
    std::vector<cf> sy(F);
    // The preamble correlation peak is pulled by the V-sweep (up to 0.3 sample, which costs 64-QAM several dB), so the sampling instant is refined on the known symbols:
    // the shift (within +-0.4 sample, 0.1 steps, parabola) that maximises the amplitude of the preamble + pilot fit.
    auto refine = [&](const std::function<void(double)>& sampleAt) {
        double a[9];
        int bi = 0;
        for (int g = 0; g < 9; g++) {
            sampleAt((g - 4) * 0.1);
            a[g] = pilotFit(sy, p).a;
            if (a[g] > a[bi]) bi = g;
        }
        double d = (bi - 4) * 0.1;
        if (bi > 0 && bi < 8) {
            const double den = a[bi - 1] - 2 * a[bi] + a[bi + 1];
            if (den < -1e-12) d += 0.1 * 0.5 * (a[bi - 1] - a[bi + 1]) / den;
        }
        sampleAt(d);
        return d;
    };
    t0 += refine([&](double dl) {
        for (int k = 0; k < F; k++) sy[k] = interp(mf_, t0 + dl + k * kSps) * cexpj(-kTwoPi * f1 * k);
    });
    Fit fit = pilotFit(sy, p);
    double f2 = f1 + fit.w / kTwoPi;

    // ---- V-sweep removal on the carrier-corrected raw samples, then a second matched filter pass
    rf.chirpRemoved = false;
    if (o_.removeChirp) {
        const long sA = static_cast<long>(std::floor(t0)) - 28;
        const long segLen = static_cast<long>(F) * kSps + 60;
        const double off = t0 - static_cast<double>(sA);
        std::vector<cf> seg(segLen, cf(0, 0));
        for (long i = 0; i < segLen; i++) {
            const long g = sA + i;
            if (g < 0 || g >= static_cast<long>(buf_.size())) continue;
            const double tau = (static_cast<double>(i) - off) / kSps;
            seg[i] = buf_[g] * cexpj(-kTwoPi * f2 * tau);
        }
        const double period = static_cast<double>(p.sweepFrames) * F;
        const long i0 = static_cast<long>(std::ceil(off)), i1 = i0 + static_cast<long>(F) * kSps;
        std::vector<cf> ch(segLen, cf(0, 0));
        auto fillChirp = [&](double dSamples) {
            if (i1 > i0) chirpFill(static_cast<double>(hdr.sweepIdx) * F + (static_cast<double>(i0) - off - dSamples) / kSps, 1.0 / kSps, static_cast<int>(std::min(i1, segLen) - i0), period, ch.data() + i0);
        };
        // The chirp is sampled at the same instants as the data, but the timing estimate has an error of a fraction of a sample and the chirp sweeps up to 0.45 cycles per
        // symbol, so a small timing error is a large phase error of the chirp: a grid around the estimate picks the shift that matches best.
        double bestD = 0, bestV = -1;
        std::vector<double> vals;
        for (int gi = -4; gi <= 4; gi++) {
            fillChirp(gi * 0.1);
            cf acc(0, 0);
            for (long i = i0; i < i1 && i < segLen; i++) acc += seg[i] * std::conj(ch[i]);
            vals.push_back(std::abs(acc));
            if (vals.back() > bestV) { bestV = vals.back(); bestD = gi * 0.1; }
        }
        {
            const int bi = static_cast<int>(std::lround(bestD / 0.1)) + 4;
            if (bi > 0 && bi < 8) {
                const double den = vals[bi - 1] - 2 * vals[bi] + vals[bi + 1];
                if (den < -1e-12) bestD += 0.1 * 0.5 * (vals[bi - 1] - vals[bi + 1]) / den;
            }
        }
        fillChirp(bestD);
        const int nq = 1;
        for (int q = 0; q < nq; q++) {
            const long qa = i0 + (i1 - i0) * q / nq, qb = i0 + (i1 - i0) * (q + 1) / nq;
            cf num(0, 0);
            for (long i = qa; i < qb && i < segLen; i++) num += seg[i] * std::conj(ch[i]);
            const cf c = num / static_cast<float>(qb - qa);
            for (long i = qa; i < qb && i < segLen; i++) seg[i] -= c * ch[i];
        }
        std::vector<cf> sm(segLen, cf(0, 0));
        filterRange(seg, taps_, sm, kHalf, segLen - kHalf);
        refine([&](double dl) {
            for (int k = 0; k < F; k++) sy[k] = interp(sm, off + dl + k * kSps);
        });
        fit = pilotFit(sy, p);
        rf.chirpRemoved = true;
        // the segment was derotated by f2: the residual slope is in fit.w
        f2 += fit.w / kTwoPi;
    }

    // ---- one-tap equaliser, soft demap, decode
    std::vector<cf> zall(F);
    for (int k = 0; k < F; k++) zall[k] = sy[k] / fit.gain(k);
    double nv = std::max(1e-3, fit.res / (fit.a * fit.a));
    const auto& rm = regionMap(p.dataSyms);
    const int base = hdrEnd;
    const int kInfo = kris::ldpcK(kBlockN, p.rate);
    std::vector<std::vector<uint8_t>> infoBits(kBlocksPerFrame, std::vector<uint8_t>(kInfo));
    rf.blocks.assign(kBlocksPerFrame, RxBlock());
    auto decodeAll = [&](const std::vector<cf>& zz, double var) {
        std::vector<cf> d(rm.data.size());
        for (size_t i = 0; i < rm.data.size(); i++) d[i] = zz[base + rm.data[i]];
        std::vector<float> llrTx(d.size() * p.bitsPerSym), llr(kFrameCodedBits);
        kris::qamDemapSoft(d.data(), d.size(), p.mod, static_cast<float>(var), llrTx.data());
        deinterleaveDescrambleLlr(llrTx.data(), llr.data());
        int okCount = 0;
        for (int b = 0; b < kBlocksPerFrame; b++) {
            RxBlock& blk = rf.blocks[b];
            blk = RxBlock();
            blk.iters = kris::ldpcDecode(kBlockN, p.rate, llr.data() + b * kBlockN, infoBits[b].data(), 50);
            std::vector<uint8_t> bytes(p.blockPayloadBytes + 2, 0);
            for (int i = 0; i < kInfo; i++) bytes[i / 8] |= static_cast<uint8_t>((infoBits[b][i] & 1) << (7 - i % 8));
            const uint16_t crc = kris::crc16(bytes.data(), p.blockPayloadBytes);
            blk.ok = blk.iters >= 0 && crc == static_cast<uint16_t>((bytes[p.blockPayloadBytes] << 8) | bytes[p.blockPayloadBytes + 1]);
            blk.bytes.assign(bytes.begin(), bytes.begin() + p.blockPayloadBytes);
            okCount += blk.ok ? 1 : 0;
        }
        return okCount;
    };
    // Residual sweep cancellation with hard decisions, before the first decode: the raw-sample removal estimates the chirp against data that is itself noise to it (about
    // -20 to -30 dB of the data, worse near the band edge), which costs 64-QAM dearly. In the symbol domain the chirp is a tone against the decision errors only.
    auto hardTargets = [&](const std::vector<cf>& zz, double var) {
        std::vector<cf> d(F);
        for (int k = 0; k < kPreambleSyms; k++) d[k] = zc[k];
        const auto hs = headerSymbols(hdr);
        for (int k = 0; k < kHeaderCodedBits; k++) d[kPreambleSyms + k] = hs[k];
        std::vector<cf> dd(rm.data.size());
        for (size_t i = 0; i < rm.data.size(); i++) dd[i] = zz[base + rm.data[i]];
        std::vector<float> llr(dd.size() * p.bitsPerSym);
        kris::qamDemapSoft(dd.data(), dd.size(), p.mod, static_cast<float>(var), llr.data());
        std::vector<uint8_t> bits(llr.size());
        for (size_t i = 0; i < llr.size(); i++) bits[i] = llr[i] < 0 ? 1 : 0;
        const auto dm = kris::qamMap(bits.data(), bits.size(), p.mod);
        for (size_t i = 0; i < rm.pilot.size(); i++) d[base + rm.pilot[i]] = cf(pilotValue(static_cast<int>(i)), 0.0f);
        for (size_t i = 0; i < rm.data.size(); i++) d[base + rm.data[i]] = dm[i];
        return d;
    };
    rf.chirpCancelled = false;
    if (o_.removeChirp && rf.chirpRemoved) {
        const double period = static_cast<double>(p.sweepFrames) * F;
        std::vector<cf> chU(static_cast<size_t>(F));   // the chirp at the frame's symbols, worked out once (the same values the loops below asked for each time)
        for (int k = 0; k < F; k++) chU[static_cast<size_t>(k)] = chirpUnit(static_cast<double>(hdr.sweepIdx) * F + k, period);
        for (int iter = 0; iter < 2; iter++) {
            const std::vector<cf> d = hardTargets(zall, nv);
            constexpr int nq = 2;
            for (int q = 0; q < nq; q++) {
                const int qa = F * q / nq, qb = F * (q + 1) / nq;
                cf num(0, 0);
                for (int k = qa; k < qb; k++) num += (zall[k] - d[k]) * std::conj(chU[static_cast<size_t>(k)]);
                const cf c = num / static_cast<float>(qb - qa);
                for (int k = qa; k < qb; k++) zall[k] -= c * chU[static_cast<size_t>(k)];
            }
        }
        double pr = 0;
        for (size_t i = 0; i < rm.pilot.size(); i++) pr += std::norm(zall[base + rm.pilot[i]] - cf(pilotValue(static_cast<int>(i)), 0.0f));
        nv = std::max(1e-3, pr / static_cast<double>(rm.pilot.size()));
    }
    int okCount = decodeAll(zall, nv);
    rf.eqPasses = 0;

    // known symbols of the frame: preamble, header, pilots and the re-encoded decoder output (right where the blocks decoded, close to it elsewhere)
    auto buildTargets = [&]() {
        std::vector<cf> d(F);
        const auto hs = headerSymbols(hdr);
        for (int k = 0; k < kPreambleSyms; k++) d[k] = zc[k];
        for (int k = 0; k < kHeaderCodedBits; k++) d[kPreambleSyms + k] = hs[k];
        std::vector<uint8_t> code(kFrameCodedBits), tx(kFrameCodedBits);
        for (int b = 0; b < kBlocksPerFrame; b++) kris::ldpcEncode(kBlockN, p.rate, infoBits[b].data(), code.data() + b * kBlockN);
        interleaveScramble(code.data(), tx.data());
        const auto dm = kris::qamMap(tx.data(), tx.size(), p.mod);
        for (size_t i = 0; i < rm.pilot.size(); i++) d[base + rm.pilot[i]] = cf(pilotValue(static_cast<int>(i)), 0.0f);
        for (size_t i = 0; i < rm.data.size(); i++) d[base + rm.data[i]] = dm[i];
        return d;
    };

    // ---- residual sweep cancellation in the symbol domain: the chirp that the raw-sample removal left (its estimate is disturbed by the data itself, about -20 to -30 dB)
    // is a tone-like sequence in (equalised symbols - known symbols); estimated per quarter frame against the decoder's own re-encoded output and subtracted.
    if (okCount < kBlocksPerFrame && o_.removeChirp) {
        const double period = static_cast<double>(p.sweepFrames) * F;
        std::vector<cf> chU(static_cast<size_t>(F));
        for (int k = 0; k < F; k++) chU[static_cast<size_t>(k)] = chirpUnit(static_cast<double>(hdr.sweepIdx) * F + k, period);
        const std::vector<cf> d = buildTargets();
        std::vector<cf> zn = zall;
        constexpr int nq = 4;
        for (int q = 0; q < nq; q++) {
            const int qa = F * q / nq, qb = F * (q + 1) / nq;
            cf num(0, 0);
            for (int k = qa; k < qb; k++) num += (zall[k] - d[k]) * std::conj(chU[static_cast<size_t>(k)]);
            const cf c = num / static_cast<float>(qb - qa);
            for (int k = qa; k < qb; k++) zn[k] -= c * chU[static_cast<size_t>(k)];
        }
        double err = 0;
        for (int k = 0; k < F; k++) err += std::norm(zn[k] - d[k]);
        const double var = std::max(1e-3, err / F);
        auto saveBlocks = rf.blocks;
        auto saveInfo = infoBits;
        const int ok2 = decodeAll(zn, var);
        if (ok2 > okCount) {
            okCount = ok2;
            zall = zn;
            nv = var;
            rf.chirpCancelled = true;
        } else {
            rf.blocks = saveBlocks;
            infoBits = saveInfo;
        }
    }

    // ---- decision-directed FIR equaliser
    if (okCount < kBlocksPerFrame && o_.equalizer) {
        constexpr int L = 3, T = 2 * L + 1;
        for (int pass = 0; pass < 2 && okCount < kBlocksPerFrame; pass++) {
            const std::vector<cf> d = buildTargets();
            std::vector<std::complex<double>> M(T * T, 0.0), r(T, 0.0);
            for (int k = L; k < F - L; k++) {
                std::complex<double> x[T];
                for (int i = 0; i < T; i++) x[i] = std::complex<double>(zall[k + i - L]);
                const std::complex<double> dk(d[k]);
                for (int i = 0; i < T; i++) {
                    r[i] += std::conj(x[i]) * dk;
                    for (int j = 0; j < T; j++) M[i * T + j] += std::conj(x[i]) * x[j];
                }
            }
            double tr = 0;
            for (int i = 0; i < T; i++) tr += M[i * T + i].real();
            for (int i = 0; i < T; i++) M[i * T + i] += 1e-3 * tr / T;
            std::vector<std::complex<double>> c;
            if (!solve(M, r, T, c)) break;
            std::vector<cf> zn(F, cf(0, 0));
            double err = 0;
            int cnt = 0;
            for (int k = L; k < F - L; k++) {
                std::complex<double> o = 0.0;
                for (int i = 0; i < T; i++) o += c[i] * std::complex<double>(zall[k + i - L]);
                zn[k] = cf(static_cast<float>(o.real()), static_cast<float>(o.imag()));
                err += std::norm(std::complex<double>(zn[k]) - std::complex<double>(d[k]));
                cnt++;
            }
            for (int k = 0; k < L; k++) { zn[k] = zall[k]; zn[F - 1 - k] = zall[F - 1 - k]; }
            const double var = std::max(1e-3, err / std::max(1, cnt));
            // keep the better result
            auto saveBlocks = rf.blocks;
            auto saveInfo = infoBits;
            const int ok2 = decodeAll(zn, var);
            if (ok2 > okCount) {
                okCount = ok2;
                zall = zn;
                nv = var;
                rf.eqPasses = pass + 1;
            } else {
                rf.blocks = saveBlocks;
                infoBits = saveInfo;
                break;
            }
        }
    }
    rf.hdr = hdr;
    rf.prof = p;
    rf.allOk = okCount == kBlocksPerFrame;
    rf.esn0Db = 10.0 * std::log10(1.0 / nv);
    rf.cfoHz = f2 * p.symRate;
    {   // up to 400 of the equalised data symbols, evenly spread, for a constellation display
        const size_t nd = rm.data.size(), step = std::max<size_t>(1, nd / 400);
        for (size_t i = 0; i < nd; i += step) rf.dataSyms.push_back(zall[base + rm.data[i]]);
    }
    endSample = t0 + static_cast<double>(F) * kSps;
    return St::Ok;
}

}  // namespace dect2::kavb
