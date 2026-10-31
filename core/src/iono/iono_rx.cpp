#include "dect2/iono/iono_rx.h"
#include "dect2/results/results_io.h"
#include "dect2/iono/iono_tel.h"
#include "dect2/iono/iono_dsp.h"
#include "dect2/iono/iono_gen.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>

namespace dect2 {

struct IonoReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> cb;
    double rate = 0;
    std::atomic<double> centerReq{0}, originReq{0};
    std::atomic<bool> resetReq{false}, lockReq{true};
    std::atomic<int> selReq{-1};
    std::atomic<bool> obsHave{false};
    std::atomic<double> obsLat{0}, obsLon{0};
    iono::Processor proc;
    bool started = false;
    uint64_t inSamples = 0, nextPub = 0;
    size_t seenEvents = 0;
    IonoTelemetry pub;
    // the results file: a copy of the processor's sounders and timeline, taken by the feed thread every 10 s of signal (mu); Clear and
    // a loaded file reach the processor through these requests
    std::string snap;
    uint64_t nextSnap = 0;
    bool clearReq = false, loadReq = false;
    std::string loadBlob;

    void publish() {
        IonoTelemetry t;
        proc.fill(t);
        // log each sweep once it is complete
        if (t.timeline.size() < seenEvents) seenEvents = 0;
        if (cb)
            for (size_t i = seenEvents; i < t.timeline.size(); i++) {
                const IonoEvent& e = t.timeline[i];
                char b[160];
                snprintf(b, sizeof b, "Ionospheric sounders: sounder %d, %.0f kHz/s sweep, %.2f - %.2f MHz, %.1f dB%s", e.sounder, e.rateHz / 1e3,
                         e.fLoHz / 1e6, e.fHiHz / 1e6, e.snrDb, e.predicted ? " (expected)" : "");
                cb(b);
            }
        seenEvents = t.timeline.size();
        std::lock_guard<std::mutex> lk(mu);
        // a Clear or a loaded file that came in while this feed ran: t still holds the old sounders (the processor takes the request at
        // the next feed), and publishing it would bring back what clearResults() just emptied
        if (clearReq || loadReq) return;
        t.seq = pub.seq + 1;
        pub = std::move(t);
    }
};

IonoReceiver::IonoReceiver() : p_(new Impl) {}
IonoReceiver::~IonoReceiver() = default;

void IonoReceiver::configure(double r) {
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->rate = r;
        IonoTelemetry t;                 // the sounders and the timeline stay (clearResults)
        t.seq = p_->pub.seq; t.sounders = std::move(p_->pub.sounders); t.timeline = std::move(p_->pub.timeline);
        p_->pub = std::move(t);
    }
    p_->resetReq = true;
    if (p_->cb && r > 0) {
        char b[128];
        snprintf(b, sizeof b, "Ionospheric sounders: %.3f Msps, looking for chirps of 100, 125, 150, 200 and 500 kHz/s", r / 1e6);
        p_->cb(b);
    }
}
void IonoReceiver::setCenterHz(double hz) { p_->centerReq = hz; }
void IonoReceiver::setTimeOrigin(double sec) { p_->originReq = sec; p_->resetReq = true; }
bool IonoReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate > 0; }
void IonoReceiver::reset() { p_->resetReq = true; }
void IonoReceiver::setLogCallback(std::function<void(const std::string&)> cb) { p_->cb = std::move(cb); }
void IonoReceiver::setLocked(bool on) { p_->lockReq = on; }
void IonoReceiver::setSelected(int i) { p_->selReq = i; }
void IonoReceiver::setObserver(bool have, double lat, double lon) { p_->obsLat = lat; p_->obsLon = lon; p_->obsHave = have; }

void IonoReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.rate <= 0) return;
    if (m.resetReq.exchange(false) || !m.started) {
        m.proc.configure(m.rate, m.centerReq.load(), m.originReq.load());
        m.started = true; m.inSamples = 0; m.nextPub = 0; m.seenEvents = 0; m.nextSnap = 0;
    }
    m.proc.setCenterHz(m.centerReq.load());
    m.proc.setLocked(m.lockReq.load());
    m.proc.setSelected(m.selReq.load());
    m.proc.setObserver(m.obsHave.load(), m.obsLat.load(), m.obsLon.load());
    {
        std::lock_guard<std::mutex> lk(m.mu);
        if (m.clearReq) { m.proc.clear(); m.clearReq = false; m.snap = m.proc.saveResults(); }
        if (m.loadReq) { m.proc.loadResults(m.loadBlob); m.loadBlob.clear(); m.loadReq = false; }
    }
    m.proc.feed(x, n);
    m.inSamples += n;
    if (m.inSamples >= m.nextSnap) {
        m.nextSnap = m.inSamples + (uint64_t)(m.rate * 10);
        std::string b = m.proc.saveResults();
        std::lock_guard<std::mutex> lk(m.mu);
        m.snap = std::move(b);
    }
    if (m.inSamples >= m.nextPub) { m.nextPub = m.inSamples + (uint64_t)(m.rate / 4); m.publish(); }
}

void IonoReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->clearReq = true; p_->loadReq = false; p_->loadBlob.clear();
    p_->snap.clear();
    p_->pub.sounders.clear(); p_->pub.timeline.clear(); p_->pub.history.clear(); p_->pub.traces.clear(); p_->pub.ionoDb.clear(); p_->pub.swept.clear();
    p_->pub.sel = -1;
    p_->pub.seq++;
}
std::string IonoReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->snap.empty()) { iono::Processor empty; return empty.saveResults(); }
    return p_->snap;
}
bool IonoReceiver::loadResults(const std::string& blob) {
    iono::Processor probe;
    if (!probe.loadResults(blob)) return false;
    IonoTelemetry t;
    probe.fill(t);
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->loadBlob = blob; p_->loadReq = true; p_->clearReq = false;
    p_->snap = blob;
    p_->pub.sounders = t.sounders; p_->pub.timeline = t.timeline;
    p_->pub.seq++;
    return true;
}

bool IonoReceiver::telemetry(IonoTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq == lastSeq) return false;
    out = p_->pub;
    return true;
}

ModeTuning ionoTuning() {
    ModeTuning t;
    t.stdMode = 54; t.id = "iono"; t.name = "Ionospheric sounders";
    t.minMhz = 0.1; t.maxMhz = 40; t.defMhz = kIonoSynthDefMhz;
    t.sampleRate = 20e6;          // a HackRF at 10 MHz sees 1 .. 19 MHz of the 0 .. 30 MHz the sweeps cover
    t.basebandHz = 0;
    t.bandwidthMhz = 20;
    t.minSampleRate = 250e3;
    t.carrierAtCentre = true;     // a sweep crosses the centre: the detector skips the spike bins itself
    return t;
}

std::string ionoSummary(const IonoTelemetry& t) {
    if (t.state == 0) return "ionospheric sounders: waiting for samples";
    int withPeriod = 0;
    for (const auto& s : t.sounders) if (s.periodSec > 0) withPeriod++;
    char b[200];
    snprintf(b, sizeof b, "%.2f - %.2f MHz captured, %zu sounder%s (%d with a period), %zu sweeps heard, %d in the span now, %.1f dBFS",
             t.spanLoHz / 1e6, t.spanHiHz / 1e6, t.sounders.size(), t.sounders.size() == 1 ? "" : "s", withPeriod, t.timeline.size(),
             t.activeSweeps, t.powerDb);
    return b;
}

} // namespace dect2
