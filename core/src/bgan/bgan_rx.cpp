// BGAN receiver (bgan_rx.h).
#include "dect2/bgan/bgan_rx.h"
#include "dect2/bgan/bgan_manager.h"
#include <cstdio>
#include <mutex>

namespace dect2 {

struct BganReceiver::Impl {
    bgan::Manager mgr;
    double rate = 0;
    uint64_t last = 0;
    std::mutex mu;
    BganTelemetry cache;
    bool have = false;
};

BganReceiver::BganReceiver() : p_(std::make_unique<Impl>()) {}
BganReceiver::~BganReceiver() = default;

void BganReceiver::configure(double r) {
    p_->rate = r;
    bgan::ManagerConfig c; c.rate = r;
    p_->mgr.configure(c);
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->have = false; p_->last = 0;
}
void BganReceiver::setSignalOffset(double) {}
void BganReceiver::setCenterHz(double hz) { p_->mgr.setCenter(hz); }
bool BganReceiver::ready() const { return p_->mgr.ready(); }
void BganReceiver::reset() { p_->mgr.reset(); }
void BganReceiver::clearResults() { p_->mgr.clearResults(); }
void BganReceiver::setLogCallback(std::function<void(const std::string&)> cb) { p_->mgr.setLogCallback(std::move(cb)); }
void BganReceiver::setScanStatus(bool s, int a, int b) { p_->mgr.setScanStatus(s, a, b); }

void BganReceiver::feed(const std::complex<float>* x, size_t n) {
    p_->mgr.feed(x, n);
    BganTelemetry t;
    if (!p_->mgr.snapshot(t, p_->last)) return;
    p_->last = t.seq;
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->cache = std::move(t);
    p_->cache.seq = p_->last;
    p_->have = true;
}

bool BganReceiver::telemetry(BganTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->have || p_->cache.seq <= lastSeq) return false;
    out = p_->cache;
    return true;
}

ModeTuning bganTuning() {
    ModeTuning t;
    t.stdMode = 247; t.id = "bgan"; t.name = "Inmarsat BGAN";
    t.minMhz = 1518; t.maxMhz = 1559; t.defMhz = 1545.0;
    t.sampleRate = 2000000;
    t.basebandHz = 2000000;
    t.bandwidthMhz = 1.6;
    t.minSampleRate = 600000;
    t.tuneOffsetHz = 0;
    return t;
}

std::string bganSummary(const BganTelemetry& t) {
    char b[160];
    snprintf(b, sizeof b, "%d carriers, %d BGAN bearers%s%s", t.carriers, t.bearers, t.satellites.empty() ? "" : ", ", t.satellites.c_str());
    return b;
}

} // namespace dect2
