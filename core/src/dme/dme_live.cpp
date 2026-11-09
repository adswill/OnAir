// DME for the engine: the envelope is |IQ|, the detector is dme/dme_rx.cpp. See dme_live.h.
#include "dect2/dme/dme_live.h"

#include <cmath>

namespace dect2 {

void DmeLiveReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(mu_);
    rate_ = inputRateHz;
    ready_ = rate_ >= dmeTuning().minSampleRate - 1;
    rx_.configure(rate_);
    snap_ = DmeTelemetry();
    lastIdent_.clear();
    seq_++;
}

void DmeLiveReceiver::setSignalOffset(double) {}

void DmeLiveReceiver::setCenterHz(double hz) {
    std::lock_guard<std::mutex> lk(mu_);
    centerHz_ = hz;
    seq_++;
}

bool DmeLiveReceiver::ready() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ready_;
}

void DmeLiveReceiver::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    rx_.reset();
    snap_ = DmeTelemetry();
    lastIdent_.clear();
    seq_++;
}

void DmeLiveReceiver::feed(const cf32* x, size_t n) {
    if (n == 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (!ready_) return;
    env_.resize(n);
    for (size_t k = 0; k < n; k++) env_[k] = std::abs(x[k]);
    rx_.feed(env_.data(), n);
    publish();
    seq_++;
}

void DmeLiveReceiver::publish() {
    const dme::Telemetry& t = rx_.telemetry();
    snap_.channel = t.channel == dme::Channel::X ? 1 : t.channel == dme::Channel::Y ? 2 : 0;
    snap_.pairRatePps = t.pairRatePps;
    snap_.pairsX = t.pairsX;
    snap_.pairsY = t.pairsY;
    snap_.pairsYInterrogation = t.pairsYInterrogation;
    snap_.identIntervalUs = t.identIntervalUs;
    snap_.centerHz = centerHz_;
    {
        bool y = false;
        snap_.channelNumber = centerHz_ > 0 ? dme::channelFromReplyMHz(centerHz_ / 1e6, y) : 0;
        snap_.channelIsY = y;
    }
    snap_.levelDb = static_cast<float>(t.levelDb);
    snap_.identKeyed = t.identKeyed;
    snap_.tacanValid = t.tacanValid; snap_.tacanRadialDeg = t.tacanRadialDeg; snap_.tacanDepth15 = t.tacanDepth15; snap_.tacanDepth135 = t.tacanDepth135;
    snap_.ident = t.ident;
    snap_.dataValid = t.pairRatePps > 0.0;
    uint64_t letters = 0;
    for (char c : t.ident) if (c != ' ') letters++;
    snap_.blocksOk = letters;
    snap_.state = !t.ident.empty() ? 2 : (t.pairsX + t.pairsY) > 0 ? 1 : 0;
    if (snap_.ident != lastIdent_) {
        lastIdent_ = snap_.ident;
        if (log_ && !lastIdent_.empty()) log_("DME ident " + lastIdent_);
    }
}

bool DmeLiveReceiver::telemetry(DmeTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(mu_);
    if (seq_ == lastSeq) return false;
    out = snap_;
    out.seq = seq_;
    return true;
}

void DmeLiveReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(mu_);
    log_ = std::move(cb);
}

ModeTuning dmeTuning() {
    ModeTuning t;
    t.stdMode = 123; t.id = "dme"; t.name = "DME";
    t.minMhz = 0.962; t.maxMhz = 1.213; t.defMhz = 1.104;   // the DME reply band (ICAO Annex 10); modeTuning() opens the range
    t.sampleRate = 2.4e6;                    // 3.5 us pulses need 2 Msps or more: the sample rate asked of the radio
    t.basebandHz = 0;
    t.bandwidthMhz = 1.0;
    t.minSampleRate = 2e6;
    t.tuneOffsetHz = 0;
    return t;
}

} // namespace dect2
