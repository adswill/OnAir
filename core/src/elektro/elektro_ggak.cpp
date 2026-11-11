// Elektro-L GGAK demodulator and test generator (elektro_ggak.h).
#include "dect2/elektro/elektro_ggak.h"
#include "dect2/dsp/rrc.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/results/results_io.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr size_t kHist = 120, kLogMax = 80, kFramesMax = 80;
constexpr int kAcqFft = 4096, kAcqBlocks = 8;

using dect2::dsp::rrc;

struct Fir {
    std::vector<float> h;
    std::vector<cf32> buf;
    size_t pos = 0;
    void set(std::vector<float> taps) { h = std::move(taps); buf.assign(2 * h.size(), cf32(0, 0)); pos = 0; }
    void push(cf32 x) { buf[pos] = x; buf[pos + h.size()] = x; pos = (pos + 1) % h.size(); }
    cf32 out() const {
        const cf32* b = buf.data() + pos;
        float re = 0, im = 0;
        const size_t n = h.size();
        for (size_t i = 0; i < n; i++) { re += h[n - 1 - i] * b[i].real(); im += h[n - 1 - i] * b[i].imag(); }
        return cf32(re, im);
    }
    void clear() { std::fill(buf.begin(), buf.end(), cf32(0, 0)); pos = 0; }
};

inline cf32 cubic(const cf32* y, double t) {
    const int i = (int)std::floor(t);
    const float f = (float)(t - i);
    const cf32 p0 = y[i - 1], p1 = y[i], p2 = y[i + 1], p3 = y[i + 2];
    const cf32 a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
    const cf32 b = p0 - 2.5f * p1 + 2.f * p2 - 0.5f * p3;
    const cf32 c = -0.5f * p0 + 0.5f * p2;
    return ((a * f + b) * f + c) * f + p1;
}

void loopGains(double bnT, double zeta, double& alpha, double& beta) {
    const double th = bnT / (zeta + 1 / (4 * zeta)), d = 1 + 2 * zeta * th + th * th;
    alpha = 4 * zeta * th / d;
    beta = 4 * th * th / d;
}

int16_t be16(const uint8_t* p) { return (int16_t)((uint16_t)p[0] << 8 | p[1]); }
} // namespace

uint16_t ggakCrc16(const uint8_t* d, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++) c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x8005 : c << 1);
    }
    return c;
}
void ggakPutCrc(uint8_t frame[kGgakFrameBytes]) {
    const uint16_t c = ggakCrc16(frame + 4, (size_t)(kGgakFrameBytes - 6));
    frame[kGgakFrameBytes - 2] = (uint8_t)(c >> 8);
    frame[kGgakFrameBytes - 1] = (uint8_t)c;
}
bool ggakCrcOk(const uint8_t frame[kGgakFrameBytes]) {
    const uint16_t got = (uint16_t)((uint16_t)frame[kGgakFrameBytes - 2] << 8 | frame[kGgakFrameBytes - 1]);
    return got == ggakCrc16(frame + 4, (size_t)(kGgakFrameBytes - 6));
}

void ggakParse(GgakFrame& f) {
    f.marker = f.raw[4] >> 4;
    f.sub = f.raw[4] & 0x0F;
    f.timestamp = 0;
    for (int i = 0; i < 8; i++) f.timestamp = (f.timestamp << 8) | f.raw[5 + i];
    f.crcOk = ggakCrcOk(f.raw);
    const uint8_t* p = f.raw + 13;
    f.bx = (float)be16(p);
    f.by = (float)be16(p + 2);
    f.bz = (float)be16(p + 4);
    const float m = std::sqrt(f.bx * f.bx + f.by * f.by + f.bz * f.bz);
    f.magOk = m > 10.f && m < 65000.f;
}

void ggakBuildTestFrame(uint8_t out[kGgakFrameBytes], int marker, int sub, uint64_t timestamp, int16_t bx, int16_t by, int16_t bz) {
    std::memset(out, 0xAA, kGgakFrameBytes);
    out[0] = (uint8_t)(kGgakAsm >> 24);
    out[1] = (uint8_t)(kGgakAsm >> 16);
    out[2] = (uint8_t)(kGgakAsm >> 8);
    out[3] = (uint8_t)kGgakAsm;
    out[4] = (uint8_t)(((marker & 15) << 4) | (sub & 15));
    for (int i = 7; i >= 0; i--) out[5 + (7 - i)] = (uint8_t)(timestamp >> (8 * i));
    auto put = [&](int o, int16_t v) { out[o] = (uint8_t)((uint16_t)v >> 8); out[o + 1] = (uint8_t)v; };
    for (int k = 0; k < 13; k++) {
        put(13 + k * 6, bx);
        put(15 + k * 6, by);
        put(17 + k * 6, bz);
    }
    ggakPutCrc(out);
}

struct GgakReceiver::Impl {
    std::mutex mu;
    GgakTelemetry pub;
    std::function<void(const std::string&)> logCb;
    std::atomic<bool> resetReq{false}, clearReq{false};
    std::atomic<double> offset{0};
    double rateSet = 0, symbolRate = kGgakSymbolRate;
    double rate = 0, fs2 = 0, sps = 0;
    int D = 1, decPhase = 0;
    Fir lpf, mf;
    double appliedOffset = 0, cfo = 0;
    std::complex<double> nco{1, 0}, rot{1, 0};
    int ncoCount = 0;
    std::vector<cf32> y;
    double pos = 0, spsEst = 0, tAlpha = 0, tBeta = 0;
    cf32 yprev{0, 0};
    float agc = 1;
    double cPhase = 0, cFreq = 0, cAlpha = 0, cBeta = 0;
    float lockM = 0, meanAbs = 0.5f, meanIm2 = 0.1f;
    bool acq = true, carrierLock = false, frameLock = false;
    std::vector<cf32> acqBuf;
    std::vector<float> acqSpec;
    int acqBlocks = 0;
    int64_t sinceAcq = 0, lowLock = 0, nIn = 0;
    std::unique_ptr<Fft> fft;
    uint32_t bitReg = 0;
    int collected = 0, fillBits = 0, polarity = 0;   // 0 unknown, 1 normal, -1 inverted
    uint8_t frame[kGgakFrameBytes]{};
    uint8_t last[kGgakFrameBytes]{};
    bool haveLast = false;
    uint64_t ok = 0, bad = 0, dup = 0;
    double nextReport = 0, levelAcc = 0;
    int64_t levelN = 0;
    std::deque<cf32> constel;
    std::deque<float> snrH, bxH, byH, bzH;
    std::deque<GgakFrame> frames;
    std::deque<std::string> logLines;
    uint64_t seq = 0;
    bool carrierReported = false, frameReported = false;

    double minInputRate() const { return std::max(20000.0, 2.1 * symbolRate); }
    double carrierHz() const { return cfo + cFreq / (2 * kPi) * symbolRate; }

    void addLog(const std::string& s, bool engine) {
        logLines.push_front(s);
        while (logLines.size() > kLogMax) logLines.pop_back();
        if (engine) {
            std::function<void(const std::string&)> cb;
            { std::lock_guard<std::mutex> lk(mu); cb = logCb; }
            if (cb) cb(s);
        }
    }

    void setup(double r) {
        rate = r;
        // about 4.8 samples per symbol after the decimation: the squared signal's line (twice the carrier offset) stays inside the
        // acquisition FFT for offsets up to +-fs2 / 4 (about +-6 kHz), and the channel filter passes the signal 6 kHz off before the
        // offset is known (2.3 samples per symbol gave +-2.9 kHz and cut an offset signal's edge)
        D = std::max(1, (int)std::floor(r / (4.8 * symbolRate)));
        fs2 = r / D;
        sps = fs2 / symbolRate;
        if (D > 1) {
            const int n = 8 * D + 1;
            std::vector<float> h((size_t)n);
            const double fc = 0.4 * fs2 / r;
            double sum = 0;
            for (int i = 0; i < n; i++) {
                const double m = i - (n - 1) / 2.0, w = 0.54 - 0.46 * std::cos(2 * kPi * i / (n - 1));
                h[(size_t)i] = (float)(w * (std::fabs(m) < 1e-9 ? 2 * fc : std::sin(2 * kPi * fc * m) / (kPi * m)));
                sum += h[(size_t)i];
            }
            for (auto& v : h) v = (float)(v / sum);
            lpf.set(h);
        }
        const int half = (int)std::ceil(6 * sps);
        std::vector<float> h((size_t)(2 * half + 1));
        double e = 0;
        for (int i = -half; i <= half; i++) { h[(size_t)(i + half)] = (float)rrc(i / sps, 0.5); e += h[(size_t)(i + half)] * h[(size_t)(i + half)]; }
        for (auto& v : h) v = (float)(v / std::sqrt(e));
        mf.set(h);
        fft = std::make_unique<Fft>(kAcqFft);
        loopGains(0.004, 0.707, tAlpha, tBeta);
        resetState();
    }

    void setRot() {
        const double w = -2 * kPi * (appliedOffset + cfo) / rate;
        rot = std::complex<double>(std::cos(w), std::sin(w));
    }
    void resetCarrier() { cPhase = 0; cFreq = 0; lockM = 0; double a, b; loopGains(0.01, 0.707, a, b); cAlpha = a; cBeta = b; }
    void resetBits() { bitReg = 0; collected = 0; fillBits = 0; polarity = 0; frameLock = false; }
    void resetState() {
        cfo = 0; appliedOffset = offset.load(); setRot(); nco = 1;
        lpf.clear(); mf.clear(); decPhase = 0;
        y.clear(); pos = 2 + sps; spsEst = sps; yprev = 0; agc = 1;
        resetCarrier();
        acq = true; carrierLock = false; acqBuf.clear(); acqSpec.assign(kAcqFft, 0.f); acqBlocks = 0; sinceAcq = 0; lowLock = 0;
        meanAbs = 0.5f; meanIm2 = 0.1f;
        resetBits();
        haveLast = false;
        carrierReported = frameReported = false;
    }

    void acquire(cf32 v) {
        acqBuf.push_back(v * v);
        if ((int)acqBuf.size() < kAcqFft) return;
        fft->forward(acqBuf.data());
        for (int i = 0; i < kAcqFft; i++) acqSpec[(size_t)i] += std::norm(acqBuf[(size_t)i]);
        acqBuf.clear();
        if (++acqBlocks < kAcqBlocks) return;
        const double binHz = fs2 / kAcqFft / 2;
        int best = -1;
        float bv = 0;
        double mean = 0;
        for (int i = 0; i < kAcqFft; i++) {
            mean += acqSpec[(size_t)i];
            if (acqSpec[(size_t)i] > bv) { bv = acqSpec[(size_t)i]; best = i; }
        }
        mean /= kAcqFft;
        if (best >= 0 && bv > 12 * mean) {
            const double k = (best < kAcqFft / 2 ? best : best - kAcqFft);
            cfo += k * binHz;
            setRot();
            resetCarrier();
            acq = false; sinceAcq = 0;
        }
        std::fill(acqSpec.begin(), acqSpec.end(), 0.f);
        acqBlocks = 0;
    }

    void acceptFrame(const uint8_t* bytes) {
        GgakFrame f;
        std::memcpy(f.raw, bytes, kGgakFrameBytes);
        f.timeSec = nIn / std::max(1.0, rate);
        ggakParse(f);
        if (haveLast && std::memcmp(last, bytes, kGgakFrameBytes) == 0) {
            f.duplicate = true;
            dup++;
            return;   // FEC-ski: the identical second copy
        }
        std::memcpy(last, bytes, kGgakFrameBytes);
        haveLast = true;
        if (f.crcOk) ok++; else bad++;
        if (f.crcOk && f.magOk) {
            auto push = [](std::deque<float>& h, float v) { h.push_back(v); if (h.size() > kHist) h.pop_front(); };
            push(bxH, f.bx); push(byH, f.by); push(bzH, f.bz);
        }
        frames.push_front(std::move(f));
        while (frames.size() > kFramesMax) frames.pop_back();
        if (!frameReported) {
            frameReported = true;
            char b[96];
            snprintf(b, sizeof b, "GGAK: frames locked (marker %d)", frames.front().marker);
            addLog(b, true);
        }
        frameLock = true;
    }

    void bit(int hard) {
        bitReg = (bitReg << 1) | (uint32_t)(hard & 1);
        if (collected == 0) {
            const int e = __builtin_popcount(bitReg ^ kGgakAsm);
            const int ei = __builtin_popcount(bitReg ^ ~kGgakAsm);
            if (e <= 2 || ei <= 2) {
                polarity = e <= 2 ? 1 : -1;
                collected = 4;
                fillBits = 0;
                std::memset(frame, 0, sizeof frame);
                frame[0] = (uint8_t)(kGgakAsm >> 24);
                frame[1] = (uint8_t)(kGgakAsm >> 16);
                frame[2] = (uint8_t)(kGgakAsm >> 8);
                frame[3] = (uint8_t)kGgakAsm;
            }
            return;
        }
        const int b = polarity < 0 ? (hard ^ 1) : hard;
        frame[collected] = (uint8_t)((frame[collected] << 1) | (b & 1));
        if (++fillBits >= 8) {
            fillBits = 0;
            collected++;
            if (collected >= kGgakFrameBytes) {
                acceptFrame(frame);
                collected = 0;
                polarity = 0;
            }
        }
    }

    void symbol(cf32 v) {
        const cf32 z = v * cf32((float)std::cos(-cPhase), (float)std::sin(-cPhase));
        const float re = z.real(), im = z.imag();
        const float e = (re >= 0 ? im : -im);
        cPhase += cAlpha * e + cFreq;
        cFreq += cBeta * e;
        if (cPhase > kPi) cPhase -= 2 * kPi; else if (cPhase < -kPi) cPhase += 2 * kPi;
        const float p = re * re + im * im + 1e-12f;
        lockM += 0.002f * ((re * re - im * im) / p - lockM);
        meanAbs += 0.002f * (std::fabs(re) - meanAbs);
        meanIm2 += 0.002f * (im * im - meanIm2);
        constel.push_back(z);
        if (constel.size() > 256) constel.pop_front();
        if (!acq) {
            sinceAcq++;
            if (sinceAcq == 20000) { double a, b; loopGains(0.004, 0.707, a, b); cAlpha = a; cBeta = b; }
            if (sinceAcq > 8000) {
                const bool l = lockM > 0.35f;
                if (l != carrierLock) {
                    carrierLock = l;
                    if (l && !carrierReported) { carrierReported = true; char b[96]; snprintf(b, sizeof b, "GGAK: carrier locked, offset %.1f kHz", carrierHz() / 1000); addLog(b, true); }
                }
                lowLock = lockM < 0.2f ? lowLock + 1 : 0;
                if (lowLock > 40000) { acq = true; carrierLock = false; lowLock = 0; resetBits(); }
            }
        }
        bit(re < 0 ? 1 : 0);
    }

    void timing() {
        if ((int)y.size() < 8) return;
        while (pos + 3 < (double)y.size() - 1) {
            const cf32 a = cubic(y.data(), pos);
            const cf32 b = cubic(y.data(), pos - spsEst / 2);
            const float err = (yprev.real() - a.real()) * b.real() + (yprev.imag() - a.imag()) * b.imag();
            yprev = a;
            const float p = std::norm(a);
            agc += 0.01f * (1.f - agc * agc * p);
            agc = std::max(0.1f, std::min(agc, 20.f));
            spsEst += tBeta * err;
            spsEst = std::max(sps * 0.995, std::min(sps * 1.005, spsEst));
            pos += spsEst + tAlpha * err * sps;
            symbol(a * agc);
        }
        const int drop = (int)std::floor(pos - spsEst - 4);
        if (drop > 0 && drop <= (int)y.size()) { y.erase(y.begin(), y.begin() + drop); pos -= drop; }
    }

    void feed(const cf32* x, size_t n) {
        y.reserve(y.size() + n / D + 8);
        const double off = offset.load();
        if (off != appliedOffset) { appliedOffset = off; setRot(); }
        for (size_t i = 0; i < n; i++) {
            nco *= rot;
            if (++ncoCount >= 1024) { nco /= std::abs(nco); ncoCount = 0; }
            const cf32 v = x[i] * cf32((float)nco.real(), (float)nco.imag());
            levelAcc += std::norm(x[i]); levelN++;
            cf32 d = v;
            if (D > 1) {
                lpf.push(v);
                if (++decPhase < D) continue;
                decPhase = 0;
                d = lpf.out();
            }
            mf.push(d);
            const cf32 o = mf.out();
            y.push_back(o);
            if (acq) acquire(d);   // before the matched filter: it would cut a signal that is still off by up to 6 kHz
        }
        timing();
    }

    void report() {
        GgakTelemetry t;
        t.state = frameLock ? 2 : carrierLock ? 1 : 0;
        t.cfoHz = carrierHz();
        const float n2 = std::max(1e-9f, 2 * meanIm2);
        t.snrDb = (float)(10 * std::log10(std::max(1e-6f, meanAbs * meanAbs / n2)));
        t.levelDb = levelN ? (float)(10 * std::log10(std::max(1e-20, levelAcc / levelN))) : -200.f;
        levelAcc = 0; levelN = 0;
        t.dataValid = t.state >= 2;
        t.blocksOk = ok; t.blocksBad = bad;
        t.timeSec = nIn / std::max(1.0, rate);
        t.inputRate = rate;
        t.symbolRate = fs2 / std::max(1e-9, spsEst);
        t.carrierLock = carrierLock || frameLock;
        t.frameLock = frameLock;
        t.lockMetric = std::max(0.f, lockM);
        t.constellation.assign(constel.begin(), constel.end());
        auto push = [](std::deque<float>& h, float v) { h.push_back(v); if (h.size() > kHist) h.pop_front(); };
        push(snrH, t.snrDb);
        t.snrHist.assign(snrH.begin(), snrH.end());
        t.bxHist.assign(bxH.begin(), bxH.end());
        t.byHist.assign(byH.begin(), byH.end());
        t.bzHist.assign(bzH.begin(), bzH.end());
        t.frames.assign(frames.begin(), frames.end());
        t.log.assign(logLines.begin(), logLines.end());
        if (!frames.empty()) {
            t.lastMarker = frames.front().marker;
            t.lastSub = frames.front().sub;
            t.lastTimestamp = frames.front().timestamp;
            t.bx = frames.front().bx; t.by = frames.front().by; t.bz = frames.front().bz;
            t.magOk = frames.front().magOk;
            t.bmag = std::sqrt(t.bx * t.bx + t.by * t.by + t.bz * t.bz);
        }
        std::lock_guard<std::mutex> lk(mu);
        t.seq = ++seq;
        pub = std::move(t);
    }
};

GgakReceiver::GgakReceiver() : p_(std::make_unique<Impl>()) {}
GgakReceiver::~GgakReceiver() = default;

void GgakReceiver::configure(double r) {
    { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; }
    if (r <= 0) return;
    p_->setup(r);
    p_->nIn = 0; p_->nextReport = 0.25 * r;
}
void GgakReceiver::setSignalOffset(double hz) { p_->offset = hz; }
bool GgakReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= p_->minInputRate() - 1; }
void GgakReceiver::reset() { p_->resetReq = true; }
void GgakReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->pub.frames.clear(); p_->pub.log.clear(); p_->frames.clear(); p_->logLines.clear();
    p_->ok = p_->bad = p_->dup = 0; p_->haveLast = false; p_->pub.seq++;
    p_->clearReq = true;
}
void GgakReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false) && m.rate > 0) m.resetState();
    if (m.clearReq.exchange(false)) { m.frames.clear(); m.logLines.clear(); m.ok = m.bad = m.dup = 0; m.haveLast = false; }
    if (m.rate <= 0 || n == 0) return;
    size_t i = 0;
    while (i < n) {
        const size_t left = (size_t)std::max<int64_t>(1, (int64_t)m.nextReport - m.nIn);
        const size_t k = std::min(n - i, left);
        if (m.rate >= m.minInputRate() - 1) m.feed(x + i, k);
        m.nIn += (int64_t)k;
        i += k;
        if ((double)m.nIn >= m.nextReport) { m.nextReport += 0.25 * m.rate; m.report(); }
    }
}
bool GgakReceiver::telemetry(GgakTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void GgakReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->logCb = std::move(cb); }

namespace {
struct GgakRec { uint8_t raw[kGgakFrameBytes]{}; int marker = 0, sub = 0; uint64_t timestamp = 0; bool crcOk = false; };
}
std::string GgakReceiver::saveResults() const {
    std::vector<GgakRec> v;
    { std::lock_guard<std::mutex> lk(p_->mu); for (const auto& f : p_->frames) { GgakRec r; std::memcpy(r.raw, f.raw, kGgakFrameBytes); r.marker = f.marker; r.sub = f.sub; r.timestamp = f.timestamp; r.crcOk = f.crcOk; v.push_back(r); if (v.size() > 40) break; } }
    return results::pack(v);
}
bool GgakReceiver::loadResults(const std::string& blob) {
    std::vector<GgakRec> v;
    if (!results::unpack(blob, v)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->frames.clear();
    for (const auto& r : v) {
        GgakFrame f;
        std::memcpy(f.raw, r.raw, kGgakFrameBytes);
        ggakParse(f);
        f.crcOk = r.crcOk;
        p_->frames.push_back(f);
    }
    p_->pub.seq++;
    return true;
}

struct GgakGenerator::Impl {
    // Pulse shaping: each symbol is a root-raised-cosine pulse centred on its own time (sample units); a sample is the sum of the pulses of
    // the symbols within +-6 symbols of it. (The earlier version ran sample-spaced taps over a symbol-spaced history, which smeared the
    // symbols into a narrow, mostly silent signal that the receiver never locked to.)
    GgakGenConfig c;
    double sps = 0, spsEff = 0, half = 0, t = 0, nextT = 0, peak = 1;
    std::deque<std::pair<double, float>> syms;   // (time in samples, +-amplitude)
    int bitI = 0, copy = 0, marker = 0;
    uint8_t frame[kGgakFrameBytes]{};
    std::complex<double> nco{1, 0}, rot{1, 0};
    genutil::NoiseSource noise;
    uint64_t ts = 1;
    int16_t bx = 12000, by = -3400, bz = 41000;

    explicit Impl(const GgakGenConfig& cfg) : c(cfg), noise(cfg.seed) {
        sps = c.rate / c.symbolRate;
        spsEff = sps / (1.0 + c.sroPpm * 1e-6);
        half = 6 * sps;
        peak = rrc(0, c.rolloff);
        if (std::fabs(peak) < 1e-9) peak = 1;
        const double w = 2 * kPi * c.cfoHz / c.rate;
        rot = std::complex<double>(std::cos(w), std::sin(w));
        nextFrame();
    }
    void nextFrame() {
        ggakBuildTestFrame(frame, marker, marker == 7 ? 7 : 0, ts, bx, by, bz);
        marker = (marker + 1) & 7;
        ts++;
        bitI = 0;
    }
    int nextBit() {
        const int byte = bitI / 8, bit = 7 - (bitI % 8);
        const int b = (frame[byte] >> bit) & 1;
        bitI++;
        if (bitI >= kGgakFrameBits) {
            bitI = 0;
            copy++;
            if (copy >= 2) { copy = 0; nextFrame(); }   // FEC-ski: send twice
        }
        return b;
    }
    void generate(cf32* out, size_t n) {
        const float amp = (float)c.amplitude;
        const double nVar = c.snrDb >= 99 ? 0 : amp * amp * std::pow(10.0, -c.snrDb / 10.0);
        for (size_t i = 0; i < n; i++) {
            while (nextT <= t + half) { syms.emplace_back(nextT, nextBit() ? -amp : amp); nextT += spsEff; }
            while (!syms.empty() && syms.front().first < t - half) syms.pop_front();
            double acc = 0;
            for (const auto& sy : syms) acc += sy.second * rrc((t - sy.first) / sps, c.rolloff);
            cf32 v((float)(acc / peak), 0.f);
            nco *= rot;
            if ((i & 1023) == 0) nco /= std::abs(nco);
            v *= cf32((float)nco.real(), (float)nco.imag());
            if (nVar > 0) v += noise.next((float)std::sqrt(nVar));
            out[i] = v;
            t += 1;
            if (t > 1e9) { for (auto& sy : syms) sy.first -= 1e9; nextT -= 1e9; t -= 1e9; }
        }
    }
};

GgakGenerator::GgakGenerator(const GgakGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
GgakGenerator::~GgakGenerator() = default;
void GgakGenerator::generate(cf32* out, size_t n) { p_->generate(out, n); }

} // namespace dect2
