// General receiver (see general_rx.h): one channel of the band mixed down, filtered, demodulated (NFM, WFM, AM, USB, LSB, CW),
// squelched and played; optionally recorded to WAV, one file per transmission.
#include "dect2/general/general_rx.h"
#include "dect2/general/am_proc.h"
#include "dect2/dsp/baseband_filter.h"
#include "dect2/audio/flac_enc.h"
#include "dect2/audio/mp3_enc.h"
#include "dect2/general/general_tel.h"
#include "dect2/audio/audioout.h"
#include "dect2/audio/neural_nr.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/qo100/qo100_dsp.h"
#include "dect2/common/timecompat.h"
#include "../hfdig/hfdig_usb.h"
#include <algorithm>
#include <cerrno>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {

using dect2::math::kPi;
constexpr double kOutLevel = 0.5;          // full deviation / full AM depth / an SSB tone under the AGC: this peak
constexpr double kBlockSec = 0.01;         // squelch decisions
constexpr double kHangCwSec = 1.0;      // CW and beacons (NDBs) key on and off: a longer mute hang so the squelch does not chatter on every dot and gap
constexpr double kHangSec = 0.15;      // the mute: long enough that a click does not get through. The file hang is separate (setTransmissionGate).
constexpr int kNoiseN = 512;               // noise spectrum of the decimated stream
constexpr int kNoiseAvg = 8;
constexpr double kMedianToMean = 0.959;    // median / mean of an average of 8 exponential bins (chi-square, 16 degrees)

const double kNfmW[] = {6250, 12500, 25000};
const double kWfmW[] = {150000, 180000, 200000};
const double kAmW[] = {4000, 6000, 9000, 12000};
const double kSsbW[] = {1800, 2400, 2700, 3000};
const double kCwW[] = {100, 250, 500, 1000};
const double kDsbW[] = {3000, 5000, 6000, 9000};
const double kRawW[] = {12000, 24000, 40000};

// Real FIR with a doubled history (the window never wraps), output every `decim` inputs
class RealFir {
public:
    void design(int n, double fc, int decim) {
        const auto lp = hfdig::lowpass(n, fc, 7.0);
        taps_.assign(lp.begin(), lp.end());
        hist_.assign(2 * (size_t)n, 0.f);
        decim_ = std::max(1, decim);
        reset();
    }
    void reset() { std::fill(hist_.begin(), hist_.end(), 0.f); pos_ = 0; ph_ = 0; }
    bool push(float x, float& y) {
        const int T = (int)taps_.size();
        hist_[(size_t)pos_] = x; hist_[(size_t)(pos_ + T)] = x;
        if (++pos_ >= T) pos_ = 0;
        if (++ph_ < decim_) return false;
        ph_ = 0;
        const float* w = &hist_[(size_t)pos_];
        float s = 0;
        for (int k = 0; k < T; k++) s += taps_[(size_t)k] * w[k];   // symmetric taps: the order does not matter
        y = s;
        return true;
    }
private:
    std::vector<float> taps_, hist_;
    int pos_ = 0, ph_ = 0, decim_ = 1;
};

struct Cfg {
    double chanOff = 0;
    int demod = GenNfm;
    double width = 12500;
    bool sqOn = true, sqAuto = true;
    float sqDb = -60;
    bool agc = true;
    float gainDb = 20;
    double deemphUs = 0;
    double pitch = 700;
    int amDet = 0;              // AM: 0 envelope, 1 synchronous (both sidebands), 2 synchronous upper sideband, 3 lower
    bool nbOn = false;          // noise blanker
    float nbDb = 12;            // a sample this far above the mean power is an impulse
    bool amAuto = false;        // AM: the Auto quality processor (am_proc.h)
    int amNr = -1, amBw = 0;    // its noise reduction (-1 auto, 0 off, 1 .. 100) and audio bandwidth (0 auto)
    bool amEnhance = false;
};

void put16(FILE* f, uint16_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; fwrite(b, 1, 2, f); }
void put32(FILE* f, uint32_t v) { uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; fwrite(b, 1, 4, f); }
void wavHeader(FILE* f, uint32_t frames) {
    const uint32_t data = frames * 2;
    fwrite("RIFF", 1, 4, f); put32(f, 36 + data); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 1); put16(f, 1); put32(f, (uint32_t)kGeneralAudioRate); put32(f, (uint32_t)kGeneralAudioRate * 2);
    put16(f, 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, data);
}

// RBJ biquads for the audio filters (high-pass, low-pass, notch) at 48 kHz
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    bool on = false;
    void set(int kind, double f0, double q) {   // kind 0 high-pass, 1 low-pass, 2 notch; f0 <= 0: off
        z1 = z2 = 0;
        on = f0 > 20 && f0 < kGeneralAudioRate / 2 - 100;
        if (!on) return;
        const double w = 2 * dect2::math::kPi * f0 / kGeneralAudioRate, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
        if (kind == 0) { b0 = (1 + c) / 2; b1 = -(1 + c); b2 = (1 + c) / 2; }
        else if (kind == 1) { b0 = (1 - c) / 2; b1 = 1 - c; b2 = (1 - c) / 2; }
        else { b0 = 1; b1 = -2 * c; b2 = 1; }
        a1 = -2 * c / a0; a2 = (1 - al) / a0; b0 /= a0; b1 /= a0; b2 /= a0;
    }
    float run(float x) { if (!on) return x; const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return (float)y; }
};

// The speaker and the transmission file each have their own filters: the file keeps the signal while the squelch has muted the speaker.
struct AudioFilt {
    float curHp = -1, curLp = -1, curNotch = -1;
    Biquad hp, lp, notch;
    void reset() { hp.z1 = hp.z2 = lp.z1 = lp.z2 = notch.z1 = notch.z2 = 0; }
    void apply(std::vector<float>& a, float hpHz, float lpHz, float ntHz) {
        if (hpHz != curHp) { curHp = hpHz; hp.set(0, hpHz, 0.707); }
        if (lpHz != curLp) { curLp = lpHz; lp.set(1, lpHz, 0.707); }
        if (ntHz != curNotch) { curNotch = ntHz; notch.set(2, ntHz, 12.0); }
        if (!hp.on && !lp.on && !notch.on) return;
        for (float& v : a) v = notch.run(lp.run(hp.run(v)));
    }
};

// The last few seconds of audio or channel IQ, so a file can start before the squelch opened.
template <class T>
struct Delay {
    std::vector<T> b;
    size_t wr = 0, filled = 0;
    void assign(size_t n) { b.assign(n, T{}); wr = 0; filled = 0; }
    void clear() { wr = 0; filled = 0; if (!b.empty()) std::fill(b.begin(), b.end(), T{}); }
    void push(const T* x, size_t n) {
        if (b.empty() || !x || n == 0) return;
        for (size_t i = 0; i < n; i++) {
            b[wr] = x[i];
            if (++wr == b.size()) wr = 0;
            if (filled < b.size()) filled++;
        }
    }
    void copyTail(size_t n, std::vector<T>& dst) const {
        if (b.empty() || filled == 0 || n == 0) return;
        if (n > filled) n = filled;
        const size_t start = (wr + b.size() - n) % b.size();
        const size_t first = std::min(n, b.size() - start);
        dst.insert(dst.end(), b.begin() + (std::ptrdiff_t)start, b.begin() + (std::ptrdiff_t)(start + first));
        if (first < n) dst.insert(dst.end(), b.begin(), b.begin() + (std::ptrdiff_t)(n - first));
    }
};

} // namespace

const char* generalDemodName(int d) {
    static const char* const k[] = {"NFM", "WFM", "AM", "USB", "LSB", "CW", "DSB", "RAW", "CW-L"};
    return d >= 0 && d < GenDemodCount ? k[d] : "?";
}

const double* generalWidthPresets(int d, int& count) {
    switch (d) {
    case GenWfm: count = 3; return kWfmW;
    case GenAm: count = 4; return kAmW;
    case GenUsb: case GenLsb: count = 4; return kSsbW;
    case GenCw: case GenCwl: count = 4; return kCwW;
    case GenDsb: count = 4; return kDsbW;
    case GenRaw: count = 3; return kRawW;
    default: count = 3; return kNfmW;
    }
}

double generalDefaultWidth(int d) {
    switch (d) {
    case GenWfm: return 180000;
    case GenAm: return 9000;
    case GenUsb: case GenLsb: return 2700;
    case GenCw: case GenCwl: return 500;
    case GenDsb: return 6000;
    case GenRaw: return 24000;
    default: return 12500;
    }
}

void generalWidthRange(int d, double& lo, double& hi) {
    switch (d) {
    case GenWfm: lo = 50000; hi = 220000; break;
    case GenAm: lo = 1000; hi = 20000; break;
    case GenUsb: case GenLsb: lo = 500; hi = 6000; break;
    case GenCw: case GenCwl: lo = 50; hi = 3000; break;
    case GenDsb: lo = 1000; hi = 20000; break;
    case GenRaw: lo = 2000; hi = 44000; break;   // the channel runs at 48 kHz
    default: lo = 2000; hi = 30000; break;
    }
}

double generalClampWidth(int d, double hz) {
    double lo, hi;
    generalWidthRange(d, lo, hi);
    return std::clamp(std::isfinite(hz) ? hz : generalDefaultWidth(d), lo, hi);
}

void generalFilterEdges(int d, double w, double& lo, double& hi) {
    w = generalClampWidth(d, w);
    if (d == GenUsb) { lo = kGeneralSsbLowCutHz; hi = kGeneralSsbLowCutHz + w; }
    else if (d == GenLsb) { lo = -kGeneralSsbLowCutHz - w; hi = -kGeneralSsbLowCutHz; }
    else { lo = -w / 2; hi = w / 2; }
}

double generalDefaultDeemphUs(int d) { return d == GenWfm ? 50 : 0; }

struct GeneralReceiver::Impl {
    std::mutex mu;
    Cfg cfgSet, cfg;
    std::atomic<bool> cfgDirty{true}, resetReq{false};
    std::atomic<double> sigOff{0};
    double rate = 0;
    std::atomic<float> volume{1.f};
    std::atomic<bool> muted{false}, silent{false};
    std::function<void(const float*, size_t)> tapSet, tap;
    std::atomic<bool> tapReq{false};
    std::function<void(const std::string&)> logCb;

    // the chain
    double curRate = 0;
    bool wide = false, built = false;
    double mixHz = 1e300;
    qo100::Nco nco;
    qo100::Decimator dec;
    ExactResampler rs;
    double decRate = 0, demodRate = kGeneralAudioRate;
    std::vector<cf32> clean, decd, base;
    // channel filter
    std::vector<cf32> taps, hist;
    int hpos = 0;
    double fLo = 0, fHi = 0;
    // noise beside the channel
    std::unique_ptr<Fft> fft;
    std::vector<cf32> fbuf;
    std::vector<float> win;
    double wsum2 = 1;
    std::vector<double> acc;
    int accN = 0;
    double noisePsd = -1;      // per Hz, beside the channel; < 0 not measured yet
    double noisePsdIn = -1;    // per Hz, the median bin inside the voice passband (USB / LSB)
    bool noiseImpossible = false;
    // demodulators
    cf32 prev{0, 0};
    double gLin = 10, relEnv = 0, carK = 0;
    float deAl = 1;
    double car = 0, env = 0, bfoPh = 0;
    bool carInit = false;
    float dcX = 0, dcY = 0, deY = 0;
    RealFir audLp;
    // Synchronous AM: a PLL on the carrier (wide for the first half second, then 25 Hz; it holds its frequency while the carrier fades
    // below a quarter of its level, so selective fading does not throw it off), the carrier taken out as the mean of the in-phase part,
    // the gain from the carrier's level over a second (not the fast one: a fading carrier does not pump the sound). One sideband: the
    // positive (upper) or negative (lower) frequencies of the derotated signal through a Hilbert filter, which leaves an interferer on the
    // other side out.
    static constexpr int kHilN = 127;
    double samPh = 0, samFr = 0, samCar = 0, samCarLong = 0, samT = 0;
    cf32 samCarC{0, 0};
    bool samInit = false;
    float hil[kHilN] = {};
    cf32 samHist[2 * kHilN] = {};
    int samHp = 0;
    void samReset() {
        samPh = samFr = samCar = samCarLong = samT = 0; samCarC = cf32(0, 0); samInit = false; samHp = 0;
        for (auto& v : samHist) v = cf32(0, 0);
        if (hil[0] == 0.f && hil[1] == 0.f) for (int k = 0; k < kHilN; k++) {
            const int n = k - kHilN / 2;
            const double w = 0.42 - 0.5 * std::cos(2 * kPi * k / (kHilN - 1)) + 0.08 * std::cos(4 * kPi * k / (kHilN - 1));
            hil[k] = n % 2 ? (float)(2.0 / (kPi * n) * w) : 0.f;
        }
    }
    float samDemod(cf32 z) {
        const cf32 r = z * cf32((float)std::cos(-samPh), (float)std::sin(-samPh));
        const double m = std::abs(z);
        if (!samInit) { samCar = m; samCarLong = m; samCarC = cf32((float)m, 0); samInit = true; }
        samCar += carK * (r.real() - samCar);
        samCarC += (float)carK * (r - samCarC);
        samCarLong += (1 - std::exp(-1.0 / demodRate)) * (m - samCarLong);
        samT += 1 / demodRate;
        const double bn = samT < 0.5 ? 120.0 : 25.0, wn = 2 * bn / (0.707 + 1 / (4 * 0.707)), wT = wn / demodRate;
        const double e = std::atan2((double)r.imag(), (double)r.real());
        if (std::abs(samCarC) >= 0.25 * samCarLong || samT < 0.5) { samFr += wT * wT * e; samPh += samFr + 2 * 0.707 * wT * e; }
        else samPh += samFr;
        samFr = std::clamp(samFr, -2 * kPi * 500 / demodRate, 2 * kPi * 500 / demodRate);
        if (samPh > kPi) samPh -= 2 * kPi; else if (samPh < -kPi) samPh += 2 * kPi;
        double s = r.real();
        if (cfg.amDet >= 2) {   // audio of one sideband: Re(r) -+ H{Im r}, Re(r) delayed to the middle of the filter
            samHp = (samHp + kHilN - 1) % kHilN;
            samHist[samHp] = samHist[samHp + kHilN] = r;
            const cf32* h = &samHist[samHp];
            double hi = 0;
            for (int k = 0; k < kHilN; k++) hi += hil[k] * h[k].imag();
            s = cfg.amDet == 2 ? h[kHilN / 2].real() - hi : h[kHilN / 2].real() + hi;
        }
        const double c = samCarLong + 1e-9;
        return (float)(cfg.agc ? (s - samCar) / c * kOutLevel : (s - samCar) * gLin);
    }
    // noise blanker on the decimated channel (before the channel filter, where an impulse is still short): a sample whose power is more
    // than nbDb above the running mean (0.1 s, impulses left out of it) is replaced, with its neighbours, by the last good one
    double nbMean = -1;
    int nbHold = 0;
    cf32 nbG1{0, 0}, nbG2{0, 0}, nbFill{0, 0};   // the last two samples passed, the value an impulse is replaced with
    uint64_t nbBlanked = 0;
    long nbRun = -1;                              // where the current run of blanked samples starts in this block (-1 none)
    void blank(std::vector<cf32>& v, double rate) {
        const double thr = std::pow(10.0, cfg.nbDb / 10.0), a = 1.0 / (0.1 * rate);
        nbRun = nbHold > 0 ? 0 : -1;
        for (size_t i = 0; i < v.size(); i++) {
            const double p = std::norm(v[i]);
            if (nbMean < 0) nbMean = p;
            const bool imp = nbMean > 0 && p > thr * nbMean;
            // the mean is fed with every sample clipped to the threshold: it follows a signal that rises (from the silence at the start)
            // instead of taking all of it for impulses
            nbMean += (std::min(p, thr * std::max(nbMean, 1e-30)) - nbMean) * a;
            if (nbMean <= 0) nbMean = p;
            if (imp) {
                if (nbHold == 0) {   // the samples before it go too (the channel filter rings ahead of an impulse as well)
                    const size_t back = std::min<size_t>(i, 3);
                    nbFill = i > back ? v[i - back - 1] : nbG1;
                    for (size_t k = i - back; k < i; k++) v[k] = nbFill;
                    nbRun = (long)(i - back);
                }   // the edge before it goes too
                nbHold = 4;
            }
            if (nbHold > 0) {
                v[i] = nbFill; nbHold--; nbBlanked++;
            } else {
                // the gap is closed with a straight line from the last good sample to this one (a held value makes a step at each end)
                if (nbRun >= 0) {
                    const long s = nbRun, len = (long)i - s;
                    for (long k = 0; k < len; k++) v[(size_t)(s + k)] = nbFill + (v[i] - nbFill) * (float)(k + 1) / (float)(len + 1);
                    nbRun = -1;
                }
                nbG2 = nbG1; nbG1 = v[i];
            }
        }
    }
    // AM through the processor (Auto quality, or a synchronous detector chosen by hand)
    AmProcessor amp;
    std::vector<float> ampOut;
    bool useAmp() const { return cfg.demod == GenAm && !wide && (cfg.amAuto || cfg.amDet > 0); }
    void ampConfig() {
        AmProcConfig c;
        c.autoQuality = cfg.amAuto;
        c.detector = cfg.amAuto ? (cfg.amDet > 0 ? cfg.amDet : -1) : cfg.amDet;
        c.bandwidthHz = cfg.amAuto ? cfg.amBw : (int)std::lround(cfg.width / 2);
        c.nr = cfg.amAuto ? cfg.amNr : 0;
        c.nbDb = cfg.nbOn ? (int)std::lround(cfg.nbDb) : (cfg.amAuto ? -1 : 0);
        c.notch = cfg.amAuto ? -1 : 0;
        c.eq = cfg.amAuto ? -1 : 0;
        c.enhance = cfg.amAuto && cfg.amEnhance;
        c.agc = cfg.agc; c.leveler = cfg.amAuto; c.gainDb = cfg.gainDb;
        amp.setConfig(c);
    }
    // squelch
    double blkPow = 0, lvl = -1;
    int blkN = 0, blkLen = 480;
    bool open = false;
    double hang = 0;
    float ramp = 0;
    double fmSum = 0; uint64_t fmN = 0; double cfo = 0;
    uint64_t blkOpen = 0, blkShut = 0;
    double audPow = 0; uint64_t audN = 0;
    // sound
    std::vector<float> aud, dryAud, pcm;
    std::vector<cf32> iqBlk;             // channel samples of this feed, when a transmission file wants them
    std::unique_ptr<AudioOut> out;
    // WAV
    std::mutex wavMu;
    FILE* wav = nullptr;
    FILE* iqFile = nullptr;
    // audio filters: high-pass, low-pass and a notch. The speaker hears the squelched audio; the file hears the signal.
    std::atomic<float> afHp{0}, afLp{0}, afNotch{0};
    AudioFilt spk, dry;
    NeuralNr nrSpk{48000}, nrDry{48000};   // neural noise remover (RNNoise), off by default: the speaker's audio and the file's
    FlacEnc flac; bool flacOn = false;   // the open file is FLAC, not WAV
    Mp3Enc mp3; bool mp3On = false; bool recMp3 = false;   // MP3 through LAME when it is installed
    bool recFlac = false; std::vector<std::string> recTags;   // format and tags of the per-transmission files
    void openFile(const std::string& path, bool asFlac, bool asMp3 = false) {   // with wavMu held; wav is null when it failed
        wav = fopen(path.c_str(), "wb");
        flacOn = false; mp3On = false;
        if (!wav) return;
        if (asMp3) { mp3On = mp3.begin(wav, (uint32_t)kGeneralAudioRate, 64, recTags); if (!mp3On) { fclose(wav); wav = nullptr; } return; }
        if (asFlac) { flacOn = flac.begin(wav, (uint32_t)kGeneralAudioRate, recTags); if (!flacOn) { fclose(wav); wav = nullptr; } }
        else wavHeader(wav, 0);
    }
    uint32_t wavFrames = 0;
    std::string wavPath, wavErr;
    std::vector<int16_t> wbuf;
    // per-transmission recording (wavMu held for the plain fields; the atomics are read from the sample loop)
    bool txOn = false; std::string txDir, txPrefix; double txMinSec = 0.5; int txN = 0, txKept = 0; bool txFile = false;
    std::atomic<double> txPreRoll{0}, txHang{0}, listenHz{0};
    std::atomic<bool> txIq{false}, txCsv{false}, wantDry{false}, wantIq{false}, plainRec{false};
    bool plainGate = false; double plainClosedFor = 0;   // single-file recording with the squelch on: written only while open, plus hang
    bool iqThis = false;                // this file has a channel-IQ sibling
    std::string iqPath, txIso;
    double txHz = 0, txOpenSec = 0, txClosedFor = 0;
    int txDemod = 0;
    uint64_t iqSamples = 0;
    Delay<float> preAud;
    Delay<cf32> preIq;
    // telemetry
    GeneralTelemetry pub;
    double pubAcc = 0;

    void log(const std::string& s) { std::function<void(const std::string&)> cb; { std::lock_guard<std::mutex> lk(mu); cb = logCb; } if (cb) cb(s); }

    void build() {
        built = false;
        if (curRate <= 0) return;
        wide = cfg.demod == GenWfm;
        demodRate = wide ? kGeneralWideRate : kGeneralAudioRate;
        const double pass = wide ? 115000.0 : 26000.0;
        dec.configure(curRate, pass, demodRate * (wide ? 1.0 : 2.0));
        decRate = dec.outRate();
        if (!rs.configure(decRate, demodRate)) return;
        blkLen = (int)std::lround(demodRate * kBlockSec);
        carK = 1 - std::exp(-1.0 / (0.05 * demodRate));
        relEnv = 1 - std::exp(-1.0 / (0.5 * demodRate));
        fft = std::make_unique<Fft>(kNoiseN);
        win.resize(kNoiseN); wsum2 = 0;
        for (int i = 0; i < kNoiseN; i++) { win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / kNoiseN)); wsum2 += (double)win[(size_t)i] * win[(size_t)i]; }
        if (wide) audLp.design(121, 15000.0 / demodRate, (int)std::lround(demodRate / kGeneralAudioRate));
        else audLp.design(63, 4000.0 / demodRate, 1);
        design();
        mixHz = 1e300;
        built = true;
        amp.configure(demodRate);
        clearState();
    }

    void design() {
        generalFilterEdges(cfg.demod, cfg.width, fLo, fHi);
        const double w = fHi - fLo;
        const int T0 = wide ? 63 : w < 1500 ? 511 : 255;
        const auto lp = bbf::overrideTaps("general", hfdig::lowpass(T0, 0.5 * w / demodRate, 7.0), demodRate, w);   // the user's baseband filter, if switched on
        const int T = (int)lp.size();
        const double w0 = 2 * kPi * 0.5 * (fLo + fHi) / demodRate, c = (T - 1) * 0.5;
        taps.assign((size_t)T, cf32(0, 0));
        for (int k = 0; k < T; k++) {
            const cf32 h((float)(lp[(size_t)k] * std::cos(w0 * (k - c))), (float)(lp[(size_t)k] * std::sin(w0 * (k - c))));
            taps[(size_t)(T - 1 - k)] = h;   // reversed: the history is kept oldest first
        }
        hist.assign(2 * (size_t)T, cf32(0, 0));
        hpos = 0;
        acc.assign(kNoiseN, 0.0); accN = 0; fbuf.clear(); noisePsd = -1; noisePsdIn = -1;   // the bins beside the channel moved
    }

    void clearState() {
        nco.reset(); dec.reset(); rs.reset();
        std::fill(hist.begin(), hist.end(), cf32(0, 0)); hpos = 0;
        acc.assign(kNoiseN, 0.0); accN = 0; fbuf.clear(); noisePsd = -1; noisePsdIn = -1;
        prev = cf32(0, 0); car = 0; env = 0; carInit = false; bfoPh = 0; samReset(); nbMean = -1; nbHold = 0; nbG1 = nbG2 = nbFill = cf32(0, 0);
        dcX = dcY = deY = 0; audLp.reset(); dry.reset();
        blkPow = 0; blkN = 0; lvl = -1; open = false; hang = 0; ramp = 0;
        fmSum = 0; fmN = 0; cfo = 0;
        preAud.clear(); preIq.clear();
        if (out) out->flush();
    }

    void noiseFeed(const cf32* x, size_t n) {
        if (!fft) return;
        for (size_t i = 0; i < n; i++) {
            fbuf.push_back(x[i]);
            if ((int)fbuf.size() < kNoiseN) continue;
            for (int k = 0; k < kNoiseN; k++) fbuf[(size_t)k] *= win[(size_t)k];
            fft->forward(fbuf.data());
            for (int k = 0; k < kNoiseN; k++) acc[(size_t)k] += std::norm(fbuf[(size_t)k]);
            fbuf.clear();
            if (++accN < kNoiseAvg) continue;
            // Beside the channel (NFM, AM, CW) and, separately, the bins inside the passband (USB / LSB: speech lifts a few of them,
            // the median stays near the noise). Both inside +-0.4 of the rate.
            const double bin = decRate / kNoiseN, guard = std::max(2 * bin, 0.15 * (fHi - fLo));
            std::vector<double> v, inb;
            for (int k = 0; k < kNoiseN; k++) {
                const double f = (k < kNoiseN / 2 ? k : k - kNoiseN) * bin;
                if (std::fabs(f) > 0.4 * decRate) continue;
                const double p = acc[(size_t)k] / accN;
                if (f > fLo && f < fHi) inb.push_back(p);
                else if (!(f > fLo - guard && f < fHi + guard)) v.push_back(p);
            }
            acc.assign(kNoiseN, 0.0); accN = 0;
            noiseImpossible = v.size() < 16;
            if (noiseImpossible) noisePsd = -1;
            else {
                std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
                const double psd = v[v.size() / 2] / kMedianToMean / wsum2 / decRate;
                noisePsd = noisePsd < 0 ? psd : 0.7 * noisePsd + 0.3 * psd;
            }
            if (inb.size() < 6) noisePsdIn = -1;   // a very narrow filter has too few bins for a median
            else {
                std::nth_element(inb.begin(), inb.begin() + inb.size() / 2, inb.end());
                const double psd = inb[inb.size() / 2] / kMedianToMean / wsum2 / decRate;
                noisePsdIn = noisePsdIn < 0 ? psd : 0.7 * noisePsdIn + 0.3 * psd;
            }
        }
    }

    void squelchBlock() {
        const double p = blkPow / std::max(1, blkN);
        blkPow = 0; blkN = 0;
        const double b = 1 - std::exp(-kBlockSec / 0.06);
        lvl = lvl < 0 ? p : lvl + b * (p - lvl);
        const bool ssb = (cfg.demod == GenUsb || cfg.demod == GenLsb) && cfg.sqAuto && noisePsdIn > 0;
        const double noiseCh = (ssb ? noisePsdIn : noisePsd) > 0 ? (ssb ? noisePsdIn : noisePsd) * (fHi - fLo) : -1;
        bool want;
        if (!cfg.sqOn) want = true;
        else if (ssb) want = lvl > noiseCh * (open ? 2.5 : 4.0);   // 6 dB over the noise inside the passband, closes under 4
        else if (cfg.sqAuto) {
            if (noiseCh <= 0) want = noiseImpossible;   // no band beside the channel to compare with: the squelch stays open
            else want = lvl > noiseCh * (open ? 2.5 : 4.0);   // opens 6 dB over the noise, closes under 4 dB
        } else {
            const double thr = std::pow(10.0, cfg.sqDb / 10.0);
            want = lvl >= (open ? thr / 1.26 : thr);
        }
        if (want) { open = true; hang = generalIsCw(cfg.demod) ? kHangCwSec : kHangSec; }
        else if (open) { hang -= kBlockSec; if (hang <= 0) open = false; }
        (open ? blkOpen : blkShut)++;
    }

    // one channel sample at demodRate: the filtered z gives 0 or 1 audio samples
    void demod(cf32 z) {
        float a = 0;
        bool have = true;
        const double g = gLin;
        switch (cfg.demod) {
        case GenNfm: case GenWfm: {
            const cf32 d = z * std::conj(prev);
            prev = z;
            const double f = std::atan2((double)d.imag(), (double)d.real()) * demodRate / (2 * kPi);
            if (std::norm(z) > 0) { fmSum += f; fmN++; }
            const float s = (float)(f / (cfg.demod == GenNfm ? 5000.0 : 75000.0) * kOutLevel);
            float y;
            have = audLp.push(s, y);
            if (!have) break;
            // DC (a station off the channel), then the de-emphasis
            const float hp = y - dcX + (float)(1 - 2 * kPi * 20 / kGeneralAudioRate) * dcY;
            dcX = y; dcY = hp;
            if (cfg.deemphUs > 0) { deY += deAl * (hp - deY); a = deY; }
            else a = hp;
            break;
        }
        case GenAm: {
            if (useAmp()) { have = false; break; }   // the processor makes the sound (feed)
            const double e = std::abs(z);
            if (!carInit) { car = e; carInit = true; }
            car += carK * (e - car);
            a = (float)(cfg.agc ? (e - car) / std::max(car, 1e-9) * kOutLevel : (e - car) * g);
            break;
        }
        default: {   // USB, LSB, DSB, RAW, CW: the real part (CW: moved up by the pitch; CW-L: mirrored first, so the note falls as the signal rises)
            cf32 v = cfg.demod == GenCwl ? std::conj(z) : z;
            if (generalIsCw(cfg.demod)) {
                v *= cf32((float)std::cos(bfoPh), (float)std::sin(bfoPh));
                bfoPh += 2 * kPi * cfg.pitch / demodRate;
                if (bfoPh > 2 * kPi) bfoPh -= 2 * kPi;
            }
            const double m = std::abs(z);
            env += (m > env ? 0.05 : relEnv) * (m - env);
            a = (float)(cfg.agc ? v.real() * kOutLevel / (env + 1e-9) : v.real() * g);
            break;
        }
        }
        if (!have) return;
        emit(a);
    }
    void emit(float a) {
        const float dry = std::clamp(a, -1.f, 1.f);
        const float target = open ? 1.f : 0.f;
        ramp += std::clamp(target - ramp, -1.f / 240, 1.f / 240);   // 5 ms
        aud.push_back(std::clamp(dry * ramp, -1.f, 1.f));
        if (wantDry.load(std::memory_order_relaxed) || plainRec.load(std::memory_order_relaxed)) dryAud.push_back(dry);
    }

    void play() {
        const bool haveAud = !aud.empty();
        if (haveAud) {
            spk.apply(aud, afHp, afLp, afNotch);
            nrSpk.process(aud.data(), aud.size());   // no-op while off
            for (float v : aud) audPow += (double)v * v;
            audN += aud.size();
            if (tapReq.exchange(false)) { std::lock_guard<std::mutex> lk(mu); tap = tapSet; }
            if (tap) tap(aud.data(), aud.size());
        }
        if (!dryAud.empty()) { dry.apply(dryAud, afHp, afLp, afNotch); nrDry.process(dryAud.data(), dryAud.size()); }
        {
            std::lock_guard<std::mutex> lk(wavMu);
            if (txOn && open && !wav) beginTransmission();
            if (wav) {
                if (txFile) writePcm(dryAud.data(), dryAud.size());
                else if (cfg.sqOn && !dryAud.empty()) {
                    // squelch on: the single file keeps only what the squelch let through, plus the pre-roll and the hang
                    const double dt = (double)dryAud.size() / kGeneralAudioRate;
                    if (open) {
                        if (!plainGate) {
                            plainGate = true;
                            std::vector<float> pre;
                            preAud.copyTail((size_t)std::lround(std::clamp(txPreRoll.load(), 0.0, 3.0) * kGeneralAudioRate), pre);
                            writePcm(pre.data(), pre.size());
                        }
                        plainClosedFor = 0;
                    } else if (plainGate) {
                        plainClosedFor += dt;
                        if (plainClosedFor > std::clamp(txHang.load(), 0.0, 3.0)) plainGate = false;
                    }
                    if (plainGate) writePcm(dryAud.data(), dryAud.size());
                } else if (haveAud) { plainGate = false; writePcm(aud.data(), aud.size()); }
                if (txFile && iqThis) writeIq(iqBlk.data(), iqBlk.size());
            }
            if (txFile) {
                const double dt = (double)dryAud.size() / kGeneralAudioRate;
                if (open) { txClosedFor = 0; txOpenSec += dt; }
                else txClosedFor += dt;
                const double hangFor = std::clamp(txHang.load(), 0.0, 3.0);
                if (!open && txClosedFor >= hangFor) endTransmission();
            }
            if (txOn || (wav && !txFile)) {
                sizeRings();
                if (!dryAud.empty()) preAud.push(dryAud.data(), dryAud.size());
                if ((txIq.load() || iqThis) && !iqBlk.empty()) preIq.push(iqBlk.data(), iqBlk.size());
            }
        }
        if (!haveAud || silent.load()) return;
        pcm.clear();
        for (float v : aud) { pcm.push_back(v); pcm.push_back(v); }
        if (!out) { out = std::make_unique<AudioOut>(); out->start((int)kGeneralAudioRate); out->setStartThreshold((int)kGeneralAudioRate / 10); }
        out->setVolume(volume.load()); out->setMuted(muted.load());
        if (out->bufferedFrames() > (int)kGeneralAudioRate / 2) out->flush();   // the radio's clock runs ahead of the sound card: catch up
        if (!out->playing()) out->setStartThreshold((int)kGeneralAudioRate / 10);
        out->write(pcm.data(), (int)(pcm.size() / 2));
    }

    void publish() {
        GeneralTelemetry t;
        t.seq = pub.seq + 1;
        t.demod = cfg.demod; t.rate = curRate; t.demodRate = demodRate;
        t.chanOffsetHz = cfg.chanOff; t.filterLoHz = fLo; t.filterHiHz = fHi;
        const bool ssb = (cfg.demod == GenUsb || cfg.demod == GenLsb) && noisePsdIn > 0;
        const double noisePow = (ssb ? noisePsdIn : noisePsd) > 0 ? (ssb ? noisePsdIn : noisePsd) * (fHi - fLo) : -1;
        t.levelDb = lvl > 0 ? (float)(10 * std::log10(lvl)) : -200;
        t.noiseDb = noisePow > 0 ? (float)(10 * std::log10(noisePow)) : -200;
        t.dataValid = noisePow > 0 && lvl > 0;
        t.snrDb = t.dataValid ? t.levelDb - t.noiseDb : 0;
        t.squelchOpen = open;
        if (useAmp()) {
            const AmProcStatus s = amp.status();
            t.amActive = true; t.amLocked = s.locked; t.amCarrierHz = (float)s.carrierHz; t.amBandwidthHz = s.bandwidthHz; t.amSnrDb = s.snrDb;
            t.amDetector = s.detector; t.amNrDb = s.nrDb; t.amStatus = s.text;
        }
        t.squelchDb = !cfg.sqOn ? -200 : cfg.sqAuto ? (noisePow > 0 ? t.noiseDb + 6 : -200) : cfg.sqDb;
        t.state = !built || lvl < 0 ? 0 : open ? 2 : 1;
        if (fmN) { cfo = fmSum / fmN; fmSum = 0; fmN = 0; }
        t.cfoHz = (cfg.demod == GenNfm || cfg.demod == GenWfm) && open ? cfo : 0;
        t.blocksOk = blkOpen; t.blocksBad = blkShut;
        t.audioDb = audN ? (float)(10 * std::log10(audPow / audN + 1e-20)) : -200;
        audPow = 0; audN = 0;
        {
            std::lock_guard<std::mutex> lk(wavMu);
            t.recording = wav != nullptr; t.recordSec = wavFrames / kGeneralAudioRate; t.recordPath = wavPath; t.recordError = wavErr;
        }
        std::lock_guard<std::mutex> lk(mu);
        pub = std::move(t);
    }

    void sizeRings() {   // with wavMu held
        const size_t aN = (size_t)std::lround(3.0 * kGeneralAudioRate);
        if (preAud.b.size() != aN) preAud.assign(aN);
        if (txIq.load() || iqThis) {
            const size_t iN = (size_t)std::lround(3.0 * std::max(demodRate, kGeneralAudioRate));
            if (preIq.b.size() != iN) preIq.assign(iN);
        }
    }
    static bool fileThere(const std::string& p) {
        FILE* f = fopen(p.c_str(), "rb");
        if (!f) return false;
        fclose(f);
        return true;
    }
    void writePcm(const float* x, size_t n) {   // with wavMu held
        if (!wav || !x || n == 0) return;
        for (size_t off = 0; off < n; ) {
            const size_t k = std::min(n - off, (size_t)4096);
            wbuf.resize(k);
            for (size_t i = 0; i < k; i++) wbuf[i] = (int16_t)std::lround(std::clamp(x[off + i], -1.f, 1.f) * 32767.f);
            if (mp3On) mp3.write(wbuf.data(), wbuf.size());
            else if (flacOn) flac.write(wbuf.data(), wbuf.size());
            else fwrite(wbuf.data(), 2, k, wav);   // little-endian hosts (macOS, Windows, Linux on x86 / ARM)
            wavFrames += (uint32_t)k;
            off += k;
        }
    }
    void writeIq(const cf32* x, size_t n) {   // with wavMu held. Complex float, little-endian, at demodRate.
        if (!iqFile || !x || n == 0) return;
        fwrite(x, sizeof(cf32), n, iqFile);
        iqSamples += n;
    }
    void beginTransmission() {   // with wavMu held, squelch just opened, no file yet
        const char* ext = recMp3 ? "mp3" : recFlac ? "flac" : "wav";
        txIso.clear(); txHz = listenHz.load(); txDemod = cfg.demod;
        if (!txPrefix.empty()) {
            char nm[64];
            snprintf(nm, sizeof nm, "_%d.%s", ++txN, ext);
            wavPath = txDir + "/" + txPrefix + nm;
        } else {
            const std::time_t now = std::time(nullptr);
            std::tm tm{};
            localTime(now, &tm);
            char stamp[32], iso[32];
            std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
            std::strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%S", &tm);
            txIso = iso;
            char base[192];
            snprintf(base, sizeof base, "onair_%s_%.6fMHz_%s", stamp, txHz / 1e6, generalDemodName(txDemod));
            std::string path = txDir + "/" + base + "." + ext;
            for (int n = 2; fileThere(path) && n < 100; n++) {
                char b[240];
                snprintf(b, sizeof b, "%s/%s_%d.%s", txDir.c_str(), base, n, ext);
                path = b;
            }
            wavPath = path;
        }
        openFile(wavPath, recFlac, recMp3);
        wavFrames = 0; txOpenSec = 0; txClosedFor = 0; iqSamples = 0; iqPath.clear(); iqThis = false;
        txFile = wav != nullptr;
        if (!wav) { wavErr = std::string("cannot write ") + wavPath; return; }
        wavErr.clear();
        if (txIq.load()) {
            std::string iq = wavPath;
            const auto dot = iq.find_last_of('.');
            if (dot != std::string::npos) iq.resize(dot);
            char rate[32];
            snprintf(rate, sizeof rate, "_%.0fk.cf32", demodRate / 1000.0);
            iq += rate;
            iqFile = fopen(iq.c_str(), "wb");
            if (iqFile) { iqPath = iq; iqThis = true; }
        }
        const double pr = std::clamp(txPreRoll.load(), 0.0, 3.0);
        if (pr > 0) {
            std::vector<float> pre;
            preAud.copyTail((size_t)std::lround(pr * kGeneralAudioRate), pre);
            writePcm(pre.data(), pre.size());
            if (iqThis) {
                std::vector<cf32> pi;
                preIq.copyTail((size_t)std::lround(pr * demodRate), pi);
                writeIq(pi.data(), pi.size());
            }
        }
    }
    void appendCsv(double sec, const std::string& path) {   // with wavMu held
        if (!txCsv.load() || txIso.empty()) return;
        const std::string cp = txDir + "/transmissions.csv";
        FILE* f = fopen(cp.c_str(), "ab+");
        if (!f) return;
        if (fseek(f, 0, SEEK_END) == 0 && ftell(f) == 0)
            fputs("time,frequency_hz,mode,duration_s,path\n", f);
        fprintf(f, "%s,%.1f,%s,%.3f,\"%s\"\n", txIso.c_str(), txHz, generalDemodName(txDemod), sec, path.c_str());
        fclose(f);
    }
    void endTransmission() {   // with wavMu held: the squelch has shut and the recording hang has run out
        const double sec = wavFrames / kGeneralAudioRate;
        const double openSec = txOpenSec;
        const std::string path = wavPath, iq = iqPath;
        closeWav();
        txFile = false; iqThis = false;
        if (openSec < txMinSec) {
            std::remove(path.c_str());
            if (!iq.empty()) std::remove(iq.c_str());
            return;
        }
        txKept++;
        appendCsv(sec, path);
    }
    void closeWav() {   // with wavMu held
        if (wav) {
            if (mp3On) { mp3.finish(); mp3On = false; }
            else if (flacOn) { flac.finish(); flacOn = false; }
            else { fflush(wav); fseek(wav, 0, SEEK_SET); wavHeader(wav, wavFrames); }
            fclose(wav);
            wav = nullptr;
        }
        if (iqFile) { fclose(iqFile); iqFile = nullptr; }
    }
};

GeneralReceiver::GeneralReceiver() : p_(std::make_unique<Impl>()) {}
GeneralReceiver::~GeneralReceiver() {
    std::lock_guard<std::mutex> lk(p_->wavMu);
    if (p_->txFile) p_->endTransmission(); else p_->closeWav();
}

void GeneralReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->cfgDirty = true; p_->resetReq = true; }
void GeneralReceiver::setSignalOffset(double hz) { p_->sigOff = hz; }
bool GeneralReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate >= generalTuning().minSampleRate - 1; }
void GeneralReceiver::reset() { p_->resetReq = true; }
void GeneralReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->logCb = std::move(cb); }

void GeneralReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.cfgDirty.exchange(false)) {
        Cfg c; double r;
        { std::lock_guard<std::mutex> lk(m.mu); c = m.cfgSet; r = m.rate; }
        const bool rebuild = !m.built || r != m.curRate || (c.demod == GenWfm) != (m.cfg.demod == GenWfm);
        const bool demodCh = c.demod != m.cfg.demod;
        const bool refilter = demodCh || c.width != m.cfg.width;
        const bool retuned = c.chanOff != m.cfg.chanOff;
        if (rebuild || retuned || demodCh) {   // the open file is the old channel: finish it before the dial or the mode moves
            std::lock_guard<std::mutex> lk(m.wavMu);
            if (m.txFile) m.endTransmission();
            if (!rebuild) { m.preAud.clear(); m.preIq.clear(); }
        }
        m.cfg = c; m.curRate = r;
        m.gLin = std::pow(10.0, c.gainDb / 20.0);
        m.deAl = c.deemphUs > 0 ? (float)(1 - std::exp(-1.0 / (kGeneralAudioRate * c.deemphUs * 1e-6))) : 1.f;
        if (rebuild) m.build();
        else if (refilter) { m.design(); m.prev = cf32(0, 0); m.carInit = false; m.env = 0; m.deY = 0; m.samReset(); }
        if (!rebuild && demodCh) m.amp.reset();
        m.ampConfig();
        if (retuned && !rebuild) { m.acc.assign(kNoiseN, 0.0); m.accN = 0; m.fbuf.clear(); m.noisePsd = -1; m.noisePsdIn = -1; m.lvl = -1; }
    }
    if (m.resetReq.exchange(false) && m.built) m.clearState();
    if (!m.built || n == 0) return;
    const double f = m.sigOff.load() + m.cfg.chanOff;
    if (f != m.mixHz) { m.mixHz = f; m.nco.set(f, m.curRate); }
    m.clean.resize(n);
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        m.clean[i] = m.nco.mix(cf32(std::clamp(re, -2.f, 2.f), std::clamp(im, -2.f, 2.f)));
    }
    m.decd.clear();
    m.dec.process(m.clean.data(), n, m.decd);
    if (m.cfg.nbOn && !m.useAmp()) m.blank(m.decd, m.decRate);   // AM through the processor has its own blanker
    m.noiseFeed(m.decd.data(), m.decd.size());
    m.base.clear();
    m.rs.process(m.decd.data(), m.decd.size(), m.base);
    m.aud.clear(); m.dryAud.clear(); m.iqBlk.clear();
    m.ampOut.clear();
    if (m.useAmp()) m.amp.process(m.base.data(), m.base.size(), m.ampOut);
    const int T = (int)m.taps.size();
    for (const cf32 v : m.base) {
        m.hist[(size_t)m.hpos] = v; m.hist[(size_t)(m.hpos + T)] = v;
        if (++m.hpos >= T) m.hpos = 0;
        const cf32* w = &m.hist[(size_t)m.hpos];
        float ar = 0, ai = 0;
        for (int k = 0; k < T; k++) {
            const cf32 h = m.taps[(size_t)k], s = w[k];
            ar += h.real() * s.real() - h.imag() * s.imag();
            ai += h.real() * s.imag() + h.imag() * s.real();
        }
        const cf32 z(ar, ai);
        m.blkPow += std::norm(z);
        if (++m.blkN >= m.blkLen) m.squelchBlock();
        if (m.iqThis || m.wantIq.load(std::memory_order_relaxed)) m.iqBlk.push_back(z);
        m.demod(z);
    }
    for (float v : m.ampOut) m.emit(v);
    m.play();
    m.pubAcc += n / m.curRate;
    if (m.pubAcc >= 0.1) { m.pubAcc = 0; m.publish(); }
}

bool GeneralReceiver::telemetry(GeneralTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq == lastSeq) return false;
    out = p_->pub;
    return true;
}

void GeneralReceiver::setChannelOffsetHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (p_->cfgSet.chanOff != hz) { p_->cfgSet.chanOff = hz; p_->cfgDirty = true; } }
void GeneralReceiver::setDemod(int d) {
    if (d < 0 || d >= GenDemodCount) return;
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->cfgSet.demod != d) { p_->cfgSet.demod = d; p_->cfgSet.width = generalClampWidth(d, p_->cfgSet.width); p_->cfgDirty = true; }
}
void GeneralReceiver::setWidthHz(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    const double w = generalClampWidth(p_->cfgSet.demod, hz);
    if (p_->cfgSet.width != w) { p_->cfgSet.width = w; p_->cfgDirty = true; }
}
void GeneralReceiver::setSquelch(bool on, bool autoMode, float db) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Cfg& c = p_->cfgSet;
    if (c.sqOn != on || c.sqAuto != autoMode || c.sqDb != db) { c.sqOn = on; c.sqAuto = autoMode; c.sqDb = db; p_->cfgDirty = true; }
}
void GeneralReceiver::setAgc(bool on, float g) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->cfgSet.agc != on || p_->cfgSet.gainDb != g) { p_->cfgSet.agc = on; p_->cfgSet.gainDb = g; p_->cfgDirty = true; }
}
void GeneralReceiver::setDeemphUs(double us) { std::lock_guard<std::mutex> lk(p_->mu); if (p_->cfgSet.deemphUs != us) { p_->cfgSet.deemphUs = std::max(0.0, us); p_->cfgDirty = true; } }
void GeneralReceiver::setCwPitchHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); hz = std::clamp(hz, 200.0, 1500.0); if (p_->cfgSet.pitch != hz) { p_->cfgSet.pitch = hz; p_->cfgDirty = true; } }
void GeneralReceiver::setAmDetector(int det) {
    std::lock_guard<std::mutex> lk(p_->mu);
    det = std::clamp(det, 0, 3);
    if (p_->cfgSet.amDet != det) { p_->cfgSet.amDet = det; p_->cfgDirty = true; }
}
void GeneralReceiver::setAmAuto(bool on, int nr, int bandwidthHz, bool enhance) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Cfg& c = p_->cfgSet;
    if (c.amAuto != on || c.amNr != nr || c.amBw != bandwidthHz || c.amEnhance != enhance) { c.amAuto = on; c.amNr = nr; c.amBw = bandwidthHz; c.amEnhance = enhance; p_->cfgDirty = true; }
}
void GeneralReceiver::setNoiseBlanker(bool on, float thresholdDb) {
    std::lock_guard<std::mutex> lk(p_->mu);
    thresholdDb = std::clamp(thresholdDb, 6.f, 30.f);
    if (p_->cfgSet.nbOn != on || p_->cfgSet.nbDb != thresholdDb) { p_->cfgSet.nbOn = on; p_->cfgSet.nbDb = thresholdDb; p_->cfgDirty = true; }
}
void GeneralReceiver::setVolume(float v) { p_->volume = std::clamp(v, 0.f, 1.f); }
void GeneralReceiver::setNoiseRemover(bool on, float strength) { p_->nrSpk.setStrength(strength); p_->nrDry.setStrength(strength); p_->nrSpk.setEnabled(on); p_->nrDry.setEnabled(on); }
float GeneralReceiver::noiseRemoverVad() const { return p_->nrSpk.enabled() ? p_->nrSpk.vad() : 0.f; }
void GeneralReceiver::setAudioFilters(double hpHz, double lpHz, double notchHz) { p_->afHp = (float)hpHz; p_->afLp = (float)lpHz; p_->afNotch = (float)notchHz; }
void GeneralReceiver::setMuted(bool m) { p_->muted = m; }
void GeneralReceiver::setSilent(bool s) { p_->silent = s; }
void GeneralReceiver::setAudioTap(std::function<void(const float*, size_t)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->tapSet = std::move(cb); p_->tapReq = true; }

bool GeneralReceiver::startRecording(const std::string& path) {
    std::string msg;
    bool ok;
    {
        std::lock_guard<std::mutex> lk(p_->wavMu);
        if (p_->txFile) p_->endTransmission(); else p_->closeWav();
        const bool asFlac = path.size() > 5 && path.compare(path.size() - 5, 5, ".flac") == 0;
        const bool asMp3 = path.size() > 4 && path.compare(path.size() - 4, 4, ".mp3") == 0;
        p_->openFile(path, asFlac, asMp3);
        p_->wavFrames = 0;
        p_->wavPath = path;
        p_->plainGate = false; p_->plainClosedFor = 0;
        ok = p_->wav != nullptr;
        p_->plainRec.store(ok);
        if (ok) { p_->wavErr.clear(); msg = "General receiver: recording audio to " + path; }
        else { p_->wavErr = std::string("cannot write ") + path + ": " + std::strerror(errno); msg = "General receiver: " + p_->wavErr; }
    }
    p_->log(msg);
    return ok;
}

void GeneralReceiver::stopRecording() {
    std::string msg;
    {
        std::lock_guard<std::mutex> lk(p_->wavMu);
        if (!p_->wav) return;
        p_->plainRec.store(false);
        char b[64]; snprintf(b, sizeof b, " (%.1f s)", p_->wavFrames / kGeneralAudioRate);
        msg = "General receiver: recording stopped, " + p_->wavPath + b;
        if (p_->txFile) p_->endTransmission(); else p_->closeWav();
    }
    p_->log(msg);
}

void GeneralReceiver::setTransmissionRecording(bool on, const std::string& dir, const std::string& prefix, double minSec) {
    std::lock_guard<std::mutex> lk(p_->wavMu);
    if (!on && p_->txOn && p_->txFile) p_->endTransmission();
    p_->txOn = on; p_->txDir = dir; p_->txPrefix = prefix; p_->txMinSec = std::max(0.0, minSec);
    p_->wantDry.store(on);
    p_->wantIq.store(on && p_->txIq.load());
    if (on) { p_->txN = 0; p_->txKept = 0; }
}
void GeneralReceiver::setListenHz(double hz) { p_->listenHz.store(std::isfinite(hz) ? hz : 0); }
void GeneralReceiver::setTransmissionGate(double preRollSec, double hangSec, bool iq, bool csv) {
    std::lock_guard<std::mutex> lk(p_->wavMu);
    p_->txPreRoll.store(std::clamp(preRollSec, 0.0, 3.0));
    p_->txHang.store(std::clamp(hangSec, 0.0, 3.0));
    p_->txIq.store(iq);
    p_->txCsv.store(csv);
    p_->wantIq.store(p_->txOn && iq);
}
void GeneralReceiver::setRecordFormat(bool flac, const std::vector<std::string>& tags) {
    std::lock_guard<std::mutex> lk(p_->wavMu);
    p_->recFlac = flac; p_->recTags = tags;
}
void GeneralReceiver::setRecordMp3(bool on) {
    std::lock_guard<std::mutex> lk(p_->wavMu);
    p_->recMp3 = on && Mp3Enc::available();
}
bool GeneralReceiver::mp3Available() { return Mp3Enc::available(); }
int GeneralReceiver::transmissionFiles() const { std::lock_guard<std::mutex> lk(p_->wavMu); return p_->txKept; }

bool GeneralReceiver::recording() const { std::lock_guard<std::mutex> lk(p_->wavMu); return p_->wav != nullptr; }

std::string generalSummary(const GeneralTelemetry& t) {
    char b[160];
    const double w = t.filterHiHz - t.filterLoHz;
    if (t.state == 0) { snprintf(b, sizeof b, "%s %.4g kHz, no signal yet", generalDemodName(t.demod), w / 1e3); return b; }
    if (t.dataValid) snprintf(b, sizeof b, "%s %.4g kHz, %.1f dBFS, SNR %.1f dB, squelch %s%s", generalDemodName(t.demod), w / 1e3, t.levelDb, t.snrDb, t.squelchOpen ? "open" : "closed", t.recording ? ", recording" : "");
    else snprintf(b, sizeof b, "%s %.4g kHz, %.1f dBFS, squelch %s%s", generalDemodName(t.demod), w / 1e3, t.levelDb, t.squelchOpen ? "open" : "closed", t.recording ? ", recording" : "");
    return b;
}

ModeTuning generalTuning() {
    ModeTuning t;
    t.stdMode = 57; t.id = "general"; t.name = "General receiver";
    t.minMhz = 0.1; t.maxMhz = 6000; t.defMhz = 145.500;   // down to long wave: the AM broadcast entry starts on medium wave (an HF / direct-sampling radio)
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 1.6;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 250000;      // the radio sits above the dial: the DC spike stays out of a channel up to WFM width
    return t;
}

} // namespace dect2
