// HD Radio receiver: skeleton (see hdr_rx.h). Nothing is decoded yet.
#include "dect2/hdr_rx.h"
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace dect2 {

struct HdrReceiver::Impl {
    std::mutex mu;                         // guards the members up to pub
    std::function<void(const std::string&)> log;
    double rate = 0;                       // from configure()
    HdrTelemetry pub;                     // the published report
    std::atomic<bool> resetReq{true};
    // receiver thread
    HdrTelemetry tel;
    double curRate = 0;
    int64_t nIn = 0;
    double nextReport = 0;
    double power = 0;                      // sum of |x|^2 since the last report
    int64_t nPower = 0;

    void resetState() {
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); curRate = rate; cb = log; }
        const uint64_t s = tel.seq;        // the report number goes on
        tel = HdrTelemetry();
        tel.seq = s;
        tel.inputRate = curRate;
        nIn = 0; nextReport = 0; power = 0; nPower = 0;
        if (cb && curRate > 0) cb("HD Radio: the decoder is not built yet, only the input level is measured");
    }

    void report() {
        tel.seq++;
        tel.timeSec = curRate > 0 ? (double)nIn / curRate : 0;
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }
};

HdrReceiver::HdrReceiver() : p_(std::make_unique<Impl>()) {}
HdrReceiver::~HdrReceiver() = default;

// configure() and reset() may come from another thread than feed(): they only leave a request that feed() carries out
void HdrReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = inputRateHz;
    p_->resetReq = true;
}
bool HdrReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rate >= hdrTuning().minSampleRate - 1;
}
void HdrReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetReq = true;
    const uint64_t s = p_->pub.seq;
    p_->pub = HdrTelemetry();
    p_->pub.seq = s;
}

void HdrReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    if (m.curRate <= 0) return;
    for (size_t i = 0; i < n; i++) m.power += (double)std::norm(x[i]);
    m.nPower += (int64_t)n;
    m.nIn += (int64_t)n;
    // about four reports a second of signal
    while ((double)m.nIn >= m.nextReport) {
        m.nextReport += 0.25 * m.curRate;
        m.report();
    }
}

bool HdrReceiver::telemetry(HdrTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void HdrReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

ModeTuning hdrTuning() {
    ModeTuning t;
    t.stdMode = 23; t.id = "hdr"; t.name = "HD Radio";
    t.minMhz = 0.52; t.maxMhz = 108; t.defMhz = 98.5;
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.4;
    t.minSampleRate = 1500000;
    t.tuneOffsetHz = 0;                  // no offset: the digital sidebands sit 129 to 198 kHz either side of an FM station (AM hybrid: within 15 kHz), the DC spike between them
    return t;
}

} // namespace dect2
