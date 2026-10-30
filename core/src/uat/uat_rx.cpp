#include "dect2/session/mode_adapters.h"
#include "dect2/uat/uat_rx.h"
#include "dect2/uat/uat_demod.h"
#include "dect2/uat/uat_proto.h"
#include "dect2/uat/uat_track.h"
#include <atomic>
#include <mutex>

namespace dect2 {

struct UatReceiver::Impl {
    std::mutex mu;
    UatDemod demod;
    UatTracker tracker;
    std::atomic<double> rate{0};
    std::function<void(const std::string&)> log;
    uint64_t seq = 0;
    double nextReport = 0.25;
    double now = 0;
    int state = 0;
    double lastGood = -1e9, lastActivity = -1e9;
    float snrEma = 0;
    bool snrInit = false;
};

UatReceiver::UatReceiver() : p_(std::make_unique<Impl>()) {
    Impl* p = p_.get();
    p->demod.setSink([p](UatRawFrame& raw) {
        UatDecoded dec;
        if (!uatDecodeFrame(raw, dec)) return;
        p->tracker.apply(dec, raw.levelDbfs, raw.timeSec);
        std::lock_guard<std::mutex> lk(p->mu);
        p->lastGood = raw.timeSec;
        if (!p->snrInit) { p->snrEma = raw.levelDbfs - p->demod.noiseDbfs(); p->snrInit = true; }
        else p->snrEma = 0.9f * p->snrEma + 0.1f * (raw.levelDbfs - p->demod.noiseDbfs());
    });
}

UatReceiver::~UatReceiver() = default;

void UatReceiver::configure(double r) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = r;
    p_->demod.configure(r);
    const double t = p_->demod.timeSec();
    p_->nextReport = t + 0.25;
    p_->now = t;
    p_->state = 0;
    p_->lastGood = p_->lastActivity = -1e9;
}

bool UatReceiver::ready() const { return p_->rate >= uatTuning().minSampleRate - 1; }

void UatReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->demod.reset();
    p_->state = 0;
    p_->lastGood = p_->lastActivity = -1e9;
}

void UatReceiver::feed(const cf32* x, size_t n) {
    if (!ready()) return;
    p_->demod.feed(x, n);
    const double t = p_->demod.timeSec();
    while (t >= p_->nextReport) {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->tracker.tick(p_->nextReport);
        p_->now = p_->nextReport;
        const int old = p_->state;
        p_->state = p_->nextReport - p_->lastGood < 5.0 ? 2 : (p_->nextReport - p_->lastActivity < 10.0 ? 1 : 0);
        p_->seq++;
        std::function<void(const std::string&)> f = p_->log;
        std::string msg;
        if (p_->state == 2 && old != 2) msg = "UAT messages are coming in";
        else if (p_->state != 2 && old == 2) msg = "no UAT messages for 5 s";
        p_->nextReport += 0.25;
        if (f && !msg.empty()) f(msg);
    }
}

bool UatReceiver::telemetry(UatTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->seq <= lastSeq) return false;
    out = UatTelemetry();
    p_->tracker.snapshot(out, p_->now);
    out.seq = p_->seq;
    out.state = p_->state;
    out.timeSec = p_->now;
    out.noiseDbfs = p_->demod.noiseDbfs();
    out.dataValid = p_->state == 2;
    out.snrDb = p_->snrInit ? p_->snrEma : 0;
    out.blocksOk = p_->tracker.good();
    out.blocksBad = p_->tracker.bad();
    sess::observeUat(out);
    return true;
}

void UatReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->log = std::move(cb);
}

void UatReceiver::setReference(double la, double lo) { std::lock_guard<std::mutex> lk(p_->mu); p_->tracker.setReference(la, lo); }
void UatReceiver::clearReference() { std::lock_guard<std::mutex> lk(p_->mu); p_->tracker.clearReference(); }
void UatReceiver::setExpiry(double s) { std::lock_guard<std::mutex> lk(p_->mu); p_->tracker.setExpiry(s); }
void UatReceiver::clearResults() { std::lock_guard<std::mutex> lk(p_->mu); p_->tracker.reset(); p_->seq++; }
std::string UatReceiver::saveResults() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->tracker.saveResults(); }
bool UatReceiver::loadResults(const std::string& blob) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->tracker.loadResults(blob, p_->now)) return false;
    p_->seq++;
    return true;
}

ModeTuning uatTuning() {
    ModeTuning t;
    t.stdMode = 85;   // 84 is VDL Mode 4
    t.id = "uat";
    t.name = "UAT (978 MHz)";
    t.minMhz = 960;
    t.maxMhz = 990;
    t.defMhz = 978.0;
    t.sampleRate = 2e6;
    t.basebandHz = 1.5e6;
    t.bandwidthMhz = 1.0;
    t.minSampleRate = 1.9e6;
    t.notCircular = true;
    return t;
}

} // namespace dect2
