// CIS HF modes receiver and engine test signal (see cishf_rx.h).
#include "dect2/cishf/cishf_rx.h"
#include "dect2/cishf/cishf_dsp.h"
#include "dect2/cishf/cishf_tel.h"
#include "dect2/marine/marine_dsp.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
}  // namespace

struct CisHfReceiver::Impl {
    const int kind;
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rate = 0, offsetHz = 0, forcedBaud = 0, forcedShift = 0;
    CisHfTelemetry pub;
    std::atomic<bool> resetReq{true}, offsetReq{false}, clearReq{false}, forcedReq{false};
    // feed thread
    double curRate = 0;
    bool running = false;
    marine::Front front;
    Half half;
    std::unique_ptr<cishf::Decoder> dec;
    std::vector<cf32> base, b12;
    int64_t nIn = 0, n12 = 0;
    double nextReport = 0, t0 = 0, secPow = 0;
    int secN = 0;
    float levelDb = -200;
    // results from the sinks (feed thread, copied under mu)
    std::string text;
    std::vector<CisHfEvent> events;
    bool changed = true;

    explicit Impl(int k) : kind(k) {}

    void resetState() {
        double off, fb, fs;
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); curRate = rate; off = offsetHz; cb = log; fb = forcedBaud; fs = forcedShift; }
        running = curRate >= cisHfTuning(kind).minSampleRate - 1;
        if (running) { front.configure(curRate); front.setOffsetHz(off); }
        half.reset();
        dec = cishf::makeDecoder(kind);
        dec->setTextSink([this](const std::string& s) {
            text += s;
            if (text.size() > kCisHfTextMax) text.erase(0, text.size() - kCisHfTextMax);
            changed = true;
        });
        dec->setEventSink([this](const std::string& s) {
            CisHfEvent e;
            e.timeSec = (double)n12 / cishf::kRate; e.utc = (int64_t)(t0 + e.timeSec); e.text = s;
            events.insert(events.begin(), e);
            if (events.size() > kCisHfEventsMax) events.pop_back();
            changed = true;
            std::function<void(const std::string&)> c;
            { std::lock_guard<std::mutex> lk(mu); c = log; }
            if (c) c(std::string(cisHfName(kind)) + ": " + s);
        });
        if (fb > 0 || fs > 0) dec->setForced(fb, fs);
        nIn = 0; n12 = 0; nextReport = 0; secPow = 0; secN = 0; levelDb = -200;
        t0 = nowUtc();
        { std::lock_guard<std::mutex> lk(mu); pub.inputRate = curRate; }
        changed = true;
        if (cb && curRate > 0) {
            if (!running) cb(std::string(cisHfName(kind)) + ": the sample rate is too low (250 kHz at least)");
            else cb(std::string(cisHfName(kind)) + (cisHfIdentifyOnly(kind) ? ": identification only (the frame format is not published), upper sideband"
                                                                             : ": receive only, upper sideband; nothing is decrypted"));
        }
    }

    void report() {
        const cishf::Status s = dec ? dec->status() : cishf::Status();
        std::lock_guard<std::mutex> lk(mu);
        CisHfTelemetry& p = pub;
        p.seq++;
        p.kind = kind;
        p.identifyOnly = cisHfIdentifyOnly(kind);
        p.state = s.state;
        p.mode = s.mode;
        p.params.clear();
        for (const auto& q : s.params) p.params.emplace_back(q.key, q.value);
        p.blocksOk = s.good; p.blocksBad = s.bad;
        p.dataValid = s.state == 2;
        p.cfoHz = s.cfoHz;
        p.snrDb = (float)s.snrDb;
        p.levelDb = levelDb;
        p.timeSec = (double)n12 / cishf::kRate;
        p.inputRate = curRate;
        p.forcedBaud = forcedBaud; p.forcedShift = forcedShift;
        if (changed) { p.text = text; p.events = events; changed = false; }
    }
};

CisHfReceiver::CisHfReceiver(int kind) : p_(std::make_unique<Impl>(kind >= 0 && kind < kCisHfKinds ? kind : 0)) {}
CisHfReceiver::~CisHfReceiver() = default;

void CisHfReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->resetReq = true; }
void CisHfReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetHz = hz; p_->offsetReq = true; }
bool CisHfReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate >= cisHfTuning(p_->kind).minSampleRate - 1; }
void CisHfReceiver::reset() { p_->resetReq = true; }
void CisHfReceiver::setForced(double baud, double shiftHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->forcedBaud = std::max(0.0, baud); p_->forcedShift = std::max(0.0, shiftHz);
    p_->forcedReq = true;
}

void CisHfReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) { m.offsetReq = false; m.forcedReq = false; m.resetState(); }
    if (m.offsetReq.exchange(false) && m.running) { std::lock_guard<std::mutex> lk(m.mu); m.front.setOffsetHz(m.offsetHz); }
    if (m.forcedReq.exchange(false) && m.dec) { double b, s; { std::lock_guard<std::mutex> lk(m.mu); b = m.forcedBaud; s = m.forcedShift; } m.dec->setForced(b, s); }
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

bool CisHfReceiver::telemetry(CisHfTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void CisHfReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void CisHfReceiver::clearResults() {
    p_->clearReq = true;
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->pub.events.clear(); p_->pub.text.clear(); p_->pub.seq++;
}

ModeTuning cisHfTuning(int kind) {
    static const char* ids[kCisHfKinds] = {"cisbee", "cis14", "cis12", "cis36", "cishdr", "cis128", "cis3000"};
    static const char* names[kCisHfKinds] = {"CIS-36-50 (BEE)", "CIS-14", "CIS-12", "CIS-36", "CIS OFDM HDR", "CIS-128", "CIS-3000"};
    if (kind < 0 || kind >= kCisHfKinds) kind = 0;
    ModeTuning t;
    t.stdMode = kCisHfFamily0 + 2 + kind;
    t.id = ids[kind];
    t.name = names[kind];
    t.minMhz = 1.5; t.maxMhz = 30; t.defMhz = 8.0;   // a starting point; the stations move between 3 and 20 MHz
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.004;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 20000;
    return t;
}

// ---------------------------------------------------------------- engine test signal
namespace {
class CisSynth : public ModeSynth {
public:
    CisSynth(int kind, const SynthConfig& s, double rate) : rate_(rate) {
        cishf::GenOpts o;
        o.seed = 11;
        o.leadSec = 0.5; o.tailSec = 0.5;
        const int opt = s.modeOpt[0];
        std::vector<cf32> x;
        switch (kind) {
        case cishf::Bee: {
            if (opt == 1) { o.idleBaud = 50; o.trafficBaud = 100; o.shiftHz = 250; }
            std::vector<int> valid;
            for (int c = 0; c < 128; c++) if (cishf::beeValidChar(c)) valid.push_back(c);
            cishf::BeeMsg m;
            uint32_t r = 5;
            auto next = [&] { r = r * 1103515245u + 12345u; return valid[(r >> 8) % valid.size()]; };
            for (int i = 0; i < 10; i++) m.key.push_back(next());
            for (int i = 0; i < 120; i++) m.chars.push_back(next());
            x = cishf::genBee(o, {m});
            break;
        }
        case cishf::Cis14: {
            std::vector<uint16_t> f;
            for (int i = 0; i < 600; i++) f.push_back(cishf::cis14Frame(2, i % 32, (i * 7) % 32, i & 3));
            o.invert = true;   // 1 = the higher tone, as the decoder shows it
            x = cishf::genCis14(o, f);
            break;
        }
        case cishf::Cis12: o.variant = opt == 1 ? 1 : 0; o.symbols = 1200; x = cishf::genCis12(o, nullptr); break;
        case cishf::Cis36: o.trafficBaud = opt == 1 ? 10 : opt == 2 ? 40 : 20; o.symbols = (int)(o.trafficBaud * 30); x = cishf::genCis36(o); break;
        case cishf::Hdr: o.variant = std::clamp(opt, 0, 3); o.symbols = 1200; x = cishf::genOfdm(cishf::Hdr, o); break;
        case cishf::C128: o.variant = 0; o.symbols = 600; x = cishf::genOfdm(cishf::C128, o); break;
        default: o.symbols = 3000 * 20; x = cishf::genCis3000(o); break;
        }
        sig_ = cishf::impair(x, s.cfoHz, s.snrDb > 0 ? s.snrDb : 30, s.sroPpm, 7);
        step_ = cishf::kRate / rate_;
        rot_ = cf32(1, 0);
        const double w = -2 * kPi * 20000.0 / rate_;   // the dial sits 20 kHz below the radio's centre
        rotStep_ = cf32((float)std::cos(w), (float)std::sin(w));
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        const size_t L = sig_.size();
        if (!L) { std::fill(out, out + n, cf32(0, 0)); return; }
        for (size_t i = 0; i < n; i++) {
            // 12-tap windowed-sinc interpolation of the 12 kHz signal (the signal is narrower than 6 kHz, so the images are small)
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

std::unique_ptr<ModeSynth> makeCisHfSynth(int kind, const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0 || kind < 0 || kind >= kCisHfKinds) return nullptr;
    return std::make_unique<CisSynth>(kind, cfg, sampleRate);
}

} // namespace dect2
