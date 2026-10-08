#include "dect2/aero_demod.h"
#include "dect2/fftutil.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {

namespace {
constexpr double kPi = 3.14159265358979323846;

int popc32(uint32_t v) { return __builtin_popcount(v); }

// the 32 bits of the arm that holds bit `first` of a 64-bit window (bit 63 = oldest): first = 1 takes 63, 61, .., 1; first = 0 takes 62, .., 0
uint32_t armWord(uint64_t w, int first) {
    uint32_t r = 0;
    for (int i = 31; i >= 0; i--) r = (r << 1) | (uint32_t)((w >> (2 * i + first)) & 1);
    return r;
}

// windowed-sinc low-pass, cutoff as a fraction of the sample rate, Blackman window, unit DC gain
std::vector<float> lowpass(int len, double cutoff) {
    std::vector<float> h(len);
    const double c = (len - 1) / 2.0;
    double sum = 0;
    for (int n = 0; n < len; n++) {
        const double t = n - c;
        const double s = t == 0 ? 2 * cutoff : std::sin(2 * kPi * cutoff * t) / (kPi * t);
        const double w = 0.42 - 0.5 * std::cos(2 * kPi * n / (len - 1)) + 0.08 * std::cos(4 * kPi * n / (len - 1));
        h[n] = (float)(s * w);
        sum += s * w;
    }
    for (auto& v : h) v = (float)(v / sum);
    return h;
}

// FIR on complex samples with a doubled delay line (no wrap in the inner loop)
class Fir {
public:
    void setTaps(std::vector<float> h) {
        h_ = std::move(h);
        line_.assign(2 * h_.size(), cf32(0, 0));
        pos_ = 0;
    }
    void clear() { std::fill(line_.begin(), line_.end(), cf32(0, 0)); pos_ = 0; }
    void push(cf32 x) {
        const size_t L = h_.size();
        line_[pos_] = x;
        line_[pos_ + L] = x;
        pos_ = pos_ + 1 == L ? 0 : pos_ + 1;
    }
    cf32 out() const {   // newest sample meets h[0]
        const size_t L = h_.size();
        const cf32* d = &line_[pos_];      // d[0] oldest .. d[L-1] newest
        float re = 0, im = 0;
        for (size_t k = 0; k < L; k++) { re += h_[L - 1 - k] * d[k].real(); im += h_[L - 1 - k] * d[k].imag(); }
        return cf32(re, im);
    }
    size_t len() const { return h_.size(); }
private:
    std::vector<float> h_;
    std::vector<cf32> line_;
    size_t pos_ = 0;
};
} // namespace

double aeroRrc(double t, double a) {
    const double eps = 1e-8;
    if (std::fabs(t) < eps) return 1.0 - a + 4.0 * a / kPi;
    if (a > 0 && std::fabs(std::fabs(t) - 1.0 / (4.0 * a)) < eps)
        return a / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * a)) + (1 - 2 / kPi) * std::cos(kPi / (4 * a)));
    const double num = std::sin(kPi * t * (1 - a)) + 4 * a * t * std::cos(kPi * t * (1 + a));
    const double den = kPi * t * (1 - (4 * a * t) * (4 * a * t));
    return num / den;
}

// ======================= framer =======================
struct AeroFramer::Impl {
    AeroFrameFormat fmt;
    AeroFrameDecoder dec;
    std::function<void(AeroFrameEvent&)> cb;
    static constexpr size_t kHist = 16384;
    std::vector<float> hist = std::vector<float>(kHist, 0.f);
    uint64_t idx = 0;                  // bits pushed so far
    uint64_t hard = 0;                 // last 64 hard bits, newest in bit 0
    enum State { Search, Collect, Expect } st = Search;
    uint64_t frameStart = 0, nextUwEnd = 0;
    bool fromSearch = false;
    int invPar[2] = {0, 0};            // inversion of the bits at even / odd positions (MSK: both the same)
    int lastUwErr = 0;
    int misses = 0, badFrames = 0;
    int dcd = 0;
    uint64_t nFrames = 0, nMiss = 0, nUwErr = 0, nFalse = 0;
    std::vector<float> coded;
    AeroFrameEvent ev;

    explicit Impl(int rate) : dec(rate) {
        const AeroFrameFormat* f = aeroFrameFormat(rate);
        if (f) fmt = *f;
        coded.resize(fmt.codedBits);
    }
    float at(uint64_t i) const { return hist[i & (kHist - 1)]; }
    int need() const { return fmt.headerBits + fmt.skipBits + fmt.codedBits; }

    // unique word ending at bit `end` (absolute): errors after the best inversion; sets inv[] for the bits of that frame
    int uwErrors(uint64_t end, int inv[2]) const {
        uint64_t w = 0;
        for (int k = fmt.uwBits - 1; k >= 0; k--) w = (w << 1) | (uint64_t)(at(end - (uint64_t)k) > 0);
        return uwErrorsWord(w, end, inv);
    }
    int uwErrorsWord(uint64_t w, uint64_t end, int inv[2]) const {
        if (!fmt.oqpsk()) {
            const int e = popc32((uint32_t)w ^ kAeroUw);
            const int i = e > 16;
            inv[0] = inv[1] = i;
            return i ? 32 - e : e;
        }
        // bit 0 of the window is position `end`; bit 1 is end - 1
        const int eB = popc32(armWord(w, 0) ^ kAeroUw), eA = popc32(armWord(w, 1) ^ kAeroUw);
        const int iB = eB > 16, iA = eA > 16;
        inv[end & 1] = iB;
        inv[(end + 1) & 1] = iA;
        return std::max(iA ? 32 - eA : eA, iB ? 32 - eB : eB);    // the worse arm decides
    }

    void push(float s) {
        hist[idx & (kHist - 1)] = s;
        hard = (hard << 1) | (uint64_t)(s > 0);
        const uint64_t cur = idx++;
        if (st == Search) {
            if (trySync(cur)) dec.reset();
            return;
        }
        if (st == Expect) {
            if (cur < nextUwEnd + 1) return;
            // the word may end one bit early or late (timing slip): take the best of the three
            int best = 99, bi[2] = {0, 0};
            uint64_t bestEnd = nextUwEnd;
            for (int d = -1; d <= 1; d++) {
                int inv[2];
                const uint64_t end = nextUwEnd + (uint64_t)(int64_t)d;
                const int e = uwErrors(end, inv);
                if (e < best || (e == best && d == 0)) { best = e; bestEnd = end; bi[0] = inv[0]; bi[1] = inv[1]; }
            }
            if (best <= 7) {
                invPar[0] = bi[0]; invPar[1] = bi[1];
                lastUwErr = best;
                nUwErr += (uint64_t)best;
                frameStart = bestEnd + 1;
                misses = 0;
            } else {
                lastUwErr = -1;
                nMiss++;
                frameStart = nextUwEnd + 1;
                if (++misses > 3) { st = Search; return; }
            }
            fromSearch = false;
            st = Collect;
            // the frame may already have started: fall through to Collect with the bits already here
        }
        if (st == Collect) {
            // a frame taken on timing alone: the word may turn up elsewhere (samples were lost), so keep looking for it
            if (lastUwErr < 0 && cur + 1 > frameStart && trySync(cur)) return;
            if (cur + 1 < frameStart + (uint64_t)need()) return;
            frame();
        }
    }

    // the word ending at bit `cur`, strictly (searching): start a frame after it
    bool trySync(uint64_t cur) {
        if (cur + 1 < (uint64_t)fmt.uwBits) return false;
        int inv[2];
        const uint64_t w = fmt.oqpsk() ? hard : (hard & 0xFFFFFFFFull);
        const int e = uwErrorsWord(w, cur, inv);
        if (e > 3) return false;
        invPar[0] = inv[0]; invPar[1] = inv[1];
        lastUwErr = e;
        frameStart = cur + 1;
        fromSearch = true;
        misses = 0;
        st = Collect;
        return true;
    }

    void frame() {
        uint16_t header = 0;
        uint64_t p = frameStart;
        auto bitAt = [&](uint64_t i) { float v = at(i); if (invPar[i & 1]) v = -v; return v; };
        for (int k = 0; k < fmt.headerBits; k++, p++) header = (uint16_t)((header << 1) | (bitAt(p) > 0));
        p += (uint64_t)fmt.skipBits;
        for (int k = 0; k < fmt.codedBits; k++, p++) coded[k] = bitAt(p);
        AeroDecodedFrame df;
        dec.decode(coded.data(), header, df);
        const int formatId = header >> 12;
        if (fromSearch && formatId != 1 && df.susOk == 0) {    // a chance match of the word: look again
            nFalse++;
            st = Search;
            return;
        }
        for (int k = 0; k < df.susOk; k++) dcd = std::min(dcd + 2, 12);
        for (int k = 0; k < df.susBad; k++) dcd = std::max(dcd - 3, 0);
        badFrames = df.susOk ? 0 : badFrames + 1;
        nFrames++;
        ev.bitRate = fmt.bitRate;
        ev.header = header;
        ev.bytes = std::move(df.bytes);
        ev.susOk = df.susOk; ev.susBad = df.susBad;
        ev.uwErrors = lastUwErr;
        ev.channelBer = df.channelBer;
        ev.bitIndex = frameStart;
        if (cb) cb(ev);
        nextUwEnd = frameStart + (uint64_t)need() - 1 + (uint64_t)fmt.uwBits;
        st = badFrames >= 4 ? Search : Expect;
    }
};

AeroFramer::AeroFramer(int bitRate) : p_(std::make_unique<Impl>(bitRate)) {}
AeroFramer::~AeroFramer() = default;
void AeroFramer::reset() {
    auto cb = std::move(p_->cb);
    const int rate = p_->fmt.bitRate;
    p_ = std::make_unique<Impl>(rate);
    p_->cb = std::move(cb);
}
void AeroFramer::push(float s) { p_->push(s); }
void AeroFramer::setCallback(std::function<void(AeroFrameEvent&)> cb) { p_->cb = std::move(cb); }
bool AeroFramer::synced() const { return p_->st != Impl::Search; }
bool AeroFramer::dataLock() const { return p_->st != Impl::Search && p_->dcd > 2; }
uint64_t AeroFramer::frames() const { return p_->nFrames; }
uint64_t AeroFramer::uwMisses() const { return p_->nMiss; }
uint64_t AeroFramer::uwBitErrors() const { return p_->nUwErr; }
uint64_t AeroFramer::falseSyncs() const { return p_->nFalse; }

// ======================= demodulator =======================
struct AeroDemod::Impl {
    int rate;
    bool msk;
    double fsIn, fsD, searchHz;
    int decim = 1;
    Fir dfir;                          // decimation filter (MSK rates)
    int decPhase = 0;
    Fir mf;                            // matched filter at fsD
    // mixer
    double mixHz = 0, mixPhase = 0;
    bool mixMoved = false;
    // acquisition
    int acqN = 0, acqK = 0, acqFill = 0, acqDone = 0;
    std::vector<cf32> acqBuf;
    std::vector<float> acqPow;
    std::unique_ptr<Fft> fft;
    bool acq = false;
    float acqMetric = 0;
    uint64_t bitsSinceAcq = 0;
    // timing
    static constexpr int kMfRing = 32;
    cf32 mfRing[kMfRing];
    int64_t mfIdx = -1;                // index of the newest matched-filter output
    double tNext = 0, step = 0, tRate = 0;
    // carrier loop
    double theta = 0, omega = 0;
    double pendOmega = 0; int pendBits = 0; int pipeBits = 0;
    // decisions
    cf32 r1 = 0, r2 = 0;               // the two previous bit samples
    float prevV = 0;
    uint64_t k = 0;                    // bits since the loops started
    double amp = 0, mu = 0, m2 = 0;
    AeroFramer framer;
    std::function<void(AeroFrameEvent&)> userCb;

    Impl(int r, double fs, double sh) : rate(r), msk(r != 10500), fsIn(fs), searchHz(sh), framer(r) {
        if (msk) {
            decim = std::max(1, (int)std::floor(fsIn / (8.0 * rate)));
            fsD = fsIn / decim;
            if (decim > 1) dfir.setTaps(lowpass(34 * decim + 1, 0.42 / decim));
            const double sps = fsD / rate;                    // samples per bit; the half-sine lasts two bits
            const int L = 2 * (int)std::floor(sps) + 1;
            std::vector<float> h(L);
            for (int n = 0; n < L; n++) {
                const double t = (n - (L - 1) / 2.0) / sps;   // in bits
                h[n] = std::fabs(t) < 1 ? (float)std::cos(kPi * t / 2) : 0.f;
            }
            mf.setTaps(h);
        } else {
            fsD = fsIn;
            const double sps = 2.0 * fsD / rate;              // samples per arm symbol
            const int L = 2 * (int)std::floor(4 * sps) + 1;
            std::vector<float> h(L);
            for (int n = 0; n < L; n++) h[n] = (float)aeroRrc((n - (L - 1) / 2.0) / sps, 1.0);
            mf.setTaps(h);
        }
        step = fsD / rate;
        // samples in flight between the mixer and the decision, in bits (for moving the loop frequency into the mixer)
        pipeBits = (int)std::lround(((dfir.len() ? (dfir.len() - 1) / 2.0 : 0) / fsIn + (mf.len() - 1) / 2.0 / fsD) * rate);
        acqN = 1;
        const double binWanted = msk ? 2.5 : 6.0;          // Hz
        while (acqN < 1 << 16 && fsD / acqN > binWanted) acqN <<= 1;
        acqK = 4;
        acqBuf.resize(acqN);
        acqPow.assign(acqN, 0.f);
        fft = std::make_unique<Fft>(acqN);
        framer.setCallback([this](AeroFrameEvent& e) {
            e.ebn0Db = ebn0();
            e.freqHz = freq();
            if (userCb) userCb(e);
        });
    }

    double freq() const { return mixHz + omega * rate / (2 * kPi); }
    float ebn0() const {
        const double var = m2 - mu * mu;
        if (mu <= 0 || var <= 1e-20) return 0;
        return (float)(10 * std::log10(mu * mu / (2 * var)));
    }

    void startAcq() {
        acq = false;
        mixHz = 0; mixMoved = true;         // search around the centre the channel was given
        acqFill = 0; acqDone = 0;
        std::fill(acqPow.begin(), acqPow.end(), 0.f);
    }

    void resetAll() {
        mixHz = 0; mixPhase = 0;
        dfir.clear(); mf.clear(); decPhase = 0;
        startAcq();
        acqMetric = 0;
        framer.reset();
        resetLoops();
    }
    void resetLoops() {
        mfIdx = -1; tNext = 0; tRate = 0;
        theta = 0; omega = 0; pendOmega = 0; pendBits = 0;
        r1 = r2 = 0; prevV = 0; k = 0;
        amp = 0; mu = 0; m2 = 0;
        bitsSinceAcq = 0;
        for (auto& v : mfRing) v = 0;
    }

    // the squared signal: lines at 2 df +- Rb/2
    void acqEvaluate() {
        const int N = acqN;
        const double bin = fsD / N;
        std::vector<float> s(acqPow);
        std::nth_element(s.begin(), s.begin() + N / 2, s.end());
        const float med = std::max(s[N / 2], 1e-30f);
        const double L = rate / 2.0 / bin;
        const int cmax = std::min((int)(2 * searchHz / bin), (int)(N / 2 - L - 2));
        auto P = [&](int i) { return acqPow[(size_t)((i % N + N) % N)]; };
        auto line = [&](double x) { return std::max(P((int)std::floor(x)), P((int)std::ceil(x))); };
        double best = 0; int bc = 0;
        for (int c = -cmax; c <= cmax; c++) {
            const double m = line(c + L) + line(c - L);
            if (m > best) { best = m; bc = c; }
        }
        acqMetric = (float)(best / (2 * med));
        if (acqMetric < 12) { startAcq(); acqMetric = (float)(best / (2 * med)); return; }
        // each line to a fraction of a bin
        auto refine = [&](double x) {
            int i0 = (int)std::floor(x);
            if (P(i0 + 1) > P(i0)) i0++;
            const double a = P(i0 - 1), b = P(i0), c = P(i0 + 1);
            const double d = a - 2 * b + c;
            const double off = std::fabs(d) > 1e-30 ? 0.5 * (a - c) / d : 0;
            return (i0 + std::max(-0.5, std::min(0.5, off))) * bin;
        };
        const double fp = refine(bc + L), fm = refine(bc - L);
        const double df = (fp + fm) / 4;                   // the lines sit at 2 df
        mixHz += df;
        mixMoved = true;
        dfir.clear(); mf.clear(); decPhase = 0;
        resetLoops();
        acq = true;
    }

    void feed(const cf32* x, size_t n) {
        double inc = -2 * kPi * mixHz / fsIn;
        for (size_t i = 0; i < n; i++) {
            const cf32 osc((float)std::cos(mixPhase), (float)std::sin(mixPhase));
            mixPhase += inc;
            if (mixPhase > kPi) mixPhase -= 2 * kPi; else if (mixPhase < -kPi) mixPhase += 2 * kPi;
            const cf32 y = x[i] * osc;
            if (decim > 1) {
                dfir.push(y);
                if (++decPhase < decim) continue;
                decPhase = 0;
                sampleD(dfir.out());
            } else {
                sampleD(y);
            }
            if (mixMoved) { mixMoved = false; inc = -2 * kPi * mixHz / fsIn; }
        }
    }

    void sampleD(cf32 z) {
        if (!acq) {
            acqBuf[acqFill++] = z * z;
            if (acqFill == acqN) {
                fft->forward(acqBuf.data());
                for (int i = 0; i < acqN; i++) acqPow[i] += std::norm(acqBuf[i]);
                acqFill = 0;
                if (++acqDone == acqK) acqEvaluate();
            }
            return;
        }
        mf.push(z);
        const cf32 m = mf.out();
        mfIdx++;
        mfRing[mfIdx & (kMfRing - 1)] = m;
        if (mfIdx < (int64_t)mf.len()) { tNext = (double)mfIdx + 1; return; }    // filter still filling
        while (tNext + 2 <= (double)mfIdx) {
            const int64_t i0 = (int64_t)std::floor(tNext);
            const double u = tNext - (double)i0;
            const cf32 p0 = mfRing[(i0 - 1) & (kMfRing - 1)], p1 = mfRing[i0 & (kMfRing - 1)];
            const cf32 p2 = mfRing[(i0 + 1) & (kMfRing - 1)], p3 = mfRing[(i0 + 2) & (kMfRing - 1)];
            const float uf = (float)u;
            const cf32 c1 = -p0 / 3.f - p1 / 2.f + p2 - p3 / 6.f;
            const cf32 c2 = p0 / 2.f - p1 + p2 / 2.f;
            const cf32 c3 = -p0 / 6.f + p1 / 2.f - p2 / 2.f + p3 / 6.f;
            const cf32 y = ((c3 * uf + c2) * uf + c1) * uf + p1;
            const double adv = bit(y);
            tNext += adv;
            if (tNext < (double)mfIdx - (kMfRing - 8)) tNext = (double)mfIdx - (kMfRing - 8);
        }
    }

    // one bit sample: loops, soft decision; returns the advance to the next bit in samples
    double bit(cf32 y) {
        const cf32 rot((float)std::cos(theta), (float)-std::sin(theta));
        const cf32 r = y * rot;
        const bool q = (k & 1) == 0;                         // even bits on the Q arm, odd on I
        const float a0 = q ? r.imag() : r.real();
        const float v = a0;
        const float av = std::fabs(v);
        if (k == 0) amp = av > 0 ? av : 1e-6;
        amp += 0.01 * (av - amp);
        const double ampF = std::max(amp, 1e-12);
        // carrier: the other component at this arm's peak, signed by this arm's decision
        double eps = q ? -r.real() * (r.imag() >= 0 ? 1 : -1) : r.imag() * (r.real() >= 0 ? 1 : -1);
        eps /= ampF;
        eps = std::max(-1.0, std::min(1.0, eps));
        const double bn = bitsSinceAcq < 400 ? 0.03 : 0.012;
        const double zeta = 0.707, th = bn / (zeta + 0.25 / zeta);
        const double kp = 4 * zeta * th / (1 + 2 * zeta * th + th * th), ki = 4 * th * th / (1 + 2 * zeta * th + th * th);
        theta += omega + kp * eps;
        omega += ki * eps;
        if (theta > kPi) theta -= 2 * kPi; else if (theta < -kPi) theta += 2 * kPi;
        // timing (Gardner on this arm: the previous sample is the arm's midpoint)
        double adv = step;
        if (k >= 2) {
            const float m = q ? r1.imag() : r1.real();
            const float pv = q ? r2.imag() : r2.real();
            double e = m * (pv - a0) / (ampF * ampF);
            e = std::max(-1.0, std::min(1.0, e));
            const double g1 = bitsSinceAcq < 400 ? 0.04 : 0.015, g2 = g1 * g1 / 8;
            tRate += g2 * e;
            tRate = std::max(-0.002, std::min(0.002, tRate));
            adv = step * (1 + tRate) + g1 * e * step / 4;
        }
        r2 = r1; r1 = r;
        // quality
        const double qa = std::max(0.002, 1.0 / (double)(k + 1));
        mu += qa * (av - mu);
        m2 += qa * ((double)av * av - m2);
        // soft bit
        float soft;
        if (msk) {
            // physical MSK: the phase step is +-90 degrees, so the arm products carry (-1)^k: even bits are "signs differ",
            // odd bits "signs equal" (JAERO: DiffDecode of imag as is, of real negated)
            soft = (k == 0) ? 0.f : ((prevV * v < 0) == q ? 1.f : -1.f) * std::min(av, std::fabs(prevV));
            prevV = v;
        } else {
            soft = v;
        }
        k++;
        bitsSinceAcq++;
        framer.push(soft / (float)ampF);
        // move a large loop frequency into the mixer; the loop gives it up when the moved samples arrive
        if (pendBits > 0) {
            if (--pendBits == 0) { omega -= pendOmega; pendOmega = 0; }
        } else if (std::fabs(omega) > 2 * kPi * 0.004) {
            pendOmega = omega;
            pendBits = std::max(1, pipeBits);
            mixHz += omega * rate / (2 * kPi);
            mixMoved = true;
        }
        // no frame after a few frame lengths: search the carrier again
        const AeroFrameFormat* f = aeroFrameFormat(rate);
        if (!framer.synced() && bitsSinceAcq > (uint64_t)(4 * f->totalBits())) startAcq();
        return adv;
    }
};

AeroDemod::AeroDemod(int bitRate, double fsIn, double searchHz) : p_(std::make_unique<Impl>(bitRate, fsIn, searchHz)) {}
AeroDemod::~AeroDemod() = default;
void AeroDemod::reset() { p_->resetAll(); }
void AeroDemod::feed(const cf32* x, size_t n) { p_->feed(x, n); }
void AeroDemod::setCallback(std::function<void(AeroFrameEvent&)> cb) { p_->userCb = std::move(cb); }
int AeroDemod::bitRate() const { return p_->rate; }
bool AeroDemod::acquired() const { return p_->acq; }
bool AeroDemod::synced() const { return p_->acq && p_->framer.synced(); }
bool AeroDemod::dataLock() const { return p_->acq && p_->framer.dataLock(); }
double AeroDemod::freqHz() const { return p_->freq(); }
float AeroDemod::ebn0Db() const { return p_->acq ? p_->ebn0() : 0.f; }
float AeroDemod::acqMetric() const { return p_->acqMetric; }
const AeroFramer& AeroDemod::framer() const { return p_->framer; }

// ======================= modulator =======================
struct AeroModulator::Impl {
    int rate;
    bool msk;
    double fs, du;                     // bits per output sample
    double u = 0;                      // current time in bits
    std::vector<int8_t> sym;           // arm values (+-1) of queued bits, from absolute bit `base`
    uint64_t base = 0;
    int8_t lastA = 0;                  // MSK: differential state
    std::vector<float> rrcTab;         // 10500: pulse in steps of 1/64 bit over +-8 bits
    Impl(int r, double f, double ppm) : rate(r), msk(r != 10500), fs(f) {
        du = rate * (1 + ppm * 1e-6) / fs;
        if (!msk) {
            rrcTab.resize(16 * 64 + 2);
            for (size_t i = 0; i < rrcTab.size(); i++) rrcTab[i] = (float)(aeroRrc((i / 64.0 - 8) / 2.0, 1.0) / std::sqrt(2.0));
        }
    }
    int8_t s(int64_t i) const {
        if (i < (int64_t)base || i >= (int64_t)(base + sym.size())) return 0;
        return sym[(size_t)(i - (int64_t)base)];
    }
    float rrc(double t) const {        // t in bits, |t| < 8
        const double x = (t + 8) * 64;
        const int i = (int)x;
        if (i < 0 || i + 1 >= (int)rrcTab.size()) return 0;
        const float f = (float)(x - i);
        return rrcTab[i] + f * (rrcTab[i + 1] - rrcTab[i]);
    }
    void push(const uint8_t* b, size_t n) {
        for (size_t i = 0; i < n; i++) {
            const uint64_t idx = base + sym.size();
            if (msk) {
                // bit 1: the arm value changes sign on even (Q) positions and keeps it on odd (I) positions, as a +-90 degree phase step does
                const bool flip = ((b[i] & 1) != 0) == ((idx & 1) == 0);
                if (!lastA) lastA = 1;
                if (flip) lastA = (int8_t)-lastA;
                sym.push_back(lastA);
            } else {
                sym.push_back((b[i] & 1) ? 1 : -1);
            }
        }
    }
    void gen(cf32* out, size_t n) {
        for (size_t j = 0; j < n; j++) {
            float I = 0, Q = 0;
            if (msk) {
                const int64_t ke = 2 * (int64_t)std::llround(u / 2);       // the Q symbol in force
                const double te = u - (double)ke;
                if (std::fabs(te) < 1) Q = s(ke) * (float)std::cos(kPi * te / 2);
                const int64_t ko = 2 * (int64_t)std::floor(u / 2) + 1;
                const double to = u - (double)ko;
                I = s(ko) * (float)std::cos(kPi * to / 2);
            } else {
                const int64_t lo = (int64_t)std::ceil(u - 8), hi = (int64_t)std::floor(u + 8);
                for (int64_t i = lo; i <= hi; i++) {
                    const int8_t v = s(i);
                    if (!v) continue;
                    const float h = v * rrc(u - (double)i);
                    if (i & 1) I += h; else Q += h;
                }
            }
            out[j] = cf32(I, Q);
            u += du;
        }
        // forget bits that no pulse reaches any more
        const int64_t keepFrom = (int64_t)std::floor(u) - 10;
        if (keepFrom > (int64_t)base + 4096) {
            const size_t drop = (size_t)(keepFrom - (int64_t)base);
            sym.erase(sym.begin(), sym.begin() + (long)std::min(drop, sym.size()));
            base += drop;
        }
    }
};

AeroModulator::AeroModulator(int bitRate, double fs, double clockPpm) : p_(std::make_unique<Impl>(bitRate, fs, clockPpm)) {}
AeroModulator::~AeroModulator() = default;
void AeroModulator::pushBits(const uint8_t* bits, size_t n) { p_->push(bits, n); }
size_t AeroModulator::queuedBits() const {
    const double end = (double)(p_->base + p_->sym.size());
    return end > p_->u ? (size_t)(end - p_->u) : 0;
}
void AeroModulator::generate(cf32* out, size_t n) { p_->gen(out, n); }

} // namespace dect2
