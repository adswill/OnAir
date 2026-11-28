// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CATS receiver and test signal (see cats_rx.h). The packet format is in cats.h.
#include "dect2/cats/cats_rx.h"
#include "dect2/cats/cats.h"
#include "dect2/cats/cats_tel.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <mutex>
#include <random>
#include <vector>

namespace dect2 {

namespace {
constexpr double kFs = 76800.0;                 // 8 samples per symbol
constexpr size_t kBuf = (size_t)(kFs * 1.5);    // the last 1.5 s are searched
constexpr size_t kHop = (size_t)(kFs * 0.5);    // every 0.5 s
double nowUtc() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
}

struct CatsReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rate = 0, offsetHz = 0;
    CatsTelemetry pub;
    std::atomic<bool> resetReq{true}, offsetReq{false}, clearReq{false};
    double curRate = 0;
    bool running = false;
    // front end: mix by -offset, average down to kFs
    std::complex<double> rot{1, 0}, rotStep{1, 0};
    double step = 0, left = 0;
    std::complex<float> acc{0, 0};
    std::vector<std::complex<float>> ring;
    size_t wr = 0; int64_t nOut = 0;
    int64_t nIn = 0;
    double nextReport = 0;
    double secPow = 0; int secN = 0;
    std::vector<float> levelHist;
    double t0 = 0;
    struct Seen { std::string key; double sec; };
    std::vector<Seen> seen;
    std::vector<CatsPacketRow> packets;
    std::vector<CatsEvent> events;
    uint64_t okTotal = 0;
    bool changed = true;

    void resetState() {
        double off;
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); curRate = rate; off = offsetHz; cb = log; }
        running = curRate >= catsTuning().minSampleRate - 1;
        if (running) { step = curRate / kFs; left = step; setOffset(off); }
        acc = {0, 0};
        ring.assign(kBuf, {0, 0});
        wr = 0; nOut = 0; nIn = 0; nextReport = 0; secPow = 0; secN = 0;
        seen.clear();
        t0 = nowUtc();
        { std::lock_guard<std::mutex> lk(mu); pub.inputRate = curRate; }
        changed = true;
        if (cb && curRate > 0) cb(running ? "CATS: 2-FSK 9600 bd, deviation 4.8 kHz, LDPC rate 1/2; identification, time, position and comment whiskers" : "CATS: the sample rate is too low (250 kHz or more)");
    }
    void setOffset(double hz) {
        const double w = -2.0 * M_PI * hz / curRate;
        rotStep = std::polar(1.0, w);
    }
    void addEvent(const std::string& s, double sec) {
        CatsEvent e; e.utc = (int64_t)(t0 + sec); e.timeSec = sec; e.text = s;
        events.insert(events.begin(), e);
        if (events.size() > kCatsEventsMax) events.pop_back();
    }
    void pushOut(std::complex<float> v) {
        ring[wr] = v; wr = (wr + 1) % kBuf; nOut++;
        secPow += (double)std::norm(v); secN++;
        if (secN >= (int)kFs) {
            levelHist.push_back((float)(10 * std::log10(secPow / secN + 1e-12)));
            if (levelHist.size() > kCatsHist) levelHist.erase(levelHist.begin());
            secPow = 0; secN = 0; changed = true;
        }
        if (nOut >= (int64_t)kBuf && nOut % (int64_t)kHop == 0) search();
    }
    void search() {
        std::vector<std::complex<float>> w(kBuf);
        for (size_t i = 0; i < kBuf; i++) w[i] = ring[(wr + i) % kBuf];
        const std::vector<cats::Packet> got = cats::demodulate(w, kFs);
        const double now = (double)nOut / kFs;
        seen.erase(std::remove_if(seen.begin(), seen.end(), [&](const Seen& s) { return now - s.sec > 1.6; }), seen.end());
        std::lock_guard<std::mutex> lk(mu);
        for (const cats::Packet& p : got) {
            char k[512];
            snprintf(k, sizeof k, "%s-%d|%llu|%.6f|%.6f|%s|%d", p.callsign.c_str(), p.ssid, (unsigned long long)p.unixTime, p.lat, p.lon, p.comment.c_str(), p.whiskers);
            bool dup = false;
            for (const Seen& s : seen) if (s.key == k) { dup = true; break; }
            if (dup) continue;
            seen.push_back({k, now});
            CatsPacketRow r;
            r.utc = (int64_t)(t0 + now); r.timeSec = now;
            r.callsign = p.callsign; r.ssid = p.ssid; r.icon = p.icon;
            r.hasTime = p.hasTime; r.unixTime = (int64_t)p.unixTime;
            r.hasGps = p.hasGps; r.lat = p.lat; r.lon = p.lon; r.altM = p.altM; r.speedMps = p.speedMps; r.headingDeg = p.headingDeg;
            r.comment = p.comment; r.whiskers = p.whiskers;
            packets.insert(packets.begin(), r);
            if (packets.size() > kCatsPacketsMax) packets.pop_back();
            okTotal++;
            char b[256];
            if (p.hasGps) snprintf(b, sizeof b, "packet from %s-%d: %.5f, %.5f, %.0f m", p.callsign.c_str(), p.ssid, p.lat, p.lon, p.altM);
            else snprintf(b, sizeof b, "packet from %s-%d (%d whiskers)", p.callsign.c_str(), p.ssid, p.whiskers);
            addEvent(b, now);
            changed = true;
        }
    }
    void report() {
        std::lock_guard<std::mutex> lk(mu);
        if (!changed && pub.seq > 0) return;
        CatsTelemetry& p = pub;
        p.seq++;
        p.timeSec = (double)nOut / kFs;
        p.inputRate = curRate;
        p.levelDb = levelHist.empty() ? -200.f : levelHist.back();
        p.levelHist = levelHist;
        p.packets = packets; p.events = events;
        p.blocksOk = okTotal;
        p.state = okTotal ? 1 : 0;
        p.dataValid = okTotal > 0;
        changed = false;
    }
};

CatsReceiver::CatsReceiver() : p_(std::make_unique<Impl>()) {}
CatsReceiver::~CatsReceiver() = default;

void CatsReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->resetReq = true; }
void CatsReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetHz = hz; p_->offsetReq = true; }
bool CatsReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate >= catsTuning().minSampleRate - 1; }
void CatsReceiver::reset() { p_->resetReq = true; }

void CatsReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) { m.offsetReq = false; m.resetState(); }
    if (m.offsetReq.exchange(false) && m.running) { double o; { std::lock_guard<std::mutex> lk(m.mu); o = m.offsetHz; } m.setOffset(o); }
    if (m.clearReq.exchange(false)) { m.packets.clear(); m.events.clear(); m.okTotal = 0; m.seen.clear(); m.changed = true; }
    if (m.curRate <= 0) return;
    m.nIn += (int64_t)n;
    if (m.running) {
        for (size_t i = 0; i < n; i++) {
            std::complex<float> v(x[i].real(), x[i].imag());
            if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = {0, 0};
            v *= std::complex<float>((float)m.rot.real(), (float)m.rot.imag());
            m.rot *= m.rotStep;
            if ((i & 1023) == 0) m.rot /= std::abs(m.rot);
            if (m.left > 1.0) { m.acc += v; m.left -= 1.0; }
            else {
                m.acc += v * (float)m.left;
                m.pushOut(m.acc / (float)m.step);
                m.acc = v * (float)(1.0 - m.left);
                m.left = m.step - (1.0 - m.left);
            }
        }
    }
    while ((double)m.nIn >= m.nextReport) { m.nextReport += 0.25 * m.curRate; m.report(); }
}

bool CatsReceiver::telemetry(CatsTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void CatsReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void CatsReceiver::clearResults() {
    p_->clearReq = true;
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->pub.events.clear(); p_->pub.packets.clear(); p_->pub.blocksOk = 0; p_->pub.state = 0; p_->pub.seq++;
}

ModeTuning catsTuning() {
    ModeTuning t;
    t.stdMode = 902;
    t.id = "cats";
    t.name = "CATS";
    t.minMhz = 144; t.maxMhz = 450; t.defMhz = 432.0;   // 70 cm; no common calling frequency is named by libCATS, so the user sets it (432 MHz is only a start)
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.025;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 20000;
    return t;
}

// ---------------------------------------------------------------- test signal
namespace {
class CatsSynth : public ModeSynth {
public:
    CatsSynth(const SynthConfig& s, double rate) : rate_(rate), rng_(7) {
        cats::Builder b;
        b.addId("TEST", 1, 3);
        b.addTime(1767225600ull);
        b.addGps(52.25, -1.5, 30123.0, 5, 90.0, 12.5);
        b.addComment("OnAir test balloon");
        const std::vector<uint8_t> frame = cats::encodeFrame(b.semi);
        sig_ = cats::modulate(frame, kFs, s.cfoHz);   // baseband at 76.8 kHz
        cycle_ = (size_t)(kFs * 2.0);
        if (sig_.size() < cycle_) sig_.resize(cycle_, {0, 0});
        for (auto& v : sig_) v *= 0.22f;
        const double snr = std::pow(10.0, s.snrDb / 10.0);
        const double pSig = 0.22 * 0.22;
        noiseSigma_ = std::sqrt(pSig / snr * (rate_ / 9600.0) / 2.0);
        if (s.snrDb >= 99) noiseSigma_ = 0;
        step_ = kFs / rate_;
        rotStep_ = std::polar(1.0, -2.0 * M_PI * 20000.0 / rate_);   // the radio sits 20 kHz above the dial
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        std::normal_distribution<float> g(0.f, 1.f);
        for (size_t i = 0; i < n; i++) {
            const size_t k = (size_t)pos_;
            const float f = (float)(pos_ - (double)k);
            const std::complex<float> a = sig_[k % cycle_], c = sig_[(k + 1) % cycle_];
            std::complex<float> v = a * (1.f - f) + c * f;
            v *= std::complex<float>((float)rot_.real(), (float)rot_.imag());
            rot_ *= rotStep_;
            if ((i & 1023) == 0) rot_ /= std::abs(rot_);
            if (noiseSigma_ > 0) v += std::complex<float>(g(rng_), g(rng_)) * (float)noiseSigma_;
            out[i] = cf32(v.real(), v.imag());
            pos_ += step_;
            if (pos_ >= (double)cycle_) pos_ -= (double)cycle_;
        }
    }
private:
    double rate_;
    std::mt19937 rng_;
    std::vector<std::complex<float>> sig_;
    size_t cycle_ = 0;
    double noiseSigma_ = 0, step_ = 1, pos_ = 0;
    std::complex<double> rot_{1, 0}, rotStep_{1, 0};
};
} // namespace

std::unique_ptr<ModeSynth> makeCatsSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<CatsSynth>(cfg, sampleRate);
}

} // namespace dect2
