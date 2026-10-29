// LR-FHSS receiver (see lrfhss_rx.h). Synthetic tests only; not checked against a real transmitter.
#include "dect2/lrfhss/lrfhss_rx.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace dect2 {
namespace lrfhss {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kSps = 8;                       // samples a bit after the channel filter
constexpr double kChanRate = kBitRate * kSps;    // 3906.25 Hz
constexpr double kHeaderSec = kHeaderBits / kBitRate;
constexpr double kWinPadSec = 0.12;              // room around a detector window for the header search
constexpr double kClaimHz = 700;                 // a hop already followed covers candidates this close
constexpr int kSyncRef = 42;                     // header bit of the first sync bit
constexpr float kSyncQ = 7.f;                    // sync word found: |correlation|^2 / energy (32 at most, about 1 on noise)
using cd = std::complex<double>;
// half a sine over two bits (16 samples)
const float kHalfSine[16] = {0.0980171f, 0.2902847f, 0.4713967f, 0.6343933f, 0.7730105f, 0.8819213f, 0.9569403f, 0.9951847f,
                             0.9951847f, 0.9569403f, 0.8819213f, 0.7730105f, 0.6343933f, 0.4713967f, 0.2902847f, 0.0980171f};

size_t pow2At(double v) { size_t n = 1; while ((double)n < v) n <<= 1; return n; }
} // namespace

struct Receiver::Impl {
    double rate = 0, lo = 0, hi = 0;
    RxStats st;
    // input ring
    std::vector<cf32> ring;
    int64_t total = 0;
    // detector (spectrogram)
    size_t N = 0;
    double binHz = 0;
    std::unique_ptr<Fft> fft;
    std::vector<float> win, hist;
    std::vector<cf32> blk;
    size_t blkFill = 0;
    int W = 0, P = 0;
    int64_t frames = 0;
    std::vector<int> persist, persistNext;
    // channel filter: block sums of D1 samples, then a low-pass FIR, then interpolation to 8 samples a bit
    int D1 = 1;
    double r1 = 0, dTot = 0, enbw = 2000;   // enbw: noise bandwidth of the channel filter, Hz
    std::vector<float> taps;

    struct Ext { std::vector<cf32> y, d, zh; };   // see extract()
    struct HeaderJob { double f, absFirst; int K; int64_t readyAt; };
    std::vector<HeaderJob> jobs;
    struct Hop { double f = 0, rel = 0, absFirst = 0; int K = 0, q0 = 0, nbits = 0; int64_t readyAt = 0; bool done = false, ok = false; Ext e; };
    struct Track {
        Header h;
        Params p;
        int s = 0, pol = 1;
        double refAbs = 0, fh = 0, payloadAbs = 0;
        RxFrame out;
        std::vector<Hop> hops;
    };
    std::vector<Track> tracks;
    struct Claim { double t0, t1, f; };
    std::vector<Claim> claims;

    void setup(double r, double l, double h) {
        rate = r; lo = l; hi = h;
        st = RxStats();
        total = 0; frames = 0; blkFill = 0;
        jobs.clear(); tracks.clear(); claims.clear();
        if (rate <= 0) { ring.clear(); return; }
        if (lo == 0 && hi == 0) { lo = -0.45 * rate; hi = 0.45 * rate; }
        ring.assign((size_t)(0.8 * rate) + 4096, cf32(0, 0));
        N = std::max<size_t>(64, pow2At(rate / kBitRate));
        binHz = rate / (double)N;
        fft = std::make_unique<Fft>((int)N);
        win.resize(N);
        for (size_t i = 0; i < N; i++) win[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * ((double)i + 0.5) / (double)N));
        blk.assign(N, cf32(0, 0));
        W = std::max(8, (int)std::lround(kHeaderSec * 0.97 * rate / (double)N));
        P = std::max(2, W / 2);
        hist.assign((size_t)W * N, 0.f);
        persist.assign(N, 0); persistNext.assign(N, 0);
        D1 = std::max(1, (int)std::floor(rate / 31250.0));
        r1 = rate / D1;
        dTot = rate / kChanRate;
        const double fc = 1000.0 / r1;
        int half = (int)std::floor(1.6 * r1 / 1000.0);
        half = std::max(8, std::min(400, half));
        taps.assign((size_t)(2 * half + 1), 0.f);
        double sum = 0;
        for (int t = -half; t <= half; t++) {
            const double x = 2 * fc * t;
            const double sinc = t == 0 ? 2 * fc : std::sin(kPi * x) / (kPi * t);
            const double w = 0.42 + 0.5 * std::cos(kPi * t / (half + 1)) + 0.08 * std::cos(2 * kPi * t / (half + 1));
            taps[(size_t)(t + half)] = (float)(sinc * w);
            sum += sinc * w;
        }
        double e2 = 0;
        for (auto& v : taps) { v = (float)(v / sum); e2 += (double)v * v; }
        enbw = r1 * e2;
    }

    bool have(int64_t a, int64_t b) const { return a >= total - (int64_t)ring.size() && b <= total && a >= 0; }

    // The candidate mixed down by f Hz and filtered to 8 samples a bit. y[m] at input time absFirst + (m - 23) * dTot (K + 31 of them);
    // d[i] the one-bit differential product (a bit's worth of integration each side) and zh[i] the matched filter of MSK's main pulse (a half
    // sine over two bits), both for the bit boundary at absFirst + (i - 3.5) * dTot (K of them)
    bool extract(double f, double absFirst, int K, Ext& e) {
        e.d.assign((size_t)std::max(0, K), cf32(0, 0));
        e.zh.assign((size_t)std::max(0, K), cf32(0, 0));
        e.y.clear();
        if (K <= 0) return false;
        const int M = K + 31;
        const int hl = (int)(taps.size() / 2);
        const double y0 = absFirst - 23 * dTot;
        const double u0 = (y0 - (D1 - 1) * 0.5) / D1, u1 = (y0 + (M - 1) * dTot - (D1 - 1) * 0.5) / D1;
        const int64_t qa = (int64_t)std::floor(u0) - hl - 1, qb = (int64_t)std::floor(u1) + 1 + hl + 1;
        const int64_t s0 = qa * D1, s1 = (qb + 1) * D1;
        if (!have(s0, s1)) return false;
        // block sums of the mixed input
        std::vector<cf32> B((size_t)(qb - qa + 1));
        double cyc = f * (double)s0 / rate;
        cyc -= std::floor(cyc);
        cd ph = std::polar(1.0, -2 * kPi * cyc);
        const cd step = std::polar(1.0, -2 * kPi * f / rate);
        const size_t cap = ring.size();
        int64_t n = s0;
        for (size_t q = 0; q < B.size(); q++) {
            cd acc(0, 0);
            for (int k = 0; k < D1; k++, n++) {
                const cf32 x = ring[(size_t)(n % (int64_t)cap)];
                acc += cd(x.real(), x.imag()) * ph;
                ph *= step;
            }
            if ((q & 255) == 0) ph /= std::abs(ph);
            B[q] = cf32((float)(acc.real() / D1), (float)(acc.imag() / D1));
        }
        // low-pass FIR at r1, evaluated where the output needs it, then linear interpolation to 8 samples a bit
        const int64_t va = qa + hl, vb = qb - hl;
        std::vector<cf32> v((size_t)(vb - va + 1));
        for (int64_t q = va; q <= vb; q++) {
            cf32 acc(0, 0);
            const cf32* b = &B[(size_t)(q - hl - qa)];
            for (size_t t = 0; t < taps.size(); t++) acc += taps[t] * b[t];
            v[(size_t)(q - va)] = acc;
        }
        std::vector<cf32>& y = e.y;
        y.assign((size_t)M, cf32(0, 0));
        for (int m = 0; m < M; m++) {
            const double u = (y0 + m * dTot - (D1 - 1) * 0.5) / D1;
            const int64_t i = (int64_t)std::floor(u);
            const float fr = (float)(u - (double)i);
            if (i < va || i + 1 > vb) continue;
            const size_t a = (size_t)(i - va);
            y[(size_t)m] = v[a] * (1 - fr) + v[a + 1] * fr;
        }
        std::vector<cf32> zb((size_t)M, cf32(0, 0)), zs((size_t)M, cf32(0, 0));
        cf32 run(0, 0);
        for (int m = 0; m < M; m++) {
            run += y[(size_t)m];
            if (m >= 8) run -= y[(size_t)(m - 8)];
            zb[(size_t)m] = run;
            if (m >= 15) {
                cf32 acc(0, 0);
                for (int t = 0; t < 16; t++) acc += kHalfSine[t] * y[(size_t)(m - t)];
                zs[(size_t)m] = acc;
            }
        }
        for (int i = 0; i < K; i++) {
            e.d[(size_t)i] = zb[(size_t)(i + 23)] * std::conj(zb[(size_t)(i + 15)]);
            e.zh[(size_t)i] = zs[(size_t)(i + 27)];
        }
        return true;
    }

    void claim(double t0, double t1, double f) { claims.push_back({t0, t1, f}); }
    bool claimed(double t0, double t1, double f) const {
        for (const Claim& c : claims)
            if (std::fabs(c.f - f) < kClaimHz && c.t0 < t1 && c.t1 > t0) {
                const double ov = std::min(t1, c.t1) - std::max(t0, c.t0);
                if (ov > 0.5 * (c.t1 - c.t0)) return true;
            }
        return false;
    }

    void decide() {
        const int64_t endAbs = frames * (int64_t)N, startAbs = endAbs - (int64_t)W * (int64_t)N;
        std::vector<double> S(N, 0.0);
        for (int w = 0; w < W; w++) {
            const float* r = &hist[(size_t)w * N];
            for (size_t i = 0; i < N; i++) S[i] += r[i];
        }
        // natural order: index i is frequency (i - N/2) * binHz
        std::vector<double> nat(N), pre(N + 1, 0.0);
        for (size_t i = 0; i < N; i++) nat[i] = S[(i + N / 2) % N];
        for (size_t i = 0; i < N; i++) pre[i + 1] = pre[i] + nat[i];
        auto range = [&](long a, long b) { a = std::max(0L, a); b = std::min((long)N - 1, b); return b >= a ? std::make_pair(pre[(size_t)b + 1] - pre[(size_t)a], (double)(b - a + 1)) : std::make_pair(0.0, 0.0); };
        const double thr = 1 + 5.5 * 2 / (3 * std::sqrt((double)W));
        std::vector<double> r(N, 0.0);
        for (long i = 2; i + 2 < (long)N; i++) {
            const double f = ((double)i - (double)(N / 2)) * binHz;
            if (f < lo || f > hi) continue;
            // the floor on each side; the higher one counts, so that the edge of a wide signal (a LoRa channel) is no peak
            const auto a = range(i - 40, i - 5), b = range(i + 5, i + 40);
            if (a.second < 4 || b.second < 4) continue;
            const double mean = std::max(a.first / a.second, b.first / b.second);
            if (mean <= 0) continue;
            r[(size_t)i] = (nat[(size_t)i - 1] + nat[(size_t)i] + nat[(size_t)i + 1]) / (3 * mean);
        }
        std::fill(persistNext.begin(), persistNext.end(), 0);
        for (long i = 3; i + 3 < (long)N; i++) {
            const double v = r[(size_t)i];
            if (v <= thr || v < r[(size_t)i - 1] || v <= r[(size_t)i + 1] || v < r[(size_t)i - 2] || v <= r[(size_t)i + 2]) continue;
            const int prev = std::max(persist[(size_t)i - 1], std::max(persist[(size_t)i], persist[(size_t)i + 1]));
            persistNext[(size_t)i] = prev + 1;
            if (prev + 1 >= 5) continue;               // a carrier that stays (no hop lasts that long)
            const double a = nat[(size_t)i - 1], b = nat[(size_t)i], c = nat[(size_t)i + 1];
            const double den = a - 2 * b + c;
            const double delta = den != 0 ? std::max(-0.5, std::min(0.5, 0.5 * (a - c) / den)) : 0;
            const double f = ((double)i - (double)(N / 2) + delta) * binHz;
            if (claimed((double)startAbs, (double)endAbs, f)) continue;
            HeaderJob j;
            j.f = f;
            j.absFirst = (double)startAbs - kWinPadSec * rate;
            j.K = (int)(((double)(endAbs - startAbs) + 2 * kWinPadSec * rate) / dTot);
            j.readyAt = (int64_t)(j.absFirst + (j.K + 8) * dTot) + (int64_t)(taps.size() + 4) * D1;
            jobs.push_back(j);
            st.candidates++;
        }
        persist.swap(persistNext);
    }

    void stftBlock() {
        for (size_t i = 0; i < N; i++) blk[i] *= win[i];
        fft->forward(blk.data());
        float* row = &hist[(size_t)(frames % W) * N];
        for (size_t i = 0; i < N; i++) row[i] = std::norm(blk[i]);
        frames++;
        if (frames >= W && frames % P == 0) decide();
    }

    // header bits (0..113) of a header around d index tau (the first sync bit)
    static cf32 at(const std::vector<cf32>& d, long i) { return i >= 0 && i < (long)d.size() ? d[(size_t)i] : cf32(0, 0); }

    void runHeaderJob(const HeaderJob& j) {
        Ext e;
        if (!extract(j.f, j.absFirst, j.K, e)) return;
        const std::vector<cf32>& d = e.d;
        std::vector<float> sync(kSyncBits);
        for (int k = 0; k < kSyncBits; k++) sync[(size_t)k] = (kLorawanSync >> (31 - k)) & 1u ? 1.f : -1.f;
        const long tmin = 8L * kSyncRef, tmax = (long)d.size() - 1 - 8L * (kHeaderBits - 1 - kSyncRef);
        if (tmax <= tmin) return;
        std::vector<float> q((size_t)(tmax - tmin + 1), 0.f);
        for (long t = tmin; t <= tmax; t++) {
            cf32 c(0, 0);
            float en = 0;
            for (int k = 0; k < kSyncBits; k++) { const cf32 v = d[(size_t)(t + 8 * k)]; c += v * sync[(size_t)k]; en += std::norm(v); }
            q[(size_t)(t - tmin)] = en > 0 ? std::norm(c) / en : 0.f;
        }
        std::vector<long> peaks;
        for (int pk = 0; pk < 3; pk++) {
            long best = -1;
            for (long t = 0; t < (long)q.size(); t++) {
                bool near = false;
                for (long p : peaks) if (std::labs(p - t) < 24) near = true;
                if (!near && (best < 0 || q[(size_t)t] > q[(size_t)best])) best = t;
            }
            if (best < 0 || q[(size_t)best] < kSyncQ) break;
            peaks.push_back(best);
        }
        for (long pk : peaks) {
            st.syncs++;
            for (long dt : {0L, -1L, 1L, -2L, 2L}) {
                const long tau = pk + tmin + dt;
                cf32 c(0, 0);
                for (int k = 0; k < kSyncBits; k++) c += at(d, tau + 8 * k) * sync[(size_t)k] * cf32(0, -1);
                if (std::abs(c) <= 0) continue;
                const cf32 rot = std::conj(c) / std::abs(c);
                std::vector<float> soft;
                for (int i = 2; i < 42; i++) soft.push_back((at(d, tau + 8L * (i - kSyncRef)) * rot).imag());
                for (int i = 74; i < kHeaderBits; i++) soft.push_back((at(d, tau + 8L * (i - kSyncRef)) * rot).imag());
                Header h = decodeHeader(soft.data());
                // coherent detection when the differential one fails (weak signals): carrier and phase from the squared boundary samples
                for (int pol = 1; pol >= -1 && !h.ok; pol -= 2)
                    if (coherentHeader(e, tau, pol, soft)) h = decodeHeader(soft.data());
                if (!h.ok) continue;
                st.headers++;
                startTrack(h, e, tau, j);
                return;
            }
        }
    }

    // The 80 coded header bits by coherent detection, the data unknown: boundary samples of the matched filter turned back a quarter turn a bit
    // (pol: which way), squared (a tone at twice the carrier error), the tone found over the whole header block, then the sign changes.
    bool coherentHeader(const Ext& e, long tau, int pol, std::vector<float>& soft) const {
        const long b0 = tau - 8L * (kSyncRef + 1);       // zh index of the boundary before header bit 0
        if (b0 < 0 || b0 + 8L * kHeaderBits >= (long)e.zh.size()) return false;
        std::vector<cd> v;
        const cf32 turn(0.f, (float)-pol);
        cf32 r(1, 0);
        for (int n = 0; n <= kHeaderBits; n++) { const cf32 x = e.zh[(size_t)(b0 + 8L * n)] * r; v.push_back(cd(x.real(), x.imag())); r *= turn; }
        double bestM = -1, bestW = 0;
        cd bestAcc(0, 0);
        for (int g = -300; g <= 300; g++) {
            const double w = g * 0.005;
            cd acc(0, 0), rot(1, 0);
            const cd stp = std::polar(1.0, -w);
            for (const cd& x : v) { acc += x * x * rot; rot *= stp; }
            if (std::norm(acc) > bestM) { bestM = std::norm(acc); bestW = w; bestAcc = acc; }
        }
        const double psi = 0.5 * std::arg(bestAcc);
        std::vector<double> x(v.size());
        for (size_t n = 0; n < v.size(); n++) x[n] = (v[n] * std::polar(1.0, -(psi + 0.5 * bestW * (double)n))).real();
        soft.clear();
        auto bit = [&](int i) { return (float)(std::fabs(x[(size_t)i + 1] + x[(size_t)i]) - std::fabs(x[(size_t)i + 1] - x[(size_t)i])); };
        for (int i = 2; i < 42; i++) soft.push_back(bit(i));
        for (int i = 74; i < kHeaderBits; i++) soft.push_back(bit(i));
        return true;
    }

    // With the header bits known: which way the tones go (polarity), the carrier to about a hertz and the level, from the whole header hop
    // (the known MSK phase path taken off, the rest is a tone). pol: +1 when a 1 bit is the upper tone.
    bool headerCarrier(const Ext& e, long tau, const std::vector<int>& a, int& pol, double& df, double& amp, double& noise) const {
        const double mEnd0 = (double)tau - 8.0 * (kSyncRef + 1) + 19.5;   // y index of the start of header bit 0
        std::vector<double> cum(kHeaderBits + 1, 0.0);
        for (int i = 0; i < kHeaderBits; i++) cum[(size_t)i + 1] = cum[(size_t)i] + a[(size_t)i];
        const long m0 = (long)std::ceil(mEnd0), m1 = (long)std::floor(mEnd0 + 8.0 * kHeaderBits);
        if (m0 < 0 || m1 >= (long)e.y.size() || m1 <= m0) return false;
        const size_t L = (size_t)(m1 - m0 + 1);
        size_t nf = 1;
        while (nf < 2 * L) nf <<= 1;
        Fft fx((int)nf);
        double bestMag = -1, bestBin = 0;
        int bestPol = 1;
        double pw = 0;
        for (long m = m0; m <= m1; m++) pw += std::norm(e.y[(size_t)m]);
        pw /= (double)L;
        const double binHzF = kChanRate / (double)nf;
        const int maxBin = (int)(800.0 / binHzF);   // the detector gives the carrier to about half a 488 Hz bin, the filter passes 1 kHz
        std::vector<float> mag(nf);
        for (int p = 1; p >= -1; p -= 2) {
            std::vector<cf32> buf(nf, cf32(0, 0));
            for (long m = m0; m <= m1; m++) {
                const double pos = ((double)m - mEnd0) / 8.0;
                const int i = std::min(kHeaderBits - 1, std::max(0, (int)std::floor(pos)));
                const double ph = kPi / 2 * (cum[(size_t)i] + a[(size_t)i] * (pos - i)) * p;
                buf[(size_t)(m - m0)] = e.y[(size_t)m] * std::polar(1.f, (float)-ph);
            }
            fx.forward(buf.data());
            for (size_t k = 0; k < nf; k++) mag[k] = std::abs(buf[k]);
            for (int k = -maxBin; k <= maxBin; k++) {
                const float v = mag[(size_t)((k + (int)nf) % (int)nf)];
                if (v > bestMag) {
                    bestMag = v; bestPol = p;
                    const float l = mag[(size_t)((k - 1 + (int)nf) % (int)nf)], r = mag[(size_t)((k + 1 + (int)nf) % (int)nf)];
                    const float den = l - 2 * v + r;
                    bestBin = k + (den != 0 ? std::max(-0.5f, std::min(0.5f, 0.5f * (l - r) / den)) : 0.f);
                }
            }
        }
        pol = bestPol;
        df = bestBin * binHzF;
        amp = bestMag / (double)L;
        noise = std::max(1e-30, pw - amp * amp);
        return true;
    }

    void startTrack(const Header& h, const Ext& e, long tau, const HeaderJob& j) {
        Track t;
        t.h = h;
        t.p.cr = h.cr; t.p.grid = h.grid; t.p.bw = h.bw; t.p.hopping = h.hopping;
        t.p.headers = h.cr == Cr1_3 ? 3 : 2;
        t.s = h.syncIndex;
        // every bit of the header block is known now
        const std::vector<uint8_t> air = headerAirBits(h.raw);
        std::vector<int> a(kHeaderBits, -1);
        for (int i = 0; i < 40; i++) a[(size_t)(2 + i)] = air[(size_t)i] ? 1 : -1;
        for (int i = 0; i < kSyncBits; i++) a[(size_t)(42 + i)] = (kLorawanSync >> (31 - i)) & 1u ? 1 : -1;
        for (int i = 0; i < 40; i++) a[(size_t)(74 + i)] = air[(size_t)(40 + i)] ? 1 : -1;
        // the timing again, now over all 112 known bits (the sync word alone can be a sample or two off at a low SNR)
        {
            long bestTau = tau;
            float bestM = -1;
            for (long o = -6; o <= 6; o++) {
                cf32 c(0, 0);
                for (int i = 2; i < kHeaderBits; i++) c += at(e.d, tau + o + 8L * (i - kSyncRef)) * (float)a[(size_t)i];
                if (std::norm(c) > bestM) { bestM = std::norm(c); bestTau = tau + o; }
            }
            tau = bestTau;
        }
        int pol = 1;
        double df = 0, amp = 0, noise = 1;
        if (!headerCarrier(e, tau, a, pol, df, amp, noise)) return;
        t.pol = pol;
        t.out.ebN0Db = (float)(10 * std::log10(std::max(1e-6, amp * amp / kBitRate / (noise / enbw))));
        t.out.levelDb = (float)(10 * std::log10(std::max(1e-20, amp * amp)));
        t.refAbs = j.absFirst + (double)tau * dTot;
        t.fh = j.f + df;
        const HopParams hp = hopParams(t.p, h.hopSeqId);
        if (!hp.ok) return;
        const double offH = hopOffsetHz(t.p, hp.nGrid, gridPositionHeader(t.p, h.hopSeqId, t.s), 0, t.s);
        const double bitAbs = kSps * dTot;
        t.payloadAbs = t.refAbs + (kHeaderBits * (t.s + 1) - kSyncRef) * bitAbs;
        for (const Track& o : tracks)
            if (o.h.hopSeqId == h.hopSeqId && std::fabs(o.payloadAbs - t.payloadAbs) < 4 * bitAbs) return;   // another copy of a frame already followed
        const Digest dg = digest(t.p, (size_t)h.len);
        const int nHops = (dg.codedBits + kFragBits - 1) / kFragBits;
        for (int jh = 0; jh < nHops; jh++) {
            Hop hop;
            hop.q0 = kHeaderBits * (t.s + 1) + kBlockBits * jh;
            hop.nbits = jh + 1 < nHops ? kBlockBits : (dg.codedBits - kFragBits * jh) + 2;
            hop.rel = hopOffsetHz(t.p, hp.nGrid, gridPositionPayload(t.p, h.hopSeqId, jh), 0, -1) - offH;
            hop.f = t.fh + hop.rel;
            // bit boundaries from the one before bit q0 to the end, 4 samples to spare on each side for the timing search
            hop.absFirst = t.refAbs + (hop.q0 - 1 - kSyncRef) * bitAbs - 4 * dTot;
            hop.K = (hop.nbits + 2) * (int)kSps + 8;
            hop.readyAt = (int64_t)(hop.absFirst + (hop.K + 12) * dTot) + (int64_t)(taps.size() + 4) * D1;
            claim(hop.absFirst, hop.absFirst + hop.K * dTot, hop.f);
            t.hops.push_back(std::move(hop));
        }
        // the header copies still to come and this one: no new header search there
        for (int s2 = t.s; s2 >= 0; s2--) {
            const double f2 = t.fh - offH + hopOffsetHz(t.p, hp.nGrid, gridPositionHeader(t.p, h.hopSeqId, s2), 0, s2);
            const double b0 = t.refAbs + (kHeaderBits * (t.s - s2) - kSyncRef) * bitAbs;
            claim(b0, b0 + kHeaderBits * bitAbs, f2);
        }
        t.out.hdr = h;
        t.out.headers = t.p.headers;
        t.out.headerHz = t.fh;
        t.out.centreHz = t.fh - offH;
        const double first = t.refAbs - (kSyncRef + 1 + kHeaderBits * (t.p.headers - 1 - t.s)) * bitAbs;
        t.out.sec = std::max(0.0, first) / rate;
        t.out.hops = nHops;
        tracks.push_back(std::move(t));
    }

    void finishTrack(Track& t, std::vector<RxFrame>& out) {
        // Timing of each hop from the differential products, then coherent detection: the bit-boundary samples of the matched filter, turned
        // back by a quarter turn a bit, are +-1 (differentially coded); their squares are a tone. The carrier left on a hop is the frame's
        // error plus the transmitter's clock error times the hop's distance from the header: both searched over all hops together, the phase
        // then hop by hop.
        const int span = 3;
        const size_t nh = t.hops.size();
        std::vector<int> best(nh, 0);
        std::vector<std::vector<cf32>> v(nh);
        for (size_t k = 0; k < nh; k++) {
            const Hop& h = t.hops[k];
            if (!h.ok) continue;
            double bm = -1;
            for (int dt = -span; dt <= span; dt++) {
                double m = 0;
                for (int b = 2; b < h.nbits; b++) m += std::fabs(at(h.e.d, 4 + 8L * (b + 1) + dt).imag());
                if (m > bm) { bm = m; best[k] = dt; }
            }
            const cf32 turn(0.f, (float)-t.pol);
            cf32 r(1, 0);
            for (int n = 0; n <= h.nbits; n++) { v[k].push_back(at(h.e.zh, 4 + 8L * n + best[k]) * r); r *= turn; }
        }
        // the squares' spectrum of every hop on a grid of 0.005 rad a bit (0.19 Hz of carrier)
        constexpr int G = 401;
        constexpr double step = 0.005, w0 = -1.0;
        std::vector<std::vector<float>> U(nh, std::vector<float>(G, 0.f));
        for (size_t k = 0; k < nh; k++) {
            if (v[k].empty()) continue;
            for (int g = 0; g < G; g++) {
                const double w = w0 + step * g;
                cd acc(0, 0), rot(1, 0);
                const cd stp = std::polar(1.0, -w);
                for (const cf32& x : v[k]) { acc += cd(x.real(), x.imag()) * cd(x.real(), x.imag()) * rot; rot *= stp; }
                U[k][(size_t)g] = (float)std::norm(acc);
            }
        }
        // carrier error (Hz) and clock error (ppm) that put the most energy on the tones
        auto wOf = [&](double dfHz, double ppm, double rel) { return 2 * 2 * kPi * (dfHz + ppm * 1e-6 * rel) / kBitRate; };
        double bestM = -1, bestDf = 0, bestPpm = 0;
        for (int ip = -60; ip <= 60; ip++) {
            const double ppm = ip * 0.5;
            for (int id = -30; id <= 30; id++) {
                const double dfHz = id * 0.2;
                double m = 0;
                for (size_t k = 0; k < nh; k++) {
                    if (v[k].empty()) continue;
                    const int g = (int)std::lround((wOf(dfHz, ppm, t.hops[k].rel) - w0) / step);
                    if (g >= 0 && g < G) m += U[k][(size_t)g];
                }
                if (m > bestM) { bestM = m; bestDf = dfHz; bestPpm = ppm; }
            }
        }
        std::vector<float> soft;
        int heard = 0;
        for (size_t k = 0; k < nh; k++) {
            const Hop& h = t.hops[k];
            const int nd = h.nbits - 2;
            if (v[k].empty()) { soft.insert(soft.end(), (size_t)nd, 0.f); continue; }
            const double w = wOf(bestDf, bestPpm, h.rel);
            cd acc(0, 0), rot(1, 0);
            const cd stp = std::polar(1.0, -w);
            for (const cf32& x : v[k]) { acc += cd(x.real(), x.imag()) * cd(x.real(), x.imag()) * rot; rot *= stp; }
            const double psi = 0.5 * std::arg(acc);
            std::vector<double> r(v[k].size());
            for (size_t n = 0; n < v[k].size(); n++) r[n] = (cd(v[k][n].real(), v[k][n].imag()) * std::polar(1.0, -(psi + 0.5 * w * (double)n))).real();
            double A = 0, A2 = 0;
            for (double x : r) { A += std::fabs(x); A2 += x * x; }
            A /= (double)r.size(); A2 /= (double)r.size();
            const double var = std::max(1e-30, A2 - A * A);
            if (A * A / var > 2.0) heard++;
            const double wgt = A / var;
            // bit b of the block (data from b = 2) is the sign change between boundaries b and b + 1
            for (int b = 2; b < h.nbits; b++) {
                const double x0 = r[(size_t)b], x1 = r[(size_t)b + 1];
                soft.push_back((float)((std::fabs(x1 + x0) - std::fabs(x1 - x0)) * wgt));
            }
        }
        t.out.hopsHeard = heard;
        t.out.crcOk = decodePayload(t.p.cr, (size_t)t.h.len, soft.data(), t.out.payload, &t.out.bitErrors);
        if (t.out.crcOk) st.frames++; else st.crcBad++;
        out.push_back(t.out);
    }

    void process(std::vector<RxFrame>& out) {
        for (size_t i = 0; i < jobs.size();) {
            if (jobs[i].readyAt <= total) {
                const HeaderJob j = jobs[i];
                jobs.erase(jobs.begin() + (long)i);
                if (!claimed(j.absFirst + kWinPadSec * rate, j.absFirst + j.K * dTot - kWinPadSec * rate, j.f)) runHeaderJob(j);
            } else i++;
        }
        for (size_t i = 0; i < tracks.size();) {
            Track& t = tracks[i];
            bool all = true;
            for (Hop& h : t.hops) {
                if (h.done) continue;
                if (h.readyAt <= total) { h.ok = extract(h.f, h.absFirst, h.K, h.e); h.done = true; }
                else all = false;
            }
            if (all) { finishTrack(t, out); tracks.erase(tracks.begin() + (long)i); }
            else i++;
        }
        const double old = (double)total - (double)ring.size();
        claims.erase(std::remove_if(claims.begin(), claims.end(), [&](const Claim& c) { return c.t1 < old; }), claims.end());
    }
};

Receiver::Receiver() : p_(std::make_unique<Impl>()) {}
Receiver::~Receiver() = default;
void Receiver::configure(double r, double lo, double hi) { p_->setup(r, lo, hi); }
void Receiver::reset() { p_->setup(p_->rate, p_->lo, p_->hi); }
double Receiver::timeSec() const { return p_->rate > 0 ? (double)p_->total / p_->rate : 0; }
const RxStats& Receiver::stats() const { return p_->st; }
double Receiver::rate() const { return p_->rate; }

void Receiver::feed(const cf32* x, size_t n, std::vector<RxFrame>& out) {
    Impl& m = *p_;
    if (m.rate <= 0 || m.ring.empty()) return;
    const size_t cap = m.ring.size();
    size_t i = 0;
    while (i < n) {
        // up to the end of the current spectrogram block
        const size_t take = std::min(n - i, m.N - m.blkFill);
        for (size_t k = 0; k < take; k++) {
            const cf32 v = std::isfinite(x[i + k].real()) && std::isfinite(x[i + k].imag()) ? x[i + k] : cf32(0, 0);
            m.ring[(size_t)((m.total + (int64_t)k) % (int64_t)cap)] = v;
            m.blk[m.blkFill + k] = v;
        }
        m.total += (int64_t)take;
        m.blkFill += take;
        i += take;
        if (m.blkFill == m.N) { m.blkFill = 0; m.stftBlock(); }
        m.process(out);
    }
}

} // namespace lrfhss
} // namespace dect2
