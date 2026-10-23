// PSK demodulator receiver (pskdsp_rx.h): stage 1 brings the search span around the dial to a moderate rate, a background job runs the blind
// analysis on the latest seconds, and a streaming chain (channeliser, matched filter, timing and carrier loops, bit decoder) follows the result.
#include "dect2/pskdsp/pskdsp_rx.h"
#include "dect2/signallab/signallab.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>

namespace dect2 {

ModeTuning pskdTuning() {
    ModeTuning t;
    t.stdMode = pskd::kStandard;
    t.id = "pskdemod"; t.name = "PSK demodulator";
    t.minMhz = 0.1; t.maxMhz = 6000; t.defMhz = 435.0;
    t.sampleRate = 1000000; t.bandwidthMhz = 0.9; t.minSampleRate = 8000; t.tuneOffsetHz = 30000;
    return t;
}

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr size_t kMaxRecent = 8192, kMaxRing = 6000000;
}

struct PskdReceiver::Impl {
    mutable std::mutex mu;
    PskdTelemetry tel;
    // settings
    pskd::Order order = pskd::Order::Auto;
    double forceBaud = 0, forceAlpha = -2, searchHz = 0, manualHz = 0, manualBw = 0;
    bool snap = true, manual = false;
    pskd::BitOptions bo;
    std::string syncText;
    // input
    double rate = 0, offset = 0, center = 0, t = 0, r1 = 0, lastPublish = -1, nextAnalyse = 1;
    bool ready = false, rescan = false;
    pskd::Channeliser stage1;
    std::vector<cf32> s1buf;
    std::vector<cf32> ring;
    // analysis job
    std::thread job;
    std::atomic<bool> jobDone{false};
    bool jobRunning = false;
    pskd::Analysis result;
    // the streaming chain
    bool chainOn = false;
    pskd::Channeliser chan;
    pskd::Demod demod;
    pskd::BitDecoder dec;
    double chainBaud = 0, chainRate = 0, unlockedSince = 0;
    pskd::Order chainOrder = pskd::Order::Bpsk;
    std::vector<cf32> chbuf, symbuf;
    pskd::Bits bitbuf, recent;
    std::vector<cf32> constel;
    uint64_t nBits = 0;
    std::function<void(const std::string&)> log;

    ~Impl() { join(); }
    void join() { if (job.joinable()) job.join(); jobRunning = false; }

    void build() {
        join();
        chainOn = false; ring.clear(); s1buf.clear(); constel.clear(); chbuf.clear(); symbuf.clear();
        t = 0; lastPublish = -1; nextAnalyse = 1; unlockedSince = 0;
        if (!ready) return;
        double span = searchHz > 0 ? searchHz : std::min(rate * 0.9, 250000.0);
        span = std::min(span, rate * 0.98);
        tel.searchHz = span;
        stage1.configure(rate, offset, span, 0);
        r1 = stage1.outRate();
        tel.specLoHz = -r1 / 2; tel.specStepHz = r1 / 1024;
    }

    pskd::AnalyseOptions options() const {
        pskd::AnalyseOptions a;
        a.forceOrder = order; a.forceBaud = forceBaud; a.forceAlpha = forceAlpha; a.snapStandard = snap;
        a.haveCarrier = manual; a.carrierHz = manualHz; a.bwHz = manualBw; a.maxSeconds = 5;
        return a;
    }

    void configureBits() {
        pskd::BitOptions b = bo;
        b.sync = pskd::parseBitsOrHex(syncText);
        dec.configure(chainOrder, b);
    }

    void startChain(const pskd::Analysis& A) {
        chainOrder = A.order;
        chainBaud = A.baud;
        const double alpha = A.rect ? 0 : A.alpha;
        const double bwSig = A.rect ? 3.0 * A.baud : (1 + std::max(0.1, A.alpha)) * A.baud * 1.3;
        chan.configure(r1, A.carrierHz, bwSig, 4.0 * A.baud);
        chainRate = chan.outRate();
        pskd::DemodConfig dc;
        dc.order = A.order; dc.sps = chainRate / A.baud; dc.alpha = alpha;
        demod.configure(dc);
        configureBits();
        chainOn = true;
        unlockedSince = t;
        char b[160];
        snprintf(b, sizeof b, "PSK demodulator: following %s at %.6g baud, %+.1f Hz from the dial", pskd::orderName(A.order), A.baud, A.carrierHz);
        if (log) log(b);
    }

    bool liveLocked() const {
        const pskd::DemodStats s = demod.stats();
        return chainOn && s.symbols > 300 && s.evm < 0.6 && s.lockMetric > 0.3;
    }

    void applyResult() {
        const pskd::Analysis& A = result;
        tel.haveAnalysis = true;
        tel.status = A.status;
        tel.log = A.log;
        tel.signal = A.signal; tel.rateFound = A.rateFound; tel.analysisLocked = A.locked;
        tel.carrierHz = A.carrierHz; tel.bwHz = A.carrier.occHz; tel.analysisSnrDb = A.carrier.snrDb;
        tel.baud = A.baud; tel.baudRaw = A.baudRaw; tel.baudSnapped = A.baudSnapped;
        tel.order = (int)A.order; tel.alpha = A.alpha; tel.rect = A.rect; tel.shape = A.shape;
        tel.evm = A.evm; tel.merDb = A.merDb; tel.esn0Db = A.esn0Db; tel.ebn0Db = A.ebn0Db; tel.bitRate = A.bitRate; tel.dopplerHzPerSec = A.dopplerHzPerSec;
        tel.line2 = A.line2; tel.line4 = A.line4; tel.line8 = A.line8; tel.lineEnv = A.lineEnv;
        tel.rates.clear();
        for (size_t i = 0; i < A.rates.size() && i < 6; i++) tel.rates.push_back({A.rates[i].baud, A.rates[i].conf});
        tel.orderEvm.clear();
        for (auto& e : A.orderEvm) tel.orderEvm.push_back({(int)e.first, e.second});
        tel.envSpecDb = A.envSpecDb; tel.envHzPerBin = A.envHzPerBin;
        tel.powSpecDb = A.powSpecDb; tel.powM = A.powM; tel.powHzPerBin = A.powHzPerBin;
        if (A.signal && A.rateFound && A.baud > 0) {
            const bool differs = !chainOn || std::fabs(A.baud - chainBaud) > 0.003 * chainBaud || A.order != chainOrder;
            if (differs && (!chainOn || !liveLocked() || rescan)) startChain(A);
            rescan = false;
        }
    }

    void runChain(const std::vector<cf32>& in) {
        chbuf.clear();
        chan.process(in.data(), in.size(), chbuf);
        symbuf.clear();
        if (!chbuf.empty()) demod.process(chbuf.data(), chbuf.size(), symbuf);
        for (const cf32& z : symbuf) { constel.push_back(z); }
        if (constel.size() > 512) constel.erase(constel.begin(), constel.end() - 512);
        bitbuf.clear();
        dec.push(symbuf.data(), symbuf.size(), bitbuf);
        if (!bitbuf.empty()) {
            nBits += bitbuf.size();
            recent.insert(recent.end(), bitbuf.begin(), bitbuf.end());
            if (recent.size() > 2 * kMaxRecent) recent.erase(recent.begin(), recent.end() - (long)kMaxRecent);
            tel.dataValid = true;
        }
    }

    void publish() {
        tel.seq++;
        tel.timeSec = t; tel.inputRate = rate; tel.centerHz = center;
        tel.analysing = jobRunning;
        // spectrum of the search span
        if (ring.size() >= 4096) {
            const size_t m = std::min<size_t>(ring.size(), 16384);
            std::vector<double> p = signallab::psd(ring.data() + (ring.size() - m), m, 1024);
            std::vector<double> sorted(p);
            std::sort(sorted.begin(), sorted.end());
            const double fl = std::max(1e-30, sorted[sorted.size() / 4]);
            tel.spectrumDb.resize(p.size());
            for (size_t i = 0; i < p.size(); i++) tel.spectrumDb[i] = (float)(10 * std::log10(std::max(1e-30, p[i]) / fl));
            tel.specLoHz = -r1 / 2; tel.specStepHz = r1 / (double)p.size();
        }
        tel.constellation.clear();
        for (const cf32& z : constel) { tel.constellation.push_back(z.real()); tel.constellation.push_back(z.imag()); }
        if (chainOn) {
            const pskd::DemodStats s = demod.stats();
            tel.liveBaud = chainRate / std::max(1e-9, s.spsNow);
            tel.liveEvm = s.evm; tel.liveLock = s.lockMetric;
            tel.liveFreqHz = s.freq * chainRate / (2 * kPi);
            tel.cfoHz = chan.offset() + tel.liveFreqHz;
            tel.snrDb = (float)(-20 * std::log10(std::max(1e-3, s.evm)));
        }
        tel.state = !ready ? 0 : liveLocked() ? 2 : 1;
        tel.bits = nBits; tel.syncs = dec.syncCount(); tel.blocksOk = dec.syncCount();
        tel.rotation = dec.rotation(); tel.conj = dec.conj(); tel.resolved = dec.resolved();
        tel.recent.assign(recent.end() - (long)std::min(recent.size(), kMaxRecent), recent.end());
        // hex rows of the last 24 x 16 bytes
        tel.rows.clear();
        const size_t take = std::min<size_t>(tel.recent.size() / 8 * 8, 24 * 16 * 8);
        if (take) {
            pskd::Bits tail(tel.recent.end() - (long)take, tel.recent.end());
            for (size_t i = 0; i < take; i += 128) tel.rows.push_back(pskd::bitsHex(tail, i, std::min<size_t>(128, take - i)));
        }
        tel.frames.clear();
        const pskd::Bits sync = pskd::parseBitsOrHex(syncText);
        if (!sync.empty()) {
            const auto at = pskd::findSync(tel.recent, sync, std::max(0, (int)sync.size() / 12));
            for (size_t k = at.size() > 12 ? at.size() - 12 : 0; k < at.size(); k++)
                tel.frames.push_back(pskd::bitsHex(tel.recent, at[k], std::min<size_t>(512, tel.recent.size() - at[k])));
        }
    }
};

PskdReceiver::PskdReceiver() : p_(std::make_unique<Impl>()) {}
PskdReceiver::~PskdReceiver() = default;

void PskdReceiver::configure(double rate) {
    std::lock_guard<std::mutex> lock(p_->mu);
    p_->ready = std::isfinite(rate) && rate >= pskdTuning().minSampleRate;
    p_->rate = rate; p_->tel.inputRate = rate;
    p_->build();
    p_->tel.state = p_->ready ? 1 : 0; p_->tel.seq++;
}
void PskdReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> lock(p_->mu);
    if (hz != p_->offset) { p_->offset = hz; p_->build(); p_->tel.seq++; }
}
void PskdReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lock(p_->mu); p_->center = hz; p_->tel.centerHz = hz; }
bool PskdReceiver::ready() const { std::lock_guard<std::mutex> lock(p_->mu); return p_->ready; }
void PskdReceiver::reset() {
    std::lock_guard<std::mutex> lock(p_->mu);
    p_->build();
    p_->tel.state = p_->ready ? 1 : 0; p_->tel.status.clear(); p_->tel.haveAnalysis = false; p_->tel.baud = 0; p_->tel.seq++;
}
void PskdReceiver::clearResults() {
    std::lock_guard<std::mutex> lock(p_->mu);
    p_->nBits = 0; p_->recent.clear(); p_->tel.dataValid = false; p_->tel.recent.clear(); p_->tel.rows.clear(); p_->tel.frames.clear(); p_->tel.seq++;
}
void PskdReceiver::setOrder(pskd::Order o) { std::lock_guard<std::mutex> lock(p_->mu); p_->order = o; p_->rescan = true; p_->nextAnalyse = 0; }
void PskdReceiver::setBaud(double b) { std::lock_guard<std::mutex> lock(p_->mu); p_->forceBaud = std::max(0.0, b); p_->rescan = true; p_->nextAnalyse = 0; }
void PskdReceiver::setAlpha(double a) { std::lock_guard<std::mutex> lock(p_->mu); p_->forceAlpha = a; p_->rescan = true; p_->nextAnalyse = 0; }
void PskdReceiver::setSnapStandard(bool on) { std::lock_guard<std::mutex> lock(p_->mu); p_->snap = on; }
void PskdReceiver::setSearchHz(double hz) {
    std::lock_guard<std::mutex> lock(p_->mu);
    if (hz != p_->searchHz) { p_->searchHz = hz; p_->build(); p_->tel.seq++; }
}
void PskdReceiver::setManualCarrier(bool on, double hz, double bw) {
    std::lock_guard<std::mutex> lock(p_->mu);
    p_->manual = on; p_->manualHz = hz; p_->manualBw = bw; p_->rescan = true; p_->nextAnalyse = 0;
}
void PskdReceiver::setBits(pskd::Diff d, pskd::Scrambler s, const std::string& sync, bool invert, bool autoAmb, int rotation, bool conj) {
    std::lock_guard<std::mutex> lock(p_->mu);
    p_->bo.diff = d; p_->bo.scr = s; p_->syncText = sync; p_->bo.invert = invert; p_->bo.autoAmbiguity = autoAmb; p_->bo.rotation = rotation & 7; p_->bo.conj = conj;
    if (p_->chainOn) p_->configureBits();
}
void PskdReceiver::analyseNow() { std::lock_guard<std::mutex> lock(p_->mu); p_->rescan = true; p_->nextAnalyse = 0; }
void PskdReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lock(p_->mu); p_->log = std::move(cb); }

void PskdReceiver::feed(const cf32* iq, size_t n) {
    if (!iq || !n) return;
    std::lock_guard<std::mutex> lock(p_->mu);
    Impl& s = *p_;
    if (!s.ready) return;
    s.s1buf.clear();
    s.stage1.process(iq, n, s.s1buf);
    s.t += (double)n / s.rate;
    s.ring.insert(s.ring.end(), s.s1buf.begin(), s.s1buf.end());
    if (s.ring.size() > kMaxRing + kMaxRing / 2) s.ring.erase(s.ring.begin(), s.ring.end() - (long)kMaxRing);
    if (s.chainOn && !s.s1buf.empty()) {
        s.runChain(s.s1buf);
        if (s.liveLocked()) s.unlockedSince = s.t;
    }
    // finished analysis
    if (s.jobRunning && s.jobDone.load()) {
        s.join();
        s.applyResult();
        s.tel.seq++;
    }
    // start the next one
    const size_t minLen = (size_t)std::max(8192.0, s.r1 * 0.8);
    if (!s.jobRunning && s.t >= s.nextAnalyse && s.ring.size() >= minLen) {
        const size_t win = std::min(s.ring.size(), (size_t)(s.r1 * 5.0));
        std::vector<cf32> copy(s.ring.end() - (long)win, s.ring.end());
        const pskd::AnalyseOptions opt = s.options();
        const double fs = s.r1;
        s.jobDone = false; s.jobRunning = true;
        s.nextAnalyse = s.t + (s.liveLocked() ? 12.0 : 3.0);
        s.job = std::thread([&s, copy = std::move(copy), opt, fs]() {
            pskd::Analysis a = pskd::analyse(copy.data(), copy.size(), fs, opt);
            s.result = std::move(a);   // read only after jobDone (the feed thread joins first)
            s.jobDone = true;
        });
    }
    // follow the carrier: move the channel when the loop has drifted
    if (s.chainOn) {
        const pskd::DemodStats st = s.demod.stats();
        const double hz = st.freq * s.chainRate / (2 * kPi);
        if (std::fabs(hz) > 0.04 * s.chainBaud) { s.chan.setOffset(s.chan.offset() + hz); s.demod.addFreq(-st.freq); }
        if (s.t - s.unlockedSince > 4.0 && !s.jobRunning) s.nextAnalyse = std::min(s.nextAnalyse, s.t);   // lost: look again
    }
    if (s.t - s.lastPublish >= 0.25) { s.lastPublish = s.t; s.publish(); }
}

bool PskdReceiver::telemetry(PskdTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lock(p_->mu);
    if (p_->tel.seq <= lastSeq) return false;
    out = p_->tel;
    return true;
}

} // namespace dect2
