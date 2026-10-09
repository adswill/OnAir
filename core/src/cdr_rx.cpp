// CDR receiver: skeleton (see cdr_rx.h). Nothing is decoded yet.
#include "dect2/cdr_rx.h"
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace dect2 {

struct CdrReceiver::Impl {
    std::mutex mu;                         // guards the members up to pub
    std::function<void(const std::string&)> log;
    double rate = 0;                       // from configure()
    CdrTelemetry pub;                     // the published report
    std::atomic<bool> resetReq{true};
    // receiver thread
    CdrTelemetry tel;
    double curRate = 0;
    int64_t nIn = 0;
    double nextReport = 0;
    double power = 0;                      // sum of |x|^2 since the last report
    int64_t nPower = 0;

    void resetState() {
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); curRate = rate; cb = log; }
        const uint64_t s = tel.seq;        // the report number goes on
        tel = CdrTelemetry();
        tel.seq = s;
        tel.inputRate = curRate;
        nIn = 0; nextReport = 0; power = 0; nPower = 0;
        if (cb && curRate > 0) cb("CDR: the decoder is not built yet, only the input level is measured");
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

CdrReceiver::CdrReceiver() : p_(std::make_unique<Impl>()) {}
CdrReceiver::~CdrReceiver() = default;

// configure() and reset() may come from another thread than feed(): they only leave a request that feed() carries out
void CdrReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = inputRateHz;
    p_->resetReq = true;
}
bool CdrReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rate >= cdrTuning().minSampleRate - 1;
}
void CdrReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetReq = true;
    const uint64_t s = p_->pub.seq;
    p_->pub = CdrTelemetry();
    p_->pub.seq = s;
}

void CdrReceiver::feed(const cf32* x, size_t n) {
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

bool CdrReceiver::telemetry(CdrTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void CdrReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

ModeTuning cdrTuning() {
    ModeTuning t;
    t.stdMode = 24; t.id = "cdr"; t.name = "CDR";
    t.minMhz = 87; t.maxMhz = 108; t.defMhz = 106.1;
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.5;
    t.minSampleRate = 1000000;
    t.tuneOffsetHz = 0;                  // no offset: the digital spectrum sits either side of the station's centre
    return t;
}

} // namespace dect2
