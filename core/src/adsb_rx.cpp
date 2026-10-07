// ADS-B receiver: the pulse demodulator (adsb_demod), the frame checks and the aircraft table (adsb_track), reports four times a second.
// Everything runs in feed(); telemetry() and the setters only take the lock that protects the table.
#include "dect2/adsb_rx.h"
#include "dect2/adsb_demod.h"
#include <atomic>
#include <cmath>
#include <mutex>

namespace dect2 {

struct AdsbReceiver::Impl {
    std::mutex mu;                       // protects tracker, the report fields and the callbacks
    AdsbTracker tracker;
    AdsbDemod demod;
    std::atomic<double> rate{0};         // read by ready() from other threads
    std::atomic<float> kPulse{3.0f}, kGap{2.0f};    // preamble thresholds: set from any thread, handed to the demodulator by feed()
    std::function<void(const std::string&)> log;
    std::function<void(const AdsbFrame&)> frameCb;
    std::atomic<bool> resetPending{false};

    // report state (under mu)
    uint64_t seq = 0;
    double nextReport = 0.25;
    double now = 0;                      // signal time of the last report
    int state = 0;
    double lastGood = -1e9, lastActivity = -1e9, lastNewLog = -1e9;
    float noiseDbfs = -120;
    uint64_t preambles = 0;

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

AdsbReceiver::~AdsbReceiver() = default;

void AdsbReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = inputRateHz;
    p_->demod.configure(inputRateHz);
    p_->demod.setThresholds(p_->kPulse, p_->kGap);
    p_->tracker.reset();
    p_->nextReport = 0.25; p_->now = 0; p_->state = 0;
    p_->lastGood = p_->lastActivity = p_->lastNewLog = -1e9;
    p_->resetPending = false;
}

bool AdsbReceiver::ready() const { return p_->rate >= adsbTuning().minSampleRate - 1; }

void AdsbReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->tracker.reset();
    p_->state = 0;
    p_->lastGood = p_->lastActivity = -1e9;
    p_->resetPending = true;         // the demodulator is only touched by feed()
}

void AdsbReceiver::feed(const cf32* x, size_t n) {
    Impl& p = *p_;
    if (!ready()) return;
    if (p.resetPending.exchange(false)) p.demod.reset();
    p.demod.setThresholds(p.kPulse, p.kGap);
    p.demod.feed(x, n);
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
void AdsbReceiver::setExpiry(double seconds) { std::lock_guard<std::mutex> lk(p_->mu); AdsbOptions o = p_->tracker.options(); o.expirySec = seconds < 5 ? 5 : seconds; p_->tracker.setOptions(o); }
void AdsbReceiver::clearAircraft() { std::lock_guard<std::mutex> lk(p_->mu); p_->tracker.reset(); }

void AdsbReceiver::setThresholds(float pulseOverNoise, float pulseOverGap) { p_->kPulse = pulseOverNoise; p_->kGap = pulseOverGap; }

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
