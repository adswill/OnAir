// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAVL receiver (kavl/include/kavl/kavl_rx.h).
#include "kavl/kavl_rx.h"
#include "atsc3/atsc3_ldpc.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/dsp/ldpc.h"
#include "dect2/kris/kris_fec.h"
#include <algorithm>
#include <cmath>

namespace kavl {

namespace {

constexpr double kPi = 3.141592653589793;
constexpr double kTwoPi = 6.283185307179586;

// Raised-cosine pulse (the transmit and matched filters together) at x samples, 4 samples per symbol.
double rcPulse(double x) {
    const double t = x / kSps;
    const double s = std::fabs(t) < 1e-9 ? 1.0 : std::sin(kPi * t) / (kPi * t);
    const double d = 1.0 - (2.0 * kRrcBeta * t) * (2.0 * kRrcBeta * t);
    if (std::fabs(d) < 1e-6) return s * (kPi / 4.0);
    return s * std::cos(kPi * kRrcBeta * t) / d;
}

// Gain of the early-late timing detector: y(T+1) - y(T-1) = -2 eps rc'(1) for a timing error eps (samples).
double timingGain() {
    const double h = 1e-3;
    return -2.0 * (rcPulse(1.0 + h) - rcPulse(1.0 - h)) / (2.0 * h);
}

void lsLine(const std::vector<double>& x, const std::vector<double>& y, double& a, double& b) {
    const size_t n = x.size();
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; i++) {
        sx += x[i];
        sy += y[i];
        sxx += x[i] * x[i];
        sxy += x[i] * y[i];
    }
    const double den = n * sxx - sx * sx;
    b = std::fabs(den) > 1e-12 ? (n * sxy - sx * sy) / den : 0.0;
    a = n ? (sy - b * sx) / n : 0.0;
}

}  // namespace

struct Receiver::Impl {
    Options o;
    std::unique_ptr<dect2::ExactResampler> rs;
    std::vector<cf> rsOut;
    cf dc{0, 0}, w{0, 0};
    bool dcInit = false;
    std::vector<cf> buf, mf, q;   // input at 4 sps, matched filter output, q[k] = mf[k + 4] conj(mf[k])
    long mfDone = 0;              // mf[i] valid for i < mfDone
    long scan = 0;
    long long base = 0;           // absolute 4-sps index of buf[0]
    int rejects = 0;
    std::vector<cf> dref;         // conj of the preamble's symbol-to-symbol products
    double kt = timingGain();

    explicit Impl(const Options& op) : o(op) {
        const double r4 = kSps * o.symRate;
        if (o.inRate > 0 && std::fabs(o.inRate - r4) > 1e-6 * r4) {
            rs = std::make_unique<dect2::ExactResampler>();
            if (!rs->configure(o.inRate, r4)) rs.reset();
        }
        const auto& p = preambleSymbols();
        dref.resize(kPreambleSyms - 1);
        for (int m = 0; m < kPreambleSyms - 1; m++) dref[static_cast<size_t>(m)] = std::conj(p[static_cast<size_t>(m + 1)] * std::conj(p[static_cast<size_t>(m)]));
    }

    void reset() {
        if (rs) rs->reset();
        dc = w = cf(0, 0);
        dcInit = false;
        buf.clear();
        mf.clear();
        q.clear();
        mfDone = scan = 0;
        base = 0;
        rejects = 0;
    }

    // DC removal and blind IQ-imbalance correction of a chunk, in place.
    void iqFix(cf* x, size_t n) {
        if (!o.iqCorrect || n == 0) return;
        cf mean(0, 0);
        for (size_t i = 0; i < n; i++) mean += x[i];
        mean /= static_cast<float>(n);
        const float a = dcInit ? std::min(1.f, static_cast<float>(n) / 200000.f) : 1.f;
        dc = dc * (1.f - a) + mean * a;
        dcInit = true;
        double pr = 0, pi = 0, pw = 0;
        for (size_t i = 0; i < n; i++) {
            const cf y = x[i] - dc;
            const cf z = y - w * std::conj(y);
            x[i] = z;
            const cf z2 = z * z;
            pr += z2.real();
            pi += z2.imag();
            pw += std::norm(z);
        }
        if (pw > 1e-20) {
            const float g = std::min(1.f, static_cast<float>(n) / 100000.f);
            w += cf(static_cast<float>(pr / pw), static_cast<float>(pi / pw)) * (0.5f * g);
        }
    }

    void computeMf() {
        const auto& taps = rrc();
        const long nt = static_cast<long>(taps.size()), half = nt / 2;
        const long avail = static_cast<long>(buf.size()) - (nt - 1 - half);
        if (avail <= mfDone) return;
        mf.resize(static_cast<size_t>(avail));
        for (long i = mfDone; i < avail; i++) {
            cf acc(0, 0);
            const long s = i - half;
            for (long k = 0; k < nt; k++) {
                const long j = s + k;
                if (j >= 0) acc += buf[static_cast<size_t>(j)] * taps[static_cast<size_t>(k)];
            }
            mf[static_cast<size_t>(i)] = acc;
        }
        const long q0 = static_cast<long>(q.size());
        if (avail - kSps > q0) {
            q.resize(static_cast<size_t>(avail - kSps));
            for (long i = q0; i < avail - kSps; i++) q[static_cast<size_t>(i)] = mf[static_cast<size_t>(i + kSps)] * std::conj(mf[static_cast<size_t>(i)]);
        }
        mfDone = avail;
    }

    // Differential preamble metric at sample n (needs mf up to n + 4 * 63).
    double metric(long n, cf* dOut = nullptr) const {
        cf d(0, 0);
        double e = 0;
        for (int m = 0; m < kPreambleSyms - 1; m++) d += q[static_cast<size_t>(n + kSps * m)] * dref[static_cast<size_t>(m)];
        for (int m = 0; m < kPreambleSyms; m++) e += std::norm(mf[static_cast<size_t>(n + kSps * m)]);
        if (dOut) *dOut = d;
        return e > 1e-20 ? std::abs(d) / e : 0.0;
    }

    // Cubic (4-point Lagrange) interpolation of the matched filter output at fractional index t.
    cf at(double t) const {
        const long i = static_cast<long>(std::floor(t));
        const float m = static_cast<float>(t - static_cast<double>(i));
        auto v = [&](long k) { return (k >= 0 && k < mfDone) ? mf[static_cast<size_t>(k)] : cf(0, 0); };
        return v(i - 1) * (-m * (m - 1) * (m - 2) / 6.0f) + v(i) * ((m + 1) * (m - 1) * (m - 2) / 2.0f) + v(i + 1) * (-(m + 1) * m * (m - 2) / 2.0f) +
               v(i + 2) * ((m + 1) * m * (m - 1) / 6.0f);
    }

    bool decodeFrame(double t0, double f0, RxFrame& rf, double& endT);
    std::vector<RxFrame> push(const cf* iq, size_t n);
};

bool Receiver::Impl::decodeFrame(double t0, double f0, RxFrame& rf, double& endT) {
    const auto& pre = preambleSymbols();
    double a = 0.0, b = 0.0;   // timing: T(k) = t0 + a + (kSps + b) k
    auto T = [&](int k) { return t0 + a + (kSps + b) * k; };
    auto rot = [](double f, int k) {
        const double ph = -kTwoPi * f * k;
        return cf(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
    };

    // 1. fine carrier offset from the preamble quarters
    double f = f0;
    cf h(0, 0);
    double nvPre = 1.0;
    for (int pass = 0; pass < 2; pass++) {
        cf Q[4] = {};
        std::vector<cf> z(kPreambleSyms);
        for (int m = 0; m < kPreambleSyms; m++) {
            z[static_cast<size_t>(m)] = at(T(m)) * rot(f, m) * std::conj(pre[static_cast<size_t>(m)]);
            Q[m / 16] += z[static_cast<size_t>(m)];
        }
        cf acc(0, 0);
        for (int i = 0; i < 3; i++) acc += Q[i + 1] * std::conj(Q[i]);
        f += std::arg(acc) / (kTwoPi * 16.0);
        h = cf(0, 0);
        for (int m = 0; m < kPreambleSyms; m++) h += at(T(m)) * rot(f, m) * std::conj(pre[static_cast<size_t>(m)]);
        h /= static_cast<float>(kPreambleSyms);
        double nv = 0;
        for (int m = 0; m < kPreambleSyms; m++) nv += std::norm(at(T(m)) * rot(f, m) * std::conj(pre[static_cast<size_t>(m)]) - h);
        nvPre = std::max(1e-4, nv / kPreambleSyms / std::max(1e-20, static_cast<double>(std::norm(h))));
    }
    if (std::norm(h) < 1e-20) return false;

    // 2. header
    std::vector<float> hl(kHeaderCodedBits);
    for (int i = 0; i < kHeaderCodedBits; i++) {
        const int k = kHeaderStart + i;
        const cf y = at(T(k)) * rot(f, k) / h;
        hl[static_cast<size_t>(i)] = static_cast<float>(4.0 * y.real() / nvPre);
    }
    const auto hb = dect2::kris::viterbiDecode(0, hl, true);
    if (hb.size() < static_cast<size_t>(kHeaderInfoBits) || !parseHeader(hb.data(), rf.hdr)) return false;
    const Modcod* mc = modcod(rf.hdr.modcod);
    if (!mc) return false;
    rf.mc = mc;

    // 3. pilots: timing, carrier phase, gains
    const RegionMap& rm = regionMap();
    std::vector<int> pk(kPilots);
    std::vector<double> pkd(kPilots);
    for (int i = 0; i < kPilots; i++) {
        pk[static_cast<size_t>(i)] = kRegionStart + rm.pilot[static_cast<size_t>(i)];
        pkd[static_cast<size_t>(i)] = pk[static_cast<size_t>(i)];
    }
    auto smooth = [](const std::vector<cf>& g, int W) {
        std::vector<cf> s(g.size());
        const int n = static_cast<int>(g.size());
        for (int i = 0; i < n; i++) {
            cf acc(0, 0);
            int c = 0;
            for (int j = std::max(0, i - W); j <= std::min(n - 1, i + W); j++, c++) acc += g[static_cast<size_t>(j)];
            s[static_cast<size_t>(i)] = acc / static_cast<float>(c);
        }
        return s;
    };
    std::vector<cf> g(kPilots);
    auto pilotGains = [&]() {
        for (int i = 0; i < kPilots; i++) g[static_cast<size_t>(i)] = at(T(pk[static_cast<size_t>(i)])) * rot(f, pk[static_cast<size_t>(i)]) * pilotValue(i);
    };
    for (int it = 0; it < 3; it++) {
        pilotGains();
        const auto gs = smooth(g, 3);
        std::vector<double> e(kPilots);
        for (int i = 0; i < kPilots; i++) {
            const int k = pk[static_cast<size_t>(i)];
            const cf dlt = (at(T(k) + 1.0) - at(T(k) - 1.0)) * rot(f, k) * pilotValue(i);
            const double den = std::max(1e-20, static_cast<double>(std::norm(gs[static_cast<size_t>(i)]))) * kt;
            e[static_cast<size_t>(i)] = std::clamp((dlt * std::conj(gs[static_cast<size_t>(i)])).real() / den, -1.0, 1.0);
        }
        double ea, eb;
        lsLine(pkd, e, ea, eb);
        a += ea;
        b += eb;
    }
    // carrier phase line over the pilots (residual offset)
    pilotGains();
    {
        std::vector<double> ph(kPilots);
        double prev = std::arg(g[0]);
        ph[0] = prev;
        for (int i = 1; i < kPilots; i++) {
            double p = std::arg(g[static_cast<size_t>(i)]);
            while (p - prev > kPi) p -= kTwoPi;
            while (p - prev < -kPi) p += kTwoPi;
            ph[static_cast<size_t>(i)] = p;
            prev = p;
        }
        double pa, pb;
        lsLine(pkd, ph, pa, pb);
        f += pb / kTwoPi;
    }
    pilotGains();
    const int W = 2;
    const auto gs = smooth(g, W);
    double nvAbs = 0, sig = 0;
    for (int i = 0; i < kPilots; i++) {
        nvAbs += std::norm(g[static_cast<size_t>(i)] - gs[static_cast<size_t>(i)]);
        sig += std::norm(gs[static_cast<size_t>(i)]);
    }
    nvAbs = nvAbs / kPilots * (2.0 * W + 1.0) / (2.0 * W);
    sig /= kPilots;
    double n0 = std::max(1e-5, nvAbs / std::max(1e-20, sig));
    rf.esn0Db = 10.0 * std::log10(1.0 / n0);

    // raw (carrier-corrected, not equalised) data symbols and the pilot-interpolated gain
    std::vector<cf> yRaw(kRegionSyms), gain(kRegionSyms);
    for (int r = 0; r < kRegionSyms; r++) {
        const int k = kRegionStart + r;
        yRaw[static_cast<size_t>(r)] = at(T(k)) * rot(f, k);
    }
    for (int i = 0; i + 1 < kPilots; i++) {
        const int r0 = rm.pilot[static_cast<size_t>(i)], r1 = rm.pilot[static_cast<size_t>(i + 1)];
        for (int r = r0; r <= r1; r++) {
            const float u = static_cast<float>(r - r0) / static_cast<float>(r1 - r0);
            gain[static_cast<size_t>(r)] = gs[static_cast<size_t>(i)] * (1.f - u) + gs[static_cast<size_t>(i + 1)] * u;
        }
    }

    // 4. demap + decode
    const int bps = mc->bitsPerSym, nBits = mc->blocks * kLdpcN;
    const auto* qc = dect2::atsc3::qcCode(kLdpcN, mc->rate15);
    const float alpha = dect2::atsc3::qcAlpha(kLdpcN, mc->rate15);
    rf.blocks.assign(static_cast<size_t>(mc->blocks), RxBlock());
    std::vector<std::vector<uint8_t>> hard(static_cast<size_t>(mc->blocks));
    std::vector<cf> zs(kDataSyms);
    std::vector<float> llrTx(static_cast<size_t>(nBits)), llr(static_cast<size_t>(nBits));
    auto equalise = [&]() {
        for (int i = 0; i < kDataSyms; i++) {
            const int r = rm.data[static_cast<size_t>(i)];
            const cf gg = gain[static_cast<size_t>(r)];
            zs[static_cast<size_t>(i)] = std::norm(gg) > 1e-20f ? yRaw[static_cast<size_t>(r)] / gg : cf(0, 0);
        }
    };
    auto decodeBlocks = [&]() {
        dect2::kris::qamDemapSoft(zs.data(), zs.size(), mc->mod, static_cast<float>(n0), llrTx.data());
        deinterleaveDescrambleLlr(llrTx.data(), llr.data(), nBits);
        int okCount = 0;
        for (int bI = 0; bI < mc->blocks; bI++) {
            RxBlock& blk = rf.blocks[static_cast<size_t>(bI)];
            if (blk.ok) {
                okCount++;
                continue;
            }
            std::vector<float> l(llr.begin() + static_cast<long>(bI) * kLdpcN, llr.begin() + static_cast<long>(bI + 1) * kLdpcN);
            for (int i = mc->payloadBytes * 8 + 32; i < mc->kBits; i++) l[static_cast<size_t>(i)] = 50.f;   // known zero padding
            for (auto& v : l) v = std::clamp(v, -50.f, 50.f);
            std::vector<uint8_t> hd;
            int iters = -1;
            bool pass = false;
            if (qc) pass = qc->decodeFast(l, o.maxIter, hd, &iters, alpha, true);
            else pass = dect2::atsc3::ldpc16200(mc->rate15).decode(l, o.maxIter, hd, &iters);
            hard[static_cast<size_t>(bI)] = hd;
            if (!pass || hd.size() < static_cast<size_t>(mc->kBits)) continue;
            std::vector<uint8_t> bytes(static_cast<size_t>(mc->payloadBytes), 0);
            for (int i = 0; i < mc->payloadBytes * 8; i++) bytes[static_cast<size_t>(i / 8)] = static_cast<uint8_t>(bytes[static_cast<size_t>(i / 8)] | ((hd[static_cast<size_t>(i)] & 1) << (7 - i % 8)));
            uint32_t crc = 0;
            for (int i = 0; i < 32; i++) crc = (crc << 1) | (hd[static_cast<size_t>(mc->payloadBytes * 8 + i)] & 1u);
            if (crc != dect2::kris::crc32(bytes.data(), bytes.size())) continue;
            blk.ok = true;
            blk.iters = iters;
            blk.bytes = std::move(bytes);
            okCount++;
        }
        return okCount;
    };
    equalise();
    int okCount = decodeBlocks();

    // decisions of the whole frame (decoder output; exact for the code words that passed) -> data symbols
    auto decisions = [&]() {
        std::vector<uint8_t> coded(static_cast<size_t>(nBits), 0);
        for (int bI = 0; bI < mc->blocks; bI++) {
            const auto& hd = hard[static_cast<size_t>(bI)];
            if (hd.size() >= static_cast<size_t>(kLdpcN)) {
                std::copy(hd.begin(), hd.begin() + kLdpcN, coded.begin() + static_cast<long>(bI) * kLdpcN);
            } else {   // no decoder output: hard decisions of the LLRs
                for (int i = 0; i < kLdpcN; i++) coded[static_cast<size_t>(bI * kLdpcN + i)] = llr[static_cast<size_t>(bI * kLdpcN + i)] < 0 ? 1 : 0;
            }
        }
        std::vector<uint8_t> tx(coded.size());
        interleaveScramble(coded.data(), tx.data(), nBits);
        return dect2::kris::qamMap(tx.data(), tx.size(), mc->mod);
    };

    // 5. decision-directed passes
    const int L = 12;
    for (int pass = 0; o.ddPass && pass < 2 && okCount < mc->blocks; pass++) {
        const auto dec = decisions();
        std::vector<cf> ref(kRegionSyms);
        for (int i = 0; i < kPilots; i++) ref[static_cast<size_t>(rm.pilot[static_cast<size_t>(i)])] = cf(pilotValue(i), 0.f);
        for (int i = 0; i < kDataSyms; i++) ref[static_cast<size_t>(rm.data[static_cast<size_t>(i)])] = dec[static_cast<size_t>(i)];
        // least-squares gain over +-L symbols (running sums)
        std::vector<cf> num(kRegionSyms + 1, cf(0, 0));
        std::vector<double> den(kRegionSyms + 1, 0.0);
        for (int r = 0; r < kRegionSyms; r++) {
            num[static_cast<size_t>(r + 1)] = num[static_cast<size_t>(r)] + yRaw[static_cast<size_t>(r)] * std::conj(ref[static_cast<size_t>(r)]);
            den[static_cast<size_t>(r + 1)] = den[static_cast<size_t>(r)] + std::norm(ref[static_cast<size_t>(r)]);
        }
        std::vector<cf> g2(kRegionSyms);
        double err = 0, pw = 0;
        for (int r = 0; r < kRegionSyms; r++) {
            const int lo = std::max(0, r - L), hi = std::min(kRegionSyms, r + L + 1);
            const double dd = den[static_cast<size_t>(hi)] - den[static_cast<size_t>(lo)];
            g2[static_cast<size_t>(r)] = dd > 1e-20 ? (num[static_cast<size_t>(hi)] - num[static_cast<size_t>(lo)]) / static_cast<float>(dd) : gain[static_cast<size_t>(r)];
            err += std::norm(yRaw[static_cast<size_t>(r)] - g2[static_cast<size_t>(r)] * ref[static_cast<size_t>(r)]);
            pw += std::norm(g2[static_cast<size_t>(r)]);
        }
        const double n0new = std::max(1e-5, err / std::max(1e-20, pw));
        const auto savedGain = gain;
        const double savedN0 = n0;
        gain = g2;
        n0 = n0new;
        equalise();
        const int nowOk = decodeBlocks();
        rf.ddPasses = pass + 1;
        if (nowOk <= okCount) {   // no gain: keep the earlier estimate for the display and the noise figure
            gain = savedGain;
            n0 = savedN0;
            equalise();
            break;
        }
        okCount = nowOk;
    }

    // modulation error ratio on the final decisions
    {
        const auto dec = decisions();
        double e = 0, s = 0;
        for (int i = 0; i < kDataSyms; i++) {
            e += std::norm(zs[static_cast<size_t>(i)] - dec[static_cast<size_t>(i)]);
            s += std::norm(dec[static_cast<size_t>(i)]);
        }
        rf.merDb = e > 1e-20 ? 10.0 * std::log10(s / e) : 99.0;
    }
    rf.allOk = okCount == mc->blocks;
    rf.cfoHz = f * o.symRate;
    rf.clockPpm = -b / kSps * 1e6;
    rf.dataSyms.assign(zs.begin(), zs.begin() + std::min<long>(2000, static_cast<long>(zs.size())));
    endT = T(kFrameSyms);
    return true;
}

std::vector<RxFrame> Receiver::Impl::push(const cf* iq, size_t n) {
    std::vector<RxFrame> out;
    const size_t old = buf.size();
    if (rs) {
        rsOut.clear();
        std::vector<cf> tmp(iq, iq + n);
        iqFix(tmp.data(), tmp.size());
        rs->process(tmp.data(), tmp.size(), rsOut);
        buf.insert(buf.end(), rsOut.begin(), rsOut.end());
    } else {
        buf.insert(buf.end(), iq, iq + n);
        iqFix(buf.data() + old, n);
    }
    computeMf();

    const long win = kSps * (kPreambleSyms - 1);
    const double ratio = (o.inRate > 0 ? o.inRate : kSps * o.symRate) / (kSps * o.symRate);
    for (;;) {
        if (scan + win + 16 >= static_cast<long>(q.size())) break;
        const double m = metric(scan);
        if (m < o.detect) {
            scan++;
            continue;
        }
        // peak within the next 12 samples
        if (scan + 13 + win >= static_cast<long>(q.size())) break;
        long best = scan;
        double bm = m;
        for (long k = scan + 1; k <= scan + 12; k++) {
            const double mk = metric(k);
            if (mk > bm) {
                bm = mk;
                best = k;
            }
        }
        // the whole frame must be in the buffer
        const long need = best + static_cast<long>(kSps * kFrameSyms * 1.001) + 32;
        if (need >= mfDone) break;
        cf dpk;
        metric(best, &dpk);
        double frac = 0.0;
        if (best > 0) {
            const double m0 = metric(best - 1), m2 = metric(best + 1), den = m0 - 2.0 * bm + m2;
            if (std::fabs(den) > 1e-12) frac = std::clamp(0.5 * (m0 - m2) / den, -0.5, 0.5);
        }
        const double t0 = static_cast<double>(best) + frac;
        const double f0 = std::arg(dpk) / kTwoPi;
        RxFrame rf;
        double endT = 0;
        if (decodeFrame(t0, f0, rf, endT)) {
            rf.startSample = (static_cast<double>(base) + t0) * ratio;
            out.push_back(std::move(rf));
            scan = std::max(scan + 1, static_cast<long>(std::floor(endT)) - 3 * kSps);
        } else {
            rejects++;
            scan = best + kSps;
        }
    }
    // drop what is behind the scan position
    if (scan > (1L << 21)) {
        const long drop = scan - 4096;
        buf.erase(buf.begin(), buf.begin() + drop);
        mf.erase(mf.begin(), mf.begin() + drop);
        q.erase(q.begin(), q.begin() + std::min<long>(drop, static_cast<long>(q.size())));
        mfDone -= drop;
        scan -= drop;
        base += drop;
    }
    return out;
}

Receiver::Receiver() : Receiver(Options()) {}
Receiver::Receiver(const Options& o) : d_(std::make_unique<Impl>(o)) {}
Receiver::~Receiver() = default;
std::vector<RxFrame> Receiver::push(const cf* iq, size_t n) { return d_->push(iq, n); }
void Receiver::reset() { d_->reset(); }
int Receiver::headerRejects() const { return d_->rejects; }
cf Receiver::iqWeight() const { return d_->w; }

}  // namespace kavl
