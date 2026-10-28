// KAFT receiver: preamble search (lag-251 autocorrelation, then correlation with the width's Zadoff-Chu root), timing / carrier offset / phase from
// the preamble, header decode, pilot-based equalisation, per-block LDPC decode with the CRC-32. See kaft_rx.h.
#include "dect2/kaft/kaft_rx.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>

namespace dect2::kaft {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr size_t kLagSyms = (size_t)kZcLen;                       // 251
constexpr size_t kLag = kLagSyms * (size_t)kSps;                  // 1004 samples
constexpr size_t kWinSyms = (size_t)(kZcLen + kZcGuard);          // 261: the part of the preamble that repeats with lag 251
constexpr size_t kWin = kWinSyms * (size_t)kSps;                  // 1044 samples
constexpr int kMaxDown = 20000;                                     // above this the resampling ratio is taken from the rate rounded to 1 kHz

using cd = std::complex<double>;

// 8-tap Lanczos interpolator, 256 fractional positions
const float* lanczosTable() {
    static const std::vector<float> tab = [] {
        std::vector<float> t(257 * 8);
        auto sinc = [](double x) { return x == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x); };
        for (int q = 0; q <= 256; q++) {
            const double f = q / 256.0;
            double sum = 0;
            double w[8];
            for (int k = 0; k < 8; k++) {
                const double x = (double)(k - 3) - f;
                w[k] = std::fabs(x) < 4.0 ? sinc(x) * sinc(x / 4.0) : 0.0;
                sum += w[k];
            }
            for (int k = 0; k < 8; k++) t[(size_t)(q * 8 + k)] = (float)(w[k] / sum);
        }
        return t;
    }();
    return tab.data();
}

cf interp(const std::vector<cf>& x, double pos) {
    const double fl = std::floor(pos);
    long i0 = (long)fl;
    int q = (int)std::lround((pos - fl) * 256.0);
    if (q >= 256) { i0++; q = 0; }
    const float* w = lanczosTable() + q * 8;
    const long n = (long)x.size();
    float re = 0, im = 0;
    for (int k = 0; k < 8; k++) {
        const long idx = i0 - 3 + k;
        if (idx < 0 || idx >= n) continue;
        re += w[k] * x[(size_t)idx].real();
        im += w[k] * x[(size_t)idx].imag();
    }
    return cf(re, im);
}

// correlation of the interpolated symbol-rate samples starting at tau with the preamble, derotated by f (Hz)
double preambleRho(const std::vector<cf>& mf, double tau, double fHz, const std::vector<cf>& pre, double symRate) {
    cd c = 0;
    double e = 0, ep = 0;
    for (size_t i = 0; i < pre.size(); i++) {
        const cf s = interp(mf, tau + (double)kSps * (double)i);
        const double ph = -2.0 * kPi * fHz * (double)i / symRate;
        const cd r = cd(s.real(), s.imag()) * cd(std::cos(ph), std::sin(ph));
        c += r * std::conj(cd(pre[i].real(), pre[i].imag()));
        e += std::norm(r);
        ep += std::norm(cd(pre[i].real(), pre[i].imag()));
    }
    return std::norm(c) / std::max(e * ep, 1e-30);
}

}  // namespace

// ---------------------------------------------------------------------------------------------- receiver

FrameReceiver::FrameReceiver(const FrameReceiverConfig& cfg) : cfg_(cfg) {
    if (cfg_.inputRate < 1) cfg_.inputRate = kRefRate;
    // the exact ratio, unless it is huge: 64/7 Msps (9142857, DVB-T2's 8 MHz rate) shares almost no factor with the widths and needed 329 million
    // taps (1.3 GB, seconds on every engine start). The rate rounded to a whole kHz (16 ppm there, less than a radio's clock error) keeps it small.
    int ratioRate = cfg_.inputRate;
    for (int w = 0; w < kNumWidths; w++)
        if (cfg_.inputRate / std::gcd(kSps * kWidths[w].symRate, cfg_.inputRate) > kMaxDown) ratioRate = std::max(1000, (int)std::lround(cfg_.inputRate / 1000.0) * 1000);
    double longest = 0;
    for (int w = 0; w < kNumWidths; w++) {
        const int fsw = kSps * kWidths[w].symRate;
        const int g = std::gcd(fsw, ratioRate);
        up_[w] = fsw / g;
        down_[w] = ratioRate / g;
        const int syms = std::max(kWidths[w].frameSymbols, frameSymbolCount(kCtrlProfile, kCtrlMaxBlocks));
        longest = std::max(longest, (double)syms * kSps * down_[w] / up_[w]);
    }
    minProcess_ = (size_t)(longest * 1.2) + 4096;
    for (int w = 0; w < kNumWidths; w++) {   // the windowed-sinc taps of resample() (kaft_frame.cpp), once
        const int up = up_[w], down = down_[w];
        const int mx = std::max(up, down), H = 6 * mx;
        tapH_[w] = H;
        tap_[w].assign((size_t)(2 * H + 1), 0.f);
        for (int d = -H; d <= H; d++) {
            const double xx = (double)d / mx;
            const double sinc = d == 0 ? 1.0 : std::sin(kPi * xx) / (kPi * xx);
            const double win = 0.42 + 0.5 * std::cos(kPi * (double)d / H) + 0.08 * std::cos(2.0 * kPi * (double)d / H);
            tap_[w][(size_t)(d + H)] = (float)((double)up / mx * sinc * win);
        }
    }
}

namespace {
int64_t floorDivI(int64_t a, int64_t b) { int64_t q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) q--; return q; }
}

void FrameReceiver::extendDec(int w, bool final) {
    const int64_t up = up_[w], down = down_[w], H = tapH_[w];
    const int64_t endAbs = (int64_t)base_ + (int64_t)in_.size();   // exclusive
    std::vector<cf>& d = dec_[w];
    const int64_t m0 = decSt_[w] + (int64_t)d.size();
    int64_t m1;   // exclusive
    if (final) m1 = floorDivI(endAbs * up, down);
    else m1 = floorDivI((endAbs - 1) * up - H, down) + 1;
    if (m1 <= m0) return;
    const size_t oldN = d.size();
    d.resize(oldN + (size_t)(m1 - m0));
    const float* tap = tap_[w].data();
    for (int64_t m = m0; m < m1; m++) {
        const int64_t pos = m * down;
        int64_t k0 = floorDivI(pos - H + up - 1, up), k1 = floorDivI(pos + H, up);
        k0 = std::max<int64_t>(k0, (int64_t)base_);
        k1 = std::min<int64_t>(k1, endAbs - 1);
        float re = 0, im = 0;
        for (int64_t k = k0; k <= k1; k++) {
            const float t = tap[pos - k * up + H];
            const cf& x = in_[(size_t)(k - (int64_t)base_)];
            re += t * x.real();
            im += t * x.imag();
        }
        d[oldN + (size_t)(m - m0)] = cf(re, im);
    }
}

void FrameReceiver::extendMf(int w, bool final) {
    const std::vector<float>& h = rrcFilter();
    const long T = (long)h.size(), c = (T - 1) / 2;
    const std::vector<cf>& d = dec_[w];
    std::vector<cf>& mf = mf_[w];
    const long N = (long)d.size();
    const long n0 = (long)mf.size();
    const long n1 = final ? N : N - c;
    if (n1 <= n0) return;
    mf.resize((size_t)n1);
    const float* x = reinterpret_cast<const float*>(d.data());
    for (long n = n0; n < n1; n++) {
        float re, im;
        if (n >= c && n + c < N) {   // the filter is symmetric: one multiplication per pair of taps
            re = h[(size_t)c] * x[2 * n];
            im = h[(size_t)c] * x[2 * n + 1];
            const float* hp = h.data() + c;
            const float* xp = x + 2 * n;
            const float* xm = x + 2 * n;
            float r2 = 0.f, i2 = 0.f;
            for (long j = 1; j <= c; j++) {
                r2 += hp[j] * (xp[2 * j] + xm[-2 * j]);
                i2 += hp[j] * (xp[2 * j + 1] + xm[-2 * j + 1]);
            }
            re += r2;
            im += i2;
        } else {
            re = 0.f; im = 0.f;
            const long k0 = std::max<long>(0, c - n), k1 = std::min<long>(T - 1, N - 1 - n + c);
            for (long k = k0; k <= k1; k++) {
                re += h[(size_t)k] * x[2 * (n + k - c)];
                im += h[(size_t)k] * x[2 * (n + k - c) + 1];
            }
        }
        mf[(size_t)n] = cf(re, im);
    }
}

// drops the converted streams in front of the input that was dropped (a margin stays for the filters and the interpolator)
void FrameReceiver::trimFront() {
    for (int w = 0; w < kNumWidths; w++) {
        const int64_t keepFrom = floorDivI((int64_t)base_ * up_[w], down_[w]) - 128;
        int64_t n = std::min<int64_t>(keepFrom - decSt_[w], (int64_t)std::min(mf_[w].size(), dec_[w].size()));
        if (n <= 0) continue;
        dec_[w].erase(dec_[w].begin(), dec_[w].begin() + n);
        mf_[w].erase(mf_[w].begin(), mf_[w].begin() + n);
        decSt_[w] += n;
    }
}

void FrameReceiver::push(const cf* x, size_t n) {
    in_.insert(in_.end(), x, x + n);
    sinceProcess_ += n;
    if (in_.size() >= minProcess_ && sinceProcess_ >= minProcess_ / 2) process(false);
}

void FrameReceiver::flush() { process(true); }

std::vector<RxFrame> FrameReceiver::take() {
    std::vector<RxFrame> r;
    r.swap(frames_);
    return r;
}

void FrameReceiver::process(bool final) {
    sinceProcess_ = 0;
    if (in_.empty()) return;
    std::vector<Cand> cands;
    double off[kNumWidths] = {0, 0, 0};   // input position (relative to in_[0]) of mf_[w][0]
    for (int w = 0; w < kNumWidths; w++) {
        if (cfg_.forceWidth >= 0 && cfg_.forceWidth != w) continue;
        extendDec(w, final);
        extendMf(w, final);
        const std::vector<cf>& mf = mf_[w];
        const double inPerOwn = (double)down_[w] / (double)up_[w];
        off[w] = (double)decSt_[w] * inPerOwn - (double)base_;
        const size_t n = mf.size();
        if (n < kWin + kLag + 16) continue;
        // lag-251 autocorrelation with running sums
        std::vector<cd> cs(n - kLag + 1);
        std::vector<double> e(n + 1);
        cs[0] = 0;
        for (size_t i = 0; i + kLag < n; i++)
            cs[i + 1] = cs[i] + cd(mf[i].real(), mf[i].imag()) * std::conj(cd(mf[i + kLag].real(), mf[i + kLag].imag()));
        e[0] = 0;
        for (size_t i = 0; i < n; i++) e[i + 1] = e[i] + (double)std::norm(mf[i]);
        const size_t nS = n - kLag - kWin + 1;
        bool inRun = false;
        size_t best = 0, lastAbove = 0;
        double bestM = 0;
        cd bestP = 0;
        auto emit = [&]() {
            Cand c;
            c.w = w;
            const std::vector<cf>& pre = preambleSymbols((Width)w);
            const double symRate = kWidths[w].symRate;
            const double fs = kSps * symRate;
            long lo = (long)best - 12, hi = (long)best + 12;
            lo = std::max<long>(lo, 4);
            if ((size_t)hi + (size_t)kSps * (size_t)(kPreambleSyms - 1) + 16 >= n) {
                c.incomplete = true;
                c.posIn = ((double)best + 4.0) * inPerOwn + off[w];
                cands.push_back(c);
                return;
            }
            // coarse carrier offset from blocks of 16 symbols: the powers of the block correlations (insensitive to the rotation) pick the timing, the phase
            // between neighbouring blocks gives the offset (unambiguous to +-symRate/32), whatever the lag-251 phase says
            double f1 = -std::arg(bestP) * fs / (2.0 * kPi * (double)kLag);
            {
                constexpr int kB = 16, kNb = kPreambleSyms / kB;
                double bestBlk = -1;
                for (long t = lo; t <= hi; t++) {
                    cd b[kNb];
                    double tot = 0;
                    for (int j = 0; j < kNb; j++) {
                        cd acc = 0;
                        for (int i = 0; i < kB; i++) {
                            const size_t k = (size_t)(j * kB + i);
                            const cf& sv = mf[(size_t)t + (size_t)kSps * k];
                            acc += cd(sv.real(), sv.imag()) * std::conj(cd(pre[k].real(), pre[k].imag()));
                        }
                        b[j] = acc;
                        tot += std::norm(acc);
                    }
                    if (tot > bestBlk) {
                        bestBlk = tot;
                        cd d = 0;
                        for (int j = 0; j + 1 < kNb; j++) d += b[j + 1] * std::conj(b[j]);
                        f1 = std::arg(d) / (double)kB * symRate / (2.0 * kPi);
                    }
                }
            }
            // integer search with the coarse offset removed
            double bestV = -1;
            long bestT = lo;
            for (long t = lo; t <= hi; t++) {
                cd acc = 0;
                for (size_t i = 0; i < pre.size(); i++) {
                    const cf& s = mf[(size_t)t + (size_t)kSps * i];
                    const double ph = -2.0 * kPi * f1 * (double)i / symRate;
                    acc += cd(s.real(), s.imag()) * cd(std::cos(ph), std::sin(ph)) * std::conj(cd(pre[i].real(), pre[i].imag()));
                }
                const double v = std::norm(acc);
                if (v > bestV) { bestV = v; bestT = t; }
            }
            // fractional timing
            double tau = (double)bestT, bestR = -1;
            for (int k = -9; k <= 9; k++) {
                const double t = (double)bestT + 0.1 * k;
                const double r = preambleRho(mf, t, f1, pre, symRate);
                if (r > bestR) { bestR = r; tau = t; }
            }
            // carrier offset from the two repeats, symbol-rate samples
            std::vector<cf> s(pre.size());
            for (size_t i = 0; i < pre.size(); i++) {
                const cf v = interp(mf, tau + (double)kSps * (double)i);
                const double ph = -2.0 * kPi * f1 * (double)i / symRate;   // the coarse offset out, so that the repeats only have the residual to show
                s[i] = v * cf((float)std::cos(ph), (float)std::sin(ph));
            }
            cd p2 = 0;
            for (size_t i = 0; i + kLagSyms < pre.size(); i++)
                p2 += cd(s[i].real(), s[i].imag()) * std::conj(cd(s[i + kLagSyms].real(), s[i + kLagSyms].imag()));
            const double f2 = f1 - std::arg(p2) * symRate / (2.0 * kPi * (double)kLagSyms);
            const double rho = preambleRho(mf, tau, f2, pre, symRate);
            if (rho < (double)cfg_.verifyThreshold) return;
            c.tau = tau;
            c.cfoHz = (float)f2;
            c.rho = (float)rho;
            c.posIn = tau * inPerOwn + off[w];
            cands.push_back(c);
        };
        // Digital silence (exact zeros between frames, as in a recorded file made without noise): the differences of the running sums are then
        // rounding residue of both numerator and denominator, and their ratio would come out large. A window is only looked at when its energy is
        // a real share of the average.
        const double eFloor = 1e-6 * (e[n] / (double)n) * (double)kWin;
        for (size_t s = 0; s < nS; s++) {
            const cd P = cs[s + kWin] - cs[s];
            const double e1 = e[s + kWin] - e[s], e2 = e[s + kLag + kWin] - e[s + kLag];
            const double m = (e1 < eFloor || e2 < eFloor) ? 0.0 : std::abs(P) / std::sqrt(std::max(e1 * e2, 1e-30));
            if (m > (double)cfg_.detectThreshold) {
                if (!inRun) { inRun = true; best = s; bestM = m; bestP = P; }
                else if (m > bestM) { best = s; bestM = m; bestP = P; }
                lastAbove = s;
            } else if (inRun && s - lastAbove > kWin) {
                emit();
                inRun = false;
            }
        }
        if (inRun) emit();
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.posIn < b.posIn; });

    double nextOk = -1e300, consumed = 0;
    bool stalled = false;
    double stallPos = 0;
    double maxSymIn = 0;
    for (int w = 0; w < kNumWidths; w++) maxSymIn = std::max(maxSymIn, (double)kSps * down_[w] / up_[w]);
    for (const Cand& c : cands) {
        if (c.posIn < nextOk) continue;
        const double symIn = (double)kSps * down_[c.w] / up_[c.w];
        if (c.incomplete) {
            if (!final) { stalled = true; stallPos = c.posIn; break; }
            continue;
        }
        RxFrame f;
        double endIn = 0;
        const Status st = decodeAt(c.w, mf_[c.w], c, f, endIn);
        endIn += off[c.w];
        if (st == Status::Incomplete) {
            if (!final) { stalled = true; stallPos = c.posIn; break; }
            continue;
        }
        if (st == Status::Reject) continue;
        f.startSample = base_ + (uint64_t)std::max(0.0, c.posIn);
        f.endSample = base_ + (uint64_t)std::max(0.0, endIn);
        frames_.push_back(std::move(f));
        nextOk = endIn - 16.0 * symIn;
        consumed = endIn;
    }

    size_t dropTo;
    if (final) dropTo = in_.size();
    else if (stalled) dropTo = (size_t)std::max(0.0, stallPos - 64.0 * maxSymIn);
    else if (consumed > 0) dropTo = (size_t)std::max(0.0, std::min(consumed, (double)in_.size()) - 32.0 * maxSymIn);
    else {
        const size_t keep = (size_t)(800.0 * maxSymIn);
        dropTo = in_.size() > keep ? in_.size() - keep : 0;
    }
    dropTo = std::min(dropTo, in_.size());
    in_.erase(in_.begin(), in_.begin() + (long)dropTo);
    base_ += dropTo;
    trimFront();
}

FrameReceiver::Status FrameReceiver::decodeAt(int w, const std::vector<cf>& mf, const Cand& c, RxFrame& out, double& endIn) const {
    const WidthInfo& wi = kWidths[w];
    const double symRate = wi.symRate;
    const double inPerOwn = (double)down_[w] / (double)up_[w];
    const double margin = 48.0;
    auto avail = [&](int nSyms) { return c.tau + (double)kSps * (double)nSyms + margin < (double)mf.size(); };
    if (!avail(kPreambleSyms + kHeaderSyms)) return Status::Incomplete;

    const double w0 = -2.0 * kPi * (double)c.cfoHz / symRate;
    auto symAt = [&](size_t i) -> cf {
        const cf v = interp(mf, c.tau + (double)kSps * (double)i);
        const double ph = w0 * (double)i;
        return v * cf((float)std::cos(ph), (float)std::sin(ph));
    };
    const std::vector<cf>& pre = preambleSymbols((Width)w);
    std::vector<cf> s((size_t)(kPreambleSyms + kHeaderSyms));
    for (size_t i = 0; i < s.size(); i++) s[i] = symAt(i);

    // channel from the preamble
    cd num = 0;
    double den = 0;
    for (size_t i = 0; i < pre.size(); i++) {
        num += cd(s[i].real(), s[i].imag()) * std::conj(cd(pre[i].real(), pre[i].imag()));
        den += std::norm(cd(pre[i].real(), pre[i].imag()));
    }
    const cd h0 = num / std::max(den, 1e-30);
    double v0 = 0;
    for (size_t i = 0; i < pre.size(); i++) v0 += std::norm(cd(s[i].real(), s[i].imag()) - h0 * cd(pre[i].real(), pre[i].imag()));
    v0 /= (double)pre.size();
    const double a0 = std::norm(h0);
    if (a0 < 1e-20) return Status::Reject;

    // header
    std::vector<cf> eqH((size_t)kHeaderSyms);
    const cf ih((float)(h0.real() / a0), (float)(-h0.imag() / a0));   // 1 / h0
    for (size_t i = 0; i < eqH.size(); i++) eqH[i] = s[(size_t)kPreambleSyms + i] * ih;
    std::vector<float> llrH((size_t)kHeaderSyms);
    kris::qamDemapSoft(eqH.data(), eqH.size(), kris::Mod::Bpsk, (float)std::max(v0 / a0, 1e-6), llrH.data());
    const std::vector<uint8_t> hbits = kris::viterbiDecode(0, llrH, true);
    FrameHeader hd;
    if (!decodeHeaderBits(hbits, hd)) return Status::Reject;
    if ((int)hd.width != w) return Status::Reject;
    const int nb = hd.nBlocks;
    const int profile = hd.profile;
    if (nb < 1) return Status::Reject;
    if (hd.type == FrameType::Data ? nb > maxBlocksPerFrame(profile, hd.width) : nb > kCtrlMaxBlocks) return Status::Reject;
    const int N = frameSymbolCount(profile, nb);
    if (!avail(N)) return Status::Incomplete;

    // payload symbols
    const size_t nPay = (size_t)N - (size_t)(kPreambleSyms + kHeaderSyms);
    std::vector<cf> pay(nPay);
    for (size_t j = 0; j < nPay; j++) pay[j] = symAt((size_t)(kPreambleSyms + kHeaderSyms) + j);
    const size_t G = (nPay + (size_t)kPilotPeriod - 1) / (size_t)kPilotPeriod;
    const std::vector<cf> pil = pilotSymbols(G);
    std::vector<cd> g(G), gs(G);
    for (size_t k = 0; k < G; k++) {
        const cf p = pay[k * (size_t)kPilotPeriod] * std::conj(pil[k]);
        g[k] = cd(p.real(), p.imag());
    }
    // local linear (complex) smoothing of the pilot gains
    const long W = 12;
    for (long k = 0; k < (long)G; k++) {
        const long lo = std::max<long>(0, k - W), hi = std::min<long>((long)G - 1, k + W);
        double S0 = 0, S1 = 0, S2 = 0;
        cd Y0 = 0, Y1 = 0;
        for (long m = lo; m <= hi; m++) {
            const double x = (double)(m - k);
            S0 += 1;
            S1 += x;
            S2 += x * x;
            Y0 += g[(size_t)m];
            Y1 += x * g[(size_t)m];
        }
        const double det = S0 * S2 - S1 * S1;
        gs[(size_t)k] = det > 1e-9 ? (S2 * Y0 - S1 * Y1) / det : Y0 / S0;
    }
    double resid = 0, amp2 = 0;
    for (size_t k = 0; k < G; k++) {
        resid += std::norm(g[k] - gs[k]);
        amp2 += std::norm(gs[k]);
    }
    resid /= (double)G;
    amp2 /= (double)G;
    if (G >= 8) resid *= 1.09;   // the local fit absorbs about 2 of its 25 degrees of freedom
    if (amp2 < 1e-20) return Status::Reject;
    const double nvEq = resid / amp2;

    // equalised data symbols
    const int bps = bitsPerSymbol(profile);
    const kris::Mod mod = profileInfo(profile).mod;
    const size_t nData = (size_t)symbolsPerBlock(profile);
    std::vector<cf> eq(nData);
    std::vector<float> llr((size_t)kBlockBits);
    (void)bps;
    out.blocks.assign((size_t)nb, RxBlock());
    for (int b = 0; b < nb; b++) {
        for (size_t i = 0; i < nData; i++) {
            const size_t d = (size_t)b * nData + i;
            const size_t j = d + d / (size_t)(kPilotPeriod - 1) + 1;
            const size_t k = j / (size_t)kPilotPeriod, u = j % (size_t)kPilotPeriod;
            cd gain = gs[k];
            if (k + 1 < G) gain += (gs[k + 1] - gs[k]) * ((double)u / (double)kPilotPeriod);
            const cd r = cd(pay[j].real(), pay[j].imag()) / gain;
            eq[i] = cf((float)r.real(), (float)r.imag());
        }
        kris::qamDemapSoft(eq.data(), eq.size(), mod, (float)std::max(nvEq, 1e-6), llr.data());
        std::vector<uint8_t> payload;
        if (decodeBlock(profile, hd.frameNo, b, llr.data(), payload)) {
            out.blocks[(size_t)b].ok = true;
            out.blocks[(size_t)b].payload = std::move(payload);
        }
    }
    out.header = hd;
    out.esN0Db = (float)(10.0 * std::log10(amp2 / std::max(resid, 1e-30)));
    out.cfoHz = c.cfoHz;
    out.preambleRho = c.rho;
    endIn = (c.tau + (double)kSps * (double)N) * inPerOwn;
    return Status::Ok;
}

std::vector<RxFrame> decodeWaveform(const std::vector<cf>& x, const FrameReceiverConfig& cfg) {
    FrameReceiver rx(cfg);
    rx.push(x);
    rx.flush();
    return rx.take();
}

}  // namespace dect2::kaft
