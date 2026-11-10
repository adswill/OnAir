// cdma2000 / IS-95 pilot search and Sync Channel decoding (cdma.h). The pilots are found by a segmented FFT correlation against the
// complex pilot PN sequence (32 coherent segments of 1024 chips summed in power, carrier offset hypotheses by shifting the spectrum);
// each pilot is then tracked over the block (timing and sample clock from one correlation per PN period, the carrier from the pilot's
// phase), the Sync Channel (Walsh 32) is demodulated against the pilot, de-interleaved, Viterbi decoded and its capsules reassembled.
#include "dect2/cdma/cdma.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/dsp/rrc.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>

namespace dect2::cdma {

namespace {
constexpr int kSeg = 1024, kNSeg = kPnLen / kSeg;
constexpr int kAcqN = 2 * kPnLen;                 // acquisition FFT: two PN periods at one sample per chip
constexpr double kBinHz = kChipRate / kAcqN;      // 18.75 Hz
using cd = std::complex<double>;

const std::vector<cf32>& pnC() {                  // conj of the chip values, (I - jQ)
    static const std::vector<cf32> v = [] {
        std::vector<cf32> r(kPnLen);
        for (int i = 0; i < kPnLen; i++) r[(size_t)i] = cf32(pnI()[(size_t)i] ? -1.f : 1.f, pnQ()[(size_t)i] ? 1.f : -1.f);
        return r;
    }();
    return v;
}
// FFT of each zero padded segment of the reference, conjugated, for the acquisition correlation
const std::vector<std::vector<cf32>>& segRefs() {
    static const std::vector<std::vector<cf32>> v = [] {
        std::vector<std::vector<cf32>> r(kNSeg, std::vector<cf32>(kAcqN, cf32(0, 0)));
        Fft f(kAcqN);
        for (int s = 0; s < kNSeg; s++) {
            for (int i = 0; i < kSeg; i++) r[(size_t)s][(size_t)(s * kSeg + i)] = std::conj(pnC()[(size_t)(s * kSeg + i)]);   // chip values
            f.forward(r[(size_t)s].data());
            for (auto& z : r[(size_t)s]) z = std::conj(z);
        }
        return r;
    }();
    return v;
}

cf32 at(const std::vector<cf32>& y, double t) {   // linear interpolation
    if (t < 0) return cf32(0, 0);
    const size_t i = (size_t)t;
    if (i + 1 >= y.size()) return cf32(0, 0);
    const float f = (float)(t - (double)i);
    return y[i] * (1 - f) + y[i + 1] * f;
}

// Non-coherent pilot energy at sample position pos (start of a PN period), carrier w (rad / sample)
double periodEnergy(const std::vector<cf32>& y, double pos, double w, cd* coh = nullptr) {
    const auto& r = pnC();
    double e = 0; cd all = 0;
    for (int s = 0; s < kNSeg; s++) {
        cd acc = 0;
        const double t0 = pos + 2.0 * s * kSeg;
        const cd rot0 = std::polar(1.0, -w * t0), step = std::polar(1.0, -w * 2.0);
        cd rot = rot0;
        for (int i = 0; i < kSeg; i++) {
            const double t = t0 + 2.0 * i;
            const size_t k = (size_t)t;
            if (k + 1 >= y.size()) return e;
            const float f = (float)(t - (double)k);
            const cf32 v = y[k] * (1 - f) + y[k + 1] * f;
            acc += cd(v * r[(size_t)(s * kSeg + i)]) * rot;
            rot *= step;
        }
        e += std::norm(acc);
        all += acc;
    }
    if (coh) *coh = all;
    return e;
}

struct Cand { int d; double m; double cfo; };
}  // namespace

std::vector<Found> search(const cf32* x, size_t n, const SearchOpts& o, double* ms) {
    const auto t0c = std::chrono::steady_clock::now();
    std::vector<Found> out;
    if (n < (size_t)(2 * kAcqN + 4096)) return out;
    // matched filter (root raised cosine 0.2, 2 samples per chip)
    const int H = 12;
    std::vector<float> h(2 * H + 1);
    double he = 0;
    for (int k = -H; k <= H; k++) { h[(size_t)(k + H)] = (float)dsp::rrc(k / 2.0, 0.2); he += h[(size_t)(k + H)] * h[(size_t)(k + H)]; }
    for (auto& v : h) v = (float)(v / std::sqrt(he * 2));
    std::vector<cf32> y(n);
    for (size_t i = 0; i < n; i++) {
        cf32 a(0, 0);
        for (int k = -H; k <= H; k++) { const int64_t j = (int64_t)i + k; if (j >= 0 && j < (int64_t)n) a += x[j] * h[(size_t)(k + H)]; }
        y[i] = a;
    }
    double io = 0;
    for (auto& v : y) io += std::norm(v);
    io /= (double)n;
    if (io <= 0) return out;

    // ---- acquisition: one sample per chip (the even samples), two PN periods, all delays of one period
    std::vector<cf32> A(kAcqN);
    for (int i = 0; i < kAcqN; i++) A[(size_t)i] = y[(size_t)(2 * i)];
    Fft F(kAcqN);
    F.forward(A.data());
    const auto& R = segRefs();
    std::vector<float> best(kPnLen, 0.f), bestCfo(kPnLen, 0.f), acc(kPnLen);
    std::vector<cf32> P(kAcqN);
    const int binStep = 27;   // 506 Hz
    const int nb = (int)std::ceil(o.maxCfoHz / (binStep * kBinHz));
    for (int b = -nb; b <= nb; b++) {
        const int sh = b * binStep;
        std::fill(acc.begin(), acc.end(), 0.f);
        for (int s = 0; s < kNSeg; s++) {
            for (int k = 0; k < kAcqN; k++) P[(size_t)k] = A[(size_t)((k + sh + kAcqN) % kAcqN)] * R[(size_t)s][(size_t)k];
            F.inverse(P.data());
            for (int d = 0; d < kPnLen; d++) acc[(size_t)d] += std::norm(P[(size_t)d]);
        }
        for (int d = 0; d < kPnLen; d++) if (acc[(size_t)d] > best[(size_t)d]) { best[(size_t)d] = acc[(size_t)d]; bestCfo[(size_t)d] = (float)(sh * kBinHz); }
    }
    std::vector<float> srt(best);
    std::nth_element(srt.begin(), srt.begin() + kPnLen / 2, srt.end());
    const double med = srt[kPnLen / 2];
    std::vector<Cand> cands;
    for (int d = 0; d < kPnLen; d++) {
        const float v = best[(size_t)d];
        if (v < o.thresh * med) continue;
        bool peak = true;
        for (int k = -4; k <= 4 && peak; k++) if (k && best[(size_t)((d + k + kPnLen) % kPnLen)] > v) peak = false;
        if (peak) cands.push_back({d, v, bestCfo[(size_t)d]});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.m > b.m; });
    if ((int)cands.size() > o.maxCells + 2) cands.resize((size_t)o.maxCells + 2);
    if (getenv("ONAIR_CDMA_DEBUG")) for (auto& c : cands) fprintf(stderr, "[cdma] cand d %d m/med %.1f cfo %.0f\n", c.d, c.m / med, c.cfo);

    const int periods = (int)((n - 4096) / (2 * kPnLen));
    for (const Cand& c : cands) {
        Found fd;
        double w = 2 * M_PI * c.cfo / kFs;
        // ---- timing over the block: one correlation per PN period
        std::vector<double> px, py, pw;
        double pos = 2.0 * c.d, per = 2.0 * kPnLen;
        // the delay found refers to the first period; earlier start is pos - per if that fits
        for (int p = 0; p < periods; p++) {
            const double pred = pos + per * p;
            if (pred + per + 8 > (double)n) break;
            double e[9];
            for (int k = -4; k <= 4; k++) e[k + 4] = periodEnergy(y, pred + k, w);
            int bk = 0;
            for (int k = 1; k < 9; k++) if (e[k] > e[bk]) bk = k;
            double frac = 0;
            if (bk > 0 && bk < 8) { const double a = e[bk - 1], b = e[bk], cc = e[bk + 1], den = a - 2 * b + cc; if (den < 0) frac = 0.5 * (a - cc) / den; }
            px.push_back(p); py.push_back(pred + (bk - 4) + frac); pw.push_back(e[bk]);
            if (px.size() >= 2) {   // refit for the next prediction
                double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
                for (size_t i = 0; i < px.size(); i++) { sw += pw[i]; sx += pw[i] * px[i]; sy += pw[i] * py[i]; sxx += pw[i] * px[i] * px[i]; sxy += pw[i] * px[i] * py[i]; }
                const double den = sw * sxx - sx * sx;
                if (den > 0) { per = (sw * sxy - sx * sy) / den; pos = (sy - per * sx) / sw; }
            } else pos = py[0];
        }
        if (px.size() < 4) continue;
        fd.clockPpm = (per / (2.0 * kPnLen) - 1) * 1e6;
        const double scale = per / (2.0 * kPnLen);   // samples per half chip
        // ---- despread the pilot and the Sync Channel per Walsh period (64 chips)
        const int nW = (int)px.size() * (kPnLen / 64);
        std::vector<cd> zp((size_t)nW), zs((size_t)nW);
        const auto& r = pnC();
        double pyr = 0;
        for (int k = 0; k < nW; k++) {
            cd a = 0, b = 0;
            for (int ww = 0; ww < 64; ww++) {
                const int ch = k * 64 + ww;
                const double t = pos + 2.0 * scale * ch;
                const cd v = cd(at(y, t) * r[(size_t)(ch % kPnLen)]) * std::polar(1.0, -w * t);
                a += v;
                b += (ww & 32) ? -v : v;   // W32 of 64: + for the first 32 chips, - for the last 32
                if ((k & 15) == 0) pyr += std::norm(at(y, t));
            }
            zp[(size_t)k] = a; zs[(size_t)k] = b;
        }
        // carrier: the pilot's phase advance over 8, then 64 Walsh periods
        for (int lag : {8, 64}) {
            cd d = 0;
            for (int k = 0; k + lag < nW; k++) d += zp[(size_t)(k + lag)] * std::conj(zp[(size_t)k]);
            const double dw = std::arg(d) / lag;   // rad per Walsh period
            for (int k = 0; k < nW; k++) { const cd rr = std::polar(1.0, -dw * k); zp[(size_t)k] *= rr; zs[(size_t)k] *= rr; }
            w += dw / (64 * 2.0 * scale);
        }
        fd.cfoHz = w * kFs / (2 * M_PI);
        // ---- Sync Channel symbols (4 Walsh periods each) against the pilot
        const int nSym = nW / 4;
        std::vector<float> soft((size_t)nSym);
        double ep = 0;
        for (int m = 0; m < nSym; m++) {
            cd pr = 0;
            for (int k = 4 * m - 6; k < 4 * m + 10; k++) if (k >= 0 && k < nW) pr += zp[(size_t)k];
            cd s = 0;
            for (int k = 4 * m; k < 4 * m + 4; k++) s += zs[(size_t)k];
            const double ap = std::abs(pr);
            soft[(size_t)m] = ap > 0 ? (float)(std::real(s * std::conj(pr)) / ap) : 0.f;
            ep += std::norm(pr) / (16.0 * 16.0 * 64.0 * 64.0 * 2.0);   // |chip|^2 = 2
        }
        ep /= std::max(1, nSym);
        const double iod = pyr / std::max(1.0, (double)((nW + 15) / 16) * 64);
        fd.ecIoDb = 10 * std::log10(std::max(1e-12, ep / std::max(1e-20, iod)));
        fd.pilotDbfs = 10 * std::log10(std::max(1e-20, ep));
        // noise scale of the soft values: the Sync Channel is weak, so normalise by the spread
        double sv = 0;
        for (float v : soft) sv += v * v;
        sv = std::sqrt(sv / std::max(1, nSym));
        // ---- de-interleave, combine the repeats, decode
        const int nFr = nSym / 128;
        std::vector<float> llr;
        llr.reserve((size_t)nFr * 64);
        for (int f = 0; f < nFr; f++) {
            float in[128];
            for (int i = 0; i < 128; i++) in[interleaveAddr(i, 7, 1)] = soft[(size_t)(f * 128 + i)];
            for (int q = 0; q < 64; q++) llr.push_back(sv > 0 ? (float)((in[2 * q] + in[2 * q + 1]) / sv) : 0.f);
        }
        const Bits bits = viterbiK9(llr);
        SyncReassembler ra;
        for (int f = 0; f < nFr; f++) {
            std::vector<Bits> pdus;
            ra.frame(&bits[(size_t)f * 32], pdus);
            for (const Bits& pdu : pdus) {
                SyncMsg m;
                if (parseSyncPdu(pdu, m)) { fd.msg = m; fd.hasMsg = true; fd.msgs++; }
            }
        }
        fd.crcFails = ra.crcFails;
        fd.frames = nFr;
        fd.delaySamples = pos;
        bool dup = false;
        for (auto& e : out) {
            const double dd = std::fabs(std::remainder(e.delaySamples - pos, 2.0 * kPnLen));
            if (dd < 8 || (fd.hasMsg && e.hasMsg && e.msg.pilotPn == fd.msg.pilotPn)) dup = true;
        }
        if (dup) continue;
        if (!fd.hasMsg && fd.ecIoDb < -24) continue;
        out.push_back(fd);
        if ((int)out.size() >= o.maxCells) break;
    }
    if (!out.empty()) {
        const double ref = out[0].delaySamples;
        for (auto& f : out) {
            double d = std::fmod(f.delaySamples - ref, 2.0 * kPnLen);
            if (d < 0) d += 2.0 * kPnLen;
            f.relOffset = (int)std::lround(d / 2.0 / kOffsetUnit) % 512;
        }
    }
    if (ms) *ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0c).count();
    return out;
}

}  // namespace dect2::cdma
