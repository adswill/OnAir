// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Analog FPV video receiver, see fpv_live.h.
//
//   IQ -> spectrum (1024-point FFT, averaged): power of every channel of the plans inside the capture, noise floor
//      -> channel on the centre (within 4 MHz) mixed to baseband -> analog TV picture decoder in FM video mode (picture, sync, standard)
//                                                                -> discriminator -> sound subcarrier (6.0 / 6.5 MHz) -> FM demodulator -> 48 kHz
#include "dect2/fpv/fpv_live.h"
#include "dect2/fpv/fpv_chan.h"
#include "dect2/audio/audioout.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace dect2 {

namespace {

constexpr int kFft = 1024;
constexpr double kWindowHz = 5e6;       // half the width in which the power of a channel is added up
constexpr double kPresentDb = 8.0;      // a channel this far above the noise floor holds a transmitter
constexpr double kTuneWithinHz = 4e6;   // the radio's centre is taken for a channel when it is this close
constexpr double kPublishSec = 0.1;
constexpr double kRefHz = 7.1e6;        // beside the sound subcarriers: the discriminator's noise level there

// an atan2 good to about 1e-7 rad (the polynomial of Abramowitz & Stegun 4.4.49 on [0, 1])
inline float fastAtan2(float y, float x) {
    const float ax = std::fabs(x), ay = std::fabs(y);
    const float mx = std::max(ax, ay), mn = std::min(ax, ay);
    if (mx <= 0.f) return 0.f;
    const float a = mn / mx, s = a * a;
    float r = a * (1.f + s * (-0.3333314528f + s * (0.1999355085f + s * (-0.1420889944f + s * (0.1065626393f + s * (-0.0752896400f + s * (0.0429096138f + s * (-0.0161657367f + s * 0.0028662257f))))))));   // Abramowitz & Stegun 4.4.49, error 2e-8
    if (ay > ax) r = 1.57079632679f - r;
    if (x < 0) r = 3.14159265359f - r;
    return y < 0 ? -r : r;
}

struct Mixer {
    double re = 1, im = 0, dre = 1, dim = 0;
    void set(double hz, double fs) { dre = std::cos(-2 * M_PI * hz / fs); dim = std::sin(-2 * M_PI * hz / fs); re = 1; im = 0; }
    void norm() { const double a = 1.0 / std::sqrt(re * re + im * im); re *= a; im *= a; }
};

// A real stream mixed down by `Mixer` and low-passed and decimated by D (Blackman windowed sinc, the cut-off at `cutHz`)
struct DecFir {
    int n = 0, d = 1, pos = 0, cnt = 0;
    std::vector<float> h;
    std::vector<cf32> buf;
    Mixer mx;
    double sumP = 0;
    uint64_t nP = 0;
    void configure(double fs, double hz, int decim, double cutHz) {
        d = decim; n = 256; pos = 0; cnt = 0;
        h.assign((size_t)n, 0.f);
        buf.assign((size_t)n * 2, cf32(0, 0));
        double s = 0;
        for (int k = 0; k < n; k++) {
            const double t = k - (n - 1) / 2.0, w = 0.42 - 0.5 * std::cos(2 * M_PI * k / (n - 1)) + 0.08 * std::cos(4 * M_PI * k / (n - 1));
            const double x = 2 * M_PI * cutHz / fs * t;
            h[(size_t)k] = (float)((std::fabs(t) < 1e-9 ? 2 * cutHz / fs : std::sin(x) / (M_PI * t)) * w);
            s += h[(size_t)k];
        }
        for (float& v : h) v /= (float)s;
        mx.set(hz, fs);
        sumP = 0; nP = 0;
    }
    // the decimated complex output is appended to `out`
    void process(const float* x, size_t cnN, std::vector<cf32>& out) {
        for (size_t i = 0; i < cnN; i++) {
            const float zr = x[i] * (float)mx.re, zi = x[i] * (float)mx.im;
            const double r = mx.re * mx.dre - mx.im * mx.dim;
            mx.im = mx.re * mx.dim + mx.im * mx.dre; mx.re = r;
            buf[(size_t)pos] = buf[(size_t)pos + (size_t)n] = cf32(zr, zi);
            if (++pos >= n) pos = 0;
            if (++cnt >= d) {
                cnt = 0;
                float ar = 0, ai = 0;
                const cf32* b = &buf[(size_t)pos];
                for (int k = 0; k < n; k++) { ar += h[(size_t)k] * b[k].real(); ai += h[(size_t)k] * b[k].imag(); }
                out.emplace_back(ar, ai);
                sumP += (double)ar * ar + (double)ai * ai; nP++;
            }
        }
        mx.norm();
    }
    void takePower(double& p) { p = nP ? sumP / (double)nP : 0; sumP = 0; nP = 0; }
};

} // namespace

struct FpvReceiver::Impl {
    // ---- shared with the interface thread
    std::mutex mu;
    FpvTelemetry pub;
    std::function<void(const std::string&)> logCb;
    std::atomic<double> centre{0};
    std::atomic<bool> resetReq{false}, clearReq{false};
    std::atomic<bool> audioOn{false}, muted{false}, silent{false};
    std::atomic<double> subWanted{0};
    std::atomic<float> volume{1.f};
    std::mutex tapMu;
    std::function<void(const float*, size_t)> tap;

    // ---- the analysis thread
    AtvReceiver atv;
    double fs = 0;
    bool configured = false;
    std::vector<FpvChannel> chans;
    struct Meas { float level = -200, over = 0; double t = -1; bool present = false; };
    std::vector<Meas> meas;
    Fft fft{kFft};
    std::vector<float> win;
    double winSum = 0, winSq = 0;
    std::vector<double> acc;
    int accN = 0;
    std::vector<cf32> seg;
    int segFill = 0;
    double skipLeft = 0, segEvery = 0;
    double settleLeft = 0;
    double lastCentre = -1;
    double floorBin = 0;      // noise floor per bin (power), tracked: down at once, up slowly
    bool floorValid = false;
    uint64_t nIn = 0, nextPub = 0;
    std::vector<float> specOut;
    // the tuned channel
    double tunedHz = 0;
    int tunedIdx = -1;
    Mixer lo;
    double loOffset = 0;
    std::vector<cf32> shifted;
    // video
    AtvTelemetry video;
    uint64_t videoSeq = 0;
    // sound
    DecFir mx[4];             // 6.0, 6.5, reference, and the one that is demodulated
    int audioD = 1;
    double audioRate = 0;
    ExactResampler rs;
    std::vector<float> disc, mono;
    std::vector<cf32> y, yr, aud;
    cf32 prevY{1, 0};
    float prevIq[2] = {1, 0};
    float dcAcc = 0;
    double audioPow[3] = {0, 0, 0};
    float subDb[2] = {-120, -120};
    double subUsed = 0;
    float audioLevelDb = -120;
    bool audioPresent = false;
    std::unique_ptr<AudioOut> audio;
    std::vector<float> inter;
    float hist[kFpvRssiHist] = {};
    std::vector<float> rssiHist;
    uint64_t seqOut = 0;
    uint64_t nextHist = 0;

    Impl() {
        chans = fpvAllChannels();
        meas.assign(chans.size(), Meas());
        win.resize(kFft);
        for (int i = 0; i < kFft; i++) { win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / kFft)); winSum += win[(size_t)i]; winSq += (double)win[(size_t)i] * win[(size_t)i]; }
        acc.assign(kFft, 0.0);
        seg.assign(kFft, cf32(0, 0));
        atv.setSilent(true);
        atv.setModulation(1);
    }

    void log(const std::string& s) { std::function<void(const std::string&)> cb; { std::lock_guard<std::mutex> lk(mu); cb = logCb; } if (cb) cb(s); }

    void configure(double rate) {
        fs = rate;
        atv.configure(rate);
        configured = true;
        segEvery = std::max(1.0, rate / 400.0);
        nIn = 0; nextPub = (uint64_t)(kPublishSec * rate);
        nextHist = (uint64_t)rate;
        floorValid = false;
        resetSignal();
        audioD = std::max(1, (int)std::lround(rate / 250e3));
        audioRate = rate / audioD;
        mx[0].configure(rate, 6.0e6, audioD, 70e3);
        mx[1].configure(rate, 6.5e6, audioD, 70e3);
        mx[2].configure(rate, kRefHz, audioD, 70e3);
        mx[3].configure(rate, 6.5e6, audioD, 70e3);
        subUsed = 6.5e6;
        rs.configure(audioRate, 48000.0);
        std::lock_guard<std::mutex> lk(mu);
        pub = FpvTelemetry();
    }

    void resetSignal() {
        atv.reset();
        std::fill(acc.begin(), acc.end(), 0.0); accN = 0; segFill = 0; skipLeft = 0;
        settleLeft = 0.15 * fs;
        prevY = cf32(1, 0);
        rs.reset();
        dcAcc = 0;
        video = AtvTelemetry();
        if (audio) audio->flush();
    }

    bool ready() const { return configured && fs >= fpvTuning().minSampleRate - 1; }

    // ---------------------------------------------------------------- the spectrum
    void collect(const cf32* x, size_t n) {
        size_t i = 0;
        while (i < n) {
            if (settleLeft > 0) { const size_t k = std::min(n - i, (size_t)std::ceil(settleLeft)); settleLeft -= (double)k; i += k; continue; }
            if (skipLeft >= 1.0) { const size_t k = std::min(n - i, (size_t)skipLeft); skipLeft -= (double)k; i += k; continue; }
            const size_t k = std::min(n - i, (size_t)(kFft - segFill));
            std::copy(x + i, x + i + k, seg.begin() + segFill);
            segFill += (int)k; i += k;
            if (segFill >= kFft) {
                std::vector<cf32> t(seg);
                for (int j = 0; j < kFft; j++) t[(size_t)j] *= win[(size_t)j];
                fft.forward(t.data());
                for (int j = 0; j < kFft; j++) acc[(size_t)j] += std::norm(t[(size_t)j]);
                accN++;
                segFill = 0;
                skipLeft += segEvery - kFft;
                if (skipLeft < 0) skipLeft = 0;
            }
        }
    }

    // ---------------------------------------------------------------- the mixer and the sound
    void feedVideo(const cf32* x, size_t n, double off) {
        if (off == 0.0) { atv.feed(x, n); return; }
        shifted.resize(n);
        double re = lo.re, im = lo.im;
        const double dre = std::cos(-2 * M_PI * off / fs), dim = std::sin(-2 * M_PI * off / fs);
        for (size_t i = 0; i < n; i++) {
            shifted[i] = cf32(x[i].real() * (float)re - x[i].imag() * (float)im, x[i].real() * (float)im + x[i].imag() * (float)re);
            const double r = re * dre - im * dim;
            im = re * dim + im * dre; re = r;
        }
        const double a = 1.0 / std::sqrt(re * re + im * im);
        lo.re = re * a; lo.im = im * a;
        atv.feed(shifted.data(), n);
    }

    void feedAudio(const cf32* x, size_t n) {
        disc.resize(n);
        float pr = prevIq[0], pi = prevIq[1];
        for (size_t i = 0; i < n; i++) {
            const float xr = x[i].real(), xi = x[i].imag();
            // x[i] * conj(x[i-1])
            disc[i] = fastAtan2(xi * pr - xr * pi, xr * pr + xi * pi);
            pr = xr; pi = xi;
        }
        prevIq[0] = pr; prevIq[1] = pi;
        std::vector<cf32> sink;
        for (int m = 0; m < 3; m++) { sink.clear(); mx[m].process(disc.data(), n, sink); }
        y.clear();
        mx[3].process(disc.data(), n, y);
        if (y.empty()) return;
        // FM demodulation of the chosen subcarrier: Hz of deviation, scaled so that 50 kHz is full scale
        yr.resize(y.size());
        const float k = (float)(audioRate / (2 * M_PI) / 50e3);
        for (size_t i = 0; i < y.size(); i++) {
            const float v = fastAtan2(y[i].imag() * prevY.real() - y[i].real() * prevY.imag(), y[i].real() * prevY.real() + y[i].imag() * prevY.imag()) * k;
            prevY = y[i];
            yr[i] = cf32(v, 0.f);
        }
        aud.clear();
        rs.process(yr.data(), yr.size(), aud);
        mono.resize(aud.size());
        double e = 0;
        for (size_t i = 0; i < aud.size(); i++) {
            dcAcc += 0.002f * (aud[i].real() - dcAcc);
            float v = aud[i].real() - dcAcc;
            v = std::max(-1.f, std::min(1.f, v));
            mono[i] = audioPresent ? v : 0.f;
            e += (double)v * v;
        }
        if (!aud.empty()) {
            const double r = std::sqrt(e / (double)aud.size());
            audioLevelDb = (float)(20 * std::log10(std::max(r, 1e-6)));
        }
        emit();
    }

    void emit() {
        if (mono.empty()) return;
        {
            std::lock_guard<std::mutex> lk(tapMu);
            if (tap) tap(mono.data(), mono.size());
        }
        if (silent.load()) return;
        if (!audio) {
            audio = std::make_unique<AudioOut>();
            audio->start(48000);
            audio->setStartThreshold(48000 / 5);
        }
        audio->setVolume(volume.load()); audio->setMuted(muted.load());
        const size_t nn = mono.size();
        inter.resize(nn * 2 + 2);
        for (size_t i = 0; i < nn; i++) { inter[2 * i] = mono[i]; inter[2 * i + 1] = mono[i]; }
        int frames = (int)nn;
        if (audio->playing()) {   // keep the queue near 0.2 s against the clock difference of radio and sound card
            const int queued = audio->bufferedFrames();
            if (queued > 14400 && frames > 1) frames--;
            else if (queued < 4800 && frames > 0) { inter[2 * (size_t)frames] = inter[2 * (size_t)frames - 2]; inter[2 * (size_t)frames + 1] = inter[2 * (size_t)frames - 1]; frames++; }
        }
        audio->write(inter.data(), frames);
    }

    // ---------------------------------------------------------------- the report
    void publish() {
        const double nowSec = (double)nIn / fs;
        const double c = lastCentre;
        // the spectrum of the last interval
        std::vector<float> bins(kFft);
        const bool fresh = accN >= 8;
        if (fresh) {
            const double norm = 1.0 / ((double)accN * winSum * winSum);
            for (int j = 0; j < kFft; j++) bins[(size_t)j] = (float)(acc[(size_t)j] * norm);
            std::fill(acc.begin(), acc.end(), 0.0); accN = 0;
            // the noise floor per bin: a low percentile of the central bins; it falls at once and rises by 0.2 dB a second
            std::vector<float> mid;
            const int half = (int)(0.4 * kFft);
            for (int j = -half; j < half; j++) mid.push_back(bins[(size_t)((j + kFft) % kFft)]);
            std::nth_element(mid.begin(), mid.begin() + (long)(mid.size() * 0.15), mid.end());
            const double est = std::max(1e-14, (double)mid[(size_t)(mid.size() * 0.15)] * 1.15);
            if (!floorValid || est < floorBin) floorBin = est;
            else floorBin = std::min(est, floorBin * std::pow(10.0, 0.2 * kPublishSec / 10.0));
            floorValid = true;
            // 256 points for the screen, dB
            specOut.assign(256, -140.f);
            for (int j = 0; j < 256; j++) {
                double s = 0;
                for (int q = 0; q < 4; q++) s += bins[(size_t)(((j * 4 + q) - 512 + kFft) % kFft)];
                specOut[(size_t)j] = (float)(10 * std::log10(std::max(1e-14, s / 4)));
            }
        }
        const double binHz = fs / kFft;
        const double floorWin = floorBin * (2 * kWindowHz / binHz) / 1.5;   // the noise in the window of a channel
        const float floorDb = (float)(10 * std::log10(std::max(1e-14, floorWin)));
        std::vector<FpvChannelInfo> info(chans.size());
        double tunedCfo = 0;
        for (size_t i = 0; i < chans.size(); i++) {
            Meas& m = meas[i];
            const double off = chans[i].freqHz - c;
            const bool inCap = c > 0 && std::fabs(off) <= fs / 2 - kWindowHz;
            if (fresh && inCap) {
                double p = 0, pf = 0;
                for (int j = 0; j < kFft; j++) {
                    const double f = (j < kFft / 2 ? j : j - kFft) * binHz;
                    if (std::fabs(f - off) <= kWindowHz) { p += bins[(size_t)j]; pf += bins[(size_t)j] * (f - off); }
                }
                p /= 1.5;
                m.level = (float)(10 * std::log10(std::max(1e-14, p)));
                m.over = m.level - floorDb;
                m.present = m.over >= kPresentDb;
                m.t = nowSec;
                if ((int)i == tunedIdx && p > 0) tunedCfo = pf / (p * 1.5);
            }
            FpvChannelInfo& f = info[i];
            f.name = chans[i].name; f.band = chans[i].band; f.ch = chans[i].ch; f.freqHz = chans[i].freqHz;
            f.levelDb = m.level; f.overDb = m.over; f.present = m.present && m.t >= 0; f.inCapture = inCap;
            f.ageSec = m.t < 0 ? -1 : nowSec - m.t;
        }
        // the audio subcarriers
        if (audioOn.load()) {
            for (int m = 0; m < 3; m++) mx[m].takePower(audioPow[m]);
            const double ref = std::max(1e-18, audioPow[2]);
            for (int m = 0; m < 2; m++) {
                const double f = m == 0 ? 6.0e6 : 6.5e6;
                subDb[m] = (float)(10 * std::log10(std::max(1e-18, audioPow[m]) / (ref * (f / kRefHz) * (f / kRefHz))));
            }
            const double want = subWanted.load();
            double use = want > 0 ? want : (subDb[0] > subDb[1] ? 6.0e6 : 6.5e6);
            if (use != subUsed) { mx[3].configure(fs, use, audioD, 70e3); subUsed = use; prevY = cf32(1, 0); }
            const int pick = want > 0 ? -1 : (subDb[0] > subDb[1] ? 0 : 1);
            audioPresent = want > 0 ? true : subDb[pick] >= 6.0f;
        } else {
            audioPresent = false; subDb[0] = subDb[1] = -120;
        }

        AtvTelemetry v;
        uint64_t s = videoSeq;
        if (atv.telemetry(v, s)) { videoSeq = v.seq; video = v; }
        FpvTelemetry t;
        t.seq = ++seqOut;
        t.inputRate = fs; t.centreHz = c; t.tunedHz = tunedHz;
        t.tunedName = tunedIdx >= 0 ? chans[(size_t)tunedIdx].name : "";
        t.noiseFloorDb = floorDb;
        t.channels = std::move(info);
        t.video = video;
        t.overDb = 0; t.levelDb = -200;
        if (tunedIdx >= 0 && meas[(size_t)tunedIdx].t >= 0) { t.levelDb = meas[(size_t)tunedIdx].level; t.overDb = meas[(size_t)tunedIdx].over; }
        else if (fresh) {   // the centre is not on a channel: the whole band within +-5 MHz of the centre
            double p = 0;
            for (int j = 0; j < kFft; j++) { const double f = (j < kFft / 2 ? j : j - kFft) * binHz; if (std::fabs(f) <= kWindowHz) p += bins[(size_t)j]; }
            t.levelDb = (float)(10 * std::log10(std::max(1e-14, p / 1.5)));
            t.overDb = t.levelDb - floorDb;
        }
        t.cfoHz = tunedCfo;
        t.snrDb = video.snrDb;
        const bool carrier = t.overDb >= kPresentDb;
        t.state = video.state == 2 ? 2 : (video.state == 1 || carrier) ? 1 : 0;
        t.dataValid = video.state == 2 && video.dataValid;
        t.blocksOk = video.blocksOk; t.blocksBad = video.blocksBad;
        t.audioOn = audioOn.load();
        t.audioPresent = audioPresent;
        t.audioSubHz = subUsed;
        t.audioLevelDb = audioPresent ? audioLevelDb : -120;
        t.audioSubDb[0] = subDb[0]; t.audioSubDb[1] = subDb[1];
        t.spec = specOut; t.specSpanMhz = (float)(fs / 1e6);
        if (nIn >= nextHist) { nextHist += (uint64_t)fs; rssiHist.push_back(t.levelDb); if (rssiHist.size() > kFpvRssiHist) rssiHist.erase(rssiHist.begin()); }
        t.rssiHist = rssiHist;
        std::lock_guard<std::mutex> lk(mu);
        pub = std::move(t);
    }

    void feed(const cf32* x, size_t n) {
        if (!ready() || n == 0) return;
        if (clearReq.exchange(false)) { for (auto& m : meas) m = Meas(); floorValid = false; rssiHist.clear(); }
        if (resetReq.exchange(false)) resetSignal();
        const double c = centre.load();
        if (c != lastCentre) {
            lastCentre = c;
            resetSignal();
            tunedIdx = c > 0 ? fpvNearest(chans, c, kTuneWithinHz) : -1;
            tunedHz = tunedIdx >= 0 ? chans[(size_t)tunedIdx].freqHz : c;
            loOffset = tunedIdx >= 0 ? chans[(size_t)tunedIdx].freqHz - c : 0.0;
            lo.re = 1; lo.im = 0;
        }
        const size_t kMax = 16384;
        for (size_t done = 0; done < n;) {
            const size_t k = std::min(kMax, n - done);
            const cf32* p = x + done;
            collect(p, k);
            feedVideo(p, k, loOffset);
            if (audioOn.load()) {
                const cf32* q = p;
                if (loOffset != 0.0) q = shifted.data();
                feedAudio(q, k);
            }
            done += k;
            nIn += k;
            if (nIn >= nextPub) { nextPub = nIn + (uint64_t)(kPublishSec * fs); publish(); }
        }
    }
};

FpvReceiver::FpvReceiver() : p_(new Impl) {}
FpvReceiver::~FpvReceiver() = default;
void FpvReceiver::configure(double inputRateHz) { p_->configure(inputRateHz); }
void FpvReceiver::setSignalOffset(double) {}
void FpvReceiver::setCenterHz(double hz) { p_->centre = hz; }
bool FpvReceiver::ready() const { return p_->ready(); }
void FpvReceiver::reset() { p_->resetReq = true; }
void FpvReceiver::clearChannels() { p_->clearReq = true; }
void FpvReceiver::feed(const cf32* x, size_t n) { p_->feed(x, n); }
bool FpvReceiver::telemetry(FpvTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
std::shared_ptr<const AtvFrame> FpvReceiver::frame(uint64_t& lastSeq) { return p_->atv.frame(lastSeq); }
void FpvReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    { std::lock_guard<std::mutex> lk(p_->mu); p_->logCb = cb; }
    p_->atv.setLogCallback(std::move(cb));
}
void FpvReceiver::setDeinterlace(int mode) { p_->atv.setDeinterlace(mode); }
void FpvReceiver::setStandard(int system, int colour) { p_->atv.setStandard(system, colour); }
void FpvReceiver::setColour(bool on) { p_->atv.setColour(on); }
void FpvReceiver::setSaturation(float s) { p_->atv.setSaturation(s); }
void FpvReceiver::setAudio(bool on) { p_->audioOn = on; }
void FpvReceiver::setAudioSubcarrier(double hz) { p_->subWanted = hz; }
void FpvReceiver::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); }
void FpvReceiver::setMuted(bool m) { p_->muted = m; }
void FpvReceiver::setSilent(bool s) { p_->silent = s; }
void FpvReceiver::setAudioTap(std::function<void(const float*, size_t)> cb) { std::lock_guard<std::mutex> lk(p_->tapMu); p_->tap = std::move(cb); }

ModeTuning fpvTuning() {
    ModeTuning t;
    t.stdMode = 103; t.id = "fpv"; t.name = "FPV video (analog)";
    t.minMhz = 900; t.maxMhz = 6000; t.defMhz = 5806;   // Raceband 5
    t.sampleRate = 20e6; t.basebandHz = 18e6; t.bandwidthMhz = 18;
    t.minSampleRate = 16e6;
    t.tuneOffsetHz = 0;
    t.carrierAtCentre = true;   // the video carrier sits on the channel centre
    return t;
}

} // namespace dect2
