// DMR receiver: front end (decimation, DC removal, carrier offset correction, channel filter, discriminator, matched filter) and the glue to the link layer.
#include "dect2/dmr_rx.h"
#include "dect2/dmr_dsp.h"
#include "dect2/exact_resampler.h"
#include "dect2/fftutil.h"
#include "dmr_link.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstring>
#include <mutex>
#include <vector>

namespace dect2 {

using namespace dmr;

namespace {

constexpr double kChanPass = 5400.0, kChanStop = 9500.0;   // channel filter after the carrier offset correction
constexpr int kSpecN = 1024;
// Coarse carrier search while nothing decodes: the power spectrum before the oscillator, 8 frames (0.17 s) summed, a window of +-2.5 kHz slid over
// +-8 kHz. A radio that is 20 ppm off at 446 MHz is 9 kHz away; the sync tracking alone follows about 6 kHz.
constexpr int kAfcFrames = 8;
constexpr double kAfcRange = 8000.0, kAfcHalfWidth = 2500.0, kAfcMinStep = 1500.0, kAfcMinRatio = 4.0;
constexpr int kPublishSamples = 12000;                      // 0.25 s at 48 kHz

// Pick the integer decimation to a rate just above 48 kHz: a factor with small prime factors keeps the filters short, and the exact
// resampler takes care of what is left.
int pickDecimation(double fs) {
    const int hi = std::max(1, (int)std::floor(fs / kWorkRate));
    const int lo = std::max(1, (int)std::floor(fs / 60000.0));
    auto largestPrime = [](int v) {
        int lp = 1;
        for (int p = 2; p <= v; p++) while (v % p == 0) { lp = p; v /= p; }
        return lp;
    };
    int best = hi, bestPrime = largestPrime(hi);
    for (int d = hi; d >= lo; d--) {
        const int lp = largestPrime(d);
        if (lp <= 7) { best = d; bestPrime = lp; break; }
        if (lp < bestPrime) { best = d; bestPrime = lp; }
    }
    return best;
}

std::vector<int> splitFactors(int d) {
    std::vector<int> primes;
    for (int p = 2; d > 1;) {
        if (d % p == 0) { primes.push_back(p); d /= p; }
        else p++;
    }
    std::sort(primes.rbegin(), primes.rend());
    std::vector<int> stages;
    for (int p : primes) {
        if (!stages.empty() && stages.back() * p <= 8) stages.back() *= p;
        else stages.push_back(p);
    }
    return stages;
}

// FIR with real taps over a stream of T, one sample at a time, with a doubled history so that the dot product is contiguous
template <class T>
struct StreamFir {
    std::vector<float> h;
    std::vector<T> buf;
    size_t pos = 0;
    void design(std::vector<float> taps) {
        h = std::move(taps);
        std::reverse(h.begin(), h.end());
        buf.assign(2 * h.size(), T());
        pos = 0;
    }
    void reset() { std::fill(buf.begin(), buf.end(), T()); pos = 0; }
    T push(T x) {
        const size_t n = h.size();
        buf[pos] = x;
        buf[pos + n] = x;
        pos = (pos + 1) % n;
        const T* p = &buf[pos];      // the oldest of the n newest samples
        T acc = T();
        for (size_t k = 0; k < n; k++) acc += p[k] * h[k];
        return acc;
    }
};

} // namespace

struct DmrReceiver::Impl {
    double inRate = 0;
    bool ready = false;

    // decimation to about 48 kHz and the exact conversion
    std::vector<Fir<cf32>> stages;
    ExactResampler rs;
    bool rsPass = true;
    std::vector<cf32> bufA, bufB, xa, clean;

    // 48 kHz chain
    cf32 dc = cf32(0, 0);
    uint64_t dcCount = 0;
    double ncoPhase = 0, ncoHz = 0;
    StreamFir<cf32> chan;
    StreamFir<float> mf;
    cf32 prevB = cf32(1, 0);
    Link link;

    // measurements
    double powAcc = 0;
    uint64_t powN = 0;
    float levelDb = -120;
    std::vector<cf32> specBuf;
    std::vector<float> win;
    std::vector<double> specPow;
    bool specFresh = true;
    Fft fft{kSpecN};
    std::vector<float> afcFrames;           // kAfcFrames x kSpecN
    std::vector<double> afcSum, afcPrefix;
    int afcHead = 0, afcFilled = 0;
    double afcPrev = 1e9;                   // the last estimate: a move needs two agreeing estimates
    int samplesSincePublish = 0;
    double noiseDbfs = -120, cnrDb = 0;
    bool noiseKnown = false;

    // output
    std::mutex mu;
    DmrTelemetry tel;
    uint64_t telSeq = 0;
    // Controls and callbacks are set from the interface thread while feed() runs: atomics, and a mutex for the callbacks. There is no vocoder,
    // so volume, mute and the audio tap have no effect; they are here so that the app can drive this receiver like the others.
    std::atomic<float> volume{1.f};
    std::atomic<bool> muted{false}, silent{false};
    std::mutex cbMu;
    std::function<void(const float*, const float*, size_t)> tap;
    std::function<void(int, int, const uint8_t*)> voiceCb;
    std::function<void(const std::string&)> logCb;

    Impl() {
        win.resize(kSpecN);
        for (int i = 0; i < kSpecN; i++) win[i] = 0.5f * (1.f - (float)std::cos(2 * kPi * i / kSpecN));
        specPow.assign(kSpecN, 0.0);
        afcFrames.assign((size_t)kAfcFrames * kSpecN, 0.f);
        afcSum.assign(kSpecN, 0.0);
        specBuf.reserve(kSpecN);
        chan.design(lowpassTaps(kChanPass, kChanStop, kWorkRate, 55, 31));
        mf.design(rrcTaps(kSps, 6));
        link.onVoiceBurst = [this](int slot, int pos, const Bits& vs) {
            std::function<void(int, int, const uint8_t*)> cb;
            { std::lock_guard<std::mutex> lk(cbMu); cb = voiceCb; }
            if (!cb) return;
            uint8_t b[27];
            bitsToBytes(vs, 0, 216, b);
            cb(slot, pos, b);
        };
        link.setLog([this](const std::string& line) {
            std::function<void(const std::string&)> cb;
            { std::lock_guard<std::mutex> lk(cbMu); cb = logCb; }
            if (cb) cb(line);
        });
        reset();
    }

    void configure(double fs) {
        inRate = fs;
        ready = false;
        stages.clear();
        if (fs < dmrTuning().minSampleRate - 1) return;
        const int d = pickDecimation(fs);
        const std::vector<int> f = splitFactors(d);
        double r = fs;
        stages.assign(f.size(), Fir<cf32>());
        for (size_t i = 0; i < f.size(); i++) {
            const double out = r / f[i];
            const double pass = 22000.0, stop = std::max(pass + 4000.0, out - pass);
            stages[i].design(lowpassTaps(pass, std::min(stop, r * 0.5), r, 60, 9), f[i]);
            r = out;
        }
        const double fi = fs / d;
        rsPass = std::fabs(fi - kWorkRate) < 0.01;
        if (!rsPass && !rs.configure(fi, kWorkRate)) return;
        ready = true;
        reset();
    }

    void reset() {
        for (auto& s : stages) s.reset();
        if (!rsPass) rs.reset();
        dc = cf32(0, 0); dcCount = 0;
        ncoPhase = 0; ncoHz = 0;
        chan.reset(); mf.reset();
        prevB = cf32(1, 0);
        link.reset();
        powAcc = 0; powN = 0; levelDb = -120;
        specBuf.clear();
        std::fill(specPow.begin(), specPow.end(), 0.0);
        std::fill(afcFrames.begin(), afcFrames.end(), 0.f);
        std::fill(afcSum.begin(), afcSum.end(), 0.0);
        afcHead = 0; afcFilled = 0; afcPrev = 1e9;
        specFresh = true;
        samplesSincePublish = 0;
        noiseDbfs = -120; cnrDb = 0; noiseKnown = false;
        std::lock_guard<std::mutex> lk(mu);
        tel = DmrTelemetry();
        tel.seq = ++telSeq;
    }

    void feed(const cf32* x, size_t n) {
        if (!ready || n == 0) return;
        // not-a-number or absurd values (a damaged file, a driver fault) would poison the filters for good: they become zero
        bool bad = false;
        for (size_t i = 0; i < n; i++) {
            const float re = x[i].real(), im = x[i].imag();
            if (!(std::fabs(re) < 1e4f && std::fabs(im) < 1e4f)) { bad = true; break; }
        }
        if (bad) {
            clean.assign(x, x + n);
            for (auto& v : clean) if (!(std::fabs(v.real()) < 1e4f && std::fabs(v.imag()) < 1e4f)) v = cf32(0, 0);
            x = clean.data();
        }
        const cf32* cur = x;
        size_t cn = n;
        for (size_t s = 0; s < stages.size(); s++) {
            std::vector<cf32>& dst = (s & 1) ? bufB : bufA;
            dst.clear();
            stages[s].process(cur, cn, dst);
            cur = dst.data(); cn = dst.size();
        }
        if (!rsPass) {
            xa.clear();
            rs.process(cur, cn, xa);
            cur = xa.data(); cn = xa.size();
        }
        for (size_t i = 0; i < cn; i++) sample48(cur[i]);
    }

    void sample48(cf32 xin) {
        // DC: the radio's spike sits in the middle of the channel. A fast start, then 0.1 s.
        const float a = (float)(1.0 / std::min<double>((double)++dcCount, 4800.0));
        dc += (xin - dc) * a;
        const cf32 x = xin - dc;
        // spectrum frames
        specBuf.push_back(x);
        if ((int)specBuf.size() == kSpecN) { spectrumFrame(); specBuf.clear(); }
        // carrier offset correction
        const cf32 rot((float)std::cos(ncoPhase), (float)-std::sin(ncoPhase));
        ncoPhase += 2 * kPi * ncoHz / kWorkRate;
        if (ncoPhase > kPi) ncoPhase -= 2 * kPi; else if (ncoPhase < -kPi) ncoPhase += 2 * kPi;
        const cf32 yb = chan.push(x * rot);
        powAcc += std::norm(yb); powN++;
        // FM discriminator in Hz
        const cf32 p = yb * std::conj(prevB);
        prevB = yb;
        const float d = (float)(std::atan2(p.imag(), p.real()) * (kWorkRate / (2 * kPi)));
        const float y = mf.push(d);
        link.push(y);
        const double nd = link.takeNcoDelta();
        if (nd != 0) ncoHz += nd;
        if (++samplesSincePublish >= kPublishSamples) publish();
    }

    void spectrumFrame() {
        std::vector<cf32> buf(kSpecN);
        for (int i = 0; i < kSpecN; i++) buf[i] = specBuf[i] * win[i];
        fft.forward(buf.data());
        double ww = 0;
        for (float w : win) ww += (double)w * w;
        const double norm = 1.0 / ((double)kSpecN * ww);
        float* fr = &afcFrames[(size_t)afcHead * kSpecN];
        for (int k = 0; k < kSpecN; k++) {
            const double pw = (double)std::norm(buf[k]) * norm;
            specPow[k] = specFresh ? pw : specPow[k] + (pw - specPow[k]) * 0.1;
            afcSum[k] += pw - (double)fr[k];
            fr[k] = (float)pw;
        }
        specFresh = false;
        afcHead = (afcHead + 1) % kAfcFrames;
        if (afcFilled < kAfcFrames) afcFilled++;
        if (afcFilled == kAfcFrames) coarseCarrier();
    }

    // Where is the carrier? Only while no slot is followed. The estimate is the centre of the strongest +-2.5 kHz window, refined by the centroid of the
    // power above the noise floor (taken from 17 to 21 kHz, which the decimation filters still pass and no channel of the wanted signal reaches).
    void coarseCarrier() {
        if (link.locked()) { afcPrev = 1e9; return; }
        const double binHz = kWorkRate / kSpecN;
        // the summed spectrum in order of frequency, bin j at (j - N/2) * binHz, and its running sum
        std::vector<double>& ps = afcPrefix;
        ps.assign((size_t)kSpecN + 1, 0.0);
        for (int j = 0; j < kSpecN; j++) ps[(size_t)j + 1] = ps[(size_t)j] + afcSum[(size_t)((j + kSpecN / 2) % kSpecN)];
        auto jOf = [&](double f) { return std::max(0, std::min(kSpecN, (int)std::ceil(f / binHz) + kSpecN / 2)); };    // first bin at or above f
        auto sumRange = [&](double lo, double hi) { const int a = jOf(lo), b = jOf(hi + 1e-9); return b > a ? ps[(size_t)b] - ps[(size_t)a] : 0.0; };
        auto binsIn = [&](double lo, double hi) { return std::max(0, jOf(hi + 1e-9) - jOf(lo)); };
        const double n0sum = sumRange(17000.0, 21000.0) + sumRange(-21000.0, -17000.0);
        const int n0bins = binsIn(17000.0, 21000.0) + binsIn(-21000.0, -17000.0);
        if (n0bins == 0 || n0sum <= 0) return;
        const double n0 = n0sum / n0bins;
        double best = 0, bestC = 0;
        int bestBins = 1;
        for (double c = -kAfcRange; c <= kAfcRange; c += binHz) {
            const double sum = sumRange(c - kAfcHalfWidth, c + kAfcHalfWidth);
            if (sum > best) { best = sum; bestC = c; bestBins = std::max(1, binsIn(c - kAfcHalfWidth, c + kAfcHalfWidth)); }
        }
        if (best < kAfcMinRatio * n0 * bestBins) { afcPrev = 1e9; return; }
        double num = 0, den = 0;
        for (int j = jOf(bestC - kAfcHalfWidth - 500.0); j < jOf(bestC + kAfcHalfWidth + 500.0); j++) {
            const double f = (j - kSpecN / 2) * binHz;
            const double e = std::max(afcSum[(size_t)((j + kSpecN / 2) % kSpecN)] - n0, 0.0);
            num += f * e; den += e;
        }
        if (den <= 0) return;
        const double est = num / den;
        const bool agree = std::fabs(est - afcPrev) < 400.0;
        afcPrev = est;
        if (!agree || std::fabs(est - ncoHz) < kAfcMinStep) return;
        link.shiftOffset(est - ncoHz);
        ncoHz = est;
        afcPrev = 1e9;
        afcFilled = 0;      // the spectrum is taken before the oscillator, but let the filters and the sync search settle for a moment
    }

    void publish() {
        samplesSincePublish = 0;
        DmrTelemetry t;
        link.snapshot(t);
        // level in the channel
        if (powN) levelDb = (float)(10 * std::log10(std::max(powAcc / (double)powN, 1e-12)));
        powAcc = 0; powN = 0;
        t.levelDbfs = levelDb;
        // noise floor beside the channel (the decimation filters keep the band up to 22 kHz) and the power in 12.5 kHz
        if (!specFresh) {
            const double binHz = kWorkRate / kSpecN;
            double nsum = 0; int nn = 0;
            for (int k = 0; k < kSpecN; k++) {
                const double f = (k < kSpecN / 2 ? k : k - kSpecN) * binHz;
                if (std::fabs(f) >= 19500.0 && std::fabs(f) <= 22500.0) { nsum += specPow[k]; nn++; }
            }
            double inBand = 0; int ni = 0;
            for (int k = 0; k < kSpecN; k++) {
                const double f = (k < kSpecN / 2 ? k : k - kSpecN) * binHz;
                if (std::fabs(f) <= 6250.0) { inBand += specPow[k]; ni++; }
            }
            if (nn > 0 && nsum > 0) {
                const double nb = nsum / nn;
                const double nin = nb * ni;
                noiseDbfs = 10 * std::log10(std::max(nin, 1e-14));
                const double sig = std::max(inBand - nin, 0.0);
                cnrDb = sig > 0 ? (float)(10 * std::log10(sig / nin)) : 0.f;
                noiseKnown = true;
            }
            t.noiseDbfs = noiseKnown ? (float)noiseDbfs : -120.f;
            t.cnrDb = noiseKnown ? (float)cnrDb : 0.f;
            // 128 bins from -12 to +12 kHz, relative to the strongest
            t.spectrumDb.assign(128, -100.f);
            double mx = 1e-20;
            std::vector<double> sp(128, 0.0);
            for (int b = 0; b < 128; b++) {
                const double f0 = -12000.0 + b * 187.5 * 2;
                double s = 0; int c = 0;
                for (int k = 0; k < kSpecN; k++) {
                    const double f = (k < kSpecN / 2 ? k : k - kSpecN) * binHz;
                    if (f >= f0 && f < f0 + 375.0) { s += specPow[k]; c++; }
                }
                sp[b] = c ? s / c : 0.0;
                mx = std::max(mx, sp[b]);
            }
            for (int b = 0; b < 128; b++) t.spectrumDb[b] = (float)(10 * std::log10(std::max(sp[b] / mx, 1e-10)));
        }
        t.cfoHz = ncoHz + link.cfoResidualHz();
        std::lock_guard<std::mutex> lk(mu);
        t.seq = ++telSeq;
        tel = std::move(t);
    }
};

DmrReceiver::DmrReceiver() : p_(std::make_unique<Impl>()) {}
DmrReceiver::~DmrReceiver() = default;

void DmrReceiver::configure(double inputRateHz) { p_->configure(inputRateHz); }
bool DmrReceiver::ready() const { return p_->ready; }
void DmrReceiver::reset() { if (p_->ready) p_->reset(); }
void DmrReceiver::feed(const cf32* x, size_t n) { p_->feed(x, n); }
bool DmrReceiver::telemetry(DmrTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->tel.seq == lastSeq || p_->tel.seq < lastSeq) return false;
    out = p_->tel;
    return true;
}
void DmrReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->cbMu); p_->logCb = std::move(cb); }
void DmrReceiver::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); }
void DmrReceiver::setMuted(bool m) { p_->muted = m; }
void DmrReceiver::setSilent(bool s) { p_->silent = s; }
void DmrReceiver::setAudioTap(std::function<void(const float*, const float*, size_t)> cb) { std::lock_guard<std::mutex> lk(p_->cbMu); p_->tap = std::move(cb); }
void DmrReceiver::setVoiceCallback(std::function<void(int, int, const uint8_t*)> cb) { std::lock_guard<std::mutex> lk(p_->cbMu); p_->voiceCb = std::move(cb); }

ModeTuning dmrTuning() {
    ModeTuning t;
    t.stdMode = 11; t.id = "dmr"; t.name = "DMR";
    t.minMhz = 30; t.maxMhz = 1000; t.defMhz = 446.10625;       // PMR446 digital channel 1 (12.5 kHz channels from 446.10625 MHz)
    t.sampleRate = 2400000.0; t.basebandHz = 1750000.0; t.bandwidthMhz = 0.0125;
    t.minSampleRate = 1000000.0;
    return t;
}

} // namespace dect2
