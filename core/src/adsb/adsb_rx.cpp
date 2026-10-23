// ADS-B receiver: the pulse demodulator (adsb_demod), the frame checks and the aircraft table (adsb_track), reports four times a second.
// Everything runs in feed(); telemetry() and the setters only take the lock that protects the table.
#include "dect2/adsb/adsb_rx.h"
#include "dect2/adsb/adsb_demod.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

namespace dect2 {

namespace { int64_t steadyNow() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); } }

struct AdsbReceiver::Impl {
    std::mutex mu;                       // protects tracker, the report fields and the callbacks
    AdsbTracker tracker;
    AdsbDemod demod;
    std::atomic<double> rate{0};         // read by ready() from other threads
    std::atomic<float> kPulse{3.0f}, kGap{2.0f};    // preamble thresholds: set from any thread, handed to the demodulator by feed()
    std::atomic<float> dSum{-1}, dMin{0}, dData{0}, dWorth{0};
    std::atomic<bool> coherent{true};   // setDetector (test knob); dSum < -0.5: the demodulator's defaults
    std::function<void(const std::string&)> log;
    std::function<void(const AdsbFrame&)> frameCb;
    std::atomic<bool> resetPending{false};
    std::atomic<int64_t> lastFeedNs{0};  // steady clock of the last feed: a new start moves the clock on by the time it was stopped (#85)

    // report state (under mu)
    uint64_t seq = 0;
    double nextReport = 0.25;
    double now = 0;                      // signal time of the last report
    int state = 0;
    double lastGood = -1e9, lastActivity = -1e9, lastNewLog = -1e9;
    float noiseDbfs = -120;
    uint64_t preambles = 0;

    // another receiver's frame: checked here (CRC), then into the same table; the newest message wins
    void extFrame(int src, const uint8_t* bytes, int bits) {
        if (src < 1 || src > 255 || (bits != 112 && bits != 56)) return;
        AdsbFrame f;
        memcpy(f.bytes, bytes, bits / 8);
        f.bits = bits;
        f.df = (int)adsb::getBits(f.bytes, 1, 5);
        if (bits == 112 ? (f.df != 17 && f.df != 18) : f.df != 11) return;   // the other formats carry the address only in their parity
        if (adsb::crc24(f.bytes, bits) != 0) return;
        if (!adsb::decodeMsg(f.bytes, bits, f.msg)) return;
        f.icao = adsb::getBits(f.bytes, 9, 24);
        f.msg.icao = f.icao;
        f.levelDbfs = -120.f;
        std::lock_guard<std::mutex> lk(mu);
        f.timeSec = now;
        tracker.apply(f, (uint8_t)src);
    }
    void extUpdate(int src, const AdsbExtUpdate& u) {
        if (src < 1 || src > 255) return;
        std::lock_guard<std::mutex> lk(mu);
        tracker.applyExternal(u, (uint8_t)src, now);
    }
    AdsbFeedIn feedIn;                   // last: it stops its threads first

    void say(const std::string& s) {
        std::function<void(const std::string&)> f;
        { std::lock_guard<std::mutex> lk(mu); f = log; }
        if (f) f(s);
    }

    void report(double t) {
        // called without the lock
        std::string msg;
        {
            std::lock_guard<std::mutex> lk(mu);
            tracker.closeWindow();
            tracker.tick(t);
            now = t;
            noiseDbfs = (float)demod.noiseDbfs();
            preambles = demod.preambles();
            const int old = state;
            state = t - lastGood < 5.0 ? 2 : (t - lastActivity < 10.0 ? 1 : 0);
            if (state == 2 && old != 2) msg = "ADS-B messages are coming in";
            else if (state != 2 && old == 2) msg = "no ADS-B messages for 5 s";
            seq++;
        }
        if (!msg.empty()) say(msg);
    }
};

AdsbReceiver::AdsbReceiver() : p_(std::make_unique<Impl>()) {
    Impl* p = p_.get();
    p->demod.setSink([p](AdsbRaw& raw, bool last) {
        AdsbFrame f;
        bool ok;
        std::function<void(const AdsbFrame&)> cb;
        std::string msg;
        {
            std::lock_guard<std::mutex> lk(p->mu);
            ok = p->tracker.accept(raw, f, last);
            if (ok) {
                p->lastGood = f.timeSec;
                cb = p->frameCb;
                if (f.newAircraft && f.timeSec - p->lastNewLog >= 2.0) {
                    p->lastNewLog = f.timeSec;
                    char b[80];
                    snprintf(b, sizeof b, "new aircraft %06X, %.0f dBFS", f.icao, f.levelDbfs);
                    msg = b;
                }
            } else if (last && raw.snrDb >= 8.0f) p->lastActivity = raw.timeSec;
        }
        if (ok && cb) cb(f);
        if (!msg.empty()) p->say(msg);
        return ok;
    });
}

AdsbReceiver::~AdsbReceiver() { p_->feedIn.stop(); }

void AdsbReceiver::setExternalSources(const std::vector<std::string>& hostPorts, bool allowNonLan) {
    Impl* p = p_.get();
    AdsbFeedIn::Sink sk;
    sk.frame = [p](int src, const uint8_t* b, int bits) { p->extFrame(src, b, bits); };
    sk.update = [p](int src, const AdsbExtUpdate& u) { p->extUpdate(src, u); };
    p->feedIn.setSink(std::move(sk));
    p->feedIn.setSources(hostPorts, allowNonLan);
}

void AdsbReceiver::resetStats() { std::lock_guard<std::mutex> lk(p_->mu); p_->tracker.resetStats(); p_->seq++; }

void AdsbReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = inputRateHz;
    p_->demod.configure(inputRateHz);   // (the demodulator's clock runs on through it)
    // #85: started again after a stop (another mode ran in between): the clock moves on by the time it was stopped, so the aircraft heard
    // before show their real age and leave the table after the drop time, instead of looking as if heard a moment ago
    if (const int64_t last = p_->lastFeedNs.load(); last > 0) {
        const double gap = (double)(steadyNow() - last) * 1e-9;
        if (gap > 1.0) p_->demod.advance(gap);
    }
    p_->demod.setThresholds(p_->kPulse, p_->kGap);
    // reset() keeps the sample counter, so timeSec continues. Do not rewind the aircraft table onto a 0..now catch-up:
    // that made even/odd CPR times differ by minutes and no pair decoded.
    const double t = p_->demod.timeSec();
    p_->nextReport = t + 0.25;
    p_->now = t;
    p_->state = 0;
    p_->lastGood = p_->lastActivity = p_->lastNewLog = -1e9;
    p_->resetPending = false;
}

bool AdsbReceiver::ready() const { return p_->rate >= adsbTuning().minSampleRate - 1; }

void AdsbReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);   // the aircraft stay (clearResults): the demodulator's clock runs on through its reset
    p_->state = 0;
    p_->lastGood = p_->lastActivity = -1e9;
    p_->resetPending = true;         // the demodulator is only touched by feed()
}

void AdsbReceiver::feed(const cf32* x, size_t n) {
    Impl& p = *p_;
    if (!ready()) return;
    if (p.resetPending.exchange(false)) p.demod.reset();
    p.demod.setThresholds(p.kPulse, p.kGap);
    if (p.dSum > -0.5f) p.demod.setDetector(p.dSum, p.dMin, p.dData, p.dWorth);
    p.demod.setCoherent(p.coherent);
    p.demod.feed(x, n);
    p.lastFeedNs.store(steadyNow(), std::memory_order_relaxed);
    const double t = p.demod.timeSec();
    while (t >= p.nextReport) {
        p.report(p.nextReport);
        p.nextReport += 0.25;
    }
}

bool AdsbReceiver::telemetry(AdsbTelemetry& out, uint64_t lastSeq) {
    Impl& p = *p_;
    std::lock_guard<std::mutex> lk(p.mu);
    if (p.seq <= lastSeq) return false;
    out = AdsbTelemetry();
    p.tracker.snapshot(out, p.now);
    out.seq = p.seq;
    out.state = p.state;
    out.cfoHz = 0;
    out.dataValid = p.state == 2;
    out.blocksOk = p.tracker.good();
    out.blocksBad = p.tracker.bad();
    out.timeSec = p.now;
    out.noiseDbfs = p.noiseDbfs;
    out.preambles = p.preambles;
    if (p.state == 0) out.snrDb = 0;
    AdsbSourceInfo self;
    self.name = "this receiver"; self.state = p.state == 2 ? 1 : 0; self.msgs = p.tracker.good(); self.aircraft = p.tracker.aircraftFrom(0);
    out.sources.push_back(self);
    for (AdsbSourceInfo i : p.feedIn.info()) {
        i.aircraft = p.tracker.aircraftFrom((uint8_t)(out.sources.size()));
        out.sources.push_back(std::move(i));
    }
    return true;
}

void AdsbReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->log = std::move(cb);
}

void AdsbReceiver::setFrameCallback(std::function<void(const AdsbFrame&)> cb) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->frameCb = std::move(cb);
}

void AdsbReceiver::setReference(double lat, double lon) { std::lock_guard<std::mutex> lk(p_->mu); p_->tracker.setReference(lat, lon); }
void AdsbReceiver::clearReference() { std::lock_guard<std::mutex> lk(p_->mu); p_->tracker.clearReference(); }
void AdsbReceiver::setCorrection(int bits) { std::lock_guard<std::mutex> lk(p_->mu); AdsbOptions o = p_->tracker.options(); o.fixBits = bits < 0 ? 0 : bits > 2 ? 2 : bits; p_->tracker.setOptions(o); }
void AdsbReceiver::setDropSec(double seconds) { std::lock_guard<std::mutex> lk(p_->mu); AdsbOptions o = p_->tracker.options(); o.dropSec = seconds < 0 ? 0 : seconds; p_->tracker.setOptions(o); }
void AdsbReceiver::setExpiry(double seconds) { std::lock_guard<std::mutex> lk(p_->mu); AdsbOptions o = p_->tracker.options(); o.expirySec = seconds < 5 ? 5 : seconds; p_->tracker.setOptions(o); }
void AdsbReceiver::clearAircraft() { std::lock_guard<std::mutex> lk(p_->mu); p_->tracker.reset(); p_->seq++; }
void AdsbReceiver::clearResults() { clearAircraft(); }
std::string AdsbReceiver::saveResults() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->tracker.saveResults(); }
bool AdsbReceiver::loadResults(const std::string& blob) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->tracker.loadResults(blob, p_->now)) return false;
    p_->seq++;
    return true;
}

void AdsbReceiver::setThresholds(float pulseOverNoise, float pulseOverGap) { p_->kPulse = pulseOverNoise; p_->kGap = pulseOverGap; }
void AdsbReceiver::setCoherent(bool on) { p_->coherent = on; }
void AdsbReceiver::setDetector(float sum, float minPulse, float data, float worthDb) { p_->dMin = minPulse; p_->dData = data; p_->dWorth = worthDb; p_->dSum = sum; }

ModeTuning adsbTuning() {
    ModeTuning t;
    t.stdMode = 13; t.id = "adsb"; t.name = "ADS-B";
    t.minMhz = 1085; t.maxMhz = 1095; t.defMhz = 1090.0;
    // 4 Msps with the 3.5 MHz filter: the lowest rate at which the pulses are not smeared into each other. At 2 Msps (the HackRF's minimum, 1.75 MHz
    // filter) every message that is strong enough decodes too, but it takes about 6 dB more signal; docs/modes/adsb.md has the numbers.
    t.sampleRate = 4000000.0; t.basebandHz = 3500000.0; t.bandwidthMhz = 2;
    t.minSampleRate = 2000000.0;
    return t;
}

} // namespace dect2
