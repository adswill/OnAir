// APRS / Packet receiver: skeleton (see packet_rx.h). Nothing is decoded yet.
#include "dect2/packet_rx.h"
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace dect2 {

struct PacketReceiver::Impl {
    std::mutex mu;                         // guards the members up to pub
    std::function<void(const std::string&)> log;
    double rate = 0;                       // from configure()
    double offsetHz = 0;                   // setSignalOffset(): where the user's frequency sits in the input
    PacketTelemetry pub;                     // the published report
    std::atomic<bool> resetReq{true};
    // receiver thread
    PacketTelemetry tel;
    double curRate = 0;
    int64_t nIn = 0;
    double nextReport = 0;
    double power = 0;                      // sum of |x|^2 since the last report
    int64_t nPower = 0;

    void resetState() {
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); curRate = rate; cb = log; }
        const uint64_t s = tel.seq;        // the report number goes on
        tel = PacketTelemetry();
        tel.seq = s;
        tel.inputRate = curRate;
        nIn = 0; nextReport = 0; power = 0; nPower = 0;
        if (cb && curRate > 0) cb("APRS / Packet: the decoder is not built yet, only the input level is measured");
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

PacketReceiver::PacketReceiver() : p_(std::make_unique<Impl>()) {}
PacketReceiver::~PacketReceiver() = default;

// configure() and reset() may come from another thread than feed(): they only leave a request that feed() carries out
void PacketReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = inputRateHz;
    p_->resetReq = true;
}
void PacketReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetHz = hz; }
bool PacketReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rate >= packetTuning().minSampleRate - 1;
}
void PacketReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetReq = true;
    const uint64_t s = p_->pub.seq;
    p_->pub = PacketTelemetry();
    p_->pub.seq = s;
}

void PacketReceiver::feed(const cf32* x, size_t n) {
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

bool PacketReceiver::telemetry(PacketTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void PacketReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

ModeTuning packetTuning() {
    ModeTuning t;
    t.stdMode = 26; t.id = "packet"; t.name = "APRS / Packet";
    t.minMhz = 25; t.maxMhz = 1000; t.defMhz = 144.8;
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.025;
    t.minSampleRate = 1000000;
    t.tuneOffsetHz = 50000;                  // the radio sits 50 kHz above the channel, away from its DC spike (as Inmarsat-C and Aero)
    return t;
}

} // namespace dect2
