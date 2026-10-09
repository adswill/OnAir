// FM broadcast radio receiver: channel filter, discriminator, stereo decoder (19 kHz pilot PLL) and RDS (57 kHz BPSK).
//
//   input -> channel filter (decimate to ~500 kHz) -> discriminator -> multiplex (MPX) at ~250 kHz
//   MPX -> pilot PLL -> L+R (low-pass), L-R (x sin 2*pilot, low-pass) -> matrix -> de-emphasis -> 48 kHz audio
//   MPX -> x exp(-j 3*pilot) -> low-pass -> 19 kHz -> carrier phase from the squared signal -> matched filter -> bits -> blocks -> groups
#include "dect2/fm_rx.h"
#include "dect2/audioout.h"
#include "dect2/dsp_compat.h"
#include "dect2/fftutil.h"
#include "dect2/resampler.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <mutex>
#include <vector>

namespace dect2 {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr int kAudioRate = 48000;
constexpr double kDevHz = 75000.0;      // 100 % modulation
constexpr double kChanPass = 110e3;     // the channel filter passes +-110 kHz and is 60 dB down by +-190 kHz
constexpr double kChanStop = 190e3;
constexpr int kMpxFft = 2048;

// ---- filters

// low-pass FIR with a Kaiser window, unity gain at DC
std::vector<float> designLowpass(double fpass, double fstop, double fs, double attenDb = 62) {
    fstop = std::min(fstop, fs * 0.5 * 0.999);
    const double fc = 0.5 * (fpass + fstop) / fs;
    const double df = std::max((fstop - fpass) / fs, 1e-4);
    int n = (int)std::ceil((attenDb - 7.95) / (14.36 * df)) + 1;
    n = std::min(2001, std::max(9, n)) | 1;
    const double beta = attenDb > 50 ? 0.1102 * (attenDb - 8.7) : 0.5842 * std::pow(attenDb - 21, 0.4) + 0.07886 * (attenDb - 21);
    auto i0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 50; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
    std::vector<float> h(n);
    const int m = n / 2;
    double sum = 0;
    for (int i = 0; i < n; i++) {
        const double x = i - m;
        const double sinc = x == 0 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x);
        const double r = x / (m + 0.5);
        const double w = i0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0(beta);
        h[i] = (float)(sinc * w);
        sum += h[i];
    }
    for (auto& v : h) v = (float)(v / sum);
    return h;
}

// FIR filter that keeps one output in D, on real data: the decimating correlation of the dsp_compat kernels over the history and the new samples
class FloatFir {
public:
    void design(const std::vector<float>& taps, int d) {
        nt_ = taps.size(); rev_.assign(taps.rbegin(), taps.rend()); d_ = std::max(1, d); reset();
    }
    void reset() { hist_.assign(nt_ - 1, 0.f); cnt_ = 0; }
    // appends the outputs to out
    void process(const float* in, size_t n, std::vector<float>& out) {
        const size_t base = nt_ - 1;
        x_.resize(base + n);
        std::memcpy(x_.data(), hist_.data(), base * sizeof(float));
        std::memcpy(x_.data() + base, in, n * sizeof(float));
        const size_t j0 = (size_t)(d_ - cnt_ - 1);
        if (j0 < n) {
            const size_t nout = (n - j0 + (size_t)d_ - 1) / (size_t)d_;
            const size_t o = out.size();
            out.resize(o + nout);
            desamp(x_.data() + j0, d_, rev_.data(), out.data() + o, (int)nout, (int)nt_);
        }
        cnt_ = (int)(((size_t)cnt_ + n) % (size_t)d_);
        std::memcpy(hist_.data(), x_.data() + n, base * sizeof(float));
    }
    int decim() const { return d_; }
private:
    std::vector<float> rev_, hist_, x_;
    size_t nt_ = 1;
    int d_ = 1, cnt_ = 0;
};

// the same on complex data: the real and the imaginary parts are filtered as two real streams
template <class T> class DecimFir;
template <> class DecimFir<float> {
public:
    void design(std::vector<float> taps, int d) { f_.design(taps, d); }
    void reset() { f_.reset(); }
    void process(const float* in, size_t n, std::vector<float>& out) { f_.process(in, n, out); }
    int decim() const { return f_.decim(); }
private:
    FloatFir f_;
};
template <> class DecimFir<cf32> {
public:
    void design(std::vector<float> taps, int d) { fr_.design(taps, d); fi_.design(taps, d); }
    void reset() { fr_.reset(); fi_.reset(); }
    void process(const cf32* in, size_t n, std::vector<cf32>& out) {
        inR_.resize(n); inI_.resize(n);
        for (size_t i = 0; i < n; i++) { inR_[i] = in[i].real(); inI_[i] = in[i].imag(); }
        oR_.clear(); oI_.clear();
        fr_.process(inR_.data(), n, oR_); fi_.process(inI_.data(), n, oI_);
        const size_t o = out.size(), m = oR_.size();
        out.resize(o + m);
        for (size_t i = 0; i < m; i++) out[o + i] = cf32(oR_[i], oI_[i]);
    }
    int decim() const { return fr_.decim(); }
private:
    FloatFir fr_, fi_;
    std::vector<float> inR_, inI_, oR_, oI_;
};

struct Biquad {
    double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void bandpass(double f0, double q, double fs) {
        const double w = 2 * kPi * f0 / fs, al = std::sin(w) / (2 * q), a0 = 1 + al;
        b0 = al / a0; b1 = 0; b2 = -al / a0; a1 = -2 * std::cos(w) / a0; a2 = (1 - al) / a0;
    }
    float process(float x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return (float)y;
    }
    void reset() { z1 = z2 = 0; }
};

// one-pole de-emphasis (bilinear transform of 1 / (1 + s*tau))
struct DeEmphasis {
    double b0 = 1, a1 = 0, x1 = 0, y1 = 0;
    void configure(double tauUs, double fs) {
        const double k = 2 * fs * tauUs * 1e-6;
        b0 = 1 / (1 + k); a1 = (1 - k) / (1 + k);
    }
    float process(float x) {
        const double y = b0 * (x + x1) - a1 * y1;
        x1 = x; y1 = y;
        return (float)y;
    }
    void reset() { x1 = y1 = 0; }
};

// ---- RDS

const char* kPtyNames[32] = {"None", "News", "Current affairs", "Information", "Sport", "Education", "Drama", "Culture", "Science", "Varied",
                             "Pop music", "Rock music", "Easy listening", "Light classical", "Serious classical", "Other music", "Weather",
                             "Finance", "Children's", "Social affairs", "Religion", "Phone-in", "Travel", "Leisure", "Jazz music",
                             "Country music", "National music", "Oldies music", "Folk music", "Documentary", "Alarm test", "Alarm"};

uint32_t rdsRem(uint32_t v, int nbits) {
    for (int i = nbits - 1; i >= 10; i--) if ((v >> i) & 1) v ^= 0x5B9u << (i - 10);
    return v;
}
constexpr uint32_t kOffsets[5] = {0x0FC, 0x198, 0x168, 0x350, 0x1B4};   // A, B, C, C', D
constexpr uint32_t offsetOfPos(int pos) { return kOffsets[pos == 3 ? 4 : pos]; }   // block positions 0..3 = A, B, C (or C'), D

char rdsChar(uint8_t c) { return c >= 0x20 && c < 0x7F ? (char)c : (c == 0x0D ? '\r' : '?'); }

struct RdsDecoder {
    uint32_t reg = 0;
    bool synced = false, pending = false;
    int cnt = 0, candPos = 0, expect = 0, badRun = 0;
    uint16_t info[4] = {};
    bool ok[4] = {};
    float okPct = 0;
    uint64_t groups = 0;
    int pi = 0;
    bool tp = false, ta = false, ms = true;
    int pty = 0;
    char psBuf[9] = "        ";
    uint8_t psSeen = 0;
    std::string ps, rt;
    char rtBuf[65] = {};
    int rtAb = -1;
    int lastSyncBits = 0;

    void reset() { *this = RdsDecoder(); }
    static int posOf(int offIdx) { return offIdx == 3 ? 2 : offIdx == 4 ? 3 : offIdx; }
    static int match(uint32_t syn) { for (int i = 0; i < 5; i++) if (syn == kOffsets[i]) return i; return -1; }

    void pushBit(int bit) {
        reg = ((reg << 1) | (uint32_t)bit) & 0x3FFFFFF;
        if (synced) {
            if (++cnt < 26) return;
            cnt = 0;
            const uint32_t syn = rdsRem(reg, 26);
            bool good = syn == offsetOfPos(expect) || (expect == 2 && syn == kOffsets[3]);
            okPct += ((good ? 100.f : 0.f) - okPct) * 0.05f;
            badRun = good ? 0 : badRun + 1;
            block(expect, good, (uint16_t)(reg >> 10));
            expect = (expect + 1) & 3;
            if (badRun >= 24) { synced = false; pending = false; okPct = 0; }
            return;
        }
        if (pending) {
            if (++cnt == 26) {
                const int next = (candPos + 1) & 3;
                const int m = match(rdsRem(reg, 26));
                if (m >= 0 && posOf(m) == next) {
                    synced = true; pending = false; cnt = 0; badRun = 0; okPct = 50;
                    block(next, true, (uint16_t)(reg >> 10));
                    expect = (next + 1) & 3;
                    return;
                }
                pending = false;
            } else return;
        }
        const int m = match(rdsRem(reg, 26));
        if (m >= 0) { pending = true; cnt = 0; candPos = posOf(m); }
    }

    void block(int pos, bool good, uint16_t word) {
        if (pos == 0) std::memset(ok, 0, sizeof ok);
        ok[pos] = good; info[pos] = word;
        if (good && pos == 0) pi = word;
        if (pos != 3) return;
        if (!ok[1] || !ok[3]) return;
        groups++;
        const int type = info[1] >> 12, ver = (info[1] >> 11) & 1;
        tp = (info[1] >> 10) & 1;
        pty = (info[1] >> 5) & 31;
        const int low = info[1] & 31;
        if (ver == 1 && ok[2]) pi = info[2];
        if (type == 0) {
            ta = (low >> 4) & 1; ms = (low >> 3) & 1;
            const int seg = low & 3;
            // the four segments must arrive in order, or two versions of a changing name would be mixed
            if (seg == 0) psSeen = 1;
            else if (psSeen == (1 << seg) - 1) psSeen |= (uint8_t)(1 << seg);
            else psSeen = 0;
            if (psSeen) {
                psBuf[seg * 2] = rdsChar(info[3] >> 8); psBuf[seg * 2 + 1] = rdsChar(info[3] & 0xFF);
                if (psSeen == 15) { ps.assign(psBuf, 8); psSeen = 0; }
            }
        } else if (type == 2) {
            const int ab = (low >> 4) & 1, seg = low & 15;
            if (ab != rtAb) { std::memset(rtBuf, ' ', 64); rtAb = ab; rt.clear(); }
            if (ver == 0 && ok[2]) {
                rtBuf[seg * 4] = rdsChar(info[2] >> 8); rtBuf[seg * 4 + 1] = rdsChar(info[2] & 0xFF);
                rtBuf[seg * 4 + 2] = rdsChar(info[3] >> 8); rtBuf[seg * 4 + 3] = rdsChar(info[3] & 0xFF);
            } else if (ver == 1) {
                rtBuf[seg * 2] = rdsChar(info[3] >> 8); rtBuf[seg * 2 + 1] = rdsChar(info[3] & 0xFF);
            }
            std::string s(rtBuf, 64);
            const size_t cr = s.find('\r');
            if (cr != std::string::npos) s.resize(cr);
            while (!s.empty() && s.back() == ' ') s.pop_back();
            rt = s;
        }
    }
};

} // namespace

struct FmReceiver::Impl {
    double inRate = 0, fo = 0, fm = 0, fa = 0;
    bool ready = false;
    std::vector<DecimFir<cf32>> chan;
    DecimFir<float> mpxDecim;
    cf32 prev = cf32(1, 0);
    std::vector<cf32> a, b;                      // scratch between the channel stages
    std::vector<float> freq, mpx;

    // pilot and stereo
    Biquad bp1, bp2, nb1a, nb1b, nb2a, nb2b;   // pilot band, and two bands beside it that hold only noise
    double nPow1 = 0, nPow2 = 0, pilotSnrDb = 0;
    double phi = 0, pllFreq = 0, pLpf = 0, pI = 0, pQ = 0, pAmp2 = 0, dc = 0;
    bool pilotLock = false;
    double lockT = 0, unlockT = 0;
    float stereoBlend = 0;
    DecimFir<float> lpM, lpD;
    std::vector<float> sIn, dIn, mOut, dOut;
    DeEmphasis deL, deR;
    double deemphUs = 50;
    RationalResampler audioRs;
    bool audioRsOk = false;
    std::vector<cf32> lr, lr48;
    std::vector<float> outL, outR, inter;

    // RDS
    DecimFir<cf32> rdsLp;
    RationalResampler rdsRs;
    bool rdsRsOk = false;
    std::vector<cf32> rIn, r1, r19;
    cf32 sq = cf32(0, 0);
    cf32 mfBuf[16] = {};
    int mfPos = 0, sampleIdx = 0, bestPhase = 0;
    float phaseEnergy[16] = {};
    int prevSym = 0;
    float symAvg = 0.01f;
    std::vector<cf32> constHist;
    RdsDecoder rds;

    // measurements
    uint64_t samplesIn = 0, sincePub = 0;
    double pwSum = 0, envSum = 0; uint64_t pwN = 0;
    double freqSum = 0; uint64_t freqN = 0;
    double cfoEma = 0;
    float peakMpx = 0;
    std::vector<float> win, mpxRing;
    size_t ringPos = 0;
    std::vector<float> specPow;
    bool specFresh = true;   // the first spectrum after a reset seeds the average instead of being averaged with zeros
    Fft fft{kMpxFft};

    // output
    std::mutex mu;
    FmTelemetry tel;
    uint64_t telSeq = 0;
    std::unique_ptr<AudioOut> audio;
    bool silent = false, muted = false;
    float volume = 1.f;
    std::function<void(const float*, const float*, size_t)> tap;

    Impl() {
        win.resize(kMpxFft);
        for (int i = 0; i < kMpxFft; i++) win[i] = 0.5f * (1.f - (float)std::cos(2 * kPi * i / kMpxFft));
        mpxRing.assign(kMpxFft, 0.f);
        specPow.assign(kMpxFft / 2, 0.f);
    }

    void configure(double fs) {
        inRate = fs;
        ready = false;
        if (fs < 500e3) return;
        // channel filter: decimate to about 500 kHz in stages of at most 10
        const int dTotal = std::max(1, (int)std::lround(fs / 500e3));
        fo = fs / dTotal; fm = fo / 2; fa = fm / 5;
        std::vector<int> stages;
        int rest = dTotal;
        for (int p = 2; rest > 1;) {
            if (rest % p == 0) { if (!stages.empty() && stages.back() * p <= 10) stages.back() *= p; else stages.push_back(p); rest /= p; }
            else p++;
        }
        chan.assign(stages.size(), DecimFir<cf32>());
        double r = fs;
        for (size_t i = 0; i < stages.size(); i++) {
            const double out = r / stages[i];
            const bool last = i + 1 == stages.size();
            chan[i].design(designLowpass(kChanPass, last ? kChanStop : out - kChanPass, r), stages[i]);
            r = out;
        }
        mpxDecim.design(designLowpass(112e3, 138e3, fo), 2);
        // pilot
        bp1.bandpass(19000, 30, fm); bp2.bandpass(19000, 30, fm);
        nb1a.bandpass(17000, 30, fm); nb1b.bandpass(17000, 30, fm); nb2a.bandpass(21000, 30, fm); nb2b.bandpass(21000, 30, fm);
        // audio: 15 kHz low-pass, 5:1, then to 48 kHz
        const auto lp = designLowpass(15000, 19000, fm, 60);
        lpM.design(lp, 5); lpD.design(lp, 5);
        audioRsOk = audioRs.configure(fa, kAudioRate);
        deL.configure(deemphUs, fa); deR.configure(deemphUs, fa);
        // RDS
        const int dr = std::max(1, (int)std::lround(fm / 25000.0));
        rdsLp.design(designLowpass(3200, 10000, fm), dr);
        rdsRsOk = rdsRs.configure(fm / dr, 19000);
        ready = true;
        reset();
        if (!silent && !audio) { audio = std::make_unique<AudioOut>(); audio->start(kAudioRate); audio->setStartThreshold(kAudioRate / 5); applyAudio(); }
    }

    void applyAudio() { if (audio) { audio->setVolume(volume); audio->setMuted(muted); } }

    void reset() {
        for (auto& c : chan) c.reset();
        mpxDecim.reset(); lpM.reset(); lpD.reset(); rdsLp.reset();
        if (audioRsOk) audioRs.reset();
        if (rdsRsOk) rdsRs.reset();
        prev = cf32(1, 0);
        bp1.reset(); bp2.reset(); nb1a.reset(); nb1b.reset(); nb2a.reset(); nb2b.reset(); nPow1 = nPow2 = pilotSnrDb = 0;
        phi = pllFreq = pLpf = pI = pQ = pAmp2 = dc = 0;
        pilotLock = false; lockT = unlockT = 0; stereoBlend = 0;
        deL.reset(); deR.reset();
        sq = cf32(0, 0); std::memset(mfBuf, 0, sizeof mfBuf); mfPos = sampleIdx = bestPhase = 0;
        std::memset(phaseEnergy, 0, sizeof phaseEnergy);
        prevSym = 0; symAvg = 0.01f; constHist.clear();
        rds.reset();
        pwSum = envSum = freqSum = 0; pwN = freqN = 0; cfoEma = 0; peakMpx = 0; sincePub = 0;
        std::fill(mpxRing.begin(), mpxRing.end(), 0.f);
        std::fill(specPow.begin(), specPow.end(), 0.f); specFresh = true;
        if (audio) audio->flush();
        std::lock_guard<std::mutex> lk(mu);
        tel = FmTelemetry();
        tel.seq = ++telSeq;
    }

    void feed(const cf32* x, size_t n) {
        if (!ready || n == 0) return;
        // channel filter
        const cf32* cur = x; size_t cn = n;
        for (size_t s = 0; s < chan.size(); s++) {
            std::vector<cf32>& dst = (s & 1) ? b : a;
            dst.clear();
            chan[s].process(cur, cn, dst);
            cur = dst.data(); cn = dst.size();
        }
        samplesIn += n; sincePub += n;
        // discriminator: the frequency in units of 75 kHz
        freq.resize(cn);
        for (size_t i = 0; i < cn; i++) {
            // a NaN or infinite sample (a broken file) would stay for good in the pilot loop, the DC average and the measurements: count it as silence
            const cf32 c = std::isfinite(cur[i].real()) && std::isfinite(cur[i].imag()) ? cur[i] : cf32(0, 0);
            const cf32 p = std::conj(prev) * c;
            freq[i] = (float)(std::atan2(p.imag(), p.real()) * fo / (2 * kPi) / kDevHz);
            prev = c;
            pwSum += std::norm(c);
            envSum += std::abs(c);
        }
        pwN += cn;
        for (size_t i = 0; i < cn; i++) freqSum += freq[i];
        freqN += cn;
        mpx.clear();
        mpxDecim.process(freq.data(), cn, mpx);
        runMpx();
        if (sincePub >= inRate / 10) publish();
    }

    void runMpx() {
        const size_t n = mpx.size();
        sIn.resize(n); dIn.resize(n); rIn.resize(n);
        const double dcA = 1.0 / (0.3 * fm);
        const double lockA = 1.0 / (0.02 * fm);
        const double ampA = 1.0 / (0.05 * fm);
        const double wn = 2 * kPi * (pilotLock ? 20.0 : 80.0), zeta = 0.707;
        for (size_t i = 0; i < n; i++) {
            dc += (mpx[i] - dc) * dcA;
            const float m = mpx[i] - (float)dc;
            peakMpx = std::max(peakMpx, std::fabs(m));
            const float pb = bp2.process(bp1.process(m));
            const double ph0 = phi, sn = std::sin(ph0), cs = std::cos(ph0);
            pAmp2 += ((double)pb * pb - pAmp2) * ampA;
            const float n1 = nb1b.process(nb1a.process(m)), n2 = nb2b.process(nb2a.process(m));
            nPow1 += ((double)n1 * n1 - nPow1) * ampA;
            nPow2 += ((double)n2 * n2 - nPow2) * ampA;
            const double amp = std::max(std::sqrt(2 * pAmp2), 0.01);
            const double e = std::max(-1.5, std::min(1.5, 2.0 * pb * cs / amp));
            pI += (pb * sn - pI) * lockA;
            pQ += (pb * cs - pQ) * lockA;
            pllFreq += (wn * wn / fm / fm) * e;
            phi += 2 * kPi * 19000.0 / fm + pllFreq + (2 * zeta * wn / fm) * e;
            if (phi > 2 * kPi) phi -= 2 * kPi; else if (phi < 0) phi += 2 * kPi;
            pllFreq = std::max(-0.05, std::min(0.05, pllFreq));
            // stereo and RDS inputs
            sIn[i] = m;
            dIn[i] = (float)(2.0 * m * 2 * sn * cs);
            rIn[i] = cf32((float)(m * std::cos(3 * ph0)), (float)(-m * std::sin(3 * ph0)));
            mpxRing[ringPos] = m; ringPos = (ringPos + 1) % kMpxFft;
        }
        // lock state of the pilot, judged in seconds because the block length varies
        const double blk = n / fm;
        pilotSnrDb = 10 * std::log10((pAmp2 + 1e-12) / (0.5 * (nPow1 + nPow2) + 1e-12));   // the pilot against the noise beside it
        const bool strong = 2 * pI > 0.025 && pilotSnrDb > (pilotLock ? 8 : 12), clean = pI > 0 && std::fabs(pQ) < 0.35 * pI;
        if (strong && clean) { lockT += blk; unlockT = 0; } else { unlockT += blk; lockT = 0; }
        if (!pilotLock && lockT >= 0.08) pilotLock = true;
        if (pilotLock && (unlockT >= 0.2 || 2 * pI < 0.012)) pilotLock = false;

        mOut.clear(); dOut.clear();
        lpM.process(sIn.data(), n, mOut);
        lpD.process(dIn.data(), n, dOut);
        const size_t na = std::min(mOut.size(), dOut.size());
        lr.resize(na);
        const float target = pilotLock ? 1.f : 0.f;
        for (size_t i = 0; i < na; i++) {
            stereoBlend += (target - stereoBlend) * 0.002f;
            const float m = mOut[i], d = dOut[i] * stereoBlend;
            lr[i] = cf32(deL.process((m + d) / 0.9f * 0.8f), deR.process((m - d) / 0.9f * 0.8f));
        }
        if (audioRsOk) {
            lr48.clear();
            audioRs.process(lr.data(), na, lr48);
            outL.resize(lr48.size()); outR.resize(lr48.size()); inter.resize(lr48.size() * 2);
            for (size_t i = 0; i < lr48.size(); i++) {
                outL[i] = lr48[i].real(); outR[i] = lr48[i].imag();
                inter[2 * i] = outL[i]; inter[2 * i + 1] = outR[i];
            }
            if (audio && !lr48.empty()) {
                // the radio's clock and the sound card's differ by a few ppm: keep the queue near 0.2 s by adding or removing one frame per block
                int frames = (int)lr48.size();
                if (audio->playing()) {
                    const int queued = audio->bufferedFrames();
                    if (queued > 14400 && frames > 1) frames--;
                    else if (queued < 4800 && frames > 0) { inter.push_back(inter[inter.size() - 2]); inter.push_back(inter[inter.size() - 2]); frames++; }
                }
                audio->write(inter.data(), frames);
            }
            if (tap && !lr48.empty()) tap(outL.data(), outR.data(), lr48.size());
        }
        // RDS (only with a locked pilot: its carrier is tied to the pilot)
        if (rdsRsOk) {
            r1.clear();
            rdsLp.process(rIn.data(), n, r1);
            r19.clear();
            rdsRs.process(r1.data(), r1.size(), r19);
            if (pilotLock) for (const cf32& z : r19) rdsSample(z);
        }
    }

    void rdsSample(cf32 z) {
        sq += (z * z - sq) * (1.f / 4000.f);
        const float th = 0.5f * std::atan2(sq.imag(), sq.real());
        const cf32 r = z * cf32(std::cos(-th), std::sin(-th));
        mfBuf[mfPos] = r; mfPos = (mfPos + 1) & 15;
        static float tpl[16]; static bool tplInit = false;
        if (!tplInit) { for (int k = 0; k < 16; k++) tpl[k] = (float)std::sin(2 * kPi * (k + 0.5) / 16); tplInit = true; }
        cf32 y(0, 0);
        for (int k = 0; k < 16; k++) y += mfBuf[(mfPos + k) & 15] * tpl[k];
        const int ph = sampleIdx & 15;
        phaseEnergy[ph] += (std::norm(y) - phaseEnergy[ph]) * 0.01f;
        int best = 0;
        for (int k = 1; k < 16; k++) if (phaseEnergy[k] > phaseEnergy[best]) best = k;
        if (phaseEnergy[best] > 1.15f * phaseEnergy[bestPhase]) bestPhase = best;
        sampleIdx++;
        if (ph != bestPhase) return;
        symAvg += (std::fabs(y.real()) - symAvg) * 0.02f;
        const int sym = y.real() >= 0 ? 1 : 0;
        rds.pushBit(sym ^ prevSym);
        prevSym = sym;
        constHist.push_back(y / std::max(symAvg, 1e-6f));
        if (constHist.size() > 300) constHist.erase(constHist.begin(), constHist.begin() + 100);
    }

    void publish() {
        sincePub = 0;
        // multiplex spectrum
        std::vector<cf32> buf(kMpxFft);
        for (int i = 0; i < kMpxFft; i++) buf[i] = cf32(mpxRing[(ringPos + i) % kMpxFft] * win[i], 0);
        fft.forward(buf.data());
        for (int k = 0; k < kMpxFft / 2; k++) specPow[k] = specFresh ? std::norm(buf[k]) : specPow[k] + (std::norm(buf[k]) - specPow[k]) * 0.3f;
        specFresh = false;
        const double binHz = fm / kMpxFft;
        const double norm = (kMpxFft / 4.0) * (kMpxFft / 4.0);   // a full-deviation sine has |X| = N/4
        double hf = 0;                                            // mean-square power of the multiplex between 85 and 110 kHz (Hann: 16 / (3 N^2) per unit of |X|^2)
        for (int k = (int)(85e3 / binHz); k <= (int)(110e3 / binHz) && k < kMpxFft / 2; k++) hf += specPow[k];
        hf *= 16.0 / (3.0 * kMpxFft * kMpxFft);
        // discriminator noise rises with f^2, so the 0..15 kHz audio band holds 15^3 / (110^3 - 85^3) of what the 85..110 kHz band holds
        const double noiseAudio = std::max(hf * 0.004708, 1e-12);
        const double refAudio = 0.15 * 0.15;
        float snr = (float)std::max(0.0, std::min(60.0, 10 * std::log10(refAudio / noiseAudio)));

        FmTelemetry t;
        t.snrDb = snr;
        t.cfoHz = 0;
        if (freqN) { cfoEma += (freqSum / freqN * kDevHz - cfoEma) * 0.3; t.cfoHz = cfoEma; }
        t.devKhz = (float)(peakMpx * 75.0);
        t.levelDbfs = pwN ? (float)(10 * std::log10(std::max(pwSum / pwN, 1e-12))) : -120.f;
        t.stereo = pilotLock && stereoBlend > 0.5f;
        t.pilotPct = pilotLock ? (float)(200 * pI) : 0.f;
        t.rdsSync = rds.synced;
        t.rdsBlockOkPct = rds.okPct;
        t.rdsGroups = rds.groups;
        t.psName = rds.ps; t.radioText = rds.rt; t.ptyText = rds.groups ? kPtyNames[rds.pty & 31] : "";
        t.trafficAlert = rds.ta; t.trafficProgram = rds.tp; t.music = rds.ms; t.piCode = rds.groups ? rds.pi : 0;
        // an FM carrier has a constant envelope (spread about 0.2 at 12 dB), noise alone a Rayleigh envelope (0.52)
        double cv = 1;
        if (pwN && envSum > 0) cv = std::sqrt(std::max(0.0, pwSum / pwN / ((envSum / pwN) * (envSum / pwN)) - 1));
        t.carrier = cv < 0.4;
        if (!t.carrier) { t.snrDb = 0; snr = 0; }
        t.state = !t.carrier ? 0 : snr >= 20 ? 2 : snr >= 10 ? 1 : 0;
        if (!t.carrier) { t.stereo = false; t.pilotPct = 0; }
        t.mpxMaxHz = 80000.f;
        const int nb = (int)(80e3 / binHz);
        t.mpxDb.resize(nb);
        for (int k = 0; k < nb; k++) t.mpxDb[k] = (float)(10 * std::log10(std::max(specPow[k] / norm, 1e-12)));
        t.rdsConst.assign(constHist.end() - (ptrdiff_t)std::min<size_t>(constHist.size(), 200), constHist.end());
        pwSum = 0; envSum = 0; pwN = 0; freqSum = 0; freqN = 0; peakMpx = 0;
        std::lock_guard<std::mutex> lk(mu);
        t.seq = ++telSeq;
        tel = std::move(t);
    }
};

FmReceiver::FmReceiver() : p_(std::make_unique<Impl>()) {}
FmReceiver::~FmReceiver() {}
void FmReceiver::configure(double inputRateHz) { p_->configure(inputRateHz); }
bool FmReceiver::ready() const { return p_->ready; }
void FmReceiver::reset() { if (p_->ready) p_->reset(); }
void FmReceiver::feed(const cf32* x, size_t n) { p_->feed(x, n); }
bool FmReceiver::telemetry(FmTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->tel.seq == lastSeq) return false;
    out = p_->tel;
    return true;
}
void FmReceiver::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); p_->applyAudio(); }
void FmReceiver::setMuted(bool m) { p_->muted = m; p_->applyAudio(); }
void FmReceiver::setDeemphasis(double us) {
    p_->deemphUs = us;
    if (p_->ready) { p_->deL.configure(us, p_->fa); p_->deR.configure(us, p_->fa); }
}
void FmReceiver::setSilent(bool s) { p_->silent = s; }
void FmReceiver::setAudioTap(std::function<void(const float*, const float*, size_t)> cb) { p_->tap = std::move(cb); }

} // namespace dect2
