// Mercury receiver (see mercury_rx.h).
#include "dect2/mercury/mercury_rx.h"
#include "dect2/mercury/mercury_monitor.h"
#include "dect2/mercury/mercury_phy.h"
#include "dect2/mercury/mercury_tel.h"
#include "dect2/marine/marine_dsp.h"
#include "../audio/ham_listen.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kFs = 8000.0;
constexpr int kTaps = 95;   // 24 kHz -> 8 kHz: keeps 1500 +- 1.4 kHz above the dial

double nowUtc() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
} // namespace

struct MercuryReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rate = 0, offsetHz = 0;
    MercuryTelemetry pub;
    std::atomic<bool> resetReq{true}, offsetReq{false}, clearReq{false};
    std::atomic<uint32_t> maskSet{0xFFFFFFFFu};
    uint32_t maskNow = 0xFFFFFFFFu;
    // feed thread
    double curRate = 0;
    bool running = false;
    marine::Front front;
    hfdig::UsbAudio usb;
    std::atomic<int> demodSet{HfDemodUsb};
    hfdig::Listen listen;
    std::vector<cf32> base;
    std::vector<float> aud;
    std::vector<float> taps;
    std::vector<cf32> hist;
    int hpos = 0, phase = 0;
    uint64_t nBase = 0;           // 24 kHz samples mixed so far (the mixer's phase)
    std::vector<mercury::cf> bb;
    mercury::OfdmReceiver ofdm;
    mercury::Monitor mon;
    double t0 = 0;
    int64_t nIn = 0;
    double nextReport = 0, power = 0, secPow = 0;
    int64_t nPower = 0, secN = 0;
    std::vector<float> levelHist;
    double lastFrameSec = -1e9, lastBurstSec = -1e9;
    uint64_t lastDet = 0, monVer = 0;

    Impl() {
        // low-pass at 24 kHz for the 8 kHz output (Blackman windowed sinc, 1.6 kHz cut-off)
        taps.resize(kTaps);
        double sum = 0;
        for (int k = 0; k < kTaps; k++) {
            const double t = k - (kTaps - 1) * 0.5, c = 1600.0 / marine::kBaseRate;
            const double s = t == 0 ? 2 * c : std::sin(2 * kPi * c * t) / (kPi * t);
            const double w = 0.42 - 0.5 * std::cos(2 * kPi * k / (kTaps - 1)) + 0.08 * std::cos(4 * kPi * k / (kTaps - 1));
            taps[(size_t)k] = (float)(s * w);
            sum += s * w;
        }
        for (float& v : taps) v = (float)(v / sum);
        hist.assign(2 * kTaps, cf32(0, 0));
    }

    void say(const std::string& s) {
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); cb = log; }
        if (cb) cb(s);
    }

    void resetState() {
        double off;
        { std::lock_guard<std::mutex> lk(mu); curRate = rate; off = offsetHz; }
        running = curRate >= mercuryTuning().minSampleRate - 1;
        if (running) { front.configure(curRate); front.setOffsetHz(off); }
        usb.reset(); listen.reset();
        std::fill(hist.begin(), hist.end(), cf32(0, 0));
        hpos = 0; phase = 0; nBase = 0;
        ofdm.reset();
        t0 = nowUtc();
        nIn = 0; nextReport = 0; power = 0; nPower = 0; secPow = 0; secN = 0;
        lastDet = 0;
    }

    // 24 kHz complex (dial at 0 Hz) -> 8 kHz complex with 1500 Hz at 0 Hz
    void toBaseband(const cf32* x, size_t n) {
        bb.clear();
        const double w = -2 * kPi * 1500.0 / marine::kBaseRate;   // 1500 / 24000 = 1 / 16: the mixer repeats every 16 samples
        for (size_t i = 0; i < n; i++) {
            const cf32 v = x[i] * std::polar(1.f, (float)(w * (double)(nBase % 16)));
            nBase++;
            hist[(size_t)hpos] = v; hist[(size_t)(hpos + kTaps)] = v;
            if (++hpos >= kTaps) hpos = 0;
            if (++phase < 3) continue;
            phase = 0;
            const cf32* h = &hist[(size_t)hpos];
            cf32 acc(0, 0);
            for (int k = 0; k < kTaps; k++) acc += h[k] * taps[(size_t)(kTaps - 1 - k)];
            secPow += std::norm(acc); secN++;
            bb.push_back(acc);
        }
        // every second of input: the level
        while (secN >= 8000) {
            levelHist.push_back((float)(10 * std::log10(secPow / (double)secN + 1e-20)));
            if (levelHist.size() > 120) levelHist.erase(levelHist.begin());
            secPow = 0; secN = 0;
        }
    }

    void process() {
        ofdm.push(bb.data(), bb.size());
        for (const mercury::OfdmFrame& f : ofdm.take()) {
            const double start = (double)f.startSample / kFs, air = (double)(f.endSample - f.startSample) / kFs;
            mon.onFrame(f, t0 + start, start, air);
            if (f.ldpcOk) lastFrameSec = start + air;
            char b[200];
            snprintf(b, sizeof b, "Mercury: %s burst at %.1f s, %s, SNR %+.1f dB, offset %+.1f Hz", mercury::modeName(f.mode), start,
                     !f.ldpcOk ? "FEC failed" : f.crcPlain ? "CRC OK" : "seeded CRC", f.snrDb, f.cfoHz);
            say(b);
        }
        if (ofdm.detections() != lastDet) { lastDet = ofdm.detections(); lastBurstSec = (double)ofdm.samples() / kFs; }
    }

    void report() {
        std::lock_guard<std::mutex> lk(mu);
        const double secs = (double)ofdm.samples() / kFs;
        pub.seq++;
        pub.timeSec = secs;
        pub.inputRate = curRate;
        pub.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        const uint64_t v = mon.version();
        if (v != monVer) { mon.fill(pub); monVer = v; }
        pub.levelHist = levelHist;
        pub.state = !running ? 0 : (secs - lastFrameSec < 30 ? 2 : (secs - lastBurstSec < 10 ? 1 : 0));
        pub.dataValid = pub.state == 2;
    }
};

MercuryReceiver::MercuryReceiver() : p_(std::make_unique<Impl>()) {}
MercuryReceiver::~MercuryReceiver() = default;

void MercuryReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->resetReq = true; }
void MercuryReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetHz = hz; p_->offsetReq = true; }
bool MercuryReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate >= mercuryTuning().minSampleRate - 1; }
void MercuryReceiver::reset() { p_->resetReq = true; }

void MercuryReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) { m.offsetReq = false; m.resetState(); }
    if (m.offsetReq.exchange(false) && m.running) { std::lock_guard<std::mutex> lk(m.mu); m.front.setOffsetHz(m.offsetHz); }
    if (m.clearReq.exchange(false)) { m.mon.clear(); m.levelHist.clear(); }
    if (const uint32_t mk = m.maskSet.load(); mk != m.maskNow) {
        m.maskNow = mk;
        mercury::RxOptions o;
        o.modeMask = mk;
        m.ofdm.setOptions(o);
    }
    if (m.curRate <= 0) return;
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        m.power += (double)re * re + (double)im * im;
    }
    m.nPower += (int64_t)n;
    m.nIn += (int64_t)n;
    if (m.running) {
        m.base.clear();
        m.front.process(x, n, m.base);
        for (cf32& v : m.base) if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
        if (m.listen.on.load()) {
            m.aud.clear();
            double pw = 0;
            if (const int d = hfDemodResolve(m.demodSet.load(), 0); d != m.usb.demod()) m.usb.setDemod(d);
            m.usb.process(m.base.data(), m.base.size(), m.aud, pw);
            m.listen.play(m.aud.data(), m.aud.size());
        }
        m.toBaseband(m.base.data(), m.base.size());
        m.process();
    }
    while ((double)m.nIn >= m.nextReport) {
        m.nextReport += 0.25 * m.curRate;
        m.report();
    }
}

bool MercuryReceiver::telemetry(MercuryTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void MercuryReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void MercuryReceiver::clearResults() {
    p_->clearReq = true;
    std::lock_guard<std::mutex> lk(p_->mu);
    MercuryTelemetry t;
    t.seq = p_->pub.seq + 1;
    p_->pub = t;
    p_->monVer = 0;
}
void MercuryReceiver::flush() {
    Impl& m = *p_;
    if (!m.running) return;
    m.ofdm.flush();
    m.bb.clear();
    m.process();
    m.monVer = 0;
    m.report();
}
void MercuryReceiver::setModeMask(uint32_t mask) { p_->maskSet = mask; }
void MercuryReceiver::setKnownCalls(const std::vector<std::string>& calls) { p_->mon.setKnownCalls(calls); }
bool MercuryReceiver::fileBytes(int id, std::vector<uint8_t>& data, std::string& name) const { return p_->mon.fileBytes(id, data, name); }
bool MercuryReceiver::sessionBytes(int key, int dir, std::vector<uint8_t>& data) const { return p_->mon.sessionBytes(key, dir, data); }
void MercuryReceiver::setListen(bool on) { p_->listen.on = on; }
bool MercuryReceiver::listening() const { return p_->listen.on.load(); }
void MercuryReceiver::setVolume(float v) { p_->listen.volume = std::max(0.f, std::min(1.f, v)); }
void MercuryReceiver::setMuted(bool m) { p_->listen.muted = m; }
void MercuryReceiver::setSilent(bool s) { p_->listen.silent = s; }
void MercuryReceiver::setListenTap(std::function<void(const float* x, size_t n)> cb) { p_->listen.setTap(std::move(cb)); }
void MercuryReceiver::setDemod(int d) { p_->demodSet = d; }
int MercuryReceiver::demod() const { return p_->demodSet.load(); }

ModeTuning mercuryTuning() {
    ModeTuning t;
    t.stdMode = 232; t.id = "mercury"; t.name = "Mercury (HF OFDM modem)";
    t.minMhz = 1.8; t.maxMhz = 30; t.defMhz = 7.050;     // where Mercury's developers test (docs/CAROUSEL-REPORT.md); stations choose their own
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.004;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 20000;                              // as the other HF digital modes: the radio sits 20 kHz above the dial, away from its DC spike
    return t;
}

const MercuryPreset* mercuryPresets(int& count) {
    static const MercuryPreset kP[] = {{"80 m", 3.590}, {"40 m", 7.050}, {"30 m", 10.145}, {"20 m", 14.105}, {"17 m", 18.105}, {"15 m", 21.105}, {"10 m", 28.120}};
    count = (int)(sizeof kP / sizeof *kP);
    return kP;
}

} // namespace dect2
