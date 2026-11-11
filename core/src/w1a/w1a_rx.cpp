// W1A voiceband modes receiver and engine test signal (see w1a_rx.h).
#include "dect2/w1a/w1a_rx.h"
#include "dect2/w1a/w1a_dsp.h"
#include "dect2/w1a/w1a_tel.h"
#include "dect2/marine/marine_dsp.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <vector>

namespace dect2 {

namespace {
constexpr double kPi = 3.14159265358979323846;
double nowUtc() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// 24 kHz -> 12 kHz complex: windowed-sinc low-pass at 5.6 kHz, every second output
class Half {
public:
    Half() {
        const int c = kTaps / 2;
        double s = 0;
        for (int i = 0; i < kTaps; i++) {
            const double t = i - c, fc = 5600.0 / 24000.0;
            const double v = t == 0 ? 2 * fc : std::sin(2 * kPi * fc * t) / (kPi * t);
            h_[i] = (float)(v * (0.42 - 0.5 * std::cos(2 * kPi * i / (kTaps - 1)) + 0.08 * std::cos(4 * kPi * i / (kTaps - 1))));
            s += h_[i];
        }
        for (float& v : h_) v = (float)(v / s);
        reset();
    }
    void reset() { std::fill(std::begin(d_), std::end(d_), cf32(0, 0)); w_ = 0; odd_ = false; }
    void process(const std::vector<cf32>& in, std::vector<cf32>& out) {
        for (const cf32& x : in) {
            d_[w_] = d_[w_ + kTaps] = x;
            w_ = (w_ + 1) % kTaps;
            odd_ = !odd_;
            if (!odd_) continue;
            cf32 acc(0, 0);
            for (int i = 0; i < kTaps; i++) acc += d_[w_ + i] * h_[i];
            out.push_back(acc);
        }
    }
private:
    static constexpr int kTaps = 63;
    float h_[kTaps];
    cf32 d_[2 * kTaps];
    int w_ = 0;
    bool odd_ = false;
};

// a 16-bit mono WAV file written as the audio comes
class WavOut {
public:
    ~WavOut() { close(); }
    bool open(const std::string& path, int rate) {
        close();
        f_ = fopen(path.c_str(), "wb");
        if (!f_) return false;
        rate_ = rate; bytes_ = 0;
        header();
        return true;
    }
    bool isOpen() const { return f_ != nullptr; }
    void write(const float* x, size_t n) {
        if (!f_) return;
        std::vector<int16_t> b(n);
        for (size_t i = 0; i < n; i++) b[i] = (int16_t)std::lround(std::max(-1.f, std::min(1.f, x[i])) * 32000.f);
        fwrite(b.data(), 2, n, f_);
        bytes_ += (uint32_t)(2 * n);
    }
    void close() {
        if (!f_) return;
        fseek(f_, 0, SEEK_SET);
        header();
        fclose(f_);
        f_ = nullptr;
    }
private:
    void header() {
        auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f_); };
        auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f_); };
        fwrite("RIFF", 1, 4, f_); u32(36 + bytes_); fwrite("WAVEfmt ", 1, 8, f_); u32(16); u16(1); u16(1); u32((uint32_t)rate_); u32((uint32_t)rate_ * 2); u16(2); u16(16);
        fwrite("data", 1, 4, f_); u32(bytes_);
    }
    FILE* f_ = nullptr;
    int rate_ = 12000;
    uint32_t bytes_ = 0;
};
}  // namespace

struct W1aReceiver::Impl {
    const int kind;
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rate = 0, offsetHz = 0, forcedFc = 0;
    std::string recPath;
    W1aTelemetry pub;
    std::atomic<bool> resetReq{true}, offsetReq{false}, clearReq{false}, forcedReq{false}, recReq{false};
    // feed thread
    double curRate = 0;
    bool running = false;
    marine::Front front;
    Half half;
    std::unique_ptr<w1a::Decoder> dec;
    WavOut wav;
    std::vector<cf32> base, b12;
    int64_t nIn = 0, n12 = 0;
    double nextReport = 0, t0 = 0, secPow = 0;
    int secN = 0;
    float levelDb = -200;
    std::string text;
    std::vector<W1aEvent> events;
    bool changed = true;

    explicit Impl(int k) : kind(k) {}

    void resetState() {
        double off, fc;
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); curRate = rate; off = offsetHz; cb = log; fc = forcedFc; }
        running = curRate >= w1aTuning(kind).minSampleRate - 1;
        if (running) { front.configure(curRate); front.setOffsetHz(off); }
        half.reset();
        dec = w1a::makeDecoder(kind);
        dec->setTextSink([this](const std::string& s) {
            text += s;
            if (text.size() > kW1aTextMax) text.erase(0, text.size() - kW1aTextMax);
            changed = true;
        });
        dec->setEventSink([this](const std::string& s) {
            W1aEvent e;
            e.timeSec = (double)n12 / w1a::kRate; e.utc = (int64_t)(t0 + e.timeSec); e.text = s;
            events.insert(events.begin(), e);
            if (events.size() > kW1aEventsMax) events.pop_back();
            changed = true;
            std::function<void(const std::string&)> c;
            { std::lock_guard<std::mutex> lk(mu); c = log; }
            if (c) c(std::string(w1aName(kind)) + ": " + s);
        });
        dec->setAudioSink([this](const float* x, size_t n) { if (wav.isOpen()) wav.write(x, n); });
        if (fc > 0) dec->setForced(fc);
        nIn = 0; n12 = 0; nextReport = 0; secPow = 0; secN = 0; levelDb = -200;
        t0 = nowUtc();
        { std::lock_guard<std::mutex> lk(mu); pub.inputRate = curRate; }
        changed = true;
        if (cb && curRate > 0) {
            if (!running) cb(std::string(w1aName(kind)) + ": the sample rate is too low (250 kHz at least)");
            else cb(std::string(w1aName(kind)) + ": receive only, " + w1aScope(kind) + ", simulated signals only");
        }
    }

    void report() {
        const w1a::Status s = dec ? dec->status() : w1a::Status();
        std::lock_guard<std::mutex> lk(mu);
        W1aTelemetry& p = pub;
        p.seq++;
        p.kind = kind;
        p.state = s.state;
        p.mode = s.mode;
        p.params.clear();
        for (const auto& q : s.params) p.params.emplace_back(q.key, q.value);
        p.blocksOk = s.good; p.blocksBad = s.bad;
        p.dataValid = s.state == 2;
        p.cfoHz = s.cfoHz;
        p.snrDb = (float)s.snrDb;
        p.levelDb = levelDb;
        p.timeSec = (double)n12 / w1a::kRate;
        p.inputRate = curRate;
        p.spec = s.spec;
        p.recording = wav.isOpen();
        p.recordPath = recPath;
        if (changed) { p.text = text; p.events = events; changed = false; }
    }
};

W1aReceiver::W1aReceiver(int kind) : p_(std::make_unique<Impl>(kind >= 0 && kind < kW1aKinds ? kind : 0)) {}
W1aReceiver::~W1aReceiver() = default;

void W1aReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->resetReq = true; }
void W1aReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetHz = hz; p_->offsetReq = true; }
bool W1aReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate >= w1aTuning(p_->kind).minSampleRate - 1; }
void W1aReceiver::reset() { p_->resetReq = true; }
void W1aReceiver::setForced(double fcHz) { std::lock_guard<std::mutex> lk(p_->mu); p_->forcedFc = std::max(0.0, fcHz); p_->forcedReq = true; }
void W1aReceiver::setRecordPath(const std::string& path) { std::lock_guard<std::mutex> lk(p_->mu); p_->recPath = path; p_->recReq = true; }

void W1aReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) { m.offsetReq = false; m.forcedReq = false; m.resetState(); }
    if (m.offsetReq.exchange(false) && m.running) { std::lock_guard<std::mutex> lk(m.mu); m.front.setOffsetHz(m.offsetHz); }
    if (m.forcedReq.exchange(false) && m.dec) { double f; { std::lock_guard<std::mutex> lk(m.mu); f = m.forcedFc; } m.dec->setForced(f); }
    if (m.recReq.exchange(false)) {
        std::string path; { std::lock_guard<std::mutex> lk(m.mu); path = m.recPath; }
        if (path.empty()) m.wav.close();
        else if (!m.wav.open(path, (int)w1a::kRate)) {
            std::function<void(const std::string&)> c; { std::lock_guard<std::mutex> lk(m.mu); c = m.log; m.recPath.clear(); }
            if (c) c(std::string(w1aName(m.kind)) + ": cannot write " + path);
        }
    }
    if (m.clearReq.exchange(false)) { m.text.clear(); m.events.clear(); m.changed = true; }
    if (m.curRate <= 0) return;
    m.nIn += (int64_t)n;
    if (m.running && m.dec) {
        m.base.clear();
        m.front.process(x, n, m.base);
        for (cf32& v : m.base) if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
        m.b12.clear();
        m.half.process(m.base, m.b12);
        for (const cf32& v : m.b12) {
            m.secPow += (double)std::norm(v);
            if (++m.secN >= 12000) { m.levelDb = (float)(10 * std::log10(m.secPow / m.secN + 1e-12)); m.secPow = 0; m.secN = 0; }
        }
        m.n12 += (int64_t)m.b12.size();
        if (!m.b12.empty()) m.dec->push(m.b12.data(), m.b12.size());
    }
    while ((double)m.nIn >= m.nextReport) {
        m.nextReport += 0.25 * m.curRate;
        m.report();
    }
}

bool W1aReceiver::telemetry(W1aTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void W1aReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void W1aReceiver::clearResults() {
    p_->clearReq = true;
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->pub.events.clear(); p_->pub.text.clear(); p_->pub.seq++;
}

ModeTuning w1aTuning(int kind) {
    static const char* ids[kW1aKinds] = {"hfpacket300", "voiceinv", "elt121"};
    if (kind < 0 || kind >= kW1aKinds) kind = 0;
    ModeTuning t;
    t.stdMode = kW1aFamily0 + 2 + kind;
    t.id = ids[kind];
    t.name = w1aName(kind);
    if (kind == 0) { t.minMhz = 1.5; t.maxMhz = 30; t.defMhz = 14.1; t.bandwidthMhz = 0.004; }          // HF, upper sideband
    else if (kind == 1) { t.minMhz = 1.5; t.maxMhz = 500; t.defMhz = 145.5; t.bandwidthMhz = 0.012; }   // narrow FM voice channel
    else { t.minMhz = 121.0; t.maxMhz = 122.0; t.defMhz = 121.5; t.bandwidthMhz = 0.012; }              // 121.5 MHz AM
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 20000;
    return t;
}

// ---------------------------------------------------------------- engine test signal
namespace {
class W1aSynth : public ModeSynth {
public:
    W1aSynth(int kind, const SynthConfig& s, double rate) : rate_(rate) {
        w1a::GenOpts o;
        o.seed = 11;
        std::vector<cf32> x;
        switch (kind) {
        case w1a::Packet: o.frames = 4; o.leadSec = 0.4; o.tailSec = 0.6; x = w1a::genPacket(o); break;
        case w1a::Invert: o.seconds = 12; o.invertHz = s.modeOpt[0] == 1 ? 0 : (s.modeVal[0] > 0 ? s.modeVal[0] : 3000); x = w1a::genInvert(o); break;
        default: o.seconds = 6; o.sweepHz = s.modeVal[0] > 0 ? s.modeVal[0] : 3.0; x = w1a::genElt(o); break;
        }
        sig_ = w1a::impair(x, s.cfoHz, s.snrDb > 0 ? s.snrDb : 30, s.sroPpm, 7);
        step_ = w1a::kRate / rate_;
        rot_ = cf32(1, 0);
        const double w = -2 * kPi * 20000.0 / rate_;   // the dial sits 20 kHz below the radio's centre
        rotStep_ = cf32((float)std::cos(w), (float)std::sin(w));
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        const size_t L = sig_.size();
        if (!L) { std::fill(out, out + n, cf32(0, 0)); return; }
        for (size_t i = 0; i < n; i++) {
            // 12-tap windowed-sinc interpolation of the 12 kHz signal
            const double fl = std::floor(pos_);
            const double fr = pos_ - fl;
            const long b = (long)fl;
            cf32 acc(0, 0);
            for (int k = -5; k <= 6; k++) {
                const double t = k - fr;
                const double sinc = std::fabs(t) < 1e-9 ? 1.0 : std::sin(kPi * t) / (kPi * t);
                const double win = 0.5 + 0.5 * std::cos(kPi * t / 6.5);
                acc += sig_[(size_t)(((b + k) % (long)L + (long)L) % (long)L)] * (float)(sinc * win);
            }
            out[i] = acc * rot_ * 0.5f;
            rot_ *= rotStep_;
            if ((++cnt_ & 1023) == 0) rot_ /= std::abs(rot_);
            pos_ += step_;
            if (pos_ >= (double)L) pos_ -= (double)L;
        }
    }
private:
    double rate_, step_ = 0, pos_ = 0;
    std::vector<cf32> sig_;
    cf32 rot_, rotStep_;
    uint32_t cnt_ = 0;
};
}  // namespace

std::unique_ptr<ModeSynth> makeW1aSynth(int kind, const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0 || kind < 0 || kind >= kW1aKinds) return nullptr;
    return std::make_unique<W1aSynth>(kind, cfg, sampleRate);
}

} // namespace dect2
