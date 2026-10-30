// Inmarsat all-carriers receiver (inmsat_rx.h).
#include "dect2/inmsat/inmsat_rx.h"
#include "dect2/inmsat/inmsat_tel.h"
#include <mutex>

namespace dect2 {

struct InmsatReceiver::Impl {
    inmsat::Manager mgr;
    inmsat::VoiceMixer voice;
    double rate = 0;
    uint64_t lastMgrSeq = 0, seq = 0;
    std::mutex mu;
    InmsatTelemetry cache;
    bool have = false;
};

InmsatReceiver::InmsatReceiver() : p_(std::make_unique<Impl>()) {}
InmsatReceiver::~InmsatReceiver() = default;

void InmsatReceiver::configure(double r) {
    p_->rate = r;
    inmsat::ManagerConfig c;
    c.rate = r;
    p_->mgr.configure(c);
    // the C-channel decoders hand their coded voice frames to the mixer (which plays them through the user's codec, or keeps them in .bits)
    inmsat::VoiceMixer* voice = &p_->voice;
    p_->mgr.setVoiceSink([voice](int id, double f, const std::vector<uint8_t>& frame, double t) { voice->pushFrame(id, f, frame, t); });
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->have = false; p_->lastMgrSeq = 0;
}
void InmsatReceiver::setSignalOffset(double) {}
bool InmsatReceiver::ready() const { return p_->mgr.ready(); }
void InmsatReceiver::reset() { p_->mgr.reset(); }
void InmsatReceiver::clearResults() { p_->mgr.clearResults(); }
void InmsatReceiver::setLogCallback(std::function<void(const std::string&)> cb) { p_->mgr.setLogCallback(std::move(cb)); }
void InmsatReceiver::setCenterFrequency(double hz) { p_->mgr.setCenter(hz); }
void InmsatReceiver::setLocation(bool k, double la, double lo) { p_->mgr.setLocation(k, la, lo); }
void InmsatReceiver::setSatelliteOverride(const std::string& n) { p_->mgr.setSatelliteOverride(n); }
void InmsatReceiver::setFocus(int id) { p_->mgr.setFocus(id); }
void InmsatReceiver::setBudget(double u) { p_->mgr.setBudget(u); }
void InmsatReceiver::setInmarsatC(bool on) { p_->mgr.setInmcOn(on); }
inmsat::VoiceMixer& InmsatReceiver::voice() { return p_->voice; }

void InmsatReceiver::feed(const cf32* x, size_t n) {
    p_->mgr.feed(x, n);
    inmsat::Snapshot s;
    if (!p_->mgr.snapshot(s, p_->lastMgrSeq)) return;
    p_->lastMgrSeq = s.seq;
    p_->voice.endIdle(s.timeSec);
    const inmsat::VoiceStatus vs = p_->voice.status();
    std::lock_guard<std::mutex> lk(p_->mu);
    static_cast<inmsat::Snapshot&>(p_->cache) = std::move(s);
    p_->cache.seq = ++p_->seq;
    p_->cache.codecReady = vs.codecReady;
    p_->cache.voiceMessage = vs.message;
    p_->cache.activeCalls = vs.activeCalls;
    p_->cache.voice = vs.channels;
    p_->cache.inputRate = p_->rate;
    p_->have = true;
}

bool InmsatReceiver::telemetry(InmsatTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->have || p_->cache.seq <= lastSeq) return false;
    out = p_->cache;
    return true;
}

ModeTuning inmsatTuning() {
    ModeTuning t;
    t.stdMode = 210; t.id = "inmsat"; t.name = "Inmarsat (all carriers)";
    t.minMhz = 1525; t.maxMhz = 1559; t.defMhz = 1545.57;     // between the 600/1200 bit/s P group (about 1545.1) and the 10500 bit/s group (about 1546.0)
    t.sampleRate = 6000000;                                    // Auto: a 6 Msps radio sees both P groups; a slower radio runs as fast as it can
    t.basebandHz = 5000000;
    t.bandwidthMhz = 0.02;
    t.minSampleRate = 1200000;
    t.tuneOffsetHz = 0;                                        // the detector ignores the radio's DC spike itself
    return t;
}

} // namespace dect2
