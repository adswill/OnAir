// FM broadcast radio receiver: channel filter, discriminator, stereo decoder (19 kHz pilot PLL) and RDS (57 kHz BPSK).
//
//   input -> channel filter (decimate to ~500 kHz) -> discriminator -> multiplex (MPX) at ~250 kHz
//   MPX -> pilot PLL -> L+R (low-pass), L-R (x sin 2*pilot, low-pass) -> matrix -> de-emphasis -> 48 kHz audio
//   MPX -> x exp(-j 3*pilot) -> low-pass -> 19 kHz -> carrier phase from the squared signal -> matched filter -> bits -> blocks -> groups
#include "dect2/fm/fm_rx.h"
#include "dect2/audio/audioout.h"
#include "dect2/dsp/dsp_compat.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/fm/fm_rbds.h"
#include "dect2/fm/fm_rds.h"
#include "dect2/fm/fm_tone.h"
#include "dect2/dsp/resampler.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <mutex>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr int kAudioRate = 48000;
constexpr double kDevHz = 75000.0;      // 100 % modulation
constexpr double kChanPass = 110e3;     // the channel filter passes +-110 kHz and is 60 dB down by +-190 kHz (setChannelWidth() changes the pass band)
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
    bool off = false;   // 0 us: no de-emphasis (a station without pre-emphasis, or the user's choice)
    void configure(double tauUs, double fs) {
        off = tauUs <= 0;
        const double k = 2 * fs * std::max(tauUs, 1.0) * 1e-6;
        b0 = 1 / (1 + k); a1 = (1 - k) / (1 + k);
    }
    float process(float x) {
        if (off) return x;
        const double y = b0 * (x + x1) - a1 * y1;
        x1 = x; y1 = y;
        return (float)y;
    }
    void reset() { x1 = y1 = 0; }
};

} // namespace

struct FmReceiver::Impl {
    double inRate = 0, fo = 0, fm = 0, fa = 0;
    bool ready = false;
    std::vector<DecimFir<cf32>> chan;
    std::vector<int> chanStages;
    double chanPass = kChanPass;                 // the pass band of chan, +- Hz
    std::atomic<double> chanWant{kChanPass};     // set from another thread (setChannelWidth); feed() designs the filter again
    DecimFir<float> mpxDecim;
    cf32 prev = cf32(1, 0), prevRaw = cf32(1, 0);   // the last sample of the equalised and of the raw channel (the discriminator's reference)
    // The radio's DC spike sits on the carrier when the radio is tuned exactly to the station. Its mean over half a second is taken out of the
    // channel: the carrier itself turns by the tuning error (100 Hz per ppm of the radio at 100 MHz) and averages away, the spike does not.
    cf32 dcCh = cf32(0, 0);
    // Constant-modulus equaliser on the channel (multipath: an echo makes the envelope of the FM signal ripple and its phase wrong; the
    // equaliser finds the filter that makes the envelope constant again). kCmaTaps taps at the channel rate (about 2 us each), the main one
    // kCmaMain: echoes up to 24 us late and 6 us early.
    static constexpr int kCmaTaps = 16, kCmaMain = 3;
    cf32 cmaW[kCmaTaps] = {}, cmaX[2 * kCmaTaps] = {}, cmaRawD = cf32(0, 0);
    int cmaPos = 0;
    float cmaP = 0;
    // The equaliser is used only when the channel has multipath. Without an echo it does harm: in noise its constant-modulus optimum is a
    // narrower filter (less noise in the envelope), which cuts the outer FM sidebands: on a quiet passage or a mono station that cost RDS
    // 2 to 5 dB and the stereo separation fell to the SNR. An echo shows as an envelope that depends on the instantaneous frequency
    // (|H(f)|^2 swept by the modulation); noise does not. The mean power of the raw channel is collected in 10 kHz bins of the
    // instantaneous frequency; their spread (over the bins the modulation visits) above 6 % switches the equaliser in, below 3.5 % out.
    static constexpr int kMpBins = 16;
    double mpPow[kMpBins] = {}, mpN[kMpBins] = {};
    double mpSecs = 0;
    bool cmaUse = true;    // on from the start (as before the detector): the first judgement comes after 0.25 s
    float mpRipple = 0;
    float mpHist[8] = {}, mpAvg = 0;
    int mpHp = 0;
    void mpReset() { for (int k = 0; k < kMpBins; k++) mpPow[k] = mpN[k] = 0; mpSecs = 0; }
    void mpJudge() {
        double sum = 0, n = 0;
        int used = 0;
        double m[kMpBins];
        for (int k = 0; k < kMpBins; k++) if (mpN[k] >= 2000) { m[used++] = mpPow[k] / mpN[k]; sum += mpPow[k]; n += mpN[k]; }
        if (used >= 3 && sum > 0) {
            const double mean = sum / n;
            double v = 0;
            for (int i = 0; i < used; i++) v += (m[i] - mean) * (m[i] - mean);
            mpRipple = (float)(std::sqrt(v / used) / mean);
            // a weak signal's own noise leaves a ripple of a few % (the frequency jitter goes with the envelope's dips): the threshold rises
            // by 0.6 % per dB of SNR below 30 dB
            const float extra = 0.006f * std::max(0.f, 30.f - lastSnr);
            cmaUse = cmaUse ? mpRipple > 0.035f + extra : mpRipple > 0.06f + extra;
        } else if (used > 0) {
            mpRipple = 0; cmaUse = false;   // hardly any modulation: an echo cannot distort it
        }
        mpReset();
    }
    void cmaReset() { for (auto& w : cmaW) w = cf32(0, 0); cmaW[kCmaMain] = cf32(1, 0); for (auto& v : cmaX) v = cf32(0, 0); cmaPos = 0; cmaP = 0; }
    cf32 cma(cf32 x) {
        cmaP = cmaP > 0 ? cmaP + (std::norm(x) - cmaP) * 0.002f : std::norm(x);
        if (cmaP <= 1e-20f) { cmaRawD = x; return x; }
        const float g = 1.f / std::sqrt(cmaP);
        // the input history twice over, so that the taps read a straight run of samples
        cmaPos = (cmaPos + kCmaTaps - 1) % kCmaTaps;
        cmaX[cmaPos] = cmaX[cmaPos + kCmaTaps] = x * g;
        const cf32* h = &cmaX[cmaPos];            // h[k]: the sample k steps back
        cmaRawD = h[kCmaMain];                    // the input as far back as the main tap: the raw path in step with the equalised one
        // The complex products are written out (std::complex's operator* checks every product for NaN, which kept these two loops scalar and
        // made them most of the FM receiver's own cost): the same products, sums and order, one per statement (nothing fused), the same bits.
        float yr = 0, yi = 0;
        for (int k = 0; k < kCmaTaps; k++) {
            const float a = cmaW[k].real(), b = cmaW[k].imag(), c = h[k].real(), d = h[k].imag();
            const float ac = a * c, bd = b * d, ad = a * d, bc = b * c;
            const float pr = ac - bd, pi = ad + bc;
            yr += pr; yi += pi;
        }
        const cf32 y(yr, yi);
        const float e = std::norm(y) - 1.f;
        const cf32 ey = y * (2e-4f * e);
        const float er = ey.real(), ei = ey.imag();
        for (int k = 0; k < kCmaTaps; k++) {   // cmaW[k] -= ey * conj(h[k])
            const float c = h[k].real(), d = -h[k].imag();
            const float ac = er * c, bd = ei * d, ad = er * d, bc = ei * c;
            const float pr = ac - bd, pi = ad + bc;
            cmaW[k] = cf32(cmaW[k].real() - pr, cmaW[k].imag() - pi);
        }
        if (!(std::norm(y) < 16.f)) { const float p = cmaP; cmaReset(); cmaP = p; }   // diverged (or NaN): start again
        return y;
    }
    // Station finder: a recording made beside the station (gqrx, SDR#) or a radio tuned off it. Averaged spectra of the input; the station is
    // mixed to the centre. Only when the centre holds no station: one there, however weak beside a strong neighbour, is the one tuned to.
    static constexpr int kFindN = 4096, kFindFrames = 16;
    Fft findFft{kFindN};
    std::vector<float> findPow;
    std::vector<cf32> findBuf, mixBuf;
    int findFrames = 0;
    bool finding = true;
    std::atomic<bool> autoFind{true};            // off for a live radio: the frequency the user tuned is the station, however weak
    double unlockedSecs = 0;
    double tuneHz = 0;                           // the station's frequency in the input; mixed to the centre
    cf32 mixRot = cf32(1, 0), mixStep = cf32(1, 0);
    float dcChA = 0;
    std::vector<cf32> a, b;                      // scratch between the channel stages
    std::vector<float> freq, mpx, freqR, mpxR;   // R: the raw channel for RDS while the sound takes the equaliser (split)
    DecimFir<float> mpxDecimR;
    bool audioCma = true, split = false;

    // pilot and stereo
    Biquad bp1, bp2, nb1a, nb1b, nb2a, nb2b;   // pilot band, and two bands beside it that hold only noise
    double nPow1 = 0, nPow2 = 0, pilotSnrDb = 0;
    double phi = 0, pllFreq = 0, pLpf = 0, pI = 0, pQ = 0, pAmp2 = 0, dc = 0;
    bool pilotLock = false;
    double lockT = 0, unlockT = 0;
    float stereoBlend = 0;
    // Stereo blend by SNR (Auto only): the difference signal carries about 15 dB more noise than the sum, so a weak station goes
    // gradually to mono instead of hissing, as car radios do: full stereo above 36 dB of the SNR measure (about 22 dB carrier to noise),
    // mono below 24 dB (about 10 dB), smoothed over a second. "Stereo" in the stereo choice keeps full stereo whatever the SNR.
    float snrBlend = 1.f;
    DecimFir<float> lpM, lpD;
    std::vector<float> sIn, dIn, mOut, dOut;
    DeEmphasis deL, deR;
    double deemphUs = 50;
    std::atomic<int> rdsRegion{kRdsAuto};       // setRdsRegion(): RDS (Europe) or RBDS (US) names; Auto decides from the tuned frequency
    std::atomic<double> tunedMhz{0};
    std::atomic<bool> forceMono{false};
    // issue #213: stereo choice (0 auto, 1 mono, 2 stereo whenever a pilot is there), audio bandwidth (0 full, else a 4th order low pass in Hz),
    // squelch (dB of the SNR measure, 0 off; 2 dB hysteresis, 10 ms ramps)
    std::atomic<int> stereoMode{0};
    std::atomic<int> audioBwHz{0};
    std::atomic<float> squelchDb{0.f};
    float lastSnr = 60.f, sqGain = 1.f;
    bool sqOpen = true;
    int bwSet = 0;
    struct LpBiquad {   // the channel-width low-pass (its own name: Impl already uses the file-scope band-pass Biquad above)
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void lowpass(double fc, double q, double fs) {
            const double w = 2 * kPi * fc / fs, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
            b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - al) / a0; z1 = z2 = 0;
        }
        float process(float x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return (float)y; }
    };
    LpBiquad bwF[2][2];   // [channel][section]
    std::atomic<int> toneBass{0}, toneTreble{0};   // setTone(): the requested tone; the audio thread reconfigures tone when they change
    std::atomic<bool> toneWarm{false};
    FmTone tone;
    int toneBassSet = 0, toneTrebleSet = 0;
    bool toneWarmSet = false;
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
    bool rdsOwn = true;                          // no pilot: RDS on its own 57 kHz oscillator and Costas loop
    double nco57 = 0, costasPh = 0, costasFr = 0;
    cf32 mfBuf[16] = {};
    int mfPos = 0, sampleIdx = 0, bestPhase = 0, sinceSym = 0;
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
        dcChA = (float)(1.0 / (0.5 * fo));
        restartFinder(); setTune(0);
        chanStages.clear();
        int rest = dTotal;
        for (int p = 2; rest > 1;) {
            if (rest % p == 0) { if (!chanStages.empty() && chanStages.back() * p <= 10) chanStages.back() *= p; else chanStages.push_back(p); rest /= p; }
            else p++;
        }
        designChan(chanWant.load());
        mpxDecim.design(designLowpass(112e3, 138e3, fo), 2);
        mpxDecimR.design(designLowpass(112e3, 138e3, fo), 2);
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

    // the channel filter, passing +-pass Hz; the last stage is down by pass + 80 kHz (190 kHz for the standard 110)
    void designChan(double pass) {
        chanPass = pass;
        chan.assign(chanStages.size(), DecimFir<cf32>());
        double r = inRate;
        for (size_t i = 0; i < chanStages.size(); i++) {
            const double out = r / chanStages[i];
            const bool last = i + 1 == chanStages.size();
            chan[i].design(designLowpass(pass, last ? pass + (kChanStop - kChanPass) : out - pass, r), chanStages[i]);
            r = out;
        }
    }

    void applyAudio() { if (audio) { audio->setVolume(volume); audio->setMuted(muted); } }

    void reset() {
        for (auto& c : chan) c.reset();
        mpxDecim.reset(); mpxDecimR.reset(); audioCma = true; split = false; lpM.reset(); lpD.reset(); rdsLp.reset(); tone.reset();
        if (audioRsOk) audioRs.reset();
        if (rdsRsOk) rdsRs.reset();
        prev = prevRaw = cf32(1, 0); dcCh = cf32(0, 0); cmaReset(); mpReset(); cmaUse = true; mpRipple = 0; std::memset(mpHist, 0, sizeof mpHist); mpAvg = 0; mpHp = 0;
        bp1.reset(); bp2.reset(); nb1a.reset(); nb1b.reset(); nb2a.reset(); nb2b.reset(); nPow1 = nPow2 = pilotSnrDb = 0;
        phi = pllFreq = pLpf = pI = pQ = pAmp2 = dc = 0;
        pilotLock = false; lockT = unlockT = 0; stereoBlend = 0; snrBlend = 1.f;
        deL.reset(); deR.reset();
        sq = cf32(0, 0); rdsOwn = true; nco57 = costasPh = costasFr = 0; std::memset(mfBuf, 0, sizeof mfBuf); mfPos = sampleIdx = bestPhase = sinceSym = 0;
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

    void restartFinder() { finding = true; findFrames = 0; findPow.assign(kFindN, 0.f); unlockedSecs = 0; }
    void setTune(double hz) {
        tuneHz = hz; mixRot = cf32(1, 0);
        const double w = -2 * kPi * hz / inRate;
        mixStep = cf32((float)std::cos(w), (float)std::sin(w));
    }

    void findFeed(const cf32* x, size_t n) {
        if (n < (size_t)kFindN) return;
        findBuf.resize(kFindN);
        for (int k = 0; k < kFindN; k++) {
            const cf32 v = x[k];
            const float w = 0.5f - 0.5f * (float)std::cos(2 * kPi * k / kFindN);
            findBuf[(size_t)k] = std::isfinite(v.real() + v.imag()) ? v * w : cf32(0, 0);
        }
        findFft.forward(findBuf.data());
        for (int k = 0; k < kFindN; k++) findPow[(size_t)((k + kFindN / 2) % kFindN)] += std::norm(findBuf[(size_t)k]);   // bin kFindN/2 = DC
        if (++findFrames < kFindFrames) return;
        finding = false;
        // the score of a frequency: the median power over +-75 kHz around it (an FM channel is flat there; a DC spike or a narrow carrier is not)
        const double binHz = inRate / kFindN;
        const int half = std::max(2, (int)(75e3 / binHz));
        const int lim = (int)((inRate / 2 - 110e3) / binHz);
        if (lim < 0) return;
        std::vector<float> win;
        auto score = [&](int k) {   // k relative to DC
            win.clear();
            for (int j = k - half; j <= k + half; j++) { const int b = j + kFindN / 2; if (b >= 0 && b < kFindN) win.push_back(findPow[(size_t)b]); }
            std::nth_element(win.begin(), win.begin() + (std::ptrdiff_t)(win.size() / 2), win.end());
            return win[win.size() / 2];
        };
        const int step = std::max(1, half / 8);
        float best = -1; int bk = 0;
        for (int k = -lim; k <= lim; k += step) { const float v = score(k); if (v > best) { best = v; bk = k; } }
        std::vector<float> all(findPow);
        std::nth_element(all.begin(), all.begin() + (std::ptrdiff_t)(all.size() / 2), all.end());
        const float floorP = all[all.size() / 2];
        const float centre = score(0);
        double hz = 0;
        // A weak station (carrier to noise 5 to 10 dB) stands only 3 to 10 times over the floor in these averaged spectra, which are very steady
        // (a median over 300 bins of a 16 frame average): the bar is 3 times the floor, not 10. A station already at the centre (more than a third
        // of the best window, or 10 times the floor) is the one tuned to and stays.
        if (best > 3 * floorP && centre < std::min(10 * floorP, 0.35f * best)) {
            // the centre of the station: the power centroid over +-100 kHz around the best window
            double sp = 0, sf = 0;
            const int w = (int)(100e3 / binHz);
            for (int j = bk - w; j <= bk + w; j++) {
                const int b = j + kFindN / 2;
                if (b < 0 || b >= kFindN) continue;
                const double p = std::max(0.0, (double)findPow[(size_t)b] - floorP);
                sp += p; sf += p * j;
            }
            hz = sp > 0 ? sf / sp * binHz : bk * binHz;
        }
        if (std::fabs(hz - tuneHz) > 20e3) {
            setTune(hz);
            reset();
        }
    }

    void feed(const cf32* x, size_t n) {
        if (!ready || n == 0) return;
        if (const double w = chanWant.load(std::memory_order_relaxed); w != chanPass) designChan(w);   // a new width: the filter starts empty
        if (!autoFind.load(std::memory_order_relaxed) && (finding || tuneHz != 0)) { finding = false; if (tuneHz != 0) { setTune(0); reset(); } }
        if (finding) findFeed(x, n);
        if (tuneHz != 0) {
            mixBuf.resize(n);
            for (size_t i = 0; i < n; i++) {
                mixBuf[i] = x[i] * mixRot;
                mixRot *= mixStep;
                if ((i & 1023) == 0) mixRot /= std::abs(mixRot);
            }
            x = mixBuf.data();
        }
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
        // only when the carrier is clearly off the centre: on the centre (within 50 Hz) the mean is the carrier itself in quiet passages
        const bool dcOn = std::fabs(cfoEma) > 50;
        freq.resize(cn);
        // the sound below about 22 dB SNR (a carrier to noise near 9 dB) takes the equaliser's output: there its narrower filter keeps clicks and hiss down (threshold
        // extension, up to 6 dB better near the FM threshold); RDS keeps the raw channel unless there is multipath (the equaliser's
        // filter cuts the RDS sidebands). Two discriminator outputs, two multiplex decimators, only while they differ.
        audioCma = cmaUse || (audioCma ? lastSnr < 24.f : lastSnr < 22.f);
        const bool wasSplit = split;
        split = audioCma != cmaUse;
        if (split && !wasSplit) mpxDecimR.reset();
        if (split) freqR.resize(cn);
        for (size_t i = 0; i < cn; i++) {
            // a NaN or infinite sample (a broken file) would stay for good in the pilot loop, the DC average and the measurements: count it as silence
            cf32 c = std::isfinite(cur[i].real()) && std::isfinite(cur[i].imag()) ? cur[i] : cf32(0, 0);
            dcCh += (c - dcCh) * dcChA;
            if (dcOn) c -= dcCh;
            c = cma(c);
            const cf32 raw = cmaRawD;
            const cf32 pr = std::conj(prevRaw) * raw;
            const float fr = (float)(std::atan2(pr.imag(), pr.real()) * fo / (2 * kPi));   // the raw instantaneous frequency, Hz
            {   // binned by the frequency averaged over 8 samples (16 us): the noise's own frequency jitter, which goes with the dips of the
                // envelope, is a third of what it is per sample
                mpAvg += fr - mpHist[mpHp]; mpHist[mpHp] = fr; mpHp = (mpHp + 1) & 7;
                const int bin = (int)std::floor((mpAvg * 0.125f + 80e3f) / 10e3f);
                if (bin >= 0 && bin < kMpBins) { mpPow[bin] += std::norm(raw); mpN[bin] += 1; }
            }
            if (cmaUse || audioCma) {
                const cf32 p = std::conj(prev) * c;
                const float fc = (float)(std::atan2(p.imag(), p.real()) * fo / (2 * kPi) / kDevHz);
                freq[i] = audioCma ? fc : fr / (float)kDevHz;
                if (split) freqR[i] = cmaUse ? fc : fr / (float)kDevHz;
            } else freq[i] = (float)(fr / kDevHz);
            prev = c; prevRaw = raw;
            pwSum += std::norm(c);
            envSum += std::abs(c);
        }
        pwN += cn;
        mpSecs += cn / fo;
        if (mpSecs >= 0.25) mpJudge();
        for (size_t i = 0; i < cn; i++) freqSum += freq[i];
        freqN += cn;
        mpx.clear();
        mpxDecim.process(freq.data(), cn, mpx);
        if (split) { mpxR.clear(); mpxDecimR.process(freqR.data(), cn, mpxR); if (mpxR.size() != mpx.size()) mpxR.resize(mpx.size(), 0.f); }
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
        if (rdsOwn == pilotLock) { rdsOwn = !pilotLock; costasPh = costasFr = 0; }
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
            // RDS carrier: three times the pilot phase, or without a pilot (a mono station) a 57 kHz oscillator of its own
            const double ph57 = rdsOwn ? nco57 : 3 * ph0;
            const float mr = split ? mpxR[i] - (float)dc : m;
            rIn[i] = cf32((float)(mr * std::cos(ph57)), (float)(-mr * std::sin(ph57)));
            nco57 += 2 * kPi * 57000.0 / fm;
            if (nco57 > 2 * kPi) nco57 -= 2 * kPi;
            mpxRing[ringPos] = mr;   // the SNR measure and the spectrum: the raw channel while split (the equaliser would hide noise from it)
            ringPos = (ringPos + 1) % kMpxFft;
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
        const int sm = stereoMode.load(std::memory_order_relaxed);
        const bool pilotThere = pilotLock || (sm == 2 && 2 * pI > 0.006);   // forced stereo: a weak pilot is enough
        {
            const float want = sm == 2 ? 1.f : std::max(0.f, std::min(1.f, (lastSnr - 24.f) / 12.f));
            snrBlend += (want - snrBlend) * std::min(1.f, (float)(n / fm));   // about a second
        }
        const float target = (pilotThere && !forceMono.load(std::memory_order_relaxed)) ? snrBlend : 0.f;
        for (size_t i = 0; i < na; i++) {
            stereoBlend += (target - stereoBlend) * 0.002f;
            const float m = mOut[i], d = dOut[i] * stereoBlend;
            lr[i] = cf32(deL.process((m + d) / 0.9f * 0.8f), deR.process((m - d) / 0.9f * 0.8f));
        }
        if (audioRsOk) {
            lr48.clear();
            audioRs.process(lr.data(), na, lr48);
            outL.resize(lr48.size()); outR.resize(lr48.size()); inter.resize(lr48.size() * 2);
            for (size_t i = 0; i < lr48.size(); i++) { outL[i] = lr48[i].real(); outR[i] = lr48[i].imag(); }
            {   // the tone control: off by default, so the audio is not touched unless the user sets it
                const int tb = toneBass.load(), tt = toneTreble.load();
                const bool tw = toneWarm.load();
                if (tb != toneBassSet || tt != toneTrebleSet || tw != toneWarmSet) {
                    tone.configure(tb, tt, tw, kAudioRate);
                    toneBassSet = tb; toneTrebleSet = tt; toneWarmSet = tw;
                }
                tone.process(outL.data(), outR.data(), lr48.size());
            }
            {   // the audio bandwidth: off (full) by default
                const int bw = audioBwHz.load(std::memory_order_relaxed);
                if (bw != bwSet) {
                    bwSet = bw;
                    if (bw > 0) for (auto& ch : bwF) { ch[0].lowpass(bw, 0.5412, kAudioRate); ch[1].lowpass(bw, 1.3066, kAudioRate); }   // Butterworth, 4th order
                }
                if (bwSet > 0) for (size_t i = 0; i < lr48.size(); i++) {
                    outL[i] = bwF[0][1].process(bwF[0][0].process(outL[i]));
                    outR[i] = bwF[1][1].process(bwF[1][0].process(outR[i]));
                }
            }
            {   // the squelch: the SNR of the last report against the threshold, with hysteresis; the gain ramps over 10 ms
                const float thr = squelchDb.load(std::memory_order_relaxed);
                if (thr <= 0) sqOpen = true;
                else sqOpen = sqOpen ? lastSnr >= thr - 2.f : lastSnr >= thr;
                const float want = sqOpen ? 1.f : 0.f, step = 1.f / (0.01f * kAudioRate);
                if (want != 1.f || sqGain != 1.f) for (size_t i = 0; i < lr48.size(); i++) {
                    sqGain = want > sqGain ? std::min(want, sqGain + step) : std::max(want, sqGain - step);
                    outL[i] *= sqGain; outR[i] *= sqGain;
                }
            }
            for (size_t i = 0; i < lr48.size(); i++) { inter[2 * i] = outL[i]; inter[2 * i + 1] = outR[i]; }
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
        // RDS (with a locked pilot its carrier is tied to the pilot; without one it is followed by a Costas loop)
        if (rdsRsOk) {
            r1.clear();
            rdsLp.process(rIn.data(), n, r1);
            r19.clear();
            rdsRs.process(r1.data(), r1.size(), r19);
            for (const cf32& z : r19) rdsSample(z);
        }
    }

    void rdsSample(cf32 z) {
        cf32 r;
        if (!rdsOwn) {
            sq += (z * z - sq) * (1.f / 4000.f);
            const float th = 0.5f * std::atan2(sq.imag(), sq.real());
            r = z * cf32(std::cos(-th), std::sin(-th));
        } else {
            // no pilot: the transmitter's 57 kHz (+-6 Hz) and the radio's clock leave a residual of up to about 15 Hz, followed by a
            // second-order Costas loop. The loop is updated once per symbol from the matched filter's output (below), where the symbol's
            // whole energy is, not per sample from the raw samples (16 times the noise): about 2.5 dB better on a weak mono station.
            r = z * cf32((float)std::cos(-costasPh), (float)std::sin(-costasPh));
            costasPh += costasFr;
            if (costasPh > kPi) costasPh -= 2 * kPi; else if (costasPh < -kPi) costasPh += 2 * kPi;
        }
        mfBuf[mfPos] = r; mfPos = (mfPos + 1) & 15;
        static float tpl[16]; static bool tplInit = false;
        if (!tplInit) { for (int k = 0; k < 16; k++) tpl[k] = (float)std::sin(2 * kPi * (k + 0.5) / 16); tplInit = true; }
        cf32 y(0, 0);
        for (int k = 0; k < 16; k++) y += mfBuf[(mfPos + k) & 15] * tpl[k];
        const int ph = sampleIdx & 15;
        phaseEnergy[ph] += (std::norm(y) - phaseEnergy[ph]) * 0.01f;
        // the bit timing: the sampling phase (of 16) with the most energy after the matched filter. It moves when the radio's clock drifts against the
        // bit clock. A symbol is taken when the phase reaches the best one or has passed it by at most two samples, at most once per 8 samples: a move of the best phase
        // can then neither take a bit twice (across the wrap from 15 to 0) nor skip one (the best phase moving back past the current sample).
        int best = 0;
        for (int k = 1; k < 16; k++) if (phaseEnergy[k] > phaseEnergy[best]) best = k;
        if (phaseEnergy[best] > 1.15f * phaseEnergy[bestPhase]) bestPhase = best;
        sampleIdx++;
        sinceSym++;
        if (sinceSym < 8 || ((ph - bestPhase + 16) & 15) >= 3) return;
        sinceSym = 0;
        symAvg += (std::fabs(y.real()) - symAvg) * 0.02f;
        const int sym = y.real() >= 0 ? 1 : 0;
        if (rdsOwn) {   // the Costas loop at the symbol rate: wide (20 Hz) until blocks decode, then 5 Hz
            const double e = std::max(-1.0, std::min(1.0, (double)(sym ? y.imag() : -y.imag()) / std::max(symAvg, 1e-9f)));
            const double bn = rds.synced ? 5.0 : 20.0, wn = 2 * bn / (0.707 + 1 / (4 * 0.707)), wT = wn / 1187.5;
            costasPh += 2 * 0.707 * wT * e;
            costasFr += wT * wT * e / 16;   // the frequency is applied per sample, 16 per symbol
            costasFr = std::max(-2 * kPi * 25 / 19000.0, std::min(2 * kPi * 25 / 19000.0, costasFr));
        }
        rds.pushBit(sym ^ prevSym, std::fabs(y.real()));
        prevSym = sym;
        constHist.push_back(y / std::max(symAvg, 1e-6f));
        if (constHist.size() > 300) constHist.erase(constHist.begin(), constHist.begin() + 100);
    }

    void publish() {
        const uint64_t sincePubN = sincePub;
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
        t.cfoHz += tuneHz;
        t.devKhz = (float)(peakMpx * 75.0);
        t.levelDbfs = pwN ? (float)(10 * std::log10(std::max(pwSum / pwN, 1e-12))) : -120.f;
        t.stereo = (pilotLock || stereoMode.load() == 2) && stereoBlend > 0.5f * std::max(snrBlend, 0.1f) && !forceMono.load(std::memory_order_relaxed);
        t.stereoPct = t.stereo ? 100 * stereoBlend : 0.f;
        t.pilotPct = pilotLock ? (float)(200 * pI) : 0.f;
        t.rdsSync = rds.synced;
        t.rdsBlockOkPct = rds.okPct;
        t.rdsGroups = rds.groups;
        t.psName = rds.ps; t.radioText = rds.rt; t.radioTextComplete = rds.rtComplete; const bool rbds = rdsUseRbds(rdsRegion.load(), tunedMhz.load());
        t.rbds = rbds;
        t.ptyText = rds.groups ? rdsPtyName(rds.pty, rbds) : "";
        t.callSign = rbds && rds.groups ? rbdsCallSign(rds.pi) : "";   // US call letters are in the PI code
        t.tmc = rds.tmc.system();
        t.tmcMessages = rds.tmc.messages(std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count());
        t.trafficAlert = rds.ta; t.trafficProgram = rds.tp; t.music = rds.ms; t.piCode = rds.groups ? rds.pi : 0;
        t.rds = rds.info;
        t.multipathPct = 100 * mpRipple; t.equaliser = cmaUse;
        // an FM carrier has a constant envelope (spread about 0.2 at 12 dB), noise alone a Rayleigh envelope (0.52)
        double cv = 1;
        if (pwN && envSum > 0) cv = std::sqrt(std::max(0.0, pwSum / pwN / ((envSum / pwN) * (envSum / pwN)) - 1));
        t.carrier = cv < 0.4;
        if (!t.carrier) { t.snrDb = 0; snr = 0; }
        lastSnr = snr;   // the squelch's measure (same thread as the audio)
        t.squelched = !sqOpen;
        t.state = !t.carrier ? 0 : snr >= 20 ? 2 : snr >= 10 ? 1 : 0;
        // nothing receivable for two seconds: look for the station again (a new recording, a retune that did not reset)
        unlockedSecs = t.state == 0 ? unlockedSecs + (double)sincePubN / inRate : 0;
        if (unlockedSecs > 2 && !finding && autoFind.load(std::memory_order_relaxed)) { restartFinder(); }
        if (!t.carrier) { t.stereo = false; t.pilotPct = 0; t.stereoPct = 0; }
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
void FmReceiver::reset() { if (p_->ready) { p_->reset(); p_->restartFinder(); p_->setTune(0); } }
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
void FmReceiver::setRdsRegion(int region, double tunedMhz) {
    p_->rdsRegion = region == kRdsEurope || region == kRdsUs ? region : (int)kRdsAuto;
    p_->tunedMhz = tunedMhz;
}
void FmReceiver::setTone(int bassDb, int trebleDb, bool warm) {
    p_->toneBass = std::max(-6, std::min(6, bassDb));
    p_->toneTreble = std::max(-6, std::min(6, trebleDb));
    p_->toneWarm = warm;
}
void FmReceiver::setMono(bool on) { p_->forceMono = on; p_->stereoMode = on ? 1 : 0; }
void FmReceiver::setStereoMode(int mode) { const int m = mode >= 0 && mode <= 2 ? mode : 0; p_->stereoMode = m; p_->forceMono = m == 1; }
void FmReceiver::setAudioBandwidth(int hz) { p_->audioBwHz = hz <= 0 || hz >= 15000 ? 0 : std::max(2000, hz); }
void FmReceiver::setSquelch(float snrDb) { p_->squelchDb = std::max(0.f, std::min(60.f, snrDb)); }
void FmReceiver::setStationFinder(bool on) { p_->autoFind = on; }

void FmReceiver::setChannelWidth(double hz) {
    p_->chanWant = hz > 0 ? std::min(kMaxChannelWidthHz, std::max(kMinChannelWidthHz, hz)) * 0.5 : kChanPass;
}
double FmReceiver::channelWidth() const { return 2 * p_->chanWant.load(); }
void FmReceiver::setSilent(bool s) { p_->silent = s; }
void FmReceiver::setRdsCorrection(int level) { p_->rds.setCorrection(level >= 1, level >= 2); }
void FmReceiver::setRdsGroupTap(std::function<void(const uint16_t blocks[4], const uint8_t err[4])> cb) { p_->rds.setGroupTap(std::move(cb)); }
void FmReceiver::setAudioTap(std::function<void(const float*, const float*, size_t)> cb) { p_->tap = std::move(cb); }

} // namespace dect2
